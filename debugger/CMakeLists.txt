add_executable(pvmdbg
	pvmdbg_cli.c
	transport.h
	transport_local.c
	transport_serial.c
	transport_stream.h
	transport_stream.c
	transport_tcp.c
	net.h
	disasm.h
	disasm.cpp
	asm.h
	asm.cpp
	hostexec.h
	hostexec.c
	dbgclient.h
	dbgclient.c
	sourcemap.h
	sourcemap.c
	machine.h
	machine.c
)

target_link_libraries(pvmdbg PRIVATE pvmcore pvmenv pvmdisasm_lib pvmasm_lib)
add_dependencies(pvmdbg bytecode_listing_bytecode)
target_compile_definitions(pvmdbg PRIVATE PVM_DEFAULT_BYTECODE="${PVM_EXAMPLE_BYTECODE}")

# The assembler it now links for "exec" uses std::filesystem, whose symbols an
# older libstdc++-6.dll found first on PATH does not have -- same fix as pvmc.
if(MINGW)
	target_link_options(pvmdbg PRIVATE -static-libgcc -static-libstdc++)
endif()

if(WIN32)
	target_link_libraries(pvmdbg PRIVATE ws2_32)
endif()

pvm_publish_tool(pvmdbg)

# A board that is a process, with TCP where the UART would be: the far end for
# pvmdbg --tcp when there is no hardware, and what the TCP tests run against.
add_executable(pvmboard
	pvmboard.c
	net.h
)
target_link_libraries(pvmboard PRIVATE pvmcore pvmenv)
if(WIN32)
	target_link_libraries(pvmboard PRIVATE ws2_32)
endif()
