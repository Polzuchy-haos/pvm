# Feeds a canned session to pvmdbg on stdin and checks what came back.
#
# This lives in a CMake script rather than a plain add_test because CTest has no
# way to redirect a test's stdin, and the whole point of the tool is that it
# reads commands from there.
#
# Deliberately free of any hard-coded bytecode address: those move whenever the
# compiler's output changes, and the protocol's own semantics are covered by
# pvmdbg_test. What is checked here is that the session itself works -- it
# starts stopped, commands reach the debug agent, and the program can be run to
# completion.

execute_process(
	COMMAND ${TOOL} ${BIN}
	INPUT_FILE ${SESSION}
	OUTPUT_VARIABLE out
	ERROR_VARIABLE err
	RESULT_VARIABLE rc
)

message(STATUS "${out}${err}")

if(NOT rc EQUAL 0)
	message(FATAL_ERROR "pvmdbg exited with ${rc}")
endif()

set(expected
	"pc=0x0000"                 # a fresh session is stopped on the first instruction
	"stepped"                   # 'step' ran exactly one instruction and said so
	"no breakpoints set"        # 'breaks' reached the agent and got a real answer
	"in use"                    # 'mem' got the pool sizes and the stack depth
	"halted -- program finished" # 'run' carried the program to its HALT
	"square.hlc:"               # the map beside the .bin was found and used
	"session closed"            # 'exit' ended the session rather than the process dying
)

foreach(needle IN LISTS expected)
	string(FIND "${out}" "${needle}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "session output never contained: ${needle}")
	endif()
endforeach()
