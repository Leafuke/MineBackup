if(NOT DEFINED CLI)
    message(FATAL_ERROR "CLI is required")
endif()
if(NOT EXISTS "${CLI}")
    message(FATAL_ERROR "CLI does not exist: ${CLI}")
endif()

if(APPLE)
    execute_process(
        COMMAND otool -L "${CLI}"
        RESULT_VARIABLE inspect_result
        OUTPUT_VARIABLE dependencies
        ERROR_VARIABLE inspect_error)
    if(NOT inspect_result EQUAL 0)
        message(FATAL_ERROR "otool failed: ${inspect_error}")
    endif()
    string(TOLOWER "${dependencies}" normalized)
    foreach(forbidden IN ITEMS "glfw" "opengl.framework" "appkit.framework" "cocoa.framework")
        string(FIND "${normalized}" "${forbidden}" found)
        if(NOT found EQUAL -1)
            message(FATAL_ERROR "headless CLI links forbidden macOS dependency '${forbidden}':\n${dependencies}")
        endif()
    endforeach()
elseif(UNIX)
    execute_process(
        COMMAND ldd "${CLI}"
        RESULT_VARIABLE inspect_result
        OUTPUT_VARIABLE dependencies
        ERROR_VARIABLE inspect_error)
    if(NOT inspect_result EQUAL 0)
        message(FATAL_ERROR "ldd failed: ${inspect_error}")
    endif()
    string(TOLOWER "${dependencies}" normalized)
    foreach(forbidden IN ITEMS
            "libglfw" "libopengl" "libgl.so" "libglx.so" "libx11"
            "libwayland" "libgtk" "libgio" "libappindicator" "libayatana-appindicator")
        string(FIND "${normalized}" "${forbidden}" found)
        if(NOT found EQUAL -1)
            message(FATAL_ERROR "headless CLI links forbidden Linux dependency '${forbidden}':\n${dependencies}")
        endif()
    endforeach()
else()
    message(FATAL_ERROR "Use VerifyWindowsCliBinary.ps1 for Windows binaries")
endif()

message(STATUS "Headless CLI dependency audit passed:\n${dependencies}")
