# pvmdbg --tcp against pvmboard: the network path end to end, no hardware.
#
# pvmboard has to be running before pvmdbg connects and has to be left running
# while it does, which execute_process cannot do for two programs at once --
# so the board is started in the background (--once makes it exit when the
# debugger leaves, or after 15 s if none ever arrives), and pvmdbg's own retry
# on a refused connection covers the moment it takes to start listening.
#
# Its output goes to files, not pipes: a background process holding a pipe open
# would keep this script waiting on it.

function(start_board port extra)
	if(WIN32)
		execute_process(COMMAND cmd /c start "" /b "${BOARD}" "${BIN}" --listen 127.0.0.1:${port} --paused --once ${extra}
			OUTPUT_FILE ${WORK}/board_${port}.out ERROR_FILE ${WORK}/board_${port}.err)
	else()
		execute_process(COMMAND sh -c "\"${BOARD}\" \"${BIN}\" --listen 127.0.0.1:${port} --paused --once ${extra} >\"${WORK}/board_${port}.out\" 2>&1 &")
	endif()
endfunction()

function(expect out what)
	foreach(needle IN LISTS ARGN)
		string(FIND "${out}" "${needle}" position)
		if(position EQUAL -1)
			message(FATAL_ERROR "${what} never printed: ${needle}")
		endif()
	endforeach()
endfunction()

# ---- a program on the board, debugged over TCP ----
start_board(${PORT} "")
execute_process(
	COMMAND ${TOOL} --tcp 127.0.0.1:${PORT} --map ${MAP}
	INPUT_FILE ${SESSION}
	OUTPUT_VARIABLE out
	ERROR_VARIABLE err
	RESULT_VARIABLE rc
)
message(STATUS "${out}${err}")
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "pvmdbg --tcp exited with ${rc}")
endif()
expect("${out}" "the TCP session"
	"over TCP"                    # it connected
	"pc=0x0000"                   # to a board held before its first instruction
	"stepped"                     # STEP made the round trip
	"top first: 42"               # so did a snippet, bytes uploaded in several frames
	"halted -- program finished"  # RUN, then polling a running board until it stopped
	"square.hlc:"                 # the map named on the command line was used
	"session closed")

# ---- a program on the host, the board only executing (--host) ----
math(EXPR hostPort "${PORT} + 1")
start_board(${hostPort} "")
execute_process(
	COMMAND ${TOOL} --host ${BIN} --tcp 127.0.0.1:${hostPort}
	INPUT_FILE ${HOST_SESSION}
	OUTPUT_VARIABLE out
	ERROR_VARIABLE err
	RESULT_VARIABLE rc
)
message(STATUS "${out}${err}")
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "pvmdbg --host --tcp exited with ${rc}")
endif()
expect("${out}" "the --host session over TCP"
	"[host: square]"
	"halted -- program finished"
	"top first: 49"
	"top first: 9 81"
	"bad data address")
