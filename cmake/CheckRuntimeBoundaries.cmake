if(NOT DEFINED MINEBACKUP_REPOSITORY_ROOT)
    message(FATAL_ERROR "MINEBACKUP_REPOSITORY_ROOT is required")
endif()

set(MINEBACKUP_SOURCE_DIR ${MINEBACKUP_REPOSITORY_ROOT}/MineBackup)
set(MINEBACKUP_SRC_DIR ${MINEBACKUP_SOURCE_DIR}/src)
set(MINEBACKUP_APP_DIR ${MINEBACKUP_SRC_DIR}/app)
set(MINEBACKUP_CORE_DIR ${MINEBACKUP_SRC_DIR}/core)
set(MINEBACKUP_INFRA_DIR ${MINEBACKUP_SRC_DIR}/infra)
set(MINEBACKUP_PLATFORM_DIR ${MINEBACKUP_SRC_DIR}/platform)
set(MINEBACKUP_UI_DIR ${MINEBACKUP_SRC_DIR}/ui)
set(MINEBACKUP_UTILS_DIR ${MINEBACKUP_SRC_DIR}/utils)
set(MINEBACKUP_THIRD_PARTY_DIR ${MINEBACKUP_SOURCE_DIR}/third_party)
set(MINEBACKUP_IMGUI_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/imgui)
set(MINEBACKUP_KNOTLINK_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/knotlink-sdk-cpp-2.0)
set(MINEBACKUP_SPDLOG_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/spdlog)
include(${MINEBACKUP_REPOSITORY_ROOT}/cmake/MineBackupSources.cmake)

set(runtime_contract_files
    ${MINEBACKUP_RUNTIME_SOURCES}
    ${MINEBACKUP_APP_DIR}/ConfigSelection.h
    ${MINEBACKUP_CORE_DIR}/BackupManagerInternal.h
    ${MINEBACKUP_CORE_DIR}/BackupService.h
    ${MINEBACKUP_CORE_DIR}/HistoryRepository.h
	${MINEBACKUP_CORE_DIR}/HotRestoreCoordinator.h
    ${MINEBACKUP_CORE_DIR}/MigrationCoordinator.h
    ${MINEBACKUP_CORE_DIR}/OperationResult.h
    ${MINEBACKUP_CORE_DIR}/ProfileConfigCatalog.h
	${MINEBACKUP_CORE_DIR}/ProfileKnotLinkCommands.h
    ${MINEBACKUP_CORE_DIR}/ProfileRuntime.h
    ${MINEBACKUP_CORE_DIR}/RuntimeCloudPostHook.h
    ${MINEBACKUP_CORE_DIR}/RuntimeFileLock.h
    ${MINEBACKUP_CORE_DIR}/RuntimeIntegration.h
    ${MINEBACKUP_CORE_DIR}/RuntimeRetentionService.h
    ${MINEBACKUP_CORE_DIR}/TaskCoordinator.h
    ${MINEBACKUP_INFRA_DIR}/InterruptedTaskRecovery.h
	${MINEBACKUP_INFRA_DIR}/KnotLinkCommandDispatcher.h
    ${MINEBACKUP_INFRA_DIR}/LegacyIniConfigCodec.h
    ${MINEBACKUP_INFRA_DIR}/SingleInstanceService.h)

set(forbidden_include
    "#[ \t]*include[ \t]*[<\"](Globals\\.h|AppState\\.h|DesktopServices\\.h|imgui[^>\"]*|GLFW/[^>\"]*)[>\"]")
foreach(path IN LISTS runtime_contract_files)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Runtime contract file is missing: ${path}")
    endif()
    file(READ "${path}" content)
    string(REGEX MATCH "${forbidden_include}" violation "${content}")
    if(violation)
        file(RELATIVE_PATH relative "${MINEBACKUP_REPOSITORY_ROOT}" "${path}")
        message(FATAL_ERROR "Runtime boundary violation in ${relative}: ${violation}")
    endif()
endforeach()

# data_core 是 runtime 的下层依赖，禁止通过 TaskCoordinator 回连 runtime。
set(data_core_forbidden_include
    "#[ \t]*include[ \t]*[<\"]TaskCoordinator\\.h[>\"]")
foreach(path IN LISTS MINEBACKUP_DATA_CORE_SOURCES)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Data-core contract file is missing: ${path}")
    endif()
    file(READ "${path}" content)
    string(REGEX MATCH "${data_core_forbidden_include}" violation "${content}")
    if(violation)
        file(RELATIVE_PATH relative "${MINEBACKUP_REPOSITORY_ROOT}" "${path}")
        message(FATAL_ERROR
            "Data-core boundary violation in ${relative}: ${violation}")
    endif()
endforeach()

message(STATUS "Runtime source/header boundary audit passed")
