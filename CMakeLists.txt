cmake_minimum_required(VERSION 3.15)
project(MineBackup VERSION 1.11.7)

# For single-config generators (e.g. MinGW Makefiles/Ninja), default to Release.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/MineBackup)
set(SRC_DIR ${SOURCE_DIR}/src)
set(APP_DIR ${SRC_DIR}/app)
set(CORE_DIR ${SRC_DIR}/core)
set(INFRA_DIR ${SRC_DIR}/infra)
set(PLATFORM_DIR ${SRC_DIR}/platform)
set(UI_DIR ${SRC_DIR}/ui)
set(UTILS_DIR ${SRC_DIR}/utils)
set(THIRD_PARTY_DIR ${SOURCE_DIR}/third_party)
set(IMGUI_DIR ${THIRD_PARTY_DIR}/imgui)
set(KNOTLINK_DIR ${THIRD_PARTY_DIR}/KnotLink)
set(JSON_DIR ${THIRD_PARTY_DIR}/json)
set(STB_DIR ${THIRD_PARTY_DIR}/stb)
set(GLFW_DIR ${IMGUI_DIR}/GLFW)

set(SOURCES
    ${APP_DIR}/AppState.cpp
    ${APP_DIR}/Globals.cpp
    ${CORE_DIR}/BackupManager.cpp
    ${CORE_DIR}/GameSessionManager.cpp
    ${CORE_DIR}/HistoryManager.cpp
    ${CORE_DIR}/SpecialMode.cpp
    ${CORE_DIR}/TaskSystem.cpp
    ${INFRA_DIR}/Broadcast.cpp
    ${INFRA_DIR}/ConfigManager.cpp
    ${INFRA_DIR}/Console.cpp
    ${INFRA_DIR}/i18n.cpp
    ${UI_DIR}/HistoryUI.cpp
    ${UI_DIR}/SettingsUI.cpp
    ${UI_DIR}/SettingsUIAppearance.cpp
    ${UI_DIR}/SettingsUIConfig.cpp
    ${UI_DIR}/SettingsUISpecial.cpp
    ${UI_DIR}/WizardUI.cpp
    ${UTILS_DIR}/basic_func.cpp
    ${UTILS_DIR}/text_to_text.cpp
)

set(MAIN_SOURCES
    ${APP_DIR}/MineBackup.cpp
)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/imgui_impl_opengl3.cpp
)

set(HEADERS
    ${APP_DIR}/AppState.h
    ${APP_DIR}/DataModels.h
    ${APP_DIR}/Globals.h
    ${APP_DIR}/MainUI.h
    ${CORE_DIR}/BackupManager.h
    ${CORE_DIR}/BackupManagerRestore.inl
    ${CORE_DIR}/HistoryManager.h
    ${CORE_DIR}/TaskSystem.h
    ${INFRA_DIR}/Broadcast.h
    ${INFRA_DIR}/ConfigManager.h
    ${INFRA_DIR}/Console.h
    ${INFRA_DIR}/i18n.h
    ${PLATFORM_DIR}/PlatformCompat.h
    ${PLATFORM_DIR}/Platform_linux.h
    ${PLATFORM_DIR}/Platform_macos.h
    ${PLATFORM_DIR}/Platform_win.h
    ${UI_DIR}/IconsFontAwesome6.h
    ${UI_DIR}/SettingsUI.h
    ${UI_DIR}/SettingsUIPrivate.h
    ${UI_DIR}/UIHelpers.h
    ${UI_DIR}/imgui-all.h
    ${UTILS_DIR}/text_to_text.h
    ${JSON_DIR}/json.hpp
    ${STB_DIR}/stb_image.h
    ${IMGUI_DIR}/imgui.h
    ${IMGUI_DIR}/imgui_internal.h
    ${IMGUI_DIR}/imstb_rectpack.h
    ${IMGUI_DIR}/imstb_textedit.h
    ${IMGUI_DIR}/imstb_truetype.h
    ${IMGUI_DIR}/imconfig.h
    ${IMGUI_DIR}/imgui_impl_glfw.h
    ${IMGUI_DIR}/imgui_impl_opengl3.h
    ${IMGUI_DIR}/imgui_impl_opengl3_loader.h
    ${IMGUI_DIR}/imgui_style.h
    ${GLFW_DIR}/glfw3.h
    ${GLFW_DIR}/glfw3native.h
)

add_library(minebackup_third_party_imgui STATIC ${IMGUI_SOURCES})
target_include_directories(minebackup_third_party_imgui PUBLIC
    ${IMGUI_DIR}
    ${GLFW_DIR}
)

add_library(minebackup_core STATIC ${SOURCES} ${HEADERS})
add_executable(${PROJECT_NAME} ${MAIN_SOURCES} ${HEADERS})

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
set(GLFW_TARGET "")

if(WIN32)
    set(GLFW_BUNDLED_LIB ${IMGUI_DIR}/lib-vc2022-64/glfw3.lib)
    if(EXISTS ${GLFW_BUNDLED_LIB})
        add_library(glfw_bundled STATIC IMPORTED)
        set_target_properties(glfw_bundled PROPERTIES IMPORTED_LOCATION ${GLFW_BUNDLED_LIB})
        target_include_directories(glfw_bundled INTERFACE ${GLFW_DIR})
        set(GLFW_TARGET glfw_bundled)
    endif()
endif()

if(GLFW_TARGET STREQUAL "")
    find_package(glfw3 QUIET)
    if(glfw3_FOUND)
        if(TARGET glfw)
            set(GLFW_TARGET glfw)
        elseif(TARGET glfw3::glfw)
            set(GLFW_TARGET glfw3::glfw)
        endif()
    endif()
endif()

if(GLFW_TARGET STREQUAL "")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW3 glfw3)
        if(GLFW3_FOUND)
            add_library(glfw INTERFACE)
            target_include_directories(glfw INTERFACE ${GLFW3_INCLUDE_DIRS})
            target_link_libraries(glfw INTERFACE ${GLFW3_LIBRARIES})
            set(GLFW_TARGET glfw)
        endif()
    endif()
endif()

if(GLFW_TARGET STREQUAL "")
    find_library(GLFW_FALLBACK glfw3)
    if(GLFW_FALLBACK)
        add_library(glfw INTERFACE)
        target_link_libraries(glfw INTERFACE ${GLFW_FALLBACK})
        target_include_directories(glfw INTERFACE /usr/include)
        set(GLFW_TARGET glfw)
    endif()
endif()

if(GLFW_TARGET STREQUAL "")
    find_library(GLFW_FALLBACK glfw)
    if(GLFW_FALLBACK)
        add_library(glfw INTERFACE)
        target_link_libraries(glfw INTERFACE ${GLFW_FALLBACK})
        target_include_directories(glfw INTERFACE /usr/include)
        set(GLFW_TARGET glfw)
    endif()
endif()

if(APPLE AND GLFW_TARGET STREQUAL "")
    include(FetchContent)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.8
    )
    FetchContent_MakeAvailable(glfw)
    set(GLFW_TARGET glfw)
endif()

if(APPLE AND GLFW_TARGET STREQUAL "")
    find_library(GLFW_FALLBACK NAMES glfw3 glfw PATHS /opt/homebrew/lib /usr/local/lib /opt/local/lib)
    if(GLFW_FALLBACK)
        add_library(glfw INTERFACE)
        target_link_libraries(glfw INTERFACE ${GLFW_FALLBACK})
        target_include_directories(glfw INTERFACE /opt/homebrew/include /usr/local/include /opt/local/include)
        set(GLFW_TARGET glfw)
    endif()
endif()

if(WIN32)
    target_sources(minebackup_core PRIVATE
        ${PLATFORM_DIR}/Platform_win.cpp
        ${KNOTLINK_DIR}/OpenSocketResponser.cpp
        ${KNOTLINK_DIR}/SignalSender.cpp
    )
elseif(APPLE)
    target_sources(minebackup_core PRIVATE
        ${PLATFORM_DIR}/Platform_macos.cpp
        ${PLATFORM_DIR}/Platform_macos_tray.mm
    )
    set_source_files_properties(${PLATFORM_DIR}/Platform_macos_tray.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    # KnotLink POSIX 版本使用 header-only 实现
else()
    target_sources(minebackup_core PRIVATE ${PLATFORM_DIR}/Platform_linux.cpp)
endif()

# 若使用 MinGW 需要链接 libglfw3.a 而不是 glfw3.lib
target_link_libraries(minebackup_core PUBLIC
    OpenGL::GL
    ${GLFW_TARGET}
    Threads::Threads
    minebackup_third_party_imgui
)

target_include_directories(minebackup_core PUBLIC
    ${SOURCE_DIR}
    ${APP_DIR}
    ${CORE_DIR}
    ${INFRA_DIR}
    ${PLATFORM_DIR}
    ${UI_DIR}
    ${UTILS_DIR}
    ${THIRD_PARTY_DIR}
    ${IMGUI_DIR}
    ${KNOTLINK_DIR}
    ${JSON_DIR}
    ${STB_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE minebackup_core)

if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE ${SOURCE_DIR}/MineBackup.rc)
    target_link_libraries(minebackup_core PUBLIC ws2_32 winhttp dwmapi)
endif()

if(UNIX AND NOT APPLE AND NOT WIN32)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(APPINDICATOR3 QUIET appindicator3-0.1)
        pkg_check_modules(GTK3 QUIET gtk+-3.0)
        if(APPINDICATOR3_FOUND AND GTK3_FOUND)
            target_compile_definitions(minebackup_core PRIVATE MB_HAVE_APPINDICATOR)
            target_include_directories(minebackup_core PRIVATE ${APPINDICATOR3_INCLUDE_DIRS} ${GTK3_INCLUDE_DIRS})
            target_link_libraries(minebackup_core PUBLIC ${APPINDICATOR3_LIBRARIES} ${GTK3_LIBRARIES})
            target_compile_options(minebackup_core PRIVATE ${APPINDICATOR3_CFLAGS_OTHER} ${GTK3_CFLAGS_OTHER})
        endif()
    endif()
    find_package(X11 REQUIRED)
    if(X11_FOUND)
        target_link_libraries(minebackup_core PUBLIC X11)
    endif()
endif()

if(APPLE)
    target_link_libraries(minebackup_core PUBLIC
        "-framework Cocoa"
        "-framework Carbon"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework CoreText"
        "-framework CoreFoundation"
    )
    find_library(ICONV_LIB iconv)
    if(ICONV_LIB)
        target_link_libraries(minebackup_core PUBLIC ${ICONV_LIB})
    endif()
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)