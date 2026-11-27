# The command line's side of the drives findings of the 0.14.0 flow review.
# Run as
#   cmake -DCLI=<brodalf-cli> -DWORK=<empty folder to use> -P cli_flow_drives.cmake
# Everything it writes is under WORK, which is removed at the end.

if(NOT CLI OR NOT WORK)
    message(FATAL_ERROR "give -DCLI=<brodalf-cli> -DWORK=<folder>")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src" "${WORK}/A" "${WORK}/B" "${WORK}/tmp" "${WORK}/state" "${WORK}/away")
foreach(var TMPDIR TMP TEMP)
    set(ENV{${var}} "${WORK}/tmp")
endforeach()
set(ENV{BRODALF_LOG} "${WORK}/state/brodalf.log")
set(ENV{BRODALF_STATE} "${WORK}/state")
set(ENV{XDG_STATE_HOME} "${WORK}/state")
set(ENV{LOCALAPPDATA} "${WORK}/state")
file(WRITE "${WORK}/no-input" "")

set(failures 0)

# run(<code> <what> <args>...): run the CLI with no terminal and check its
# exit code; its output is left in OUT for has() and lacks().
macro(run code what)
    execute_process(COMMAND "${CLI}" ${ARGN}
                    WORKING_DIRECTORY "${WORK}" INPUT_FILE "${WORK}/no-input"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(OUT "${out}${err}")
    if(NOT rc STREQUAL "${code}")
        message(SEND_ERROR "${what}: exit ${rc}, expected ${code}\n${OUT}")
        math(EXPR failures "${failures} + 1")
    else()
        message(STATUS "${what}: exit ${rc}")
    endif()
endmacro()

macro(has what text)
    string(FIND "${OUT}" "${text}" at)
    if(at EQUAL -1)
        message(SEND_ERROR "${what}: the output does not say \"${text}\":\n${OUT}")
        math(EXPR failures "${failures} + 1")
    endif()
endmacro()

macro(lacks what text)
    string(FIND "${OUT}" "${text}" at)
    if(NOT at EQUAL -1)
        message(SEND_ERROR "${what}: the output should not say \"${text}\":\n${OUT}")
        math(EXPR failures "${failures} + 1")
    endif()
endmacro()

file(WRITE "${WORK}/src/a.txt" "alpha alpha")
file(WRITE "${WORK}/src/b.txt" "beta beta")
set(cat "${WORK}/c.brodalf")

run(0 "new catalog" new "${cat}" "${WORK}/src")
run(0 "first scan" scan "${cat}")

# ---- R3: a new passphrase or recovery key reaches the drives given -----------
set(ENV{BRODALF_NEW_PASSPHRASE} "first passphrase")
run(0 "set a passphrase" passphrase "${cat}")
set(ENV{BRODALF_PASSPHRASE} "first passphrase")
run(0 "set up drive A, encrypted" drive "${cat}" "${WORK}/A" A --encrypt)
run(0 "set up drive B, encrypted" drive "${cat}" "${WORK}/B" B --encrypt)
run(0 "back up to A" backup "${cat}" "${WORK}/A")
run(0 "back up to B" backup "${cat}" "${WORK}/B")

set(ENV{BRODALF_NEW_PASSPHRASE} "second passphrase")
run(0 "change the passphrase with A plugged in" passphrase "${cat}" --drive "${WORK}/A")
has("change the passphrase" "wrote a copy of the database with the new passphrase to \"A\"")
set(ENV{BRODALF_PASSPHRASE} "second passphrase")
run(0 "A's copy opens with the new passphrase" recover "${WORK}/A" "${WORK}/from-a.brodalf")
run(1 "B's copy does not" recover "${WORK}/B" "${WORK}/from-b.brodalf")
has("B's copy does not" "earlier")
lacks("B's copy does not" "error report")
set(ENV{BRODALF_PASSPHRASE} "first passphrase")
file(REMOVE "${WORK}/from-b.brodalf")
run(0 "B's copy opens with the earlier passphrase" recover "${WORK}/B" "${WORK}/from-b.brodalf")
set(ENV{BRODALF_PASSPHRASE} "second passphrase")
set(ENV{BRODALF_NEW_PASSPHRASE} "third passphrase")
run(0 "change it with no drive given" passphrase "${cat}")
has("change it with no drive given" "at its next backup")
set(ENV{BRODALF_PASSPHRASE} "third passphrase")

# ---- D11: a recovery sheet with its key inside a backup root ---------------
run(1 "a sheet with a key inside a backup root" recovery-key "${cat}" --sheet "${WORK}/src/sheet.html")
has("a sheet with a key inside a backup root" "no recovery key was made")
if(EXISTS "${WORK}/src/sheet.html")
    message(SEND_ERROR "the sheet was written inside the backup root")
    math(EXPR failures "${failures} + 1")
endif()
run(0 "the same with --yes" recovery-key "${cat}" --sheet "${WORK}/src/sheet.html" --yes)
file(REMOVE "${WORK}/src/sheet.html")
run(0 "a sheet with a key, and B plugged in" recovery-key "${cat}" --sheet "${WORK}/away/sheet.html" --drive "${WORK}/B")
has("a sheet with a key" "wrote a copy of the database with the new recovery key to \"B\"")
has("a sheet with a key" "away from the drives and the PC")
lacks("a sheet with a key" "with your drives")
file(READ "${WORK}/away/sheet.html" sheet)
set(OUT "${sheet}")
has("the sheet" "its database copy has this key")
has("the sheet" "does not have this key yet")

file(REMOVE_RECURSE "${WORK}")
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} check(s) failed")
endif()
