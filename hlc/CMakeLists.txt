add_executable(pvmcc
	hlc.cpp
	lexer.cpp
	lexer.h
	ast.h
	parser.cpp
	parser.h
	codegen.cpp
	codegen.h
)

target_include_directories(pvmcc PRIVATE ${CMAKE_SOURCE_DIR}/pvm)

if(MINGW)
	target_link_options(pvmcc PRIVATE -static-libgcc -static-libstdc++)
endif()
