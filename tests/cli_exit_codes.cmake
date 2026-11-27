# The command line's exit codes: 0 done and clean, 1 an error, 2 done with
# problems to look at, 3 stopped with Ctrl+C, 4 not possible now (the drive
# is not plugged in, or the catalog is open elsewhere: nothing was done),
# 5 many files changed at once (nothing was backed up; --accept-changes).
# cli_flow.cmake checks 4 and 5. Run as
#   cmake -DCLI=<brodalf-cli> -DWORK=<empty folder to use> -P cli_exit_codes.cmake
# Everything it writes (catalog, drive, working copies, the app log and any
# error report) is under WORK, which is removed at the end.

if(NOT CLI OR NOT WORK)
    message(FATAL_ERROR "give -DCLI=<brodalf-cli> -DWORK=<folder>")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src" "${WORK}/drive" "${WORK}/tmp" "${WORK}/state")
foreach(var TMPDIR TMP TEMP)
    set(ENV{${var}} "${WORK}/tmp")
endforeach()
set(ENV{BRODALF_LOG} "${WORK}/state/brodalf.log")
set(ENV{BRODALF_STATE} "${WORK}/state")
set(ENV{XDG_STATE_HOME} "${WORK}/state")
set(ENV{LOCALAPPDATA} "${WORK}/state")

set(failures 0)

# expect(<code> <what> <args>...): run the CLI and check its exit code.
function(expect code what)
    execute_process(COMMAND "${CLI}" ${ARGN}
                    WORKING_DIRECTORY "${WORK}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc STREQUAL "${code}")
        message(SEND_ERROR "${what}: exit ${rc}, expected ${code}\n${out}${err}")
        math(EXPR n "${failures} + 1")
        set(failures ${n} PARENT_SCOPE)
    else()
        message(STATUS "${what}: exit ${rc}")
    endif()
endfunction()

file(WRITE "${WORK}/src/a.txt" "alpha alpha")
file(WRITE "${WORK}/src/b.txt" "beta beta")
set(cat "${WORK}/c.brodalf")

expect(0 "new catalog" new "${cat}" "${WORK}/src")
expect(0 "first scan" scan "${cat}")
expect(0 "set up a drive" drive "${cat}" "${WORK}/drive" Drive)
expect(0 "clean backup" backup "${cat}" "${WORK}/drive")
expect(0 "clean check" check "${cat}" "${WORK}/drive")
expect(1 "unknown command" no-such-command "${cat}")
expect(1 "no such catalog" scan "${WORK}/missing.brodalf")

# A copy gone from the drive: the check finishes, with a problem.
file(GLOB_RECURSE copies "${WORK}/drive/BRODALF/*/a.txt")
list(LENGTH copies ncopies)
if(NOT ncopies EQUAL 1)
    message(SEND_ERROR "expected one copy of a.txt on the drive, found ${ncopies}")
    math(EXPR failures "${failures} + 1")
else()
    file(REMOVE ${copies})
    expect(2 "check with a copy missing" check "${cat}" "${WORK}/drive")
    expect(0 "backup writes it again" backup "${cat}" "${WORK}/drive")
    expect(0 "check after that" check "${cat}" "${WORK}/drive")
endif()

# Restore: everything restored, then a file left alone because another
# one is already where it would go.
expect(0 "clean restore" restore "${cat}" "${WORK}/out1" --drive "${WORK}/drive")
file(GLOB_RECURSE restored "${WORK}/out1/*a.txt")
list(LENGTH restored nrestored)
if(NOT nrestored EQUAL 1)
    message(SEND_ERROR "expected the restore to write a.txt once, found ${nrestored}")
    math(EXPR failures "${failures} + 1")
else()
    file(RELATIVE_PATH rel "${WORK}/out1" ${restored})
    file(WRITE "${WORK}/out2/${rel}" "not the same file")
    expect(2 "restore with a file in the way" restore "${cat}" "${WORK}/out2" --drive "${WORK}/drive")
endif()

# A backup root that cannot be found: the scan finishes, with a problem.
file(RENAME "${WORK}/src" "${WORK}/src-away")
expect(2 "scan with a root not found" scan "${cat}")
file(RENAME "${WORK}/src-away" "${WORK}/src")
expect(0 "scan with it back" scan "${cat}")

# Folders typed from the current folder, more than twenty in one command:
# every one is added, as a full path, so a scan run from another folder
# finds them all.
set(many "")
foreach(i RANGE 1 25)
    file(WRITE "${WORK}/many/r${i}/x.txt" "root ${i}")
    list(APPEND many "many/r${i}")
endforeach()
expect(0 "add 25 folders typed from here" add "${cat}" ${many})
execute_process(COMMAND "${CLI}" scan "${cat}" WORKING_DIRECTORY "${WORK}/tmp"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc STREQUAL "0" OR NOT out MATCHES " 25 new" OR out MATCHES "not found")
    message(SEND_ERROR "scan from another folder after adding 25 folders: exit ${rc}\n${out}${err}")
    math(EXPR failures "${failures} + 1")
else()
    message(STATUS "scan from another folder found all 25 folders")
endif()

file(REMOVE_RECURSE "${WORK}")
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} exit code check(s) failed")
endif()
