add_executable(pvmsim pvmsim.c)

target_link_libraries(pvmsim PRIVATE pvmcore)
add_dependencies(pvmsim bytecode_listing_bytecode)
target_compile_definitions(pvmsim PRIVATE PVM_DEFAULT_BYTECODE="${PVM_EXAMPLE_BYTECODE}")
