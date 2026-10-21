/*
 ============================================================================
 Name        : pvmsim.c
 Description : Runs a compiled PVM bytecode file to completion (HALT/error),
               tracing every instruction via PVM_PrintInstr.
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pvm.h"
#include "pvmprint.h"
#include "pvmenv.h"

#ifndef PVM_DEFAULT_BYTECODE
#define PVM_DEFAULT_BYTECODE "bytecode_listing.bin"
#endif

#define DEFAULT_MAX_STEPS (100000L)

static void Usage(void){
	fprintf(stderr,
		"usage: pvmsim [program.bin] [--stack N] [--ram N] [--steps N]\n"
		"       --stack N   words of stack (default %d, 1..%d)\n"
		"       --ram N     words of RAM, addresses 0..N-1 (default %d, 0..%d)\n"
		"       --steps N   give up after N instructions (default %ld)\n"
		"The pools are sized here, as a firmware sizes its own -- run a program\n"
		"against the stack and RAM of the board it is meant for.\n",
		PVM_CFG_STACK_SIZE, PVM_STACK_LEN_MAX, PVM_CFG_RAM_SIZE, PVM_CFG_USRSECT_BGN,
		DEFAULT_MAX_STEPS);
}

/* A whole non-negative number, or -1. */
static long ParseCount(const char* text){
	char* end;
	long 	value = strtol(text, &end, 0);
	return (end == text || *end || value < 0) ? -1 : value;
}

static char* LoadFile(const char* path, int* out_len){
	FILE* f = fopen(path, "rb");
	long size;
	char* buf;

	if(!f){
		fprintf(stderr, "pvmsim: cannot open '%s'\n", path);
		return 0;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc(size > 0 ? (size_t)size : 1u);
	if(buf) fread(buf, 1, (size_t)size, f);
	fclose(f);

	*out_len = (int)size;
	return buf;
}

int main(int argc, char** argv){
	const char* path 		 = PVM_DEFAULT_BYTECODE;
	long 				stackLen = PVM_CFG_STACK_SIZE;
	long 				ramLen 	 = PVM_CFG_RAM_SIZE;
	long 				maxSteps = DEFAULT_MAX_STEPS;
	PVM_State_s state;
	PVM_Word_t* stack;
	PVM_Word_t* ram;
	char* 			code;
	int 				len = 0;
	int 				result = EXIT_SUCCESS;

	for(int i = 1; i < argc; i++){
		long* option = 0;

		if(!strcmp(argv[i], "--stack")) 			 option = &stackLen;
		else if(!strcmp(argv[i], "--ram")) 	 option = &ramLen;
		else if(!strcmp(argv[i], "--steps")) option = &maxSteps;
		else if(!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")){ Usage(); return EXIT_SUCCESS; }
		else if(argv[i][0] == '-'){ Usage(); return EXIT_FAILURE; }
		else{ path = argv[i]; continue; }

		if(++i >= argc || (*option = ParseCount(argv[i])) < 0){ Usage(); return EXIT_FAILURE; }
	}

	/* Checked here as well as by PVM_Init, which only says "bad config": a
	 * message naming the limit is worth the few lines. */
	if(stackLen < 1 || stackLen > PVM_STACK_LEN_MAX){
		fprintf(stderr, "pvmsim: --stack must be 1..%d words\n", PVM_STACK_LEN_MAX);
		return EXIT_FAILURE;
	}
	if(ramLen > PVM_CFG_USRSECT_BGN){
		fprintf(stderr, "pvmsim: --ram must be 0..%d words -- RAM may not run into the user section at 0x%04X\n",
						PVM_CFG_USRSECT_BGN, PVM_CFG_USRSECT_BGN);
		return EXIT_FAILURE;
	}

	code = LoadFile(path, &len);
	if(!code) return EXIT_FAILURE;

	/* +1 so a zero-word RAM still gets a pointer; PVM_Init is told 0 words. */
	stack = calloc((size_t)stackLen, sizeof(PVM_Word_t));
	ram 	= calloc((size_t)ramLen + 1, sizeof(PVM_Word_t));

	if(!stack || !ram ||
		 PVM_Init(&state, code, len, stack, (int)stackLen, ram, (int)ramLen) != ERRORCODE_NO_ERROR){
		fprintf(stderr, "pvmsim: cannot set up a %ld-word stack and %ld words of RAM\n", stackLen, ramLen);
		result = EXIT_FAILURE;
	}
	else{
		long step;

		PVMENV_Attach(&state);

		for(step = 0; step < maxSteps; step++){
			int status;

			PVM_PrintInstr(&state);
			fflush(stdout);

			status = PVM_ExecTick(&state);
			if(status == PVM_STATUS_HALTED){
				printf("pvmsim: halted\n");
				break;
			}
			if(status < 0){
				fprintf(stderr, "pvmsim: execution error %d at step %ld\n", status, step);
				result = EXIT_FAILURE;
				break;
			}
		}

		/* Not a failure: firmware is usually a loop that never halts, and a
		 * program like that has done all it was going to by now. */
		if(step == maxSteps)
			fprintf(stderr, "pvmsim: stopped after %ld instructions without reaching HALT\n", maxSteps);
	}

	free(stack);
	free(ram);
	free(code);
	return result;
}
