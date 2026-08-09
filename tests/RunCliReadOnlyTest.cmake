if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

function(assert_json_contract expected_result expected_command)
    execute_process(
        COMMAND "${CLI}" --json ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    string(STRIP "${output}" output)
    string(JSON schema ERROR_VARIABLE json_error GET "${output}" schemaVersion)
    string(JSON command GET "${output}" command)
    if(NOT result EQUAL expected_result OR json_error OR NOT schema EQUAL 1
            OR NOT command STREQUAL expected_command)
        message(FATAL_ERROR
            "JSON contract failed (${result}): ${error}\n${output}")
    endif()
endfunction()

# Help/version do not open a profile, and parse errors use the same single
# envelope as business commands once --json has been recognized.
assert_json_contract(0 "help" --help)
assert_json_contract(0 "version" --version)
assert_json_contract(2 "parse" --unknown-option)

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

if(EXISTS "${PROFILE}/config/special-tasks.json")
    message(FATAL_ERROR "read-only commands must not migrate legacy special tasks")
endif()

set(SCRIPT_PROFILE "${TEST_ROOT}/cli-doctor-script-profile")
file(REMOVE_RECURSE "${SCRIPT_PROFILE}")
file(MAKE_DIRECTORY "${SCRIPT_PROFILE}/config" "${SCRIPT_PROFILE}/server/world")
file(WRITE "${SCRIPT_PROFILE}/config/config.ini"
"[General]\n"
"RestoreWhitelistItem=ops-marker.txt\n"
"[Config1]\n"
"ConfigName=Server\n"
"ConfigId=11111111-1111-4111-8111-111111111111\n"
"SavePath=${SCRIPT_PROFILE}/server\n"
"WorldData=\n"
"world\n"
"Primary world\n"
"*\n"
"BackupPath=${SCRIPT_PROFILE}/backups\n"
"ZipLevel=5\n"
"SmartBackup=2\n"
"UseLowPriority=0\n"
"SkipIfUnchanged=1\n"
"CloudSyncEnabled=0\n"
"[SpCfg2]\n"
"Name=Script preservation\n"
"SpecialConfigId=22222222-2222-4222-8222-222222222222\n")
file(WRITE "${SCRIPT_PROFILE}/config/jobs.json"
"{\"schemaVersion\":1,\"jobs\":[{"
"\"jobId\":\"33333333-3333-4333-8333-333333333333\","
"\"name\":\"Backup world\",\"stages\":[{"
"\"stageId\":\"44444444-4444-4444-8444-444444444444\","
"\"name\":\"Backup\",\"steps\":[{"
"\"stepId\":\"55555555-5555-4555-8555-555555555555\","
"\"name\":\"World\",\"type\":\"backup\",\"target\":{"
"\"configId\":\"11111111-1111-4111-8111-111111111111\","
"\"worldPath\":\"world\"}}]}]}]}\n")
file(WRITE "${SCRIPT_PROFILE}/config/special-tasks.json"
"{\n"
"  \"schemaVersion\": 1,\n"
"  \"specialConfigs\": [{\n"
"    \"specialConfigId\": \"22222222-2222-4222-8222-222222222222\",\n"
"    \"tasks\": [{\n"
"      \"taskId\": \"33333333-3333-4333-8333-333333333333\",\n"
"      \"name\": \"preserved script\",\n"
"      \"type\": \"script\",\n"
"      \"executionMode\": \"sequential\",\n"
"      \"enabled\": true,\n"
"      \"trigger\": {\"type\": \"once\"},\n"
"      \"command\": \"echo preserved\",\n"
"      \"workingDirectory\": \"\"\n"
"    }]\n"
"  }]\n"
"}\n")
execute_process(
    COMMAND "${CLI}" --data-dir "${SCRIPT_PROFILE}" --json --no-network doctor
    RESULT_VARIABLE script_doctor_result
    OUTPUT_VARIABLE script_doctor_json
    ERROR_VARIABLE script_doctor_error)
string(STRIP "${script_doctor_json}" script_doctor_json)
string(JSON script_doctor_schema ERROR_VARIABLE script_doctor_json_error
    GET "${script_doctor_json}" schemaVersion)
string(JSON script_doctor_code GET "${script_doctor_json}" code)
string(JSON ignored_sections GET "${script_doctor_json}" data legacySpecialConfigSectionsIgnored)
string(JSON ignored_document GET "${script_doctor_json}" data legacySpecialTasksFileIgnored)
string(JSON jobs_status GET "${script_doctor_json}" data jobs status)
string(JSON jobs_references GET "${script_doctor_json}" data jobs referencesValid)
string(JSON backup_writable GET "${script_doctor_json}" data paths 0 backupRootWritable)
string(JSON world_ready GET "${script_doctor_json}" data paths 0 worlds 0 coldRestoreReady)
string(JSON preserve_count LENGTH "${script_doctor_json}" data restorePreserve)
string(JSON preserve_first GET "${script_doctor_json}" data restorePreserve 0)
string(JSON preserve_second GET "${script_doctor_json}" data restorePreserve 1)
if(NOT script_doctor_result EQUAL 8 OR script_doctor_json_error
		OR NOT script_doctor_schema EQUAL 1
		OR NOT script_doctor_code STREQUAL "tool_unavailable"
        OR NOT ignored_sections EQUAL 1 OR NOT ignored_document
		OR NOT jobs_status STREQUAL "loaded" OR NOT jobs_references
		OR NOT backup_writable OR NOT world_ready OR NOT preserve_count EQUAL 2
		OR NOT preserve_first STREQUAL "ops-marker.txt"
		OR NOT preserve_second STREQUAL "session.lock")
    message(FATAL_ERROR
        "doctor must report retained legacy special data as ignored: "
        "${script_doctor_error}\n${script_doctor_json}")
endif()

set(INVALID_JOB_PROFILE "${TEST_ROOT}/cli-doctor-invalid-job-profile")
file(REMOVE_RECURSE "${INVALID_JOB_PROFILE}")
file(COPY "${SCRIPT_PROFILE}/" DESTINATION "${INVALID_JOB_PROFILE}")
file(WRITE "${INVALID_JOB_PROFILE}/config/jobs.json"
"{\"schemaVersion\":1,\"jobs\":[{"
"\"jobId\":\"33333333-3333-4333-8333-333333333333\","
"\"name\":\"Broken\",\"stages\":[{"
"\"stageId\":\"44444444-4444-4444-8444-444444444444\","
"\"name\":\"Backup\",\"steps\":[{"
"\"stepId\":\"55555555-5555-4555-8555-555555555555\","
"\"name\":\"Missing\",\"type\":\"backup\",\"target\":{"
"\"configId\":\"99999999-9999-4999-8999-999999999999\","
"\"worldPath\":\"world\"}}]}]}]}\n")
execute_process(
	COMMAND "${CLI}" --data-dir "${INVALID_JOB_PROFILE}" --json --no-network doctor
	RESULT_VARIABLE invalid_job_result
	OUTPUT_VARIABLE invalid_job_json
	ERROR_VARIABLE invalid_job_error)
string(STRIP "${invalid_job_json}" invalid_job_json)
string(JSON invalid_job_code ERROR_VARIABLE invalid_job_json_error GET "${invalid_job_json}" code)
string(JSON invalid_job_references GET "${invalid_job_json}" data jobs referencesValid)
if(NOT invalid_job_result EQUAL 5 OR invalid_job_json_error
		OR NOT invalid_job_code STREQUAL "invalid_profile" OR invalid_job_references)
	message(FATAL_ERROR
		"doctor must reject dangling Job backup references: "
		"${invalid_job_error}\n${invalid_job_json}")
endif()
