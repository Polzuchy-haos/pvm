#include "modulepath.h"
#include "exepath.h"
#include "lexer.h" /* HlcError */

#include <filesystem>

namespace fs = std::filesystem;

std::string ResolveModulePath(const std::string& contextPath, const std::string& name,
															 const std::string& fallbackExt, const std::vector<std::string>& includePaths){
	std::vector<fs::path> searchDirs;
	searchDirs.push_back(fs::path(contextPath).parent_path());
	searchDirs.push_back(fs::path(GetExecutableDir()) / "lib");
	for(const std::string& p : includePaths) searchDirs.push_back(p);

	/*
	 * Always resolved to an absolute path: the caller may later re-interpret
	 * this path from a different working directory (e.g. pvmc processing the
	 * generated .pvms from a build-tree location, for `extern ... from x;`),
	 * where a relative path resolved here would silently point somewhere else.
	 */
	for(const fs::path& dir : searchDirs){
		fs::path exact = dir / name;
		if(fs::exists(exact)) return fs::absolute(exact).lexically_normal().string();

		fs::path withExt = dir / (name + fallbackExt);
		if(fs::exists(withExt)) return fs::absolute(withExt).lexically_normal().string();
	}

	std::string tried;
	for(const fs::path& dir : searchDirs) tried += "\n  " + dir.string();
	throw HlcError("pvmcc: error: cannot find '" + name + "', looked in:" + tried);
}
