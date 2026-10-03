#include "RuntimeFileLock.h"

#include "text_to_text.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

using namespace std;

bool IsRuntimeFileLocked(const filesystem::path& path) {
	error_code error;
	if (!filesystem::exists(path, error) || error) return false;
#ifdef _WIN32
	HANDLE file = CreateFileW(
		path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		const DWORD code = GetLastError();
		return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
	}
	CloseHandle(file);
	return false;
#else
	const string native = wstring_to_utf8(path.wstring());
	if (native.empty()) return false;
	int file = open(native.c_str(), O_RDWR);
	if (file < 0) file = open(native.c_str(), O_RDONLY);
	if (file < 0) return false;

	struct flock lock {};
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	if (fcntl(file, F_GETLK, &lock) == 0) {
		close(file);
		return lock.l_type != F_UNLCK;
	}
	const int flockResult = flock(file, LOCK_EX | LOCK_NB);
	const bool locked = flockResult < 0
		&& (errno == EWOULDBLOCK || errno == EAGAIN);
	if (flockResult == 0) flock(file, LOCK_UN);
	close(file);
	return locked;
#endif
}

bool IsRuntimeWorldOccupied(const filesystem::path& worldPath) {
	error_code error;
	if (!filesystem::is_directory(worldPath, error) || error) return false;

	for (const filesystem::path& candidate : {
		worldPath / L"session.lock", worldPath / L"level.dat",
		worldPath / L"db" / L"LOCK"}) {
		if (IsRuntimeFileLocked(candidate)) return true;
	}

	const filesystem::path database = worldPath / L"db";
	error.clear();
	if (!filesystem::is_directory(database, error) || error) return false;
	int inspected = 0;
	for (filesystem::directory_iterator iterator(
		database, filesystem::directory_options::skip_permission_denied, error), end;
		iterator != end && !error && inspected < 20;
		iterator.increment(error), ++inspected) {
		if (IsRuntimeFileLocked(iterator->path())) return true;
	}
	return false;
}
