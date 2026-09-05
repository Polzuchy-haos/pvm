#ifndef HLC_CODEGEN_H_
#define HLC_CODEGEN_H_

#include "ast.h"
#include "lexer.h"

#include <string>

/* Generates .pvms assembly text for the given program. Throws HlcError on
 * semantic errors (undefined symbols, arity mismatches, etc). */
std::string Generate(const Program& program, const std::string& fileName);

#endif /* HLC_CODEGEN_H_ */
