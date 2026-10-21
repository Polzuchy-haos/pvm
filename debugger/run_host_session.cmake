# Runs a program that lives on the host: "pvmdbg --host" feeds the in-process
# VM one instruction at a time and does the control flow itself. A CMake script
# for the same reason as run_session.cmake -- the commands come in on stdin.
#
# square.hlc is used because what it computes is known without looking at the
# bytecode: main stores square(7) in RESULT, the first RAM word.

execute_process(
	COMMAND ${TOOL} --host ${BIN}
	INPUT_FILE ${SESSION}
	OUTPUT_VARIABLE out
	ERROR_VARIABLE err
	RESULT_VARIABLE rc
)

message(STATUS "${out}${err}")

if(NOT rc EQUAL 0)
	message(FATAL_ERROR "pvmdbg --host exited with ${rc}")
endif()

set(expected
	"[host: square]"             # the host followed CALL into the function
	"halted -- program finished" # and ran the program to its HALT
	"top first: 49"              # the device did the work: RESULT = 7 * 7 in its RAM
	"top first: 9 81"            # 'call' ran one function and showed what it left
	"top first: 42"              # exec ran a snippet from the host, an instruction at a time
	"bad data address"           # a call without arguments fails on the device, and says so
	"session closed"
)

foreach(needle IN LISTS expected)
	string(FIND "${out}" "${needle}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "host session never printed: ${needle}")
	endif()
endforeach()
