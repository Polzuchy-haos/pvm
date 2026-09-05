/*
 * pvmc.c
 *
 * Command-line entry point for the PVM assembler: pvmc <input.pvms> [-o output.bin]
 */
#include "assembler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ReplaceExtension(const char* input, char* out, int out_size){
	char* dot;
	char* slash1;
	char* slash2;
	char* slash;

	snprintf(out, out_size, "%s", input);

	dot 	= strrchr(out, '.');
	slash1 = strrchr(out, '/');
	slash2 = strrchr(out, '\\');
	slash 	= (slash1 > slash2) ? slash1 : slash2;

	if(dot && (!slash || dot > slash))
		*dot = 0;

	strncat(out, ".bin", out_size - strlen(out) - 1);
}

int main(int argc, char** argv){
	const char* input;
	char 				output[1024];
	int 				have_output = 0;
	uint8_t* 		code = 0;
	int 				len = 0;
	FILE* 			f;
	int 				i;

	if(argc < 2){
		fprintf(stderr, "usage: %s <input.pvms> [-o output.bin]\n", argv[0]);
		return 1;
	}
	input = argv[1];

	for(i = 2; i < argc; i++){
		if(strcmp(argv[i], "-o") == 0 && i + 1 < argc){
			snprintf(output, sizeof(output), "%s", argv[i + 1]);
			have_output = 1;
			i++;
		}
	}
	if(!have_output)
		ReplaceExtension(input, output, sizeof(output));

	if(PVMC_Assemble(input, &code, &len) != 0)
		return 1;

	f = fopen(output, "wb");
	if(!f){
		fprintf(stderr, "pvmc: error: cannot open output file '%s'\n", output);
		free(code);
		return 1;
	}
	fwrite(code, 1, len, f);
	fclose(f);
	free(code);

	printf("pvmc: wrote %d bytes to '%s'\n", len, output);
	return 0;
}
