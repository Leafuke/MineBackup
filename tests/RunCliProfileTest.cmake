if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

set(ROOT "${TEST_ROOT}/cli-profile")
set(PROFILE "${ROOT}/profile")
set(MANIFEST "${ROOT}/manifest.json")
set(EXPORTED "${ROOT}/exported.json")
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

function(run_success output_variable)
    execute_process(
        COMMAND "${CLI}" --json ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 30)
    string(STRIP "${output}" output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "CLI profile command failed (${result}): ${error}\n${output}")
    endif()
    string(JSON schema ERROR_VARIABLE json_error GET "${output}" schemaVersion)
    if(json_error OR NOT schema EQUAL 1)
        message(FATAL_ERROR "CLI profile stdout is not one schema-v1 envelope: ${output}")
    endif()
    set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

run_success(init_json profile init --output "${MANIFEST}")
if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR "profile init did not create the manifest")
endif()
string(JSON init_configs GET "${init_json}" data configCount)
string(JSON init_jobs GET "${init_json}" data jobCount)
if(NOT init_configs EQUAL 1 OR NOT init_jobs EQUAL 1)
    message(FATAL_ERROR "profile init should create an editable Config/Job template: ${init_json}")
endif()

run_success(validate_json profile validate --file "${MANIFEST}")
string(JSON validate_code GET "${validate_json}" code)
if(NOT validate_code STREQUAL "success")
    message(FATAL_ERROR "profile validate rejected the generated template: ${validate_json}")
endif()

run_success(apply_json --data-dir "${PROFILE}" profile apply --file "${MANIFEST}")
string(JSON apply_changes GET "${apply_json}" data changeCount)
if(NOT apply_changes EQUAL 3 OR NOT EXISTS "${PROFILE}/config/config.ini"
        OR NOT EXISTS "${PROFILE}/config/jobs.json")
    message(FATAL_ERROR "profile apply did not establish the empty Profile: ${apply_json}")
endif()

run_success(diff_json --data-dir "${PROFILE}" profile diff --file "${MANIFEST}")
string(JSON diff_changes GET "${diff_json}" data changeCount)
if(NOT diff_changes EQUAL 0)
    message(FATAL_ERROR "repeated profile apply should be idempotent: ${diff_json}")
endif()

run_success(export_json --data-dir "${PROFILE}" profile export --output "${EXPORTED}")
if(NOT EXISTS "${EXPORTED}")
    message(FATAL_ERROR "profile export did not create a manifest")
endif()
run_success(export_validate profile validate --file "${EXPORTED}")

execute_process(
    COMMAND "${CLI}" --json --data-dir "${PROFILE}" profile apply
        --file "${MANIFEST}" --prune
    RESULT_VARIABLE unsafe_prune_result
    OUTPUT_VARIABLE unsafe_prune_json
    ERROR_VARIABLE unsafe_prune_error
    TIMEOUT 30)
string(STRIP "${unsafe_prune_json}" unsafe_prune_json)
string(JSON unsafe_prune_code GET "${unsafe_prune_json}" code)
if(NOT unsafe_prune_result EQUAL 2
        OR NOT unsafe_prune_code STREQUAL "invalid_arguments")
    message(FATAL_ERROR "profile apply --prune must require explicit confirmation: ${unsafe_prune_error}\n${unsafe_prune_json}")
endif()

run_success(prune_preview --data-dir "${PROFILE}" profile apply
    --file "${MANIFEST}" --prune --dry-run)
string(JSON preview_dry_run GET "${prune_preview}" data dryRun)
if(NOT preview_dry_run)
    message(FATAL_ERROR "prune dry-run should plan without confirmation: ${prune_preview}")
endif()

execute_process(
    COMMAND "${CLI}" --json profile init --output "${MANIFEST}"
    RESULT_VARIABLE overwrite_result
    OUTPUT_VARIABLE overwrite_json
    ERROR_VARIABLE overwrite_error
    TIMEOUT 30)
if(NOT overwrite_result EQUAL 2)
    message(FATAL_ERROR "profile init should refuse overwrite without --force: ${overwrite_error}\n${overwrite_json}")
endif()
run_success(force_json profile init --output "${MANIFEST}" --force)
