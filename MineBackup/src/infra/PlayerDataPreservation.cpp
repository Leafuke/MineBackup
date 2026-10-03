#include "PlayerDataPreservation.h"

#include "ArchiveRunner.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace PlayerDataPreservation {
namespace {
namespace fs = std::filesystem;
using Bytes = std::string;
Bytes Utf8Path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return Bytes(reinterpret_cast<const char*>(value.data()), value.size());
}
fs::path PathFromUtf8(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
struct Tag { std::uint8_t type; Bytes payload; };
using Compound = std::map<Bytes, Tag>;
struct Document { Bytes name; Compound root; };
constexpr std::array<std::string_view, 13> Fields = {
    "Pos", "Rotation", "Dimension", "Inventory", "EnderItems", "XpLevel",
    "XpP", "XpTotal", "Score", "playerGameType", "Health", "foodLevel", "foodSaturationLevel"};

[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(reason); }
void CheckCancelled(std::stop_token token) {
    if (token.stop_requested()) Fail("Player preservation cancelled.");
}
std::uint8_t Byte(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(bytes[offset]);
}
std::uint32_t U32(std::string_view bytes, std::size_t offset = 0) {
    return (std::uint32_t(Byte(bytes, offset)) << 24) | (std::uint32_t(Byte(bytes, offset + 1)) << 16)
        | (std::uint32_t(Byte(bytes, offset + 2)) << 8) | Byte(bytes, offset + 3);
}
void Append16(Bytes& bytes, std::size_t value) {
    if (value > 65535) Fail("NBT string exceeds the unsigned 16-bit length limit.");
    bytes.push_back(static_cast<char>(value >> 8)); bytes.push_back(static_cast<char>(value));
}
// Java DataInput uses modified UTF-8, including surrogate code units and C0 80
// for NUL. Reject noncanonical encodings so duplicate compound names cannot be
// hidden behind overlong encodings. Bytes are otherwise preserved, not decoded.
void ValidateText(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = Byte(text, i++);
        if (first >= 1 && first <= 0x7f) continue;
        if (first == 0xc0 && i < text.size() && Byte(text, i) == 0x80) { ++i; continue; }
        if (first >= 0xc2 && first <= 0xdf && i < text.size()
            && (Byte(text, i) & 0xc0) == 0x80) { ++i; continue; }
        if (first >= 0xe0 && first <= 0xef && i + 1 < text.size()
            && (Byte(text, i) & 0xc0) == 0x80 && (Byte(text, i + 1) & 0xc0) == 0x80
            && (first != 0xe0 || Byte(text, i) >= 0xa0)) { i += 2; continue; }
        Fail("Invalid or noncanonical NBT modified UTF-8.");
    }
}

// A validating cursor, not a permissive search for byte patterns. It parses all
// 12 Java tags, checks every length before advancing, and keeps only direct
// compound fields. Nested payloads retain their exact bytes for rollback.
class Cursor {
public:
    explicit Cursor(std::string_view data) : bytes(data) {
        if (bytes.size() > MaximumFileBytes) Fail("Decoded NBT exceeds the per-file limit.");
    }
    std::uint8_t Next() { Need(1); return Byte(bytes, position++); }
    std::string_view Take(std::size_t count) { Need(count); const auto result = bytes.substr(position, count); position += count; return result; }
    Bytes Text() {
        const auto high = Next(); const auto low = Next();
        auto text = Take((std::size_t(high) << 8) | low); ValidateText(text); return Bytes(text);
    }
    std::size_t Count() {
        const auto value = U32(Take(4));
        if (value > 0x7fffffffU) Fail("Negative NBT array or list length.");
        return value;
    }
    Compound FieldsAt(std::size_t depth) {
        Compound result;
        ScanCompound(depth, &result);
        return result;
    }
    void Finished() const { if (position != bytes.size()) Fail("Trailing data after the NBT root compound."); }
private:
    std::string_view bytes;
    std::size_t position = 0;
    std::size_t tags = 0;
    void Need(std::size_t count) const { if (count > bytes.size() - position) Fail("Truncated NBT payload."); }
    void ScanCompound(std::size_t depth, Compound* fields = nullptr) {
        if (depth > MaximumDepth) Fail("NBT nesting exceeds the depth limit.");
        std::set<Bytes> names;
        for (;;) {
            const auto type = Next();
            if (type == 0) return;
            if (type > 12) Fail("Unknown NBT tag type.");
            auto name = Text();
            if (!names.insert(name).second) Fail("Duplicate NBT compound field.");
            const auto start = position;
            Scan(type, depth + 1);
            if (fields) fields->emplace(std::move(name), Tag{type, Bytes(bytes.substr(start, position - start))});
        }
    }
    void Scan(std::uint8_t type, std::size_t depth) {
        if (depth > MaximumDepth || ++tags > MaximumTags) Fail("NBT structure exceeds the depth or tag limit.");
        switch (type) {
        case 1: Take(1); break;
        case 2: Take(2); break;
        case 3: case 5: Take(4); break;
        case 4: case 6: Take(8); break;
        case 7: case 11: case 12: {
            const auto count = Count(); const std::size_t width = type == 7 ? 1 : type == 11 ? 4 : 8;
            if (count > (bytes.size() - position) / width) Fail("NBT array length exceeds the available payload.");
            Take(count * width); break;
        }
        case 8: Text(); break;
        case 9: {
            const auto element = Next(); const auto count = Count();
            if (element > 12 || (element == 0 && count != 0)) Fail("Invalid NBT list element type.");
            if (count > MaximumTags - tags) Fail("NBT list exceeds the tag limit.");
            for (std::size_t index = 0; index < count; ++index) Scan(element, depth + 1);
            break;
        }
        case 10: ScanCompound(depth); break;
        default: Fail("Unknown NBT tag type.");
        }
    }
};
Compound AsCompound(const Tag& tag) {
    if (tag.type != 10) Fail("Expected an NBT compound.");
    Cursor cursor(tag.payload); auto result = cursor.FieldsAt(0); cursor.Finished(); return result;
}
const Tag* Find(const Compound& compound, std::string_view key) {
    const auto found = compound.find(Bytes(key)); return found == compound.end() ? nullptr : &found->second;
}
Bytes EncodeCompound(const Compound& compound) {
    Bytes result;
    for (const auto& [name, tag] : compound) {
        if (result.size() > MaximumFileBytes - 4 || tag.payload.size() > MaximumFileBytes - result.size() - 4
            || name.size() > MaximumFileBytes - result.size() - 4 - tag.payload.size())
            Fail("Serialized NBT exceeds the per-file limit.");
        result.push_back(static_cast<char>(tag.type)); Append16(result, name.size()); result += name; result += tag.payload;
    }
    result.push_back('\0'); return result;
}
Document DecodeDocument(std::string_view bytes) {
    Cursor cursor(bytes);
    if (cursor.Next() != 10) Fail("Java NBT root must be a named compound.");
    auto name = cursor.Text(); auto root = cursor.FieldsAt(0); cursor.Finished(); return {std::move(name), std::move(root)};
}
Bytes EncodeDocument(const Document& document) {
    Bytes result(1, '\x0a'); Append16(result, document.name.size()); result += document.name; result += EncodeCompound(document.root);
    if (result.size() > MaximumFileBytes) Fail("Serialized NBT exceeds the per-file limit.");
    return result;
}
Compound Data(const Document& document) {
    const auto data = Find(document.root, "Data");
    if (!data) Fail("World level.dat has no Data compound.");
    return AsCompound(*data);
}
void Overlay(Compound& target, const Compound& source) {
    for (const auto name : Fields) if (const auto field = Find(source, name)) target[Bytes(name)] = *field;
}
void ValidateFields(const Compound& player) {
    for (const auto name : Fields) {
        const auto field = Find(player, name); if (!field) continue;
        const auto type = field->type; bool valid = false;
        if (name == "Pos" || name == "Rotation") valid = type == 9 && field->payload.size() >= 5
            && Byte(field->payload, 0) == (name == "Pos" ? 6 : 5) && U32(field->payload, 1) == (name == "Pos" ? 3 : 2);
        else if (name == "Inventory" || name == "EnderItems") valid = type == 9 && field->payload.size() >= 5
            && (Byte(field->payload, 0) == 10 || (Byte(field->payload, 0) == 0 && U32(field->payload, 1) == 0));
        else if (name == "Dimension") valid = type == 8 || type == 3;
        else if (name == "Health") valid = type == 5 || type == 2;
        else if (name == "XpP" || name == "foodSaturationLevel") valid = type == 5;
        else valid = type == 3;
        if (!valid) throw std::runtime_error("Invalid player field: " + Bytes(name));
    }
}
Bytes LowerAscii(Bytes value) {
    for (auto& byte : value) if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    return value;
}
Bytes NormalizeUuid(std::string_view uuid) {
    if (uuid.size() != 36) Fail("Invalid player filename UUID.");
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (uuid[i] != '-') Fail("Invalid player filename UUID."); }
        else if (!((uuid[i] >= '0' && uuid[i] <= '9') || (uuid[i] >= 'a' && uuid[i] <= 'f') || (uuid[i] >= 'A' && uuid[i] <= 'F')))
            Fail("Invalid player filename UUID.");
    }
    return LowerAscii(Bytes(uuid));
}
Bytes FormatUuid(std::string_view bytes) {
    if (bytes.size() != 16) Fail("Invalid UUID byte length.");
    constexpr char Hex[] = "0123456789abcdef"; Bytes result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result.push_back('-');
        result.push_back(Hex[Byte(bytes, i) >> 4]); result.push_back(Hex[Byte(bytes, i) & 15]);
    }
    return result;
}
Bytes DecodeUuid(const Tag& tag) {
    if (tag.type != 11 || tag.payload.size() != 20 || U32(tag.payload) != 4) Fail("Invalid UUID int-array.");
    return FormatUuid(std::string_view(tag.payload).substr(4));
}
std::optional<Bytes> ReadUuid(const Compound& player) {
    std::optional<Bytes> modern, legacy;
    if (const auto uuid = Find(player, "UUID")) modern = DecodeUuid(*uuid);
    const auto most = Find(player, "UUIDMost"), least = Find(player, "UUIDLeast");
    if (most || least) {
        if (!most || !least || most->type != 4 || least->type != 4) Fail("Invalid legacy player UUID.");
        legacy = FormatUuid(most->payload + least->payload);
    }
    if (modern && legacy && *modern != *legacy) Fail("Conflicting modern and legacy player UUID.");
    return modern ? modern : legacy;
}
void ValidatePlayer(const Compound& player, const Bytes& expected) {
    const auto identity = ReadUuid(player);
    if (identity && *identity != expected) Fail("Player NBT UUID differs from its filename.");
    ValidateFields(player);
}
void RejectLink(const fs::path& path) {
    const auto status = fs::symlink_status(path);
    if (fs::is_symlink(status)) Fail("Player preservation refuses symlinks.");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        Fail("Player preservation refuses reparse points.");
#endif
}
void ValidateRoot(const fs::path& root) {
    const auto absolute = fs::absolute(root); fs::path path;
    for (const auto& component : absolute) {
        if (component == "..") Fail("Player preservation roots must not contain parent traversal.");
        path /= component; RejectLink(path);
    }
    if (!fs::is_directory(absolute)) Fail("Player preservation requires an existing directory view.");
}
using Inventory = std::set<Bytes>;
Inventory InventoryOf(const fs::path& root, std::stop_token token) {
    ValidateRoot(root); Inventory result; std::set<Bytes> folded; std::size_t pathBytes = 0, entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        CheckCancelled(token); RejectLink(entry.path());
        if (++entries > 1000000) Fail("World inventory exceeds the entry limit.");
        const auto relative = Utf8Path(entry.path().lexically_relative(root));
        pathBytes += relative.size();
        if (pathBytes > MaximumTotalBytes) Fail("World inventory exceeds the path-byte limit.");
        if (!folded.insert(LowerAscii(relative)).second) Fail("Case-ambiguous world inventory.");
        if (entry.is_regular_file()) result.insert(relative);
        else if (!entry.is_directory()) Fail("Unsupported file type in world inventory.");
    }
    return result;
}
Bytes ReadBounded(const fs::path& path, std::size_t maximum = MaximumFileBytes) {
    RejectLink(path);
    if (!fs::is_regular_file(path)) Fail("NBT source is not a regular file.");
    const auto size = fs::file_size(path);
    if (size > maximum) Fail("Player preservation input exceeds the file limit.");
    std::ifstream input(path, std::ios::binary); if (!input) Fail("Cannot read player preservation input.");
    Bytes result(static_cast<std::size_t>(size), '\0');
    if (!input.read(result.data(), static_cast<std::streamsize>(result.size())) || input.peek() != std::char_traits<char>::eof())
        Fail("Player preservation input changed or could not be read completely.");
    return result;
}
Bytes Trim(Bytes text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == Bytes::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
void ValidateRelative(std::string_view relative) {
    if (relative.empty() || relative.front() == '/' || relative.find(':') != Bytes::npos || relative.find('\\') != Bytes::npos
        || relative.find('\0') != Bytes::npos) Fail("Unsafe player preservation relative path.");
    std::size_t start = 0;
    for (;;) {
        const auto end = relative.find('/', start); const auto part = relative.substr(start, end == Bytes::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") Fail("Unsafe player preservation relative path.");
        if (end == Bytes::npos) break;
        start = end + 1;
    }
}
Bytes ResolveLevel(const Inventory& inventory, const fs::path& root) {
    if (inventory.contains("level.dat")) return "level.dat";
    if (inventory.contains("server.properties")) {
        auto properties = ReadBounded(root / "server.properties", 1024 * 1024);
        if (properties.starts_with("\xef\xbb\xbf")) properties.erase(0, 3);
        std::optional<Bytes> configured;
        std::size_t start = 0;
        while (start < properties.size()) {
            auto end = properties.find('\n', start); if (end == Bytes::npos) end = properties.size();
            auto line = Trim(properties.substr(start, end - start)); start = end + 1;
            if (line.empty() || line[0] == '#' || line[0] == '!') continue;
            const auto equals = line.find('=');
            if (equals == Bytes::npos || LowerAscii(Trim(line.substr(0, equals))) != "level-name") continue;
            if (configured) Fail("Ambiguous server level-name.");
            auto name = Trim(line.substr(equals + 1)); std::replace(name.begin(), name.end(), '\\', '/');
            ValidateRelative(name); configured = name + "/level.dat";
        }
        if (configured) {
            if (!inventory.contains(*configured)) Fail("Configured server world is outside the managed inventory.");
            return *configured;
        }
    }
    if (inventory.contains("world/level.dat")) return "world/level.dat";
    std::optional<Bytes> found;
    for (const auto& path : inventory) if (path.ends_with("/level.dat")) {
        if (found) Fail("World root cannot be uniquely resolved.");
        found = path;
    }
    if (!found) Fail("World level.dat is missing.");
    return *found;
}
bool ModernLayout(const Compound& data, const Inventory& inventory, const Bytes& prefix) {
    bool legacy = false, modern = false;
    for (const auto& path : inventory) {
        if (!path.starts_with(prefix)) continue;
        const auto relative = path.substr(prefix.size()), folded = LowerAscii(relative);
        if ((folded.starts_with("playerdata/") && !relative.starts_with("playerdata/"))
            || (folded.starts_with("players/data/") && !relative.starts_with("players/data/")))
            Fail("Noncanonical player storage directory casing.");
        legacy |= relative.starts_with("playerdata/"); modern |= relative.starts_with("players/data/");
    }
    const auto version = Find(data, "DataVersion");
    if (version && version->type != 3) Fail("Invalid world DataVersion.");
    bool namedModern = version && U32(version->payload) <= 0x7fffffffU && U32(version->payload) >= 4786;
    bool knownLegacy = version && (U32(version->payload) <= 4671 || U32(version->payload) > 0x7fffffffU);
    if (const auto versionTag = Find(data, "Version")) {
        const auto versionData = AsCompound(*versionTag);
        if (const auto nameTag = Find(versionData, "Name")) {
            if (nameTag->type != 8) Fail("Invalid world Version.Name.");
            Cursor cursor(nameTag->payload); const auto name = cursor.Text(); cursor.Finished();
            knownLegacy |= name.starts_with("1.");
            const auto dot = name.find('.'); const auto major = name.substr(0, dot);
            unsigned value = 0; bool numeric = !major.empty();
            for (const auto c : major) {
                if (c < '0' || c > '9' || value > 100000) { numeric = false; break; }
                value = value * 10 + static_cast<unsigned>(c - '0');
            }
            namedModern |= numeric && value >= 26;
        }
    }
    const bool embedded = Find(data, "Player"), reference = Find(data, "singleplayer_uuid");
    if ((knownLegacy && (modern || reference || namedModern)) || ((legacy || embedded) && (modern || reference || namedModern)))
        Fail("Conflicting Minecraft player storage layout evidence.");
    return modern || reference || namedModern;
}
using PlayerIndex = std::map<Bytes, Bytes>;
PlayerIndex IndexPlayers(const Inventory& inventory, const Bytes& directory) {
    PlayerIndex result;
    for (const auto& path : inventory) if (path.starts_with(directory)) {
        const auto name = path.substr(directory.size());
        if (name.find('/') != Bytes::npos) continue;
        if (LowerAscii(name).ends_with(".dat") && !name.ends_with(".dat")) Fail("Noncanonical player filename extension casing.");
        if (!name.ends_with(".dat")) continue;
        const auto uuid = NormalizeUuid(std::string_view(name).substr(0, name.size() - 4));
        if (!result.emplace(uuid, path).second) Fail("Duplicate player filename UUID.");
        if (result.size() > MaximumProposals) Fail("Player count exceeds the preservation limit.");
    }
    return result;
}

class Codec {
public:
    Codec(const ArchiveRunner& runner, std::stop_token token, bool lowPriority)
        : runner_(runner), token_(token), lowPriority_(lowPriority) {}
    ~Codec() { if (!temporary_.empty()) { std::error_code ignored; fs::remove_all(temporary_, ignored); } }
    Document Read(const fs::path& path) {
        CheckCancelled(token_); auto content = ReadBounded(path); Bytes decoded;
        if (content.size() > MaximumTotalBytes - inputBytes_) Fail("NBT input exceeds the aggregate preservation limit.");
        inputBytes_ += content.size();
        if (content.size() >= 2 && Byte(content, 0) == 0x1f && Byte(content, 1) == 0x8b) {
            if (content.size() < 18 || Byte(content, 2) != 8 || (Byte(content, 3) & 0xe0)) Fail("Invalid gzip NBT header.");
            const auto end = content.size() - 4;
            const auto size = std::uint32_t(Byte(content, end)) | (std::uint32_t(Byte(content, end + 1)) << 8)
                | (std::uint32_t(Byte(content, end + 2)) << 16) | (std::uint32_t(Byte(content, end + 3)) << 24);
            if (size > MaximumFileBytes) Fail("Gzip NBT exceeds the decoded file limit.");
            const auto result = runner_.Execute({L"x", L"-tgzip", L"-so", L"--", path.wstring()}, {}, lowPriority_,
                MaximumFileBytes + 1, std::chrono::seconds(30));
            CheckCancelled(token_);
            if (result.status != ProcessStatus::Succeeded || result.outputTruncated || result.standardOutput.size() != size)
                Fail("Invalid, truncated, oversized, or unsupported gzip NBT input.");
            decoded = result.standardOutput;
        } else if (!content.empty() && Byte(content, 0) == 10) decoded = std::move(content);
        else Fail("Unsupported NBT compression; only raw and gzip Java NBT are supported.");
        if (decoded.size() > MaximumTotalBytes - decodedBytes_) Fail("Decoded NBT exceeds the aggregate preservation limit.");
        decodedBytes_ += decoded.size(); return DecodeDocument(decoded);
    }
    Bytes Write(const Document& document) {
        CheckCancelled(token_); const auto bytes = EncodeDocument(document);
        if (temporary_.empty()) {
            // Atomic directory creation avoids colliding with another restore.
            const auto base = fs::canonical(fs::temp_directory_path()); ValidateRoot(base);
            std::random_device random;
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                const auto candidate = base / ("MineBackup-NBT-" + std::to_string(random()) + "-" + std::to_string(random()));
                if (fs::create_directory(candidate)) { temporary_ = candidate; break; }
            }
            if (temporary_.empty()) Fail("Cannot allocate NBT compression staging directory.");
            fs::permissions(temporary_, fs::perms::owner_all, fs::perm_options::replace);
        }
        const auto input = temporary_ / "player.nbt";
        { std::ofstream output(input, std::ios::binary | std::ios::trunc);
          if (!output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !(output.close(), output))
              Fail("Cannot write NBT compression staging input."); }
        const auto result = runner_.Execute({L"a", L"-tgzip", L"-so", L"-an", L"--", input.wstring()}, {}, lowPriority_,
            MaximumFileBytes + 65536, std::chrono::seconds(30));
        CheckCancelled(token_);
        if (result.status != ProcessStatus::Succeeded || result.outputTruncated || result.standardOutput.size() < 18
            || result.standardOutput.size() > MaximumFileBytes
            || Byte(result.standardOutput, 0) != 0x1f || Byte(result.standardOutput, 1) != 0x8b)
            Fail("Cannot produce complete gzip-compressed player NBT.");
        return result.standardOutput;
    }
private:
    const ArchiveRunner& runner_; std::stop_token token_; bool lowPriority_;
    std::size_t inputBytes_ = 0, decodedBytes_ = 0; fs::path temporary_;
};
void CheckDestination(const fs::path& root, const fs::path& relative) {
    const auto text = Utf8Path(relative); ValidateRelative(text);
    fs::path candidate = root;
    for (const auto& component : relative) {
        candidate /= component; RejectLink(candidate);
        if (candidate != root / relative && fs::exists(candidate) && !fs::is_directory(candidate))
            Fail("Player preservation destination ancestor is not a directory.");
    }
    if (fs::exists(candidate)) {
        if (!fs::is_regular_file(candidate)) Fail("Player preservation destination is not a regular file.");
        // Truncating a hardlinked staging file would mutate its other aliases,
        // which could be outside the disposable restore tree.
        if (fs::hard_link_count(candidate) != 1) Fail("Player preservation destination must not have hardlink aliases.");
    }
}
} // namespace

bool Prepare(const fs::path& currentRoot, const fs::path& targetRoot, const ArchiveRunner& runner,
    std::vector<Proposal>& proposals, std::string& error, std::stop_token stopToken, bool lowPriority) {
    proposals.clear(); error.clear();
    try {
        const auto currentInventory = InventoryOf(currentRoot, stopToken);
        const auto targetInventory = InventoryOf(targetRoot, stopToken);
        const auto levelPath = ResolveLevel(currentInventory, currentRoot);
        if (!targetInventory.contains(levelPath)) Fail("Restore target does not contain the same world's level.dat.");
        if (ResolveLevel(targetInventory, targetRoot) != levelPath) Fail("Target and current server world roots conflict.");
        const auto prefix = levelPath.substr(0, levelPath.size() - 9);
        Codec codec(runner, stopToken, lowPriority);
        auto currentLevel = codec.Read(currentRoot / PathFromUtf8(levelPath)), targetLevel = codec.Read(targetRoot / PathFromUtf8(levelPath));
        const auto currentData = Data(currentLevel); auto targetData = Data(targetLevel);
        const auto modern = ModernLayout(currentData, currentInventory, prefix);
        if (modern != ModernLayout(targetData, targetInventory, prefix)) Fail("Player preservation cannot cross the Minecraft 26.1 layout boundary.");
        const auto directory = prefix + (modern ? "players/data/" : "playerdata/");
        const auto currentPlayers = IndexPlayers(currentInventory, directory), targetPlayers = IndexPlayers(targetInventory, directory);
        std::optional<Compound> inlinePlayer; std::optional<Bytes> inlineUuid;
        if (!modern) if (const auto player = Find(currentData, "Player")) {
            inlinePlayer = AsCompound(*player); inlineUuid = ReadUuid(*inlinePlayer); ValidateFields(*inlinePlayer);
        }
        if (const auto player = Find(targetData, "Player")) { const auto compound = AsCompound(*player); ReadUuid(compound); ValidateFields(compound); }
        if (modern) {
            if (const auto single = Find(currentData, "singleplayer_uuid"))
                if (!currentPlayers.contains(DecodeUuid(*single))) Fail("Current singleplayer_uuid does not reference an available player file.");
            if (const auto single = Find(targetData, "singleplayer_uuid")) {
                const auto uuid = DecodeUuid(*single);
                if (!targetPlayers.contains(uuid) && !currentPlayers.contains(uuid)) Fail("Target singleplayer_uuid does not reference an available player file.");
            }
        }
        // Validate target-only players too: a corrupt backup must not become a
        // partially successful preservation operation.
        for (const auto& [uuid, path] : targetPlayers) if (!currentPlayers.contains(uuid) && uuid != inlineUuid) {
            const auto player = codec.Read(targetRoot / PathFromUtf8(path)); ValidatePlayer(player.root, uuid);
        }
        std::map<Bytes, Proposal> prepared; std::size_t total = 0;
        auto add = [&](const Bytes& path, const Document& document) {
            CheckCancelled(stopToken); CheckDestination(targetRoot, PathFromUtf8(path));
            auto content = codec.Write(document); const auto key = LowerAscii(path);
            if (const auto prior = prepared.find(key); prior != prepared.end()) total -= prior->second.content.size();
            else if (prepared.size() >= MaximumProposals) Fail("Player preservation exceeds the proposal limit.");
            if (content.size() > MaximumTotalBytes - total) Fail("Player preservation exceeds the aggregate output limit.");
            total += content.size(); prepared[key] = {PathFromUtf8(path), std::move(content)};
        };
        for (const auto& [uuid, path] : currentPlayers) {
            auto source = codec.Read(currentRoot / PathFromUtf8(path)); ValidatePlayer(source.root, uuid);
            const auto& player = inlinePlayer && inlineUuid == uuid ? *inlinePlayer : source.root;
            auto destinationPath = directory + uuid + ".dat"; Document destination;
            if (const auto existing = targetPlayers.find(uuid); existing != targetPlayers.end()) {
                destinationPath = existing->second; destination = codec.Read(targetRoot / PathFromUtf8(destinationPath));
                ValidatePlayer(destination.root, uuid); Overlay(destination.root, player);
            } else destination = {"", player};
            add(destinationPath, destination);
        }
        bool levelChanged = false;
        if (inlinePlayer) {
            auto destination = Find(targetData, "Player");
            if (!destination) targetData["Player"] = {10, EncodeCompound(*inlinePlayer)};
            else {
                auto targetInline = AsCompound(*destination);
                if (inlineUuid && ReadUuid(targetInline) != inlineUuid) targetInline = *inlinePlayer;
                else Overlay(targetInline, *inlinePlayer);
                targetData["Player"] = {10, EncodeCompound(targetInline)};
            }
            levelChanged = true;
            if (inlineUuid) {
                const auto already = currentPlayers.contains(*inlineUuid);
                if (!already) {
                    auto path = directory + *inlineUuid + ".dat"; Document file;
                    if (const auto existing = targetPlayers.find(*inlineUuid); existing != targetPlayers.end()) {
                        path = existing->second; file = codec.Read(targetRoot / PathFromUtf8(path)); ValidatePlayer(file.root, *inlineUuid); Overlay(file.root, *inlinePlayer);
                    } else file = {"", *inlinePlayer};
                    add(path, file);
                }
            }
        }
        if (modern) if (const auto single = Find(currentData, "singleplayer_uuid")) { targetData["singleplayer_uuid"] = *single; levelChanged = true; }
        if (levelChanged) { targetLevel.root["Data"] = {10, EncodeCompound(targetData)}; add(levelPath, targetLevel); }
        for (auto& [key, proposal] : prepared) proposals.push_back(std::move(proposal));
        return true;
    } catch (const std::exception& exception) { proposals.clear(); error = exception.what(); return false; }
}

bool ApplyToStaging(const std::vector<Proposal>& proposals, const fs::path& stagingRoot,
    std::string& error, std::stop_token stopToken) {
    error.clear();
    try {
        ValidateRoot(stagingRoot); std::set<Bytes> seen; std::size_t total = 0;
        if (proposals.size() > MaximumProposals) Fail("Player preservation exceeds the proposal limit.");
        for (const auto& proposal : proposals) {
            CheckCancelled(stopToken); CheckDestination(stagingRoot, proposal.relativePath);
            if (!seen.insert(LowerAscii(Utf8Path(proposal.relativePath))).second) Fail("Duplicate player preservation destination.");
            if (proposal.content.size() > MaximumTotalBytes - total) Fail("Player preservation exceeds the aggregate output limit.");
            total += proposal.content.size();
        }
        for (const auto& proposal : proposals) {
            CheckCancelled(stopToken); CheckDestination(stagingRoot, proposal.relativePath);
            const auto destination = stagingRoot / proposal.relativePath; fs::create_directories(destination.parent_path());
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            if (!output.write(proposal.content.data(), static_cast<std::streamsize>(proposal.content.size())) || !(output.close(), output))
                Fail("Cannot write preserved player NBT into the staging tree.");
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
} // namespace PlayerDataPreservation
