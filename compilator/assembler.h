#ifndef PVMC_ASSEMBLER_H_
#define PVMC_ASSEMBLER_H_

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

class AssemblerError : public std::runtime_error {
public:
	explicit AssemblerError(const std::string& message) : std::runtime_error(message) {}
};

struct AssembleResult {
	std::vector<uint8_t> code;
	/* Human-readable "address  hex bytes  MNEMONIC operand" listing, grouped by label,
	 * using the *original* source operand text (symbol names, not resolved addresses). */
	std::string 					listing;
	/* "0xADDR file:line" per instruction that a ".line" directive tied to a place
	 * in the original .hlc source; empty when the input carried none. */
	std::string 					debugMap;
};

/*
 * Assembles a .pvms source file (with recursive .import expansion,
 * .const substitution and .entry block reordering, see example/bytecode_listing.pvms)
 * into raw PVM bytecode.
 *
 * Throws AssemblerError with a ready-to-print "file:line: error: ..." diagnostic on failure.
 */
AssembleResult PVMC_Assemble(const std::string& input_path);

#endif /* PVMC_ASSEMBLER_H_ */
