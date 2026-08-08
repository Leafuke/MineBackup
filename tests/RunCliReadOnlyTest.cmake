if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

set(PROFILE "${TEST_ROOT}/cli-read-only-profile")
file(REMOVE_RECURSE "${PROFILE}")
file(MAKE_DIRECTORY "${PROFILE}/config")
file(WRITE "${PROFILE}/config/config.ini"
"[Config1]\n"
"ConfigName=Server\n"
"ConfigId=config-id\n"
"SavePath=${PROFILE}/server\n"
"WorldData=\n"
"world/subdirectory\n"
"Primary world\n"
"*\n"
"BackupPath=${PROFILE}/backups\n"
"ZipLevel=5\n"
"SmartBackup=2\n"
"UseLowPriority=1\n"
"SkipIfUnchanged=1\n"
"CloudSyncEnabled=0\n"
"[SpCfg2]\n"
"Name=Nightly\n"
"SpecialConfigId=special-id\n"
"Command=echo legacy\n")

function(run_json_query output_variable)
    execute_process(
        COMMAND "${CLI}" --data-dir "${PROFILE}" --json --no-network ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "CLI query failed (${result}): ${error}\n${output}")
    endif()
    string(STRIP "${output}" output)
    string(JSON schema ERROR_VARIABLE json_error GET "${output}" schemaVersion)
    if(json_error OR NOT schema EQUAL 1)
        message(FATAL_ERROR "CLI stdout is not one schema-v1 JSON object: ${output}")
    endif()
    string(JSON code GET "${output}" code)
    if(NOT code STREQUAL "success")
        message(FATAL_ERROR "CLI query returned unexpected operation code: ${output}")
    endif()
    set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

run_json_query(config_json config list)
string(JSON config_count LENGTH "${config_json}" data configs)
string(JSON config_id GET "${config_json}" data configs 0 configId)
if(NOT config_count EQUAL 1 OR NOT config_id STREQUAL "config-id")
    message(FATAL_ERROR "config list did not return the stable ConfigId: ${config_json}")
endif()

run_json_query(world_json world list --config config-id)
string(JSON world_count LENGTH "${world_json}" data worlds)
string(JSON world_path GET "${world_json}" data worlds 0 path)
if(NOT world_count EQUAL 1 OR NOT world_path STREQUAL "world/subdirectory")
    message(FATAL_ERROR "world list did not return the normalized relative path: ${world_json}")
endif()

run_json_query(history_json history list --config config-id --world world/subdirectory)
string(JSON history_count LENGTH "${history_json}" data history)
if(NOT history_count EQUAL 0)
    message(FATAL_ERROR "history list should be empty for a new profile: ${history_json}")
endif()

foreach(query_json IN ITEMS config_json world_json history_json)
    string(JSON diagnostic_count LENGTH "${${query_json}}" diagnostics)
    if(diagnostic_count LESS 1)
        message(FATAL_ERROR "read-only commands must report pending task migration")
    endif()
endforeach()
if(EXISTS "${PROFILE}/config/special-tasks.json")
    message(FATAL_ERROR "read-only commands must not migrate legacy special tasks")
endif()
