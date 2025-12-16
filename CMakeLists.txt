cmake_minimum_required(VERSION 3.15)
project(MineBackup)

# For single-config generators (e.g. MinGW Makefiles/Ninja), default to Release.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/MineBackup)
set(IMGUI_DIR ${SOURCE_DIR}/imgui)
set(KNOTLINK_DIR ${SOURCE_DIR}/KnotLink)
set(GLFW_DIR ${SOURCE_DIR}/imgui/GLFW)

set(SOURCES
    ${SOURCE_DIR}/MineBackup.cpp
    ${SOURCE_DIR}/BackupManager.cpp
    ${SOURCE_DIR}/AppState.cpp
    ${SOURCE_DIR}/ConfigManager.cpp
    ${SOURCE_DIR}/Console.cpp
    ${SOURCE_DIR}/Broadcast.cpp
    ${SOURCE_DIR}/GameSessionManager.cpp
    ${SOURCE_DIR}/HistoryManager.cpp
    ${SOURCE_DIR}/basic_func.cpp
    ${SOURCE_DIR}/i18n.cpp
    ${SOURCE_DIR}/Platform_win.cpp
    ${SOURCE_DIR}/text_to_text.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/imgui_impl_opengl3.cpp
    ${KNOTLINK_DIR}/main.cpp
    ${KNOTLINK_DIR}/OpenSocketResponser.cpp
    ${KNOTLINK_DIR}/SignalSender.cpp
)

set(HEADERS
    ${SOURCE_DIR}/AppState.h
    ${SOURCE_DIR}/BackupManager.h
    ${SOURCE_DIR}/Broadcast.h
    ${SOURCE_DIR}/ConfigManager.h
    ${SOURCE_DIR}/Console.h
    ${SOURCE_DIR}/HistoryManager.h
    ${SOURCE_DIR}/Platform_win.h
    ${SOURCE_DIR}/text_to_text.h
    ${SOURCE_DIR}/i18n.h
    ${SOURCE_DIR}/json.hpp
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
    ${KNOTLINK_DIR}/SignalSender.hpp
    ${KNOTLINK_DIR}/TcpClient.hpp
    ${KNOTLINK_DIR}/OpenSocketResponser.hpp
)

set(RESOURCES
    ${SOURCE_DIR}/MineBackup.rc
)

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${RESOURCES})

find_package(OpenGL REQUIRED)
# find_package(glfw3 REQUIRED)
find_package(Threads REQUIRED)

# 若使用 MinGW 需要链接 libglfw3.a 而不是 glfw3.lib
target_link_libraries(${PROJECT_NAME} PRIVATE
    OpenGL::GL
    glfw
    Threads::Threads
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${SOURCE_DIR}
    ${IMGUI_DIR}
    ${KNOTLINK_DIR}
)

if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE ${SOURCE_DIR}/MineBackup.rc)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
    target_link_libraries(${PROJECT_NAME} PRIVATE winhttp)
    target_link_libraries(${PROJECT_NAME} PRIVATE dwmapi)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin

)
