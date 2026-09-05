#ifndef PVMC_ASSEMBLER_H_
#define PVMC_ASSEMBLER_H_

#include <stdint.h>

/*
 * Assembles a .pvms source file (with recursive .import expansion,
 * .const substitution and .entry block reordering, see example/bytecode_listing.pvms)
 * into raw PVM bytecode.
 *
 * On success returns 0, *out_code is a malloc'd buffer (caller frees it) and
 * *out_len is its length in bytes.
 * On failure returns non-zero; a diagnostic has already been printed to stderr.
 */
int PVMC_Assemble(const char* input_path, uint8_t** out_code, int* out_len);

#endif /* PVMC_ASSEMBLER_H_ */
