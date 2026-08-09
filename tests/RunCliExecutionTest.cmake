if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

set(PROFILE "${TEST_ROOT}/cli-execution-profile")
set(WORLD "${PROFILE}/server/world")
file(REMOVE_RECURSE "${PROFILE}")
file(MAKE_DIRECTORY "${PROFILE}/config" "${WORLD}")
string(REPEAT "minebackup-fixture-" 8192 payload)
file(WRITE "${WORLD}/level.dat" "${payload}")
file(WRITE "${PROFILE}/config/config.ini"
"[Config1]\n"
"ConfigName=Server\n"
"ConfigId=config-id\n"
"SavePath=${PROFILE}/server\n"
"WorldData=\n"
"world\n"
"Primary world\n"
"*\n"
"BackupPath=${PROFILE}/backups\n"
"ZipProgram=\n"
"ZipFormat=7z\n"
"ZipLevel=1\n"
"ZipMethod=LZMA2\n"
"CpuThreads=1\n"
"KeepCount=0\n"
"SmartBackup=2\n"
"UseLowPriority=0\n"
"SkipIfUnchanged=1\n"
"MaxSmartBackups=5\n"
"CloudSyncEnabled=1\n"
"[SpCfg2]\n"
"Name=Legacy command\n"
"SpecialConfigId=special-id\n"
"Command=echo cli-special-ok\n")

function(run_cli output_variable)
    execute_process(
        COMMAND "${CLI}" --data-dir "${PROFILE}" --json --no-network ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "CLI execution failed (${result}): ${error}\n${output}")
    endif()
    string(STRIP "${output}" output)
    string(JSON schema ERROR_VARIABLE json_error GET "${output}" schemaVersion)
    if(json_error OR NOT schema EQUAL 1)
        message(FATAL_ERROR "CLI stdout is not one schema-v1 JSON object: ${output}")
    endif()
    set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

run_cli(backup_json backup --config config-id --world world --comment "initial backup")
string(JSON backup_code GET "${backup_json}" code)
string(JSON outcome GET "${backup_json}" data outcome)
string(JSON archive GET "${backup_json}" data archivePath)
string(JSON backup_comment GET "${backup_json}" data history comment)
if(NOT backup_code STREQUAL "success" OR NOT outcome STREQUAL "created"
		OR NOT backup_comment STREQUAL "initial backup" OR NOT EXISTS "${archive}")
    message(FATAL_ERROR "backup did not create the reported archive: ${backup_json}")
endif()
if(NOT EXISTS "${PROFILE}/data/history.json")
    message(FATAL_ERROR "backup did not commit history.json")
endif()

run_cli(verify_json verify --config config-id --world world --latest)
string(JSON verify_code GET "${verify_json}" code)
string(JSON verify_selected GET "${verify_json}" data selectedBackup)
string(JSON verify_checked GET "${verify_json}" data checkedArchiveCount)
get_filename_component(archive_real "${archive}" REALPATH)
get_filename_component(verify_selected_real "${verify_selected}" REALPATH)
if(NOT verify_code STREQUAL "success" OR NOT verify_selected_real STREQUAL "${archive_real}"
        OR NOT verify_checked EQUAL 1)
    message(FATAL_ERROR "verify --latest did not select and test the local history archive: ${verify_json}")
endif()

file(WRITE "${WORLD}/level.dat" "dry-run-must-not-write")
run_cli(dry_restore_json restore --config config-id --world world --backup "${archive}" --dry-run)
string(JSON dry_restore_code GET "${dry_restore_json}" code)
string(JSON dry_restore_flag GET "${dry_restore_json}" data dryRun)
file(READ "${WORLD}/level.dat" after_dry_run)
if(NOT dry_restore_code STREQUAL "success" OR NOT dry_restore_flag
        OR NOT after_dry_run STREQUAL "dry-run-must-not-write")
    message(FATAL_ERROR "restore --dry-run modified the world or returned an invalid plan: ${dry_restore_json}")
endif()

run_cli(clean_restore_json restore --config config-id --world world --backup "${archive}" --confirm)
string(JSON clean_restore_code GET "${clean_restore_json}" code)
string(JSON clean_restore_mode GET "${clean_restore_json}" data mode)
file(READ "${WORLD}/level.dat" after_clean_restore)
if(NOT clean_restore_code STREQUAL "success" OR NOT clean_restore_mode STREQUAL "clean"
        OR NOT after_clean_restore STREQUAL "${payload}")
    message(FATAL_ERROR "clean restore did not reproduce the original world bytes: ${clean_restore_json}")
endif()

run_cli(history_json history list --config config-id --world world)
string(JSON first_comment GET "${history_json}" data history 0 comment)
if(NOT first_comment STREQUAL "initial backup")
    message(FATAL_ERROR "backup --comment was not committed to history: ${history_json}")
endif()

string(REPLACE "\\" "\\\\" cmake_json_path "${CMAKE_COMMAND}")
file(WRITE "${PROFILE}/config/jobs.json"
"{\n"
"  \"schemaVersion\": 1,\n"
"  \"jobs\": [{\n"
"    \"jobId\": \"11111111-1111-4111-8111-111111111111\",\n"
"    \"name\": \"Process contract\",\n"
"    \"stages\": [{\n"
"      \"stageId\": \"22222222-2222-4222-8222-222222222222\",\n"
"      \"name\": \"Run\",\n"
"      \"steps\": [{\n"
"        \"stepId\": \"33333333-3333-4333-8333-333333333333\",\n"
"        \"name\": \"Echo\",\n"
"        \"type\": \"process\",\n"
"        \"executable\": \"${cmake_json_path}\",\n"
"        \"arguments\": [\"-E\", \"echo\", \"job-ok\"],\n"
"        \"workingDirectory\": \"\",\n"
"        \"timeoutSeconds\": 10,\n"
"        \"maximumCapturedBytes\": 4096,\n"
"        \"lowPriority\": false\n"
"      }]\n"
"    }]\n"
"  }]\n"
"}\n")
run_cli(job_json job run --job 11111111-1111-4111-8111-111111111111)
string(JSON job_code GET "${job_json}" code)
string(JSON job_step_code GET "${job_json}" data stages 0 steps 0 code)
if(NOT job_code STREQUAL "success" OR NOT job_step_code STREQUAL "success")
    message(FATAL_ERROR "job run did not execute the explicit Process Step: ${job_json}")
endif()

run_cli(no_change_json backup --config config-id --world world)
string(JSON no_change_code GET "${no_change_json}" code)
if(NOT no_change_code STREQUAL "no_changes")
    message(FATAL_ERROR "unchanged world should return no_changes: ${no_change_json}")
endif()

file(APPEND "${WORLD}/level.dat" "changed-for-cloud-partial")
execute_process(
    COMMAND "${CLI}" --data-dir "${PROFILE}" --json backup --config config-id --world world
    RESULT_VARIABLE partial_result
    OUTPUT_VARIABLE partial_json
    ERROR_VARIABLE partial_error
    TIMEOUT 30)
string(STRIP "${partial_json}" partial_json)
string(JSON partial_schema ERROR_VARIABLE partial_json_error GET "${partial_json}" schemaVersion)
string(JSON partial_code GET "${partial_json}" code)
string(JSON partial_outcome GET "${partial_json}" data outcome)
string(JSON partial_archive GET "${partial_json}" data archivePath)
if(NOT partial_result EQUAL 10 OR partial_json_error OR NOT partial_schema EQUAL 1
        OR NOT partial_code STREQUAL "partial_success"
        OR NOT partial_outcome STREQUAL "created" OR NOT EXISTS "${partial_archive}")
    message(FATAL_ERROR
        "local success with cloud failure must preserve the archive and exit 10: ${partial_error}\n${partial_json}")
endif()

set(expected_smart_payload "${payload}changed-for-cloud-partial")
run_cli(smart_verify_json verify --config config-id --world world --latest)
string(JSON smart_verify_code GET "${smart_verify_json}" code)
string(JSON smart_chain_count LENGTH "${smart_verify_json}" data archiveChain)
if(NOT smart_verify_code STREQUAL "success" OR smart_chain_count LESS 2)
    message(FATAL_ERROR "Smart verify did not validate the complete Full/Smart chain: ${smart_verify_json}")
endif()

file(WRITE "${WORLD}/level.dat" "broken-before-smart-restore")
run_cli(smart_restore_json restore --config config-id --world world --latest --confirm)
file(READ "${WORLD}/level.dat" after_smart_restore)
if(NOT after_smart_restore STREQUAL "${expected_smart_payload}")
    message(FATAL_ERROR "Smart clean restore did not reproduce the expected bytes: ${smart_restore_json}")
endif()

file(WRITE "${WORLD}/operator-note.txt" "preserved-by-overwrite")
file(WRITE "${WORLD}/level.dat" "broken-before-overwrite")
run_cli(overwrite_restore_json restore --config config-id --world world --latest --mode overwrite --confirm)
file(READ "${WORLD}/level.dat" after_overwrite_restore)
if(NOT after_overwrite_restore STREQUAL "${expected_smart_payload}"
        OR NOT EXISTS "${WORLD}/operator-note.txt")
    message(FATAL_ERROR "overwrite restore did not overlay the chain as documented: ${overwrite_restore_json}")
endif()

run_cli(special_json run-special special-id)
string(JSON special_code GET "${special_json}" code)
string(JSON task_count LENGTH "${special_json}" data tasks)
string(JSON task_code GET "${special_json}" data tasks 0 code)
if(NOT special_code STREQUAL "success" OR NOT task_count EQUAL 1
        OR NOT task_code STREQUAL "success")
    message(FATAL_ERROR "run-special did not execute the migrated command: ${special_json}")
endif()
if(NOT EXISTS "${PROFILE}/config/special-tasks.json")
    message(FATAL_ERROR "run-special did not atomically establish special-tasks.json")
endif()
