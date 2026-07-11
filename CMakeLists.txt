cmake_minimum_required(VERSION 3.22)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MineBackupVersion.cmake)
project(MineBackup VERSION ${MINEBACKUP_VERSION} LANGUAGES C CXX)

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
set(MINEBACKUP_KNOTLINK_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/KnotLink)
set(MINEBACKUP_JSON_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/json)
set(MINEBACKUP_STB_DIR ${MINEBACKUP_THIRD_PARTY_DIR}/stb)

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
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glfw)
set(MINEBACKUP_GLFW_TARGET glfw)

add_library(minebackup_third_party_imgui STATIC ${MINEBACKUP_IMGUI_SOURCES})
target_include_directories(minebackup_third_party_imgui PUBLIC ${MINEBACKUP_IMGUI_DIR})
target_link_libraries(minebackup_third_party_imgui PUBLIC ${MINEBACKUP_GLFW_TARGET})

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
    ${MINEBACKUP_JSON_DIR} ${MINEBACKUP_STB_DIR}
)
target_compile_definitions(minebackup_data_core PUBLIC MINEBACKUP_ENABLE_V15_MIGRATION=$<BOOL:${MINEBACKUP_ENABLE_V15_MIGRATION}>)
target_compile_definitions(minebackup_desktop PUBLIC MINEBACKUP_ENABLE_V15_MIGRATION=$<BOOL:${MINEBACKUP_ENABLE_V15_MIGRATION}>)
target_include_directories(minebackup_data_core PUBLIC ${MINEBACKUP_COMMON_INCLUDE_DIRS})
target_include_directories(minebackup_desktop PUBLIC ${MINEBACKUP_COMMON_INCLUDE_DIRS})
target_link_libraries(minebackup_desktop PUBLIC
    minebackup_data_core OpenGL::GL Threads::Threads minebackup_third_party_imgui
)
target_link_libraries(MineBackup PRIVATE minebackup_desktop)

if(MSVC)
    target_compile_options(minebackup_data_core PRIVATE /utf-8 /FS)
    target_compile_options(minebackup_desktop PRIVATE /utf-8 /FS)
    target_compile_options(minebackup_third_party_imgui PRIVATE /utf-8 /FS)
    target_compile_options(MineBackup PRIVATE /utf-8 /FS)
else()
    target_compile_options(minebackup_data_core PRIVATE -fno-strict-aliasing)
    target_compile_options(minebackup_desktop PRIVATE -fno-strict-aliasing)
    target_compile_options(minebackup_third_party_imgui PRIVATE -fno-strict-aliasing)
endif()

if(WIN32)
    set_target_properties(MineBackup PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(MineBackup PRIVATE ${MINEBACKUP_GENERATED_DIR}/MineBackup.rc)
    target_include_directories(MineBackup PRIVATE ${MINEBACKUP_GENERATED_DIR})
    target_link_libraries(minebackup_data_core PUBLIC ole32 advapi32)
    target_link_libraries(minebackup_desktop PUBLIC ws2_32 winhttp dwmapi)
endif()

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(APPINDICATOR3 QUIET appindicator3-0.1)
    pkg_check_modules(GTK3 QUIET gtk+-3.0)
    if(GTK3_FOUND)
        target_compile_definitions(minebackup_desktop PRIVATE MB_HAVE_GTK)
        target_include_directories(minebackup_desktop PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(minebackup_desktop PUBLIC ${GTK3_LIBRARIES})
        target_compile_options(minebackup_desktop PRIVATE ${GTK3_CFLAGS_OTHER})
    endif()
    if(APPINDICATOR3_FOUND AND GTK3_FOUND)
        target_compile_definitions(minebackup_desktop PRIVATE MB_HAVE_APPINDICATOR)
        target_include_directories(minebackup_desktop PRIVATE ${APPINDICATOR3_INCLUDE_DIRS})
        target_link_libraries(minebackup_desktop PUBLIC ${APPINDICATOR3_LIBRARIES})
        target_compile_options(minebackup_desktop PRIVATE ${APPINDICATOR3_CFLAGS_OTHER})
    endif()
    find_package(X11 REQUIRED)
    target_link_libraries(minebackup_desktop PUBLIC X11)
endif()

if(APPLE)
    set_source_files_properties(${MINEBACKUP_PLATFORM_DIR}/Platform_macos_tray.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(minebackup_desktop PUBLIC
        "-framework Cocoa" "-framework Carbon" "-framework IOKit" "-framework CoreVideo"
        "-framework CoreText" "-framework CoreFoundation"
    )
    find_library(ICONV_LIB iconv)
    if(ICONV_LIB)
        target_link_libraries(minebackup_desktop PUBLIC ${ICONV_LIB})
    endif()
endif()

set_target_properties(MineBackup PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(minebackup_data_tests tests/TestMain.cpp)
    target_link_libraries(minebackup_data_tests PRIVATE minebackup_data_core)
    add_test(NAME minebackup.data_core COMMAND minebackup_data_tests)
endif()

add_custom_target(check_msbuild_source_parity
    COMMAND ${CMAKE_COMMAND} -DMINEBACKUP_REPOSITORY_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSourceParity.cmake
    VERBATIM
)
