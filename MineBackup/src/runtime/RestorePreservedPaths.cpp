#include "RestorePreservedPaths.h"
#include "text_to_text.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

using namespace std;
namespace RestorePreservedPaths {
namespace {
constexpr size_t ByteLimit = 64u * 1024u * 1024u;
constexpr size_t FileLimit = 4096;
wstring Fold(wstring s) {
    transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return towlower(c); });
    return s;
}
wstring Relative(const filesystem::path& path, const filesystem::path& root) {
    const auto relative = path.lexically_relative(root).generic_u8string();
    return utf8_to_wstring(string(relative.begin(), relative.end()));
}
using Inventory = map<wstring, filesystem::path>;
Inventory Inspect(const filesystem::path& root, stop_token token, Inventory& directories) {
    Inventory files;
    set<wstring> entries;
    if (filesystem::is_symlink(filesystem::symlink_status(root)))
        throw runtime_error("Preservation root is a symbolic link.");
    for (const auto& entry : filesystem::recursive_directory_iterator(root)) {
        if (token.stop_requested()) throw runtime_error("Preservation cancelled.");
        auto status = entry.symlink_status();
        if (filesystem::is_symlink(status))
            throw runtime_error("Preservation does not accept symbolic links.");
        const auto name = Fold(Relative(entry.path(), root));
        if (!entries.insert(name).second) throw runtime_error("Case-ambiguous world paths.");
        if (entries.size() > 1000000) throw runtime_error("World inventory exceeds safety limit.");
        if (filesystem::is_directory(status)) { directories.emplace(name, entry.path()); continue; }
        if (!filesystem::is_regular_file(status))
            throw runtime_error("Preservation does not accept special files.");
        if (!files.emplace(name, entry.path()).second)
            throw runtime_error("Case-ambiguous world paths.");
        if (files.size() > 1000000) throw runtime_error("World inventory exceeds safety limit.");
    }
    return files;
}
wstring Trim(wstring value) {
    const auto begin = value.find_first_not_of(L" \t\r\n");
    if (begin == wstring::npos) return {};
    return value.substr(begin, value.find_last_not_of(L" \t\r\n") - begin + 1);
}
filesystem::path ResolveLevel(const Inventory& files, const filesystem::path& root) {
    if (const auto properties = files.find(L"server.properties"); properties != files.end()) {
        const auto size = filesystem::file_size(properties->second);
        if (size > 1024u * 1024u) throw runtime_error("Server properties exceeds the preservation limit.");
        string bytes(static_cast<size_t>(size), '\0');
        ifstream stream(properties->second, ios::binary);
        if (!stream || !stream.read(bytes.data(), static_cast<streamsize>(size))
            || stream.peek() != char_traits<char>::eof())
            throw runtime_error("Server properties could not be read consistently.");
        if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
        const auto text = utf8_to_wstring(bytes);
        optional<wstring> configured;
        for (size_t start = 0; start < text.size();) {
            auto end = text.find(L'\n', start);
            if (end == wstring::npos) end = text.size();
            const auto line = Trim(text.substr(start, end - start)); start = end + 1;
            if (line.empty() || line.front() == L'#' || line.front() == L'!') continue;
            const auto separator = line.find_first_of(L"=: \t");
            if (line.substr(0, separator) != L"level-name") continue;
            if (configured) throw runtime_error("Ambiguous server level-name.");
            auto name = separator == wstring::npos ? wstring{} : Trim(line.substr(separator));
            if (!name.empty() && (name.front() == L'=' || name.front() == L':')) name = Trim(name.substr(1));
            vector<wstring> normalized; string error;
            if (!Normalize({name}, normalized, error) || normalized.front().back() == L'/')
                throw runtime_error("Server level-name must be a managed relative world path.");
            configured = normalized.front() + L"/level.dat";
        }
        // The server's configured (or default) world is authoritative even when
        // an overwrite restore leaves other world directories in staging.
        const auto expected = configured.value_or(L"world/level.dat");
        const auto level = files.find(Fold(expected));
        if (level == files.end()) throw runtime_error("Configured server world is outside the managed inventory.");
#ifndef _WIN32
        if (Relative(level->second, root) != expected || Relative(properties->second, root) != L"server.properties")
            throw runtime_error("Configured server world has noncanonical path casing.");
#endif
        return level->second;
    }
    optional<filesystem::path> level;
    for (const auto& [name, path] : files) {
        if (name != L"level.dat" && !name.ends_with(L"/level.dat")) continue;
        if (level) throw runtime_error("Preservation requires one managed Minecraft world.");
        level = path;
    }
    if (!level) throw runtime_error("World level.dat is missing.");
    return *level;
}
bool Matches(const vector<wstring>& selectors, const wstring& path) {
    return any_of(selectors.begin(), selectors.end(), [&](const auto& selector) {
        return selector.back() == L'/' ? path.starts_with(selector) : path == selector;
    });
}
}

bool Normalize(const vector<wstring>& input, vector<wstring>& output, string& error) {
    output.clear(); error.clear();
    for (auto path : input) {
        const auto begin = path.find_first_not_of(L" \t\r\n");
        if (begin == wstring::npos) { error = "Empty preserve path."; return false; }
        path = path.substr(begin, path.find_last_not_of(L" \t\r\n") - begin + 1);
        replace(path.begin(), path.end(), L'\\', L'/');
        if (path.size() > 512 || path.front() == L'/'
            || path.find_first_of(L":,*?\"<>|") != wstring::npos
            || any_of(path.begin(), path.end(), [](wchar_t c) { return iswcntrl(c); })) {
            error = "Invalid preserve path."; return false;
        }
        const auto end = path.back() == L'/' ? path.size() - 1 : path.size();
        for (size_t start = 0; start < end;) {
            auto slash = path.find(L'/', start);
            auto stop = min(slash == wstring::npos ? end : slash, end);
            auto part = path.substr(start, stop - start);
            if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') {
                error = "Preserve paths must be canonical and world-relative."; return false;
            }
            start = stop + 1;
        }
        if (!end || (end > 0 && path[end - 1] == L'/')) {
            error = "Empty preserve path segment."; return false;
        }
        const auto folded = Fold(path);
        if (any_of(output.begin(), output.end(), [&](const auto& old) {
            auto value = Fold(old);
            return value == folded || (value.back() == L'/' && folded.starts_with(value));
        })) continue;
        if (path.back() == L'/') erase_if(output, [&](const auto& old) { return Fold(old).starts_with(folded); });
        output.push_back(path);
        if (output.size() > 16) { error = "At most 16 preserve paths are supported."; return false; }
    }
    sort(output.begin(), output.end(), [](const auto& a, const auto& b) { return Fold(a) < Fold(b); });
    return true;
}

bool Apply(const filesystem::path& current, const filesystem::path& staging,
    const vector<wstring>& input, string& error, stop_token token) {
    vector<wstring> selectors;
    if (!Normalize(input, selectors, error)) return false;
    if (selectors.empty()) return true;
    try {
        Inventory sourceDirectories, targetDirectories;
        const auto source = Inspect(current, token, sourceDirectories);
        const auto target = Inspect(staging, token, targetDirectories);
        auto sourceLevel = Relative(ResolveLevel(source, current), current);
        auto targetLevel = Relative(ResolveLevel(target, staging), staging);
#ifdef _WIN32
        sourceLevel = Fold(sourceLevel); targetLevel = Fold(targetLevel);
#endif
        if (sourceLevel != targetLevel) throw runtime_error("Target and current server world roots conflict.");
        const auto prefix = Fold(sourceLevel.substr(0, sourceLevel.size() - 9));
        for (auto& selector : selectors) selector = prefix + Fold(selector);
        struct File { filesystem::path relative; string bytes; };
        vector<File> files;
        vector<filesystem::path> deletes;
        size_t bytes = 0;
        for (const auto& [name, path] : source) {
            if (!Matches(selectors, name)) continue;
            if (token.stop_requested()) throw runtime_error("Preservation cancelled.");
            const auto size = filesystem::file_size(path);
            if (size > ByteLimit - bytes) throw runtime_error("Preservation exceeds 64 MiB.");
            string content(static_cast<size_t>(size), '\0');
            ifstream stream(path, ios::binary);
            if (!stream || (size && !stream.read(content.data(), static_cast<streamsize>(size)))
                || stream.peek() != char_traits<char>::eof())
                throw runtime_error("Preserved file could not be frozen consistently.");
            bytes += content.size();
            files.push_back({path.lexically_relative(current), std::move(content)});
            if (files.size() > FileLimit) throw runtime_error("Preservation exceeds 4096 files.");
        }
        for (const auto& [name, path] : target) {
            if (Matches(selectors, name)) {
                // Recreate using current spelling, including case-only renames on Linux.
                deletes.push_back(path);
                if (files.size() + deletes.size() > FileLimit * 2)
                    throw runtime_error("Preservation exceeds file limit.");
            }
        }
        size_t absent = 0;
        for (const auto& [name, path] : target) {
            (void)path;
            if (Matches(selectors, name) && !source.contains(name)) ++absent;
        }
        if (files.size() + absent > FileLimit) throw runtime_error("Preservation exceeds 4096 files.");
        vector<filesystem::path> removeDirectories, createDirectories;
        for (const auto& [name, path] : targetDirectories)
            if (Matches(selectors, name + L"/")) removeDirectories.push_back(path);
        for (const auto& [name, path] : sourceDirectories)
            if (Matches(selectors, name + L"/")) createDirectories.push_back(path.lexically_relative(current));
        if (removeDirectories.size() + createDirectories.size() > FileLimit)
            throw runtime_error("Preservation exceeds directory limit.");
        sort(removeDirectories.begin(), removeDirectories.end(), [](const auto& a, const auto& b) {
            return distance(a.begin(), a.end()) > distance(b.begin(), b.end());
        });
        // Everything has been read and checked before staging changes begin.
        for (const auto& path : deletes) {
            if (token.stop_requested()) throw runtime_error("Preservation cancelled.");
            filesystem::remove(path);
        }
        for (const auto& path : removeDirectories) {
            if (token.stop_requested()) throw runtime_error("Preservation cancelled.");
            filesystem::remove(path); // Files and deeper selected directories were removed first.
        }
        for (const auto& relative : createDirectories) {
            if (token.stop_requested()) throw runtime_error("Preservation cancelled.");
            filesystem::create_directories(staging / relative);
        }
        for (const auto& file : files) {
            if (token.stop_requested()) throw runtime_error("Preservation cancelled.");
            auto destination = staging / file.relative;
            filesystem::create_directories(destination.parent_path());
            ofstream stream(destination, ios::binary | ios::trunc);
            if (!stream || !stream.write(file.bytes.data(), static_cast<streamsize>(file.bytes.size())))
                throw runtime_error("Could not stage preserved file.");
            stream.close();
            if (!stream) throw runtime_error("Could not finish preserved file.");
        }
        return true;
    } catch (const exception& exception) { error = exception.what(); return false; }
}
}
