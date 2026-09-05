add_library(pvmcore STATIC
	pvm.c
	pvm.h
)

target_include_directories(pvmcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
