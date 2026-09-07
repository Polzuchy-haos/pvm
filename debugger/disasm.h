#ifndef PVMDBG_DISASM_H_
#define PVMDBG_DISASM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PVMDBG_DISASM_LINE_MAX (64)

/*
 * Turns bytecode read off the device into text, one instruction per line, in the
 * same "address  bytes  MNEMONIC operand" shape the assembler's listing uses --
 * so a line here and a line there can be matched up by eye.
 *
 * `code` begins at address `baseAddr`; at most `maxLines` instructions are
 * decoded, and fewer when the bytes run out. Returns how many were written.
 *
 * The decoding itself is the disassembler pvmdisasm already uses, rather than a
 * second opcode table living here: two tables of instruction names drift apart
 * the moment anyone adds an instruction to only one of them.
 */
int PvmDbgDisasm(const uint8_t* code, int len, int baseAddr, int maxLines,
                 char lines[][PVMDBG_DISASM_LINE_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* PVMDBG_DISASM_H_ */
