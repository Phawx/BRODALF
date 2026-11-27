# The command line's restore and search, as the 0.14.0 flow review found
# them: the passphrase asked for all or nothing, --drive ignored by search,
# no "keep both", no word about a restore into a backup root. Run as
#   cmake -DCLI=<brodalf-cli> -DWORK=<empty folder to use> -P cli_restore.cmake
# Everything it writes is under WORK, which is removed at the end.

if(NOT CLI OR NOT WORK)
    message(FATAL_ERROR "give -DCLI=<brodalf-cli> -DWORK=<folder>")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src/secret" "${WORK}/drive" "${WORK}/tmp" "${WORK}/state")
foreach(var TMPDIR TMP TEMP)
    set(ENV{${var}} "${WORK}/tmp")
endforeach()
set(ENV{BRODALF_LOG} "${WORK}/state/brodalf.log")
set(ENV{BRODALF_STATE} "${WORK}/state")
set(ENV{XDG_STATE_HOME} "${WORK}/state")
set(ENV{LOCALAPPDATA} "${WORK}/state")
# No terminal: stdin is an empty file.
file(WRITE "${WORK}/no-input.txt" "")

set(failures 0)

# run(<code> <what> <must match> <args>...): run the CLI, check its exit
# code and that what it printed matches the pattern ("" for anything).
function(run code what pattern)
    execute_process(COMMAND "${CLI}" ${ARGN}
                    WORKING_DIRECTORY "${WORK}" INPUT_FILE "${WORK}/no-input.txt"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc STREQUAL "${code}" OR (pattern AND NOT "${out}${err}" MATCHES "${pattern}"))
        message(SEND_ERROR "${what}: exit ${rc} (expected ${code}), looking for \"${pattern}\" in\n${out}${err}")
        math(EXPR n "${failures} + 1")
        set(failures ${n} PARENT_SCOPE)
    else()
        message(STATUS "${what}: exit ${rc}")
    endif()
endfunction()

function(must_exist what path)
    if(NOT EXISTS "${path}")
        message(SEND_ERROR "${what}: ${path} is not there")
        math(EXPR n "${failures} + 1")
        set(failures ${n} PARENT_SCOPE)
    endif()
endfunction()

function(must_not_exist what path)
    if(EXISTS "${path}")
        message(SEND_ERROR "${what}: ${path} should not be there")
        math(EXPR n "${failures} + 1")
        set(failures ${n} PARENT_SCOPE)
    endif()
endfunction()

file(WRITE "${WORK}/src/plain.txt" "plain text")
file(WRITE "${WORK}/src/secret/s.txt" "secret text")
set(cat "${WORK}/c.brodalf")
set(ENV{BRODALF_NEW_PASSPHRASE} "a passphrase for the test")
set(ENV{BRODALF_PASSPHRASE} "a passphrase for the test")
run(0 "new catalog" "" new "${cat}" "${WORK}/src")
run(0 "first scan" "" scan "${cat}")
run(0 "set a passphrase" "" passphrase "${cat}")
run(0 "encrypt a folder" "" encrypt-folder "${cat}" src secret)
run(0 "set up a drive" "" drive "${cat}" "${WORK}/drive" Drive)
run(0 "back up" "" backup "${cat}" "${WORK}/drive")
unset(ENV{BRODALF_PASSPHRASE})
unset(ENV{BRODALF_NEW_PASSPHRASE})

# R9: without a terminal or BRODALF_PASSPHRASE the plain files still come
# back, and the rest is reported (0.14.0: stopped before restoring anything).
run(2 "restore with no passphrase" "only on encrypted copies" restore "${cat}" "${WORK}/out1" --drive "${WORK}/drive")
must_exist("the plain file is restored" "${WORK}/out1/plain.txt")
must_not_exist("the encrypted one waits" "${WORK}/out1/secret/s.txt")
set(ENV{BRODALF_PASSPHRASE} "a passphrase for the test")
run(0 "the same restore with it" "1 restored" restore "${cat}" "${WORK}/out1" --drive "${WORK}/drive")
must_exist("then the encrypted one too" "${WORK}/out1/secret/s.txt")

# R11: search connects --drive (0.14.0: ignored, so everything was offline).
run(0 "search with the drive" "plain.txt  \\[available\\]  Drive" search "${cat}" plain --drive "${WORK}/drive")
run(0 "search with a pattern" "secret/s.txt" search "${cat}" "*.TXT")

# C11: --keep-both writes the backed-up file beside the one in the way.
file(WRITE "${WORK}/out2/plain.txt" "another plain file")
run(2 "restore with a file in the way" "--keep-both" restore "${cat}" "${WORK}/out2" --drive "${WORK}/drive")
run(0 "the same with --keep-both" "beside" restore "${cat}" "${WORK}/out2" --drive "${WORK}/drive" --keep-both)
file(GLOB beside "${WORK}/out2/plain (restored *).txt")
list(LENGTH beside nbeside)
if(NOT nbeside EQUAL 1)
    message(SEND_ERROR "expected one \"plain (restored <day>).txt\" in out2, found ${nbeside}")
    math(EXPR failures "${failures} + 1")
endif()

# C16: a restore into a backup root is warned about; into a drive's
# BRODALF folder it needs --yes.
run(0 "restore into the backup root" "is in the backup root" restore "${cat}" "${WORK}/src/restored" --drive "${WORK}/drive")
run(1 "restore among a drive's copies" "among the copies" restore "${cat}" "${WORK}/drive/BRODALF" --drive "${WORK}/drive")
must_not_exist("nothing written among the copies" "${WORK}/drive/BRODALF/plain.txt")

file(REMOVE_RECURSE "${WORK}")
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} restore command check(s) failed")
endif()
