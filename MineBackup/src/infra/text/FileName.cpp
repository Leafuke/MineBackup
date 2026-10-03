#include "FileName.h"

using namespace std;

wstring SanitizeFileName(const wstring& input) {
	wstring output = input;
	const wstring invalidCharacters = L"\\/:*?\"<>|";
	for (wchar_t& character : output) {
		if (invalidCharacters.find(character) != wstring::npos) character = L'_';
	}
	return output;
}
