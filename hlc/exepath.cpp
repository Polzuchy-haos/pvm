/*
 * exepath.cpp
 *
 * Deliberately isolated from the rest of hlc/: <windows.h> declares a global
 * enumerator literally named `TokenType` (TOKEN_INFORMATION_CLASS, in
 * winnt.h) which collides with our own `enum class TokenType` in lexer.h --
 * never include both in the same translation unit.
 */
#include "exepath.h"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

std::string GetExecutableDir(){
#ifdef _WIN32
	char buf[MAX_PATH];
	DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if(len > 0 && len < MAX_PATH)
		return std::filesystem::path(std::string(buf, len)).parent_path().string();
#endif
	return std::filesystem::current_path().string(); /* best-effort fallback outside Windows */
}
