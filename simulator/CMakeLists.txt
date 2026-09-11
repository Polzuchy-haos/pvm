# The demo peripherals and SVC handlers, shared with pvmdbg so that stepping a
# program and running it see the same machine (see pvmenv.h).
add_library(pvmenv STATIC pvmenv.c pvmenv.h)
target_link_libraries(pvmenv PUBLIC pvmcore)
target_include_directories(pvmenv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(pvmsim pvmsim.c)

target_link_libraries(pvmsim PRIVATE pvmcore pvmenv)
add_dependencies(pvmsim bytecode_listing_bytecode)
target_compile_definitions(pvmsim PRIVATE PVM_DEFAULT_BYTECODE="${PVM_EXAMPLE_BYTECODE}")

pvm_publish_tool(pvmsim)
