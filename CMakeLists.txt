cmake_minimum_required(VERSION 3.10)
project(pvm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

add_subdirectory(pvm)
add_subdirectory(compilator)
add_subdirectory(simulator)
