add_executable(pvmcc
	hlc.cpp
	lexer.cpp
	lexer.h
	exepath.cpp
	exepath.h
	modulepath.cpp
	modulepath.h
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

# pvmcc looks for "import name;" in <its own directory>/lib -- keep a copy of
# the project's lib/ next to the built executable so that actually works.
# Depends on the actual lib/*.hlc files (not just "did pvmcc itself rebuild"),
# so editing a library file alone still refreshes the copy pvmcc will see.
file(GLOB PVM_LIB_SOURCES ${CMAKE_SOURCE_DIR}/lib/*.hlc)
set(PVM_LIB_STAMP ${CMAKE_CURRENT_BINARY_DIR}/lib.stamp)

add_custom_command(
	OUTPUT ${PVM_LIB_STAMP}
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lib $<TARGET_FILE_DIR:pvmcc>/lib
	COMMAND ${CMAKE_COMMAND} -E touch ${PVM_LIB_STAMP}
	DEPENDS pvmcc ${PVM_LIB_SOURCES}
	COMMENT "Copying lib/ next to pvmcc"
)
add_custom_target(pvmcc_lib ALL DEPENDS ${PVM_LIB_STAMP})

pvm_publish_tool(pvmcc)

set(PVM_LIB_STAMP ${PVM_LIB_STAMP} PARENT_SCOPE)
