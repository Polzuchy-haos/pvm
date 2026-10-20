add_executable(pvminit
	pvminit.cpp
	${CMAKE_SOURCE_DIR}/hlc/exepath.cpp
)

# exepath.cpp answers "where am I installed", which is the whole basis of the
# paths pvminit writes into what it generates. Shared with pvmcc rather than
# copied: the two have to agree about it, since pvmcc finds its lib/ the same
# way.
target_include_directories(pvminit PRIVATE ${CMAKE_SOURCE_DIR}/hlc)

if(MINGW)
	target_link_options(pvminit PRIVATE -static-libgcc -static-libstdc++)
endif()

pvm_publish_tool(pvminit)
