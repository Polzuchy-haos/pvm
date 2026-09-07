add_library(pvmcore STATIC
	pvm.c
	pvm.h
	pvmdbg.c
	pvmdbg.h
)

target_include_directories(pvmcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Exercises the debug protocol over a plain buffer -- no serial port, no board.
add_executable(pvmdbg_test pvmdbg_test.c)
target_link_libraries(pvmdbg_test PRIVATE pvmcore)
