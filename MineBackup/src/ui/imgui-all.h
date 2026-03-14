#pragma once
#ifndef _ImGui1
#define _ImGui1

#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h>
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif
#pragma comment (lib,"glfw3.lib")
#pragma comment (lib,"opengl32.lib") 
#include "imgui/imgui.h"
#include <imgui_internal.h>
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_impl_glfw.h"
#include "stb_image.h"
#include "json.hpp"
#include "IconsFontAwesome6.h"
#endif