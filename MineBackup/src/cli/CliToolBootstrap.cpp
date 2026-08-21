#include "CliToolBootstrap.h"

#include "ExternalToolManager.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

bool EnsureCliSevenZip(
	const AppPaths& paths,
	stop_token stopToken,
	wstring& error) {
	error.clear();
	if (ExternalToolManager::ResolveSevenZip({}, paths, stopToken).available) return true;
#ifdef _WIN32
	HRSRC resource = FindResourceW(
		GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), L"EXE");
	if (!resource) {
		error = L"The CLI 7-Zip resource is missing.";
		return false;
	}
	HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
	const void* data = loaded ? LockResource(loaded) : nullptr;
	const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
	const auto install = ExternalToolManager::InstallBundledSevenZipForWindows(
		data, static_cast<size_t>(size), paths, stopToken);
	if (!install.success) {
		error = install.error;
		return false;
	}
	return true;
#else
	error = L"No supported 7zz executable was found.";
	return false;
#endif
}
