/*
 * sourcemap.c
 *
 * Reads the map pvmc writes with -g: one "0xADDR path:line" per line, in
 * address order.
 */
#include "sourcemap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_MAX_LEN 	(260)
#define LINE_MAX_LEN 	(256)

typedef struct{
	uint16_t address;
	int 		 line;
	char 		 file[PATH_MAX_LEN];
} Entry_s;

static Entry_s* g_entries = 0;
static int 			g_count 	= 0;

/* Just the file name. Paths in a map are whatever pvmcc was handed, so they may
 * be absolute, relative, and use either slash -- while the person at the prompt
 * types "blink.hlc". */
const char* SourceMapBaseName(const char* path){
	const char* slash 		 = strrchr(path, '/');
	const char* backslash = strrchr(path, '\\');
	const char* last 			 = (slash > backslash) ? slash : backslash;

	return last ? last + 1 : path;
}

int SourceMapLoad(const char* path){
	FILE* f = fopen(path, "r");
	char 	line[PATH_MAX_LEN + 64];
	int 	capacity = 0;

	if(!f){
		fprintf(stderr, "pvmdbg: cannot open debug map '%s'\n", path);
		return 0;
	}

	free(g_entries);
	g_entries = 0;
	g_count 	= 0;

	while(fgets(line, (int)sizeof(line), f)){
		unsigned 	address;
		char* 		rest;
		char* 		colon;

		if(sscanf(line, "0x%X", &address) != 1) continue;

		rest = strchr(line, ' ');
		if(!rest) continue;
		rest++;

		/* The last colon, not the first: an absolute Windows path has one of its
		 * own right after the drive letter. */
		colon = strrchr(rest, ':');
		if(!colon) continue;
		*colon = '\0';

		if(g_count == capacity){
			int 			new_capacity = capacity ? capacity * 2 : 64;
			Entry_s* 	grown 			 = realloc(g_entries, (size_t)new_capacity * sizeof(Entry_s));

			if(!grown){ fclose(f); return 0; }
			g_entries = grown;
			capacity  = new_capacity;
		}

		g_entries[g_count].address = (uint16_t)address;
		g_entries[g_count].line 	 = atoi(colon + 1);
		snprintf(g_entries[g_count].file, sizeof(g_entries[g_count].file), "%s", rest);
		g_count++;
	}

	fclose(f);
	return 1;
}

int SourceMapLoaded(void){
	return g_count > 0;
}

const char* SourceMapFileFor(uint16_t addr, int* line){
	const Entry_s* best = 0;

	for(int i = 0; i < g_count; i++)
		if(g_entries[i].address <= addr && (!best || g_entries[i].address > best->address))
			best = &g_entries[i];

	if(!best) return 0;

	*line = best->line;
	return best->file;
}

int SourceMapAddrFor(const char* file, int line, uint16_t* addr){
	const Entry_s* best = 0;

	for(int i = 0; i < g_count; i++){
		if(g_entries[i].line != line) continue;
		if(strcmp(SourceMapBaseName(g_entries[i].file), SourceMapBaseName(file)) != 0) continue;

		/* The lowest address for the line: a line can produce several markers
		 * (a "for" header is three statements), and stopping at the first is
		 * what "stop on this line" means. */
		if(!best || g_entries[i].address < best->address) best = &g_entries[i];
	}

	if(!best) return 0;

	*addr = best->address;
	return 1;
}

const char* SourceMapText(const char* file, int line){
	static char text[LINE_MAX_LEN];
	FILE* 			f = fopen(file, "r");
	int 				n = 0;

	if(!f) return 0;

	while(fgets(text, (int)sizeof(text), f)){
		if(++n < line) continue;

		text[strcspn(text, "\r\n")] = '\0';
		fclose(f);
		return text;
	}

	fclose(f);
	return 0;
}
