#ifndef PVMDBG_DEBUGSYM_H_
#define PVMDBG_DEBUGSYM_H_

/*
 * The symbols pvmc writes into the .dbgmap after the line entries (see the
 * assembler's GenerateDebugMap): which addresses belong to which function,
 * where in each function's frame its parameters and locals live, and where the
 * globals are. What turns "the word at FP-3" into "x".
 *
 * Optional like the rest of the map: without symbols the session shows
 * addresses and raw stack words, and nothing fails.
 */

#include <stdint.h>

#define DEBUGSYM_NAME_MAX (32)

typedef struct{
	char 		 func[DEBUGSYM_NAME_MAX];
	char 		 name[DEBUGSYM_NAME_MAX];
	int 		 isParam;
	int 		 offset; /* words from FP */
	uint16_t from; 	 /* the variable exists from this instruction on */
} DebugVar_s;

typedef struct{
	char 		 name[DEBUGSYM_NAME_MAX];
	uint16_t addr;
	int 		 size; /* words */
} DebugGlobal_s;

/* Reads the symbol records of a .dbgmap; 1 if it held any. Quietly 0 for a map
 * from before symbols existed, or for none at all. */
int DebugSymLoad(const char* path);

/* The function `pc` is in, and where it starts; null if no function claims it. */
const char* DebugSymFuncAt(uint16_t pc, uint16_t* start);

/* Where a function starts, by name. */
int DebugSymFuncAddr(const char* name, uint16_t* addr);

int 							DebugSymVarCount(void);
const DebugVar_s* DebugSymVar(int index);

int 								 DebugSymGlobalCount(void);
const DebugGlobal_s* DebugSymGlobal(int index);
const DebugGlobal_s* DebugSymFindGlobal(const char* name);

#endif /* PVMDBG_DEBUGSYM_H_ */
