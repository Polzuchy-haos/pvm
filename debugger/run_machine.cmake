# Feeds a canned session to "pvmdbg --machine" and checks the replies.
#
# A CMake script rather than a plain add_test for the same reason as
# run_session.cmake: CTest cannot redirect a test's stdin, and reading commands
# from there is the whole point of the mode.
#
# No bytecode address is hard-coded. Addresses move whenever the compiler's
# output changes, and this test is about the protocol being answerable at all --
# what the answers mean is pvmdbg_test's job.

execute_process(
	COMMAND ${TOOL} ${BIN} --machine
	INPUT_FILE ${SESSION}
	OUTPUT_VARIABLE out
	ERROR_VARIABLE err
	RESULT_VARIABLE rc
)

message(STATUS "${out}${err}")

if(NOT rc EQUAL 0)
	message(FATAL_ERROR "pvmdbg --machine exited with ${rc}")
endif()

set(expected
	"ok pc=0 sp=0 fp=0"     # a fresh session is stopped on the first instruction
	"error unknown-command" # a verb it does not know is refused, not ignored
	"running=0"             # "poll" saw the program come to rest
	"reason=4"              # and it was DBGSTOP_HALTED that stopped it
)

foreach(needle IN LISTS expected)
	string(FIND "${out}" "${needle}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "machine session never answered with: ${needle}")
	endif()
endforeach()

# Every line is either a reply or an error -- nothing else may reach stdout,
# because the far end parses it.
string(REPLACE "\n" ";" lines "${out}")
foreach(line IN LISTS lines)
	string(STRIP "${line}" line)
	if(line STREQUAL "")
		continue()
	endif()
	if(NOT line MATCHES "^(ok|error)")
		message(FATAL_ERROR "stray line on the protocol channel: '${line}'")
	endif()
endforeach()
