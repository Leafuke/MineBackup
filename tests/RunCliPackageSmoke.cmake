if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

set(PROFILE "${TEST_ROOT}/profile")
set(WORLD "${PROFILE}/server/world")
file(REMOVE_RECURSE "${PROFILE}")
file(MAKE_DIRECTORY "${PROFILE}/config" "${WORLD}")
file(WRITE "${WORLD}/level.dat" "minebackup-packaged-cli-smoke")
file(WRITE "${PROFILE}/config/config.ini"
"[Config1]\n"
"ConfigName=Packaged CLI\n"
"ConfigId=99999999-9999-4999-8999-999999999999\n"
"SavePath=${PROFILE}/server\n"
"WorldData=\n"
"world\n"
"Packaged world\n"
"*\n"
"BackupPath=${PROFILE}/backups\n"
"ZipProgram=\n"
"ZipFormat=7z\n"
"ZipLevel=1\n"
"ZipMethod=LZMA2\n"
"CpuThreads=1\n"
"KeepCount=0\n"
"SmartBackup=1\n"
"UseLowPriority=0\n"
"SkipIfUnchanged=1\n"
"MaxSmartBackups=5\n"
"CloudSyncEnabled=0\n")

function(run_cli output_variable)
    execute_process(
        COMMAND "${CLI}" --data-dir "${PROFILE}" --json --no-network ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 30)
    string(STRIP "${output}" output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Packaged CLI command failed (${result}): ${error}\n${output}")
    endif()
    string(JSON schema ERROR_VARIABLE json_error GET "${output}" schemaVersion)
    if(json_error OR NOT schema EQUAL 1)
        message(FATAL_ERROR "Packaged CLI did not return one schema-v1 JSON object: ${output}")
    endif()
    set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

run_cli(backup_json backup --config 99999999-9999-4999-8999-999999999999 --world world)
string(JSON backup_code GET "${backup_json}" code)
string(JSON archive GET "${backup_json}" data archivePath)
if(NOT backup_code STREQUAL "success" OR NOT EXISTS "${archive}")
    message(FATAL_ERROR "Packaged CLI did not create an archive: ${backup_json}")
endif()

run_cli(verify_json verify --config 99999999-9999-4999-8999-999999999999 --world world --latest)
string(JSON verify_code GET "${verify_json}" code)
string(JSON checked_count GET "${verify_json}" data checkedArchiveCount)
if(NOT verify_code STREQUAL "success" OR NOT checked_count EQUAL 1)
    message(FATAL_ERROR "Packaged CLI did not verify its archive: ${verify_json}")
endif()
