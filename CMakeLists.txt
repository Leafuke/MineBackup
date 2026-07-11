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
set(MINEBACKUP_GLFW_INCLUDE_DIR ${MINEBACKUP_IMGUI_DIR}/GLFW)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MineBackupSources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MineBackupGenerateArtifacts.cmake)

set(MINEBACKUP_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
minebackup_generate_artifacts(${MINEBACKUP_GENERATED_DIR} ${MINEBACKUP_SOURCE_DIR})

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

set(MINEBACKUP_GLFW_TARGET "")
if(WIN32)
    set(MINEBACKUP_BUNDLED_GLFW ${MINEBACKUP_IMGUI_DIR}/lib-vc2022-64/glfw3.lib)
    if(EXISTS ${MINEBACKUP_BUNDLED_GLFW})
        add_library(minebackup_glfw_bundled STATIC IMPORTED)
        set_target_properties(minebackup_glfw_bundled PROPERTIES IMPORTED_LOCATION ${MINEBACKUP_BUNDLED_GLFW})
        target_include_directories(minebackup_glfw_bundled INTERFACE ${MINEBACKUP_GLFW_INCLUDE_DIR})
        set(MINEBACKUP_GLFW_TARGET minebackup_glfw_bundled)
    endif()
endif()

if(MINEBACKUP_GLFW_TARGET STREQUAL "")
    find_package(glfw3 QUIET)
    if(TARGET glfw)
        set(MINEBACKUP_GLFW_TARGET glfw)
    elseif(TARGET glfw3::glfw)
        set(MINEBACKUP_GLFW_TARGET glfw3::glfw)
    endif()
endif()

if(MINEBACKUP_GLFW_TARGET STREQUAL "")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW3 QUIET glfw3)
        if(GLFW3_FOUND)
            add_library(minebackup_glfw_system INTERFACE)
            target_include_directories(minebackup_glfw_system INTERFACE ${GLFW3_INCLUDE_DIRS})
            target_link_libraries(minebackup_glfw_system INTERFACE ${GLFW3_LIBRARIES})
            set(MINEBACKUP_GLFW_TARGET minebackup_glfw_system)
        endif()
    endif()
endif()

if(APPLE AND MINEBACKUP_GLFW_TARGET STREQUAL "")
    include(FetchContent)
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.3.8)
    FetchContent_MakeAvailable(glfw)
    set(MINEBACKUP_GLFW_TARGET glfw)
endif()

if(MINEBACKUP_GLFW_TARGET STREQUAL "")
    message(FATAL_ERROR "GLFW was not found. MineBackup requires an explicit GLFW target.")
endif()

add_library(minebackup_third_party_imgui STATIC ${MINEBACKUP_IMGUI_SOURCES})
target_include_directories(minebackup_third_party_imgui PUBLIC ${MINEBACKUP_IMGUI_DIR} ${MINEBACKUP_GLFW_INCLUDE_DIR})
target_link_libraries(minebackup_third_party_imgui PUBLIC ${MINEBACKUP_GLFW_TARGET})

set(MINEBACKUP_CORE_SOURCES ${MINEBACKUP_DATA_CORE_SOURCES} ${MINEBACKUP_APPLICATION_SOURCES} ${MINEBACKUP_UI_SOURCES})
if(WIN32)
    list(APPEND MINEBACKUP_CORE_SOURCES ${MINEBACKUP_WINDOWS_SOURCES})
elseif(APPLE)
    list(APPEND MINEBACKUP_CORE_SOURCES ${MINEBACKUP_MACOS_SOURCES})
else()
    list(APPEND MINEBACKUP_CORE_SOURCES ${MINEBACKUP_LINUX_SOURCES})
endif()

add_library(minebackup_core STATIC ${MINEBACKUP_CORE_SOURCES} ${MINEBACKUP_PUBLIC_HEADERS})
add_executable(MineBackup ${MINEBACKUP_MAIN_SOURCES} ${MINEBACKUP_PUBLIC_HEADERS})

target_compile_definitions(minebackup_core PUBLIC MINEBACKUP_ENABLE_V15_MIGRATION=$<BOOL:${MINEBACKUP_ENABLE_V15_MIGRATION}>)
target_include_directories(minebackup_core PUBLIC
    ${MINEBACKUP_GENERATED_DIR} ${MINEBACKUP_SOURCE_DIR} ${MINEBACKUP_APP_DIR} ${MINEBACKUP_CORE_DIR}
    ${MINEBACKUP_INFRA_DIR} ${MINEBACKUP_PLATFORM_DIR} ${MINEBACKUP_UI_DIR} ${MINEBACKUP_UTILS_DIR}
    ${MINEBACKUP_THIRD_PARTY_DIR} ${MINEBACKUP_IMGUI_DIR} ${MINEBACKUP_KNOTLINK_DIR}
    ${MINEBACKUP_JSON_DIR} ${MINEBACKUP_STB_DIR} ${MINEBACKUP_GLFW_INCLUDE_DIR}
)
target_link_libraries(minebackup_core PUBLIC OpenGL::GL Threads::Threads minebackup_third_party_imgui)
target_link_libraries(MineBackup PRIVATE minebackup_core)

if(MSVC)
    target_compile_options(minebackup_core PRIVATE /utf-8 /FS)
    target_compile_options(minebackup_third_party_imgui PRIVATE /utf-8 /FS)
    target_compile_options(MineBackup PRIVATE /utf-8 /FS)
else()
    target_compile_options(minebackup_core PRIVATE -fno-strict-aliasing)
    target_compile_options(minebackup_third_party_imgui PRIVATE -fno-strict-aliasing)
endif()

if(WIN32)
    set_target_properties(MineBackup PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(MineBackup PRIVATE ${MINEBACKUP_GENERATED_DIR}/MineBackup.rc)
    target_include_directories(MineBackup PRIVATE ${MINEBACKUP_GENERATED_DIR})
    target_link_libraries(minebackup_core PUBLIC ws2_32 winhttp dwmapi)
endif()

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(APPINDICATOR3 QUIET appindicator3-0.1)
    pkg_check_modules(GTK3 QUIET gtk+-3.0)
    if(APPINDICATOR3_FOUND AND GTK3_FOUND)
        target_compile_definitions(minebackup_core PRIVATE MB_HAVE_APPINDICATOR)
        target_include_directories(minebackup_core PRIVATE ${APPINDICATOR3_INCLUDE_DIRS} ${GTK3_INCLUDE_DIRS})
        target_link_libraries(minebackup_core PUBLIC ${APPINDICATOR3_LIBRARIES} ${GTK3_LIBRARIES})
        target_compile_options(minebackup_core PRIVATE ${APPINDICATOR3_CFLAGS_OTHER} ${GTK3_CFLAGS_OTHER})
    endif()
    find_package(X11 REQUIRED)
    target_link_libraries(minebackup_core PUBLIC X11)
endif()

if(APPLE)
    set_source_files_properties(${MINEBACKUP_PLATFORM_DIR}/Platform_macos_tray.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(minebackup_core PUBLIC
        "-framework Cocoa" "-framework Carbon" "-framework IOKit" "-framework CoreVideo"
        "-framework CoreText" "-framework CoreFoundation"
    )
    find_library(ICONV_LIB iconv)
    if(ICONV_LIB)
        target_link_libraries(minebackup_core PUBLIC ${ICONV_LIB})
    endif()
endif()

set_target_properties(MineBackup PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

add_custom_target(check_msbuild_source_parity
    COMMAND ${CMAKE_COMMAND} -DMINEBACKUP_REPOSITORY_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSourceParity.cmake
    VERBATIM
)
