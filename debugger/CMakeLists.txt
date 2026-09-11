add_executable(pvmdbg
	pvmdbg_cli.c
	transport.h
	transport_local.c
	transport_serial.c
	disasm.h
	disasm.cpp
	sourcemap.h
	sourcemap.c
	machine.h
	machine.c
)

target_link_libraries(pvmdbg PRIVATE pvmcore pvmenv pvmdisasm_lib)
add_dependencies(pvmdbg bytecode_listing_bytecode)
target_compile_definitions(pvmdbg PRIVATE PVM_DEFAULT_BYTECODE="${PVM_EXAMPLE_BYTECODE}")

pvm_publish_tool(pvmdbg)
