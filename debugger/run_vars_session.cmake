# Variables by name: the symbols pvmcc and pvmc put in the .dbgmap, read back
# by pvmdbg as a call stack and the values in each frame. Run twice -- on the
# in-process VM, and with the program on the host (--host) -- since the two
# find PC and the code in different places but must agree on the frames.
#
# arrays_and_logic.hlc: line 20 is the body of total()'s loop, called from
# main; on the third time there i is 2 and sum is 0 + 2.

function(run_session label)
	execute_process(
		COMMAND ${TOOL} ${ARGN}
		INPUT_FILE ${SESSION}
		OUTPUT_VARIABLE out
		ERROR_VARIABLE err
		RESULT_VARIABLE rc
	)
	message(STATUS "${out}${err}")

	if(NOT rc EQUAL 0)
		message(FATAL_ERROR "${label}: pvmdbg exited with ${rc}")
	endif()

	foreach(needle
		"#0  total()"                  # bt: the function PC is in
		"#1  main()"                   # and the one that called it, through the saved FP
		"arrays_and_logic.hlc:31"      # at the line of the CALL
		"i            = 2"             # locals: a loop counter three passes in
		"sum          = 2"
		"sum = 2"                      # print: the current function's variable
		"arr = {0, 2, 4, 6, 8}"        # print: a global array, word by word
		"no variable 'nosuch' here"
		"> #1  main()"                 # up: the caller is selected
		"#1 main"                      # and locals shows its variables
		"flags        = 0"
		"        sum          = 2"     # bt full: total's variables under its frame
		"already at the outermost frame (#1, main)"
		"SUM          0x0005  = 20"    # globals, once the program has run to its end
		"FLAGS        0x0006  = 15")
		string(FIND "${out}" "${needle}" position)
		if(position EQUAL -1)
			message(FATAL_ERROR "${label}: never printed: ${needle}")
		endif()
	endforeach()
endfunction()

run_session("in-process VM" ${BIN})
run_session("--host" --host ${BIN})
