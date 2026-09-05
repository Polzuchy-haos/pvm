/*
 ============================================================================
 Name        : pvmsim.c
 Description : Runs a compiled PVM bytecode file to completion (HALT/error),
               tracing every instruction via PWM_PrintInstr.
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>

#include "pvm.h"

#ifndef PVM_DEFAULT_BYTECODE
#define PVM_DEFAULT_BYTECODE "bytecode_listing.bin"
#endif

#define MAX_STEPS (100000)

PWM_State_s state;
PWM_Word_t  periph[8] = {0};

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

	buf = malloc(size);
	fread(buf, 1, size, f);
	fclose(f);

	*out_len = (int)size;
	return buf;
}

int main(int argc, char** argv){
	const char* path = (argc > 1) ? argv[1] : PVM_DEFAULT_BYTECODE;
	int len = 0;
	char* code = LoadFile(path, &len);
	int step;

	if(!code)
		return EXIT_FAILURE;

	PWM_Init(&state, code, len);
	PWM_UsrSect(&state, &periph, sizeof(periph) / sizeof(PWM_Word_t), 0);

	for(step = 0; step < MAX_STEPS; step++){
		int status;

		PWM_PrintInstr(&state);
		fflush(stdout);

		status = PWM_ExecTick(&state);
		if(status == PVM_STATUS_HALTED){
			printf("pvmsim: halted\n");
			break;
		}
		if(status < 0){
			fprintf(stderr, "pvmsim: execution error %d at step %d\n", status, step);
			free(code);
			return EXIT_FAILURE;
		}
	}

	free(code);
	return EXIT_SUCCESS;
}
