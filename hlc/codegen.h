#ifndef HLC_CODEGEN_H_
#define HLC_CODEGEN_H_

#include "ast.h"
#include "lexer.h"

#include <string>
#include <vector>

/*
 * Generates .pvms assembly text for the given program. Throws HlcError on
 * semantic errors (undefined symbols, arity mismatches, etc).
 *
 * `includePaths` is the same -I list passed to TokenizeFile -- needed here to
 * resolve `extern ... from asmFile;` declarations to a real file (see
 * hlc/modulepath.h); the resolved absolute path is what gets emitted as
 * ".import <path>" in the output, so pvmc's own (relative-only) import
 * resolution doesn't need to know anything about -I or lib/.
 *
 * `stackSummary`, when given, receives the worst-case stack usage line that also
 * heads the generated output (see Codegen::AnalyzeStack). A program that provably
 * overruns its declared stack throws instead.
 */
std::string Generate(const Program& program, const std::string& fileName,
											const std::vector<std::string>& includePaths = {},
											std::string* stackSummary = nullptr);

#endif /* HLC_CODEGEN_H_ */
