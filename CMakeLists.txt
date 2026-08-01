cmake_minimum_required(VERSION 3.22)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MineBackupVersion.cmake)
if(CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME MATCHES "^(Darwin|iOS|tvOS|watchOS)$")
    # These values must exist before project() enables the Apple toolchain.
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "MineBackup macOS architecture" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "MineBackup macOS deployment target" FORCE)
endif()
project(MineBackup VERSION ${MINEBACKUP_VERSION} LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)

    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINEBACKUP_ENABLE_V15_MIGRATION "Build the MineBackup 1.15 compatibility adapter" ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

set(MINEBACKUP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/MineBackup)
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
set(MINEBACKUP_JSON_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/json)
set(MINEBACKUP_STB_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/stb)

if(WIN32)
    set(MINEBACKUP_WINDOWS_7ZIP "${MINEBACKUP_SOURCE_DIR}/Assets/7za.exe")
    set(MINEBACKUP_WINDOWS_7ZIP_SHA256
        "d8d92a77b6b34b07deafdac6e0845076eac5bf76bf026b554d101e81363cd052")
    if(NOT EXISTS "${MINEBACKUP_WINDOWS_7ZIP}")
        message(FATAL_ERROR "The tracked MineBackup/Assets/7za.exe resource is missing.")
    endif()
    file(SHA256 "${MINEBACKUP_WINDOWS_7ZIP}" MINEBACKUP_WINDOWS_7ZIP_ACTUAL_SHA256)
    if(NOT MINEBACKUP_WINDOWS_7ZIP_ACTUAL_SHA256 STREQUAL MINEBACKUP_WINDOWS_7ZIP_SHA256)
        message(FATAL_ERROR
            "MineBackup/Assets/7za.exe does not match the pinned supply-chain hash.")
    endif()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MineBackupSources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MineBackupGenerateArtifacts.cmake)

if(MINEBACKUP_ENABLE_V15_MIGRATION)
    list(APPEND MINEBACKUP_DATA_CORE_SOURCES ${MINEBACKUP_V15_DATA_SOURCES})
    list(APPEND MINEBACKUP_APPLICATION_SOURCES ${MINEBACKUP_V15_DESKTOP_SOURCES})
endif()

set(MINEBACKUP_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
minebackup_generate_artifacts(${MINEBACKUP_GENERATED_DIR} ${MINEBACKUP_SOURCE_DIR})

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
if(NOT WIN32)
    find_package(Iconv REQUIRED)
endif()

include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(glfw
    # GLFW 3.4 crashes during glfwInit() when a Wayland compositor has no
    # wl_seat, as is intentional for headless Weston. Pin the upstream fix
    # until it is included in a tagged GLFW release:
    # https://github.com/glfw/glfw/issues/2517
    URL https://github.com/glfw/glfw/archive/3573c5a890b4878bb7357f71daaf49260c6fef15.zip
    URL_HASH SHA256=dba78a22b73c231560ef3817b7fde606170711af733c8c8065ad2e6f0285bd07
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glfw)
set(MINEBACKUP_GLFW_TARGET glfw)

add_library(minebackup_third_party_imgui STATIC ${MINEBACKUP_IMGUI_SOURCES})
target_include_directories(minebackup_third_party_imgui PUBLIC ${MINEBACKUP_IMGUI_DIR})
target_link_libraries(minebackup_third_party_imgui PUBLIC ${MINEBACKUP_GLFW_TARGET})
target_compile_definitions(minebackup_third_party_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

add_library(minebackup_third_party_spdlog STATIC ${MINEBACKUP_SPDLOG_SOURCES})
target_include_directories(minebackup_third_party_spdlog PUBLIC ${MINEBACKUP_SPDLOG_DIR}/include)
target_link_libraries(minebackup_third_party_spdlog PUBLIC Threads::Threads)
target_compile_definitions(minebackup_third_party_spdlog PUBLIC
    SPDLOG_COMPILED_LIB
    $<$<PLATFORM_ID:Windows>:SPDLOG_WCHAR_FILENAMES>
)

set(MINEBACKUP_DESKTOP_SOURCES ${MINEBACKUP_APPLICATION_SOURCES} ${MINEBACKUP_UI_SOURCES})
if(WIN32)
    list(APPEND MINEBACKUP_DESKTOP_SOURCES ${MINEBACKUP_WINDOWS_SOURCES})
elseif(APPLE)
    list(APPEND MINEBACKUP_DESKTOP_SOURCES ${MINEBACKUP_MACOS_SOURCES})
else()
    list(APPEND MINEBACKUP_DESKTOP_SOURCES ${MINEBACKUP_LINUX_SOURCES})
endif()

add_library(minebackup_data_core STATIC ${MINEBACKUP_DATA_CORE_SOURCES} ${MINEBACKUP_PUBLIC_HEADERS})
add_library(minebackup_desktop STATIC ${MINEBACKUP_DESKTOP_SOURCES} ${MINEBACKUP_PUBLIC_HEADERS})
add_executable(MineBackup ${MINEBACKUP_MAIN_SOURCES} ${MINEBACKUP_PUBLIC_HEADERS})

set(MINEBACKUP_COMMON_INCLUDE_DIRS
    ${MINEBACKUP_GENERATED_DIR} ${MINEBACKUP_SOURCE_DIR} ${MINEBACKUP_APP_DIR} ${MINEBACKUP_CORE_DIR}
    ${MINEBACKUP_INFRA_DIR} ${MINEBACKUP_PLATFORM_DIR} ${MINEBACKUP_UI_DIR} ${MINEBACKUP_UTILS_DIR}
    ${MINEBACKUP_THIRD_PARTY_DIR} ${MINEBACKUP_IMGUI_DIR} ${MINEBACKUP_KNOTLINK_DIR}
    ${MINEBACKUP_JSON_DIR} ${MINEBACKUP_STB_DIR} ${MINEBACKUP_SPDLOG_DIR}/include
)
target_compile_definitions(minebackup_data_core PUBLIC MINEBACKUP_ENABLE_V15_MIGRATION=$<BOOL:${MINEBACKUP_ENABLE_V15_MIGRATION}>)
target_compile_definitions(minebackup_desktop PUBLIC MINEBACKUP_ENABLE_V15_MIGRATION=$<BOOL:${MINEBACKUP_ENABLE_V15_MIGRATION}>)
target_include_directories(minebackup_data_core PUBLIC ${MINEBACKUP_COMMON_INCLUDE_DIRS})
target_include_directories(minebackup_desktop PUBLIC ${MINEBACKUP_COMMON_INCLUDE_DIRS})
target_link_libraries(minebackup_data_core PUBLIC Threads::Threads minebackup_third_party_spdlog)
if(NOT WIN32)
    target_link_libraries(minebackup_data_core PUBLIC Iconv::Iconv)
endif()
target_link_libraries(minebackup_desktop PUBLIC
    minebackup_data_core OpenGL::GL minebackup_third_party_imgui
)
target_link_libraries(MineBackup PRIVATE minebackup_desktop)

if(MSVC)
    target_compile_options(minebackup_data_core PRIVATE /utf-8 /FS)
    target_compile_options(minebackup_desktop PRIVATE /utf-8 /FS /Zc:preprocessor)
    target_compile_options(minebackup_third_party_imgui PRIVATE /utf-8 /FS)
    target_compile_options(minebackup_third_party_spdlog PRIVATE /utf-8 /FS)
    target_compile_options(MineBackup PRIVATE /utf-8 /FS /Zc:preprocessor)
else()
    target_compile_options(minebackup_data_core PRIVATE -fno-strict-aliasing)
    target_compile_options(minebackup_desktop PRIVATE -fno-strict-aliasing)
    target_compile_options(minebackup_third_party_imgui PRIVATE -fno-strict-aliasing)
    target_compile_options(minebackup_third_party_spdlog PRIVATE -fno-strict-aliasing)
endif()

if(APPLE
        AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18.0)
    # Xcode 16's libc++ still gates std::jthread/std::stop_token and their
    # runtime support behind this option. PUBLIC compile and link usage
    # requirements also cover every final consumer of the static data core.
    target_compile_options(minebackup_data_core PUBLIC -fexperimental-library)
    target_link_options(minebackup_data_core PUBLIC -fexperimental-library)
endif()

if(WIN32)
    set_target_properties(MineBackup PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(MineBackup PRIVATE ${MINEBACKUP_GENERATED_DIR}/MineBackup.rc)
    target_include_directories(MineBackup PRIVATE ${MINEBACKUP_GENERATED_DIR})
    target_link_libraries(minebackup_data_core PUBLIC ole32 advapi32 shell32)
    target_link_libraries(minebackup_desktop PUBLIC ws2_32 winhttp dwmapi version)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(minebackup_data_core PRIVATE _GNU_SOURCE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO2 REQUIRED gio-2.0 gio-unix-2.0)
    target_include_directories(minebackup_desktop PRIVATE ${GIO2_INCLUDE_DIRS})
    target_link_libraries(minebackup_desktop PUBLIC ${GIO2_LIBRARIES})
    target_compile_options(minebackup_desktop PRIVATE ${GIO2_CFLAGS_OTHER})
    pkg_check_modules(APPINDICATOR3 QUIET ayatana-appindicator3-0.1)
    if(APPINDICATOR3_FOUND)
        set(MINEBACKUP_USE_AYATANA_APPINDICATOR ON)
    else()
        # Keep a compatibility fallback for distributions that still package
        # the original AppIndicator API under its historical pkg-config name.
        pkg_check_modules(APPINDICATOR3 QUIET appindicator3-0.1)
    endif()
    pkg_check_modules(GTK3 QUIET gtk+-3.0)
    if(GTK3_FOUND)
        target_compile_definitions(minebackup_desktop PRIVATE MB_HAVE_GTK)
        target_include_directories(minebackup_desktop PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(minebackup_desktop PUBLIC ${GTK3_LIBRARIES})
        target_compile_options(minebackup_desktop PRIVATE ${GTK3_CFLAGS_OTHER})
    endif()
    if(APPINDICATOR3_FOUND AND GTK3_FOUND)
        target_compile_definitions(minebackup_desktop PRIVATE MB_HAVE_APPINDICATOR)
        if(MINEBACKUP_USE_AYATANA_APPINDICATOR)
            target_compile_definitions(minebackup_desktop PRIVATE MB_USE_AYATANA_APPINDICATOR)
        endif()
        target_include_directories(minebackup_desktop PRIVATE ${APPINDICATOR3_INCLUDE_DIRS})
        target_link_libraries(minebackup_desktop PUBLIC ${APPINDICATOR3_LIBRARIES})
        target_compile_options(minebackup_desktop PRIVATE ${APPINDICATOR3_CFLAGS_OTHER})
    endif()
    find_package(X11 REQUIRED)
    find_package(CURL REQUIRED)
    target_link_libraries(minebackup_desktop PUBLIC X11 CURL::libcurl)
endif()

if(APPLE)
    set(MINEBACKUP_MACOS_ICON_SOURCE
        ${MINEBACKUP_SOURCE_DIR}/Assets/icons/minebackup.png)
    set(MINEBACKUP_MACOS_ICON
        ${MINEBACKUP_GENERATED_DIR}/MineBackup.icns)
    add_custom_command(
        OUTPUT ${MINEBACKUP_MACOS_ICON}
        COMMAND /bin/bash
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/generate-icon.sh
            ${MINEBACKUP_MACOS_ICON_SOURCE}
            ${MINEBACKUP_MACOS_ICON}
        DEPENDS
            ${MINEBACKUP_MACOS_ICON_SOURCE}
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/generate-icon.sh
        COMMENT "Generating MineBackup.icns"
        VERBATIM)

    set_source_files_properties(
        ${MINEBACKUP_PLATFORM_DIR}/Platform_macos_tray.mm
        ${MINEBACKUP_PLATFORM_DIR}/MacDesktopBridge.mm
        ${MINEBACKUP_PLATFORM_DIR}/NetworkBackend_macos.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_target_properties(MineBackup PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${MINEBACKUP_GENERATED_DIR}/Info.plist)
    set(MINEBACKUP_MACOS_ASSET_RESOURCES
        ${MINEBACKUP_SOURCE_DIR}/Assets/fontawesome-sp.otf)
    set_source_files_properties(${MINEBACKUP_MACOS_ASSET_RESOURCES}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/Assets")
    set(MINEBACKUP_MACOS_EN_LOCALIZATION
        ${MINEBACKUP_SOURCE_DIR}/Assets/macos/en.lproj/InfoPlist.strings)
    set(MINEBACKUP_MACOS_ZH_LOCALIZATION
        ${MINEBACKUP_SOURCE_DIR}/Assets/macos/zh-Hans.lproj/InfoPlist.strings)
    set_source_files_properties(${MINEBACKUP_MACOS_EN_LOCALIZATION}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/en.lproj")
    set_source_files_properties(${MINEBACKUP_MACOS_ZH_LOCALIZATION}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/zh-Hans.lproj")
    set_source_files_properties(${MINEBACKUP_MACOS_ICON}
        PROPERTIES GENERATED TRUE MACOSX_PACKAGE_LOCATION "Resources")
    set(MINEBACKUP_MACOS_RESOURCES
        ${MINEBACKUP_MACOS_ASSET_RESOURCES}
        ${MINEBACKUP_MACOS_EN_LOCALIZATION}
        ${MINEBACKUP_MACOS_ZH_LOCALIZATION}
        ${MINEBACKUP_MACOS_ICON})
    target_sources(MineBackup PRIVATE ${MINEBACKUP_MACOS_RESOURCES})
    target_link_libraries(minebackup_desktop PUBLIC
        "-framework Cocoa" "-framework Carbon" "-framework IOKit" "-framework CoreVideo"
        "-framework CoreText" "-framework CoreFoundation"
        "-framework Foundation" "-framework ServiceManagement"
        "-framework UserNotifications"
    )
endif()

set_target_properties(MineBackup PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(minebackup_knotlink_server_manager_tests
        tests/KnotLinkServerManagerTest.cpp)
    target_link_libraries(minebackup_knotlink_server_manager_tests PRIVATE
        minebackup_desktop)
    if(MSVC)
        target_compile_options(minebackup_knotlink_server_manager_tests PRIVATE
            /utf-8 /FS)
    endif()
    add_test(NAME minebackup.knotlink_server_manager
        COMMAND minebackup_knotlink_server_manager_tests)

    add_executable(minebackup_knotlink_command_tests tests/KnotLinkCommandTest.cpp)
    target_link_libraries(minebackup_knotlink_command_tests PRIVATE minebackup_desktop)
    if(MSVC)
        target_compile_options(minebackup_knotlink_command_tests PRIVATE /utf-8 /FS)
    endif()
    add_test(NAME minebackup.knotlink_commands COMMAND minebackup_knotlink_command_tests)

    add_executable(minebackup_knotlink_protocol_tests tests/KnotLinkProtocolTest.cpp)
    target_link_libraries(minebackup_knotlink_protocol_tests PRIVATE minebackup_data_core)
    if(MSVC)
        target_compile_options(minebackup_knotlink_protocol_tests PRIVATE /utf-8 /FS)
    endif()
    add_test(NAME minebackup.knotlink_protocol COMMAND minebackup_knotlink_protocol_tests)

    add_executable(minebackup_knotlink_sdk_tests tests/KnotLinkSdkTest.cpp)
    target_include_directories(minebackup_knotlink_sdk_tests PRIVATE
        ${MINEBACKUP_THIRD_PARTY_DIR}/knotlink-sdk-cpp-2.0)
    target_link_libraries(minebackup_knotlink_sdk_tests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(minebackup_knotlink_sdk_tests PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_options(minebackup_knotlink_sdk_tests PRIVATE /utf-8 /FS)
    endif()
    add_test(NAME minebackup.knotlink_sdk COMMAND minebackup_knotlink_sdk_tests)

    add_executable(minebackup_data_tests
        tests/TestMain.cpp
        tests/BackupPipelineTest.cpp
        tests/CloudSyncTests.cpp
        tests/StorageMigrationTests.cpp
        tests/ProcessToolTests.cpp
        tests/RuntimeInfrastructureTests.cpp)
    target_link_libraries(minebackup_data_tests PRIVATE minebackup_data_core)
    if(MSVC)
        target_compile_options(minebackup_data_tests PRIVATE /utf-8 /FS)
    endif()
    if(WIN32)
        target_link_libraries(minebackup_data_tests PRIVATE shell32)
    endif()
    add_test(NAME minebackup.data_core COMMAND minebackup_data_tests)

    add_executable(minebackup_i18n_tests
        tests/I18nTest.cpp
        ${MINEBACKUP_INFRA_DIR}/i18n.cpp
        ${MINEBACKUP_UTILS_DIR}/text_to_text.cpp)
    target_include_directories(minebackup_i18n_tests PRIVATE
        ${MINEBACKUP_COMMON_INCLUDE_DIRS})
    if(MSVC)
        target_compile_options(minebackup_i18n_tests PRIVATE /utf-8 /FS)
    else()
        target_compile_options(minebackup_i18n_tests PRIVATE -fno-strict-aliasing)
    endif()
    if(NOT WIN32)
        target_link_libraries(minebackup_i18n_tests PRIVATE Iconv::Iconv)
    endif()
    add_test(NAME minebackup.i18n COMMAND minebackup_i18n_tests)

    add_executable(minebackup_appearance_config_tests
        tests/AppearanceConfigTest.cpp)
    target_link_libraries(minebackup_appearance_config_tests PRIVATE
        minebackup_desktop)
    if(MSVC)
        target_compile_options(minebackup_appearance_config_tests PRIVATE /utf-8 /FS)
    endif()
    add_test(NAME minebackup.appearance_config
        COMMAND minebackup_appearance_config_tests)
    set_tests_properties(minebackup.appearance_config PROPERTIES TIMEOUT 30)
endif()

add_custom_target(check_msbuild_source_parity
    COMMAND ${CMAKE_COMMAND} -DMINEBACKUP_REPOSITORY_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSourceParity.cmake
    VERBATIM
)
