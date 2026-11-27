# The command line from a task you schedule, and when things go wrong: the
# 0.14.0 flow review's command-line findings. Exit codes: 0 done and clean,
# 1 an error or a mistake, 2 done with problems to look at, 3 stopped,
# 4 not possible now (nothing done), 5 many files changed (nothing backed
# up). Run as
#   cmake -DCLI=<brodalf-cli> -DWORK=<empty folder to use> -P cli_flow.cmake
# Everything it writes is under WORK, which is removed at the end.

if(NOT CLI OR NOT WORK)
    message(FATAL_ERROR "give -DCLI=<brodalf-cli> -DWORK=<folder>")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/tmp" "${WORK}/state")
foreach(var TMPDIR TMP TEMP)
    set(ENV{${var}} "${WORK}/tmp")
endforeach()
set(ENV{BRODALF_LOG} "${WORK}/state/brodalf.log")
set(ENV{BRODALF_STATE} "${WORK}/state")
set(ENV{XDG_STATE_HOME} "${WORK}/state")
set(ENV{LOCALAPPDATA} "${WORK}/state")
foreach(var BRODALF_PASSPHRASE BRODALF_NEW_PASSPHRASE BRODALF_RECOVERY_KEY)
    unset(ENV{${var}})
endforeach()
# No terminal: what a scheduled task gets.
file(WRITE "${WORK}/no-input.txt" "")

set(failures 0)

function(fail_check msg)
    message(SEND_ERROR "${msg}")
    math(EXPR n "${failures} + 1")
    set(failures ${n} PARENT_SCOPE)
endfunction()

function(report_count out)
    file(GLOB reports "${WORK}/state/reports/*")
    list(LENGTH reports n)
    set(${out} ${n} PARENT_SCOPE)
endfunction()

# run(<code> <what> <args>...): run the CLI, check its exit code, and keep
# what it printed in OUT (stdout and stderr). An error report may be saved
# only for a real fault (code 1 and NOT "mistake").
macro(run code what)
    report_count(before)
    execute_process(COMMAND "${CLI}" ${ARGN}
                    WORKING_DIRECTORY "${WORK}" INPUT_FILE "${WORK}/no-input.txt"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(OUT "${out}${err}")
    report_count(after)
    if(NOT rc STREQUAL "${code}")
        fail_check("${what}: exit ${rc}, expected ${code}\n${OUT}")
    elseif(NOT after EQUAL before)
        fail_check("${what}: saved an error report for something that is not a fault\n${OUT}")
    else()
        message(STATUS "${what}: exit ${rc}")
    endif()
endmacro()

macro(expect_text what pattern)
    if(NOT OUT MATCHES "${pattern}")
        fail_check("${what}: expected to read \"${pattern}\" in\n${OUT}")
    endif()
endmacro()

# ---- A drive not there, a mistake, a catalog open elsewhere ----------------
file(WRITE "${WORK}/src/a.txt" "alpha alpha")
file(WRITE "${WORK}/src/b.txt" "beta beta")
file(MAKE_DIRECTORY "${WORK}/E" "${WORK}/F")
set(cat "${WORK}/c.brodalf")
run(0 "new catalog" new "${cat}" "${WORK}/src")
run(0 "scan" scan "${cat}")
run(0 "set up drive E" drive "${cat}" "${WORK}/E" E)
run(0 "set up drive F" drive "${cat}" "${WORK}/F" F)
run(0 "target of one copy" target "${cat}" 1 1)

run(1 "an option mistyped" backup "${cat}" "${WORK}/E" --paralel)
expect_text("an option mistyped" "unknown option --paralel")
run(1 "the catalog after the options" backup --plugged "${cat}")
run(1 "a catalog that is not there" scan "${WORK}/nothing.brodalf")
run(1 "a new catalog over one" new "${cat}")
run(4 "backup to a drive not plugged in" backup "${cat}" "${WORK}/not-plugged")
run(4 "check of a drive not plugged in" check "${cat}" "${WORK}/not-plugged")
# 0.14.0: nothing on either drive, exit 1 and an error report.
run(2 "backup to two drives, one not plugged in" backup "${cat}" "${WORK}/E" "${WORK}/not-plugged")
file(GLOB_RECURSE on_e "${WORK}/E/BRODALF/*/a.txt")
if(NOT on_e)
    fail_check("the drive that was there was not backed up")
endif()

# --plugged: every drive of the catalog that is there.
run(0 "backup --plugged" backup "${cat}" --plugged)
expect_text("backup --plugged" "\"F\" is plugged in")
file(GLOB_RECURSE on_f "${WORK}/F/BRODALF/*/a.txt")
if(NOT on_f)
    fail_check("backup --plugged did not back up F")
endif()
file(RENAME "${WORK}/E" "${WORK}/E-away")
file(RENAME "${WORK}/F" "${WORK}/F-away")
run(4 "backup --plugged with none there" backup "${cat}" --plugged)
file(RENAME "${WORK}/E-away" "${WORK}/E")
file(RENAME "${WORK}/F-away" "${WORK}/F")

if(NOT CMAKE_HOST_WIN32)
    # On Windows the window holds a mutex; elsewhere the lock file says it.
    file(WRITE "${cat}.lock" "")
    run(4 "the catalog open elsewhere" backup "${cat}" "${WORK}/E")
    file(REMOVE "${cat}.lock")
endif()

# need and at-risk: exit 2 when files are short of the copies wanted.
run(0 "need with every file backed up" need "${cat}")
file(WRITE "${WORK}/src/c.txt" "gamma")
run(0 "scan a new file" scan "${cat}")
run(2 "need with a file short" need "${cat}")
run(2 "at-risk with a file short" at-risk "${cat}")
run(0 "preview" preview "${cat}" "${WORK}/E")
run(0 "backup it" backup "${cat}" "${WORK}/E")
run(0 "preview of a drive up to date" preview "${cat}" "${WORK}/E")
expect_text("preview of a drive up to date" "up to date as of the scan of")

# restore: a --path that matches nothing is refused (0.14.0: exit 0, nothing restored).
run(1 "restore of a path that is not there" restore "${cat}" "${WORK}/out" --path no/such/file.txt --drive "${WORK}/E")
run(0 "restore of a path that is there" restore "${cat}" "${WORK}/out" --path a.txt --drive "${WORK}/E")

# A drive's copy of the catalog is never opened (0.14.0: scan saved over it).
file(GLOB copies "${WORK}/E/BRODALF/*/catalog-r*.brodalf")
list(GET copies 0 copy)
file(SHA256 "${copy}" before_hash)
run(1 "open a drive's copy of the catalog" scan "${copy}")
expect_text("open a drive's copy of the catalog" "recover")
file(SHA256 "${copy}" after_hash)
if(NOT before_hash STREQUAL after_hash)
    fail_check("the drive's copy of the catalog was changed")
endif()

# ---- Missing roots and wait ------------------------------------------------
file(RENAME "${WORK}/src" "${WORK}/src-away")
run(0 "roots with a root not found (not scanned this time)" roots "${cat}")
expect_text("roots with a root not found" "not found: left out of backups")
run(2 "backup with the root not found" backup "${cat}" "${WORK}/E")
expect_text("backup with the root not found" "left out of this backup: its folder cannot be found")
run(0 "wait for its disk" wait "${cat}" src on)
run(0 "roots with a root waiting" roots "${cat}")
expect_text("roots with a root waiting" "waits for its disk")
# A root that waits for its disk is no problem to look at.
run(0 "backup with the root waiting" backup "${cat}" "${WORK}/E")
run(1 "wait with a mistake" wait "${cat}" src maybe)
file(RENAME "${WORK}/src-away" "${WORK}/src")
run(0 "wait off" wait "${cat}" src off)

# ---- Many files changed at once --------------------------------------------
set(mc "${WORK}/mc.brodalf")
foreach(i RANGE 1 60)
    file(WRITE "${WORK}/mc-src/f${i}.txt" "file ${i} as it was")
endforeach()
file(MAKE_DIRECTORY "${WORK}/mc-drive")
run(0 "many: new catalog" new "${mc}" "${WORK}/mc-src")
run(0 "many: scan" scan "${mc}")
run(0 "many: set up drive" drive "${mc}" "${WORK}/mc-drive" M)
run(0 "many: first backup" backup "${mc}" "${WORK}/mc-drive")
foreach(i RANGE 1 20)
    file(WRITE "${WORK}/mc-src/f${i}.txt" "file ${i}, encrypted by someone else")
endforeach()
run(0 "many: scan the changes" scan "${mc}")
# 0.14.0: copied the 20 and exited 0.
run(5 "many: backup stops" backup "${mc}" "${WORK}/mc-drive")
expect_text("many: backup stops" "--accept-changes")
run(0 "many: the preview still has them to copy" preview "${mc}" "${WORK}/mc-drive")
expect_text("many: the preview still has them to copy" "to copy: 20 files")
run(0 "many: backup with --accept-changes" backup "${mc}" "${WORK}/mc-drive" --accept-changes)
run(0 "many: preview after" preview "${mc}" "${WORK}/mc-drive")
expect_text("many: preview after" "to copy: 0 files")

# ---- Passphrase, recovery key, encrypted folders -----------------------------
set(pc "${WORK}/pc.brodalf")
file(WRITE "${WORK}/pc-src/open/a.txt" "open text")
file(WRITE "${WORK}/pc-src/secret/b.txt" "secret text")
file(MAKE_DIRECTORY "${WORK}/pc-drive")
run(0 "pass: new catalog" new "${pc}" "${WORK}/pc-src")
run(0 "pass: scan" scan "${pc}")
run(0 "pass: set up drive" drive "${pc}" "${WORK}/pc-drive" P)
set(ENV{BRODALF_NEW_PASSPHRASE} "first passphrase")
run(0 "pass: set the passphrase" passphrase "${pc}")
unset(ENV{BRODALF_NEW_PASSPHRASE})
set(ENV{BRODALF_PASSPHRASE} "first passphrase")
run(0 "pass: encrypt a folder" encrypt-folder "${pc}" pc-src secret)
run(0 "pass: make a recovery key" recovery-key "${pc}" --json)
string(REGEX MATCH "\"recovery_key\":\"([A-Z0-9-]+)\"" found "${OUT}")
set(key "${CMAKE_MATCH_1}")
if(NOT key)
    fail_check("no recovery key in\n${OUT}")
endif()
unset(ENV{BRODALF_PASSPHRASE})

# No passphrase at hand (a scheduled task): the plain files are backed up,
# the rest waits (0.14.0: nothing backed up, exit 1 and an error report).
run(2 "pass: backup without the passphrase" backup "${pc}" "${WORK}/pc-drive" --json)
expect_text("pass: backup without the passphrase" "\"files_need_passphrase\":1")
file(GLOB_RECURSE plain_copy "${WORK}/pc-drive/*.txt")
list(FILTER plain_copy INCLUDE REGEX "/open/a\\.txt$")
if(NOT plain_copy)
    fail_check("the plain file was not backed up without the passphrase")
endif()
set(ENV{BRODALF_RECOVERY_KEY} "${key}")
run(0 "pass: backup with the recovery key" backup "${pc}" "${WORK}/pc-drive" --recovery-key --json)
expect_text("pass: backup with the recovery key" "\"files_copied\":1")
unset(ENV{BRODALF_RECOVERY_KEY})

set(ENV{BRODALF_PASSPHRASE} "first passphrase")
run(0 "pass: encrypt the catalog file" encrypt-catalog "${pc}" on)
unset(ENV{BRODALF_PASSPHRASE})
run(1 "pass: open the encrypted catalog with nothing at hand" status "${pc}")
set(ENV{BRODALF_RECOVERY_KEY} "${key}")
run(0 "pass: open it with the recovery key" status "${pc}" --recovery-key)
set(ENV{BRODALF_RECOVERY_KEY} "AAAAA-BBBBB-CCCCC-DDDDD-EEEEE-FFFFF")
run(1 "pass: a wrong recovery key" status "${pc}" --recovery-key)
# A forgotten passphrase: a new one, with the recovery key.
set(ENV{BRODALF_RECOVERY_KEY} "${key}")
set(ENV{BRODALF_NEW_PASSPHRASE} "second passphrase")
run(0 "pass: a new passphrase with the recovery key" passphrase "${pc}" --recovery-key)
unset(ENV{BRODALF_NEW_PASSPHRASE})
unset(ENV{BRODALF_RECOVERY_KEY})
set(ENV{BRODALF_PASSPHRASE} "second passphrase")
run(0 "pass: open with the new passphrase" status "${pc}")
unset(ENV{BRODALF_PASSPHRASE})

# ---- Recover: every copy, newest first, on every drive ----------------------
# E holds the older copies, F the newest; F's are all damaged.
run(0 "recover: backup to E" backup "${cat}" "${WORK}/E")
run(0 "recover: backup to F" backup "${cat}" "${WORK}/F" --accept-changes)
file(GLOB f_copies "${WORK}/F/BRODALF/*/catalog-r*.brodalf")
foreach(c ${f_copies})
    file(WRITE "${c}" "damaged")
endforeach()
# 0.14.0: tried only F's newest copy, failed, and left a damaged file there.
run(0 "recover: falls back to E" recover "${WORK}/F" "${WORK}/E" "${WORK}/recovered.brodalf")
expect_text("recover: falls back to E" "trying an older copy")
run(0 "recover: the recovered catalog opens" roots "${WORK}/recovered.brodalf")
file(GLOB e_copies "${WORK}/E/BRODALF/*/catalog-r*.brodalf")
foreach(c ${e_copies})
    file(WRITE "${c}" "damaged")
endforeach()
run(1 "recover: no copy opens" recover "${WORK}/F" "${WORK}/E" "${WORK}/recovered2.brodalf")
if(EXISTS "${WORK}/recovered2.brodalf")
    fail_check("a failed recover left a file where the new catalog was to go")
endif()

file(REMOVE_RECURSE "${WORK}")
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} command-line flow check(s) failed")
endif()
