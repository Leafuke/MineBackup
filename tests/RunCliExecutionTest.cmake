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
