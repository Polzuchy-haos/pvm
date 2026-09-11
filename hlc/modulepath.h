#ifndef HLC_MODULEPATH_H_
#define HLC_MODULEPATH_H_

#include <string>
#include <vector>

/*
 * Shared "import name;" file resolution, used both for .hlc imports (lexer.cpp,
 * fallbackExt=".hlc") and for `extern ... from name;` assembly imports
 * (codegen.cpp, fallbackExt=".pvms"). Search order (like a C compiler's
 * #include): (1) next to `contextPath`, (2) <pvmcc's own directory>/lib,
 * (3) each of `includePaths`, in order. In each directory: exact name first,
 * then name + fallbackExt. Throws HlcError listing every directory tried if
 * nothing matches.
 */
std::string ResolveModulePath(const std::string& contextPath, const std::string& name,
															 const std::string& fallbackExt, const std::vector<std::string>& includePaths);

#endif /* HLC_MODULEPATH_H_ */
