#pragma once
#ifndef _ImGui1
#define _ImGui1
#include "imgui/imgui.h"
//#include "imgui/imgui_internal.h"  // 包含内部
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include "stb_image.h"
#include "IconsFontAwesome6.h"
#include "resource.h"
#include <d3d11.h>
#pragma comment (lib,"d3d11.lib") 
//↑需要手动添加d3d11.lib文件，否则编译会报错。
#endif