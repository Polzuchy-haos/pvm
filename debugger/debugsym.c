/*
 * debugsym.c
 *
 * Reads the '@' records of a .dbgmap -- see debugsym.h, and the assembler's
 * GenerateDebugMap for the format:
 *
 *   @func   0xSTART 0xEND NAME
 *   @var    FUNC param|local NAME OFFSET 0xFROM
 *   @global NAME 0xADDR SIZE
 */
#include "debugsym.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_FMT "%31s" /* DEBUGSYM_NAME_MAX - 1 */

typedef struct{
	char 		 name[DEBUGSYM_NAME_MAX];
	uint16_t start, end;
} Func_s;

static Func_s* 				g_funcs 	= 0;
static int 						g_nFuncs 	= 0;
static DebugVar_s* 		g_vars 		= 0;
static int 						g_nVars 	= 0;
static DebugGlobal_s* g_globals = 0;
static int 						g_nGlobals = 0;

/* Appends one zeroed element to a growing array; null when out of memory. */
static void* Grow(void** array, int* count, size_t size){
	void* grown = realloc(*array, (size_t)(*count + 1) * size);

	if(!grown) return 0;
	*array = grown;
	memset((char*)grown + (size_t)*count * size, 0, size);
	return (char*)grown + (size_t)(*count)++ * size;
}

int DebugSymLoad(const char* path){
	FILE* f = fopen(path, "r");
	char 	line[256];

	free(g_funcs); 	 g_funcs 	 = 0; g_nFuncs 	 = 0;
	free(g_vars); 	 g_vars 		 = 0; g_nVars 	 = 0;
	free(g_globals); g_globals = 0; g_nGlobals = 0;

	if(!f) return 0;

	while(fgets(line, (int)sizeof(line), f)){
		unsigned a, b;
		int 		 offset, size;
		char 		 kind[8], name[DEBUGSYM_NAME_MAX], func[DEBUGSYM_NAME_MAX];

		if(line[0] != '@') continue;

		if(sscanf(line, "@func 0x%X 0x%X " NAME_FMT, &a, &b, name) == 3){
			Func_s* fn = Grow((void**)&g_funcs, &g_nFuncs, sizeof(Func_s));
			if(!fn) break;
			fn->start = (uint16_t)a;
			fn->end 	= (uint16_t)b;
			strcpy(fn->name, name);
		}
		else if(sscanf(line, "@var " NAME_FMT " %7s " NAME_FMT " %d 0x%X", func, kind, name, &offset, &a) == 5){
			DebugVar_s* v = Grow((void**)&g_vars, &g_nVars, sizeof(DebugVar_s));
			if(!v) break;
			strcpy(v->func, func);
			strcpy(v->name, name);
			v->isParam = !strcmp(kind, "param");
			v->offset  = offset;
			v->from 	 = (uint16_t)a;
		}
		else if(sscanf(line, "@global " NAME_FMT " 0x%X %d", name, &a, &size) == 3){
			DebugGlobal_s* g = Grow((void**)&g_globals, &g_nGlobals, sizeof(DebugGlobal_s));
			if(!g) break;
			strcpy(g->name, name);
			g->addr = (uint16_t)a;
			g->size = size;
		}
	}

	fclose(f);
	return g_nFuncs + g_nVars + g_nGlobals > 0;
}

const char* DebugSymFuncAt(uint16_t pc, uint16_t* start){
	for(int i = 0; i < g_nFuncs; i++){
		if(pc < g_funcs[i].start || pc >= g_funcs[i].end) continue;
		if(start) *start = g_funcs[i].start;
		return g_funcs[i].name;
	}
	return 0;
}

int DebugSymFuncAddr(const char* name, uint16_t* addr){
	for(int i = 0; i < g_nFuncs; i++){
		if(strcmp(g_funcs[i].name, name)) continue;
		*addr = g_funcs[i].start;
		return 1;
	}
	return 0;
}

int DebugSymVarCount(void){ return g_nVars; }

const DebugVar_s* DebugSymVar(int index){
	return (index >= 0 && index < g_nVars) ? &g_vars[index] : 0;
}

int DebugSymGlobalCount(void){ return g_nGlobals; }

const DebugGlobal_s* DebugSymGlobal(int index){
	return (index >= 0 && index < g_nGlobals) ? &g_globals[index] : 0;
}

const DebugGlobal_s* DebugSymFindGlobal(const char* name){
	for(int i = 0; i < g_nGlobals; i++)
		if(!strcmp(g_globals[i].name, name)) return &g_globals[i];
	return 0;
}
