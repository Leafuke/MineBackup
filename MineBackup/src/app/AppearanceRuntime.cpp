#include "AppearanceRuntime.h"

#include "AppPaths.h"
#include "Globals.h"
#include "PlatformCompat.h"
#include "ProcessRunner.h"
#include "i18n.h"
#include "imgui-all.h"
#include "ThemeManager.h"
#include "text_to_text.h"

#include <filesystem>
#include <string>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <limits.h>
#endif

using namespace std;

wstring GetDefaultUIFontPath() {
#ifdef _WIN32
	// 动态获取 Windows 字体目录
	wstring fontsDir = L"C:\\Windows\\Fonts\\";
	{
		wchar_t winDir[MAX_PATH] = {};
		if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0)
			fontsDir = wstring(winDir) + L"\\Fonts\\";
	}
	// 用户字体目录
	wstring userFontsDir;
	{
		wchar_t localAppData[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0)
			userFontsDir = wstring(localAppData) + L"\\Microsoft\\Windows\\Fonts\\";
	}

	if (g_CurrentLang == "zh_CN") {
		const wchar_t* cn_names[] = { L"msyh.ttc", L"msyh.ttf", L"msjh.ttc", L"msjh.ttf", L"SegoeUI.ttf", nullptr };
		for (const wchar_t** fn = cn_names; *fn; ++fn) {
			wstring p = fontsDir + *fn;
			if (filesystem::exists(p)) return p;
			if (!userFontsDir.empty()) {
				p = userFontsDir + *fn;
				if (filesystem::exists(p)) return p;
			}
		}
	}
	const wchar_t* en_names[] = { L"SegoeUI.ttf", L"segoeui.ttf", L"arial.ttf", nullptr };
	for (const wchar_t** fn = en_names; *fn; ++fn) {
		wstring p = fontsDir + *fn;
		if (filesystem::exists(p)) return p;
		if (!userFontsDir.empty()) {
			p = userFontsDir + *fn;
			if (filesystem::exists(p)) return p;
		}
	}
	return L"";

#elif defined(__APPLE__)
	// 使用 Core Text 按名称查找字体文件路径
	auto findFontPath = [](const char* fontName) -> wstring {
		CFStringRef cfName = CFStringCreateWithCString(kCFAllocatorDefault, fontName, kCFStringEncodingUTF8);
		if (!cfName) return L"";
		CTFontRef font = CTFontCreateWithName(cfName, 12.0, nullptr);
		CFRelease(cfName);
		if (!font) return L"";
		CFURLRef url = (CFURLRef)CTFontCopyAttribute(font, kCTFontURLAttribute);
		CFRelease(font);
		if (!url) return L"";
		char path[PATH_MAX];
		Boolean ok = CFURLGetFileSystemRepresentation(url, true, (UInt8*)path, sizeof(path));
		CFRelease(url);
		if (!ok) return L"";
		return utf8_to_wstring(string(path));
	};

	if (g_CurrentLang == "zh_CN") {
		// 中文字体：苹方 > 华文黑体 > Hiragino
		const char* cnFontNames[] = {
			"PingFangSC-Regular", "PingFang SC",
			"STHeitiSC-Light", "STHeiti",
			"Hiragino Sans GB", "Hiragino Sans",
			nullptr
		};
		for (const char** fn = cnFontNames; *fn; ++fn) {
			wstring p = findFontPath(*fn);
			if (!p.empty() && filesystem::exists(p)) return p;
		}
	}
	// 英文/默认字体
	const char* enFontNames[] = {
		"Helvetica Neue", "Helvetica",
		".AppleSystemUIFont",
		"Lucida Grande", "Arial",
		nullptr
	};
	for (const char** fn = enFontNames; *fn; ++fn) {
		wstring p = findFontPath(*fn);
		if (!p.empty() && filesystem::exists(p)) return p;
	}
	// 回退到硬编码路径
	const wstring fallbacks[] = {
		L"/System/Library/Fonts/PingFang.ttc",
		L"/System/Library/Fonts/STHeiti Light.ttc",
		L"/System/Library/Fonts/Helvetica.ttc",
		L"/System/Library/Fonts/Supplemental/Arial.ttf",
		L"/Library/Fonts/Arial.ttf"
	};
	for (const auto& cand : fallbacks) {
		if (filesystem::exists(cand)) return cand;
	}

	return L"";


#else
	// Linux: 使用 fontconfig (fc-match) 动态查找字体
	auto findFontByFc = [](const char* pattern) -> wstring {
		ProcessSpec spec;
		spec.executable = L"/usr/bin/fc-match";
		spec.arguments = {L"-f", L"%{file}", utf8_to_wstring(pattern)};
		spec.maximumCapturedBytes = 4096;
		const auto result = ProcessRunner::Run(spec);
		if (result.status != ProcessStatus::Succeeded) return L"";
		string output = result.standardOutput;
		if (!output.empty() && output.back() == '\n') output.pop_back();
		if (!output.empty() && filesystem::exists(output))
			return utf8_to_wstring(output);
		return L"";
	};

	if (g_CurrentLang == "zh_CN") {
		const char* cnPatterns[] = {
			"Noto Sans CJK SC:style=Regular",
			"Noto Sans CJK:style=Regular",
			"WenQuanYi Zen Hei",
			"WenQuanYi Micro Hei",
			"Droid Sans Fallback",
			nullptr
		};
		for (const char** p = cnPatterns; *p; ++p) {
			wstring path = findFontByFc(*p);
			if (!path.empty()) return path;
		}
	}
	// 默认字体
	const char* defaultPatterns[] = {
		"sans-serif",
		"DejaVu Sans",
		"Liberation Sans",
		"Noto Sans",
		nullptr
	};
	for (const char** p = defaultPatterns; *p; ++p) {
		wstring path = findFontByFc(*p);
		if (!path.empty()) return path;
	}
	// 回退到硬编码路径
	const wstring fallbacks[] = {
		L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		L"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
		L"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
		L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
	};
	for (const auto& cand : fallbacks) {
		if (filesystem::exists(cand)) return cand;
	}
	return L"";
#endif
}
void ApplyTheme()
{
	// ScaleAllSizes() is lossy. Always rebuild from a fresh, unscaled style so
	// repeated theme and scale changes cannot compound rounded dimensions.
	ImGuiStyle& style = ImGui::GetStyle();
	const float dpiScale = style.FontScaleDpi;
	style = ImGuiStyle();
	style.FontScaleDpi = dpiScale;

	int effectiveTheme = g_theme;
	if (effectiveTheme == static_cast<int>(ThemeId::SystemAuto)) {
		const int target = IsSystemDarkMode() ? g_systemThemeDark : g_systemThemeLight;
		effectiveTheme = (IsValidThemeId(target) && target != static_cast<int>(ThemeId::SystemAuto))
			? target
			: (IsSystemDarkMode()
				? static_cast<int>(ThemeId::WindowsDark)
				: static_cast<int>(ThemeId::WindowsLight));
	}

	auto applyBuiltInTheme = [](int theme) {
		switch (static_cast<ThemeId>(theme)) {
		case ThemeId::ImGuiDark: ImGuiTheme::ApplyImGuiDark(); break;
		case ThemeId::ImGuiLight: ImGuiTheme::ApplyImGuiLight(); break;
		case ThemeId::ImGuiClassic: ImGuiTheme::ApplyImGuiClassic(); break;
		case ThemeId::WindowsLight: ImGuiTheme::ApplyWindows11(false); break;
		case ThemeId::WindowsDark: ImGuiTheme::ApplyWindows11(true); break;
		case ThemeId::NordLight: ImGuiTheme::ApplyNord(false); break;
		case ThemeId::NordDark: ImGuiTheme::ApplyNord(true); break;
		case ThemeId::VSCodeDark: ImGuiTheme::ApplyVSCodeDark(); break;
		case ThemeId::SolarizedLight: ImGuiTheme::ApplySolarized(false); break;
		case ThemeId::SolarizedDark: ImGuiTheme::ApplySolarized(true); break;
		default: ImGuiTheme::ApplyWindows11(false); break;
		}
		ImGuiTheme::EnsureAccessibleThemeContrast(ImGui::GetStyle());
	};

	bool applied = true;
	g_customThemeError.clear();
	if (effectiveTheme == static_cast<int>(ThemeId::Custom)) {
		applied = ImGuiTheme::ApplyCustom(
			GetAppPaths().configRoot / L"custom_theme.json", &g_customThemeError);
		if (applied) {
			applied = ImGuiTheme::ValidateTextContrast(style, &g_customThemeError);
		}
	}
	else if (IsValidThemeId(effectiveTheme)) {
		applyBuiltInTheme(effectiveTheme);
		g_lastValidTheme = effectiveTheme;
	}
	else {
		effectiveTheme = static_cast<int>(ThemeId::ImGuiLight);
		g_lastValidTheme = effectiveTheme;
		applyBuiltInTheme(effectiveTheme);
	}

	if (!applied) {
		style = ImGuiStyle();
		style.FontScaleDpi = dpiScale;
		applyBuiltInTheme(g_lastValidTheme);
	}

	if (g_theme == static_cast<int>(ThemeId::Custom) && applied) {
		EnableDarkModeWin(
			ImGuiTheme::RelativeLuminance(style.Colors[ImGuiCol_WindowBg]) < 0.45f);
	}

	style.FontScaleMain = g_uiScale;
	style.ScaleAllSizes(g_uiScale);

	if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}
}
