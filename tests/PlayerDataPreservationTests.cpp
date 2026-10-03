#include "PlayerDataPreservation.h"
#include "ArchiveRunner.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Bytes = std::string;
struct Tag { unsigned char type; Bytes bytes; };
using Fields = std::map<Bytes, Tag>;
const Bytes A = "00112233-4455-6677-8899-aabbccddeeff";
const Bytes B = "11112233-4455-6677-8899-aabbccddeeff";
int failures = 0;
void Expect(bool condition, const char* message) { if (!condition) { ++failures; std::cerr << "[FAIL] " << message << '\n'; } }
Bytes Be(std::uint64_t value, std::size_t width) {
    Bytes result(width, '\0');
    for (std::size_t i = 0; i < width; ++i) result[width - i - 1] = static_cast<char>(value >> (i * 8));
    return result;
}
Bytes Named(const Bytes& name, const Tag& tag) { return Bytes(1, static_cast<char>(tag.type)) + Be(name.size(), 2) + name + tag.bytes; }
Bytes Body(const Fields& fields) { Bytes result; for (const auto& [name, tag] : fields) result += Named(name, tag); return result + '\0'; }
Bytes Doc(const Fields& fields) { return Named("", {10, Body(fields)}); }
Tag Int(std::uint32_t value) { return {3, Be(value, 4)}; }
Tag Float(float value) { return {5, Be(std::bit_cast<std::uint32_t>(value), 4)}; }
Tag String(const Bytes& value) { return {8, Be(value.size(), 2) + value}; }
Tag Compound(const Fields& value) { return {10, Body(value)}; }
Tag Uuid(const Bytes& uuid) {
    Bytes data;
    for (std::size_t i = 0; i < uuid.size();) {
        if (uuid[i] == '-') { ++i; continue; }
        data.push_back(static_cast<char>(std::stoul(uuid.substr(i, 2), nullptr, 16))); i += 2;
    }
    return {11, Be(4, 4) + data};
}
Fields Player(const Bytes& uuid, int n) {
    return {{"UUID", Uuid(uuid)}, {"Pos", {9, Bytes(1, 6) + Be(3, 4) + Be(std::bit_cast<std::uint64_t>(double(n)), 8) + Be(0, 8) + Be(0, 8)}},
        {"Rotation", {9, Bytes(1, 5) + Be(2, 4) + Float(float(n)).bytes + Float(0).bytes}},
        {"Dimension", String("minecraft:overworld")}, {"Inventory", {9, Bytes(1, 10) + Be(1, 4) + Body({{"Slot", {1, Bytes(1, 0)}}, {"count", Int(n)}})}},
        {"EnderItems", {9, Bytes(1, 10) + Be(0, 4)}}, {"XpLevel", Int(n)}, {"XpP", Float(float(n) / 100)}, {"XpTotal", Int(n * 10)},
        {"Score", Int(n)}, {"playerGameType", Int(n % 4)}, {"Health", Float(float(n))}, {"foodLevel", Int(n)},
        {"foodSaturationLevel", Float(float(n))}, {"Unselected", String("rollback-" + std::to_string(n))},
        {"Nested", Compound({{"bytes", {7, Be(2, 4) + "ab"}}, {"short", {2, Be(n, 2)}},
            {"longs", {12, Be(2, 4) + Be(n, 8) + Be(n, 8)}}, {"list", {9, Bytes(1, 9) + Be(1, 4) + Bytes(1, 8) + Be(1, 4) + String("nested").bytes}}})}};
}
Fields Preserved(Fields target, const Fields& current) {
    for (const auto* name : {"Pos", "Rotation", "Dimension", "Inventory", "EnderItems", "XpLevel", "XpP", "XpTotal", "Score", "playerGameType", "Health", "foodLevel", "foodSaturationLevel"}) {
        if (const auto it = current.find(name); it != current.end()) target[name] = it->second;
    }
    return target;
}
Bytes Level(int version, Fields extra = {}) { extra["DataVersion"] = Int(version); return Doc({{"Data", Compound(extra)}}); }
void Write(const fs::path& path, const Bytes& bytes) { fs::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary) << bytes; }
Bytes Read(const fs::path& path) { std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}; }
Bytes FakeGzip(const Bytes& bytes) {
    Bytes gzip("\x1f\x8b\x08\0\0\0\0\0\0\0", 10); gzip += bytes; gzip += Bytes(4, '\0');
    for (unsigned i = 0; i < 4; ++i) gzip.push_back(static_cast<char>(bytes.size() >> (8 * i)));
    return gzip;
}
struct State { bool truncate = false; bool fail = false; bool timedOut = false; };
ArchiveRunner Runner(const fs::path& real, const std::shared_ptr<State>& state) {
    ExternalToolResolution resolution; resolution.available = true; resolution.executable = real.empty() ? fs::path("fake-7zz") : real;
    if (!real.empty()) return ArchiveRunner(resolution);
    return ArchiveRunner(resolution, {}, [state](const ProcessSpec& spec, std::stop_token token) {
        ProcessResult result; result.status = ProcessStatus::Succeeded; result.exitCode = 0;
        Expect(spec.timeout == std::chrono::seconds(30), "gzip operations have a bounded timeout");
        Expect(spec.maximumCapturedBytes <= PlayerDataPreservation::MaximumFileBytes + 65536, "gzip capture is bounded");
        if (token.stop_requested()) { result.status = ProcessStatus::Cancelled; return result; }
        if (state->fail) { result.status = ProcessStatus::ExitedWithError; return result; }
        if (state->timedOut) { result.status = ProcessStatus::TimedOut; return result; }
        const auto bytes = Read(fs::path(spec.arguments.back()));
        if (spec.arguments.front() == L"a") result.standardOutput = FakeGzip(bytes);
        else if (bytes.size() >= 18) result.standardOutput = bytes.substr(10, bytes.size() - 18);
        else result.status = ProcessStatus::ExitedWithError;
        result.outputTruncated = state->truncate;
        return result;
    });
}
Bytes Unpack(const PlayerDataPreservation::Proposal& proposal, const fs::path& temp, const fs::path& real) {
    if (real.empty()) return proposal.content.substr(10, proposal.content.size() - 18);
    Write(temp / "unpack.gz", proposal.content);
    ExternalToolResolution resolution; resolution.available = true; resolution.executable = real;
    const auto result = ArchiveRunner(resolution).Execute({L"x", L"-tgzip", L"-so", L"--", (temp / "unpack.gz").wstring()}, {}, false,
        PlayerDataPreservation::MaximumFileBytes + 1, std::chrono::seconds(30));
    Expect(result.status == ProcessStatus::Succeeded && !result.outputTruncated, "real 7-Zip decompresses emitted NBT"); return result.standardOutput;
}
}
int main(int argc, char** argv) {
    const fs::path real = argc > 1 ? fs::absolute(argv[1]) : fs::path{};
    const auto root = fs::canonical(fs::temp_directory_path()) / ("MineBackupPlayerTests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root); auto state = std::make_shared<State>(); const auto runner = Runner(real, state);
    std::vector<PlayerDataPreservation::Proposal> proposals; Bytes error; unsigned next = 0;
    auto fixture = [&]() {
        const auto base = root / std::to_string(next++); const auto current = base / "current", target = base / "target";
        Write(current / "level.dat", Level(4671)); Write(target / "level.dat", Level(4671)); return std::pair{current, target};
    };
    auto prepare = [&](const auto& paths) { return PlayerDataPreservation::Prepare(paths.first, paths.second, runner, proposals, error); };
    auto decoded = [&](const Bytes& relative) {
        for (const auto& proposal : proposals) if (proposal.relativePath.generic_string() == relative) return Unpack(proposal, root, real);
        Expect(false, "expected staged proposal exists"); return Bytes{};
    };
    auto rejected = [&](const auto& paths, const char* message) {
        Expect(!prepare(paths), message); Expect(proposals.empty() && !error.empty(), "rejection returns no partial player results");
    };
    {
        const auto paths = fixture(); const auto current = Player(A, 20), target = Player(A, 2), newcomer = Player(B, 30);
        Write(paths.first / "playerdata" / (A + ".dat"), Doc(current)); Write(paths.second / "playerdata" / (A + ".dat"), Doc(target));
        Write(paths.first / "playerdata" / (B + ".dat"), Doc(newcomer)); Write(paths.first / "playerdata" / (A + ".dat_old"), "intentionally corrupt stale copy");
        Write(paths.first / "stats" / (A + ".json"), "current stats"); Write(paths.second / "stats" / (A + ".json"), "backup stats");
        Expect(prepare(paths), error.c_str()); Expect(proposals.size() == 2, "all current offline players are staged");
        Expect(decoded("playerdata/" + A + ".dat") == Doc(Preserved(target, current)), "only all 13 selected fields survive for existing players");
        Expect(decoded("playerdata/" + B + ".dat") == Doc(newcomer), "new UUID preserves complete current compound");
        Expect(Read(paths.second / "playerdata" / (A + ".dat")) == Doc(target), "preparation never mutates target world");
        Expect(PlayerDataPreservation::ApplyToStaging(proposals, paths.second, error), error.c_str());
        Expect(Read(paths.second / "stats" / (A + ".json")) == "backup stats", "statistics roll back normally");
        // Verify the actual gzip read path, not just output encoding.
        if (!real.empty()) {
            for (const auto& proposal : proposals) Write(paths.first / proposal.relativePath, proposal.content);
            Expect(prepare(paths), error.c_str());
        }
    }
    {
        const auto paths = fixture(); const auto authoritative = Player(A, 35), stale = Player(A, 12), oldFile = Player(A, 1), oldInline = Player(A, 2);
        Write(paths.first / "level.dat", Level(4671, {{"Player", Compound(authoritative)}, {"Time", Int(900)}}));
        Write(paths.second / "level.dat", Level(4671, {{"Player", Compound(oldInline)}, {"Time", Int(100)}}));
        Write(paths.first / "playerdata" / (A + ".dat"), Doc(stale)); Write(paths.second / "playerdata" / (A + ".dat"), Doc(oldFile));
        Expect(prepare(paths), error.c_str());
        Expect(decoded("playerdata/" + A + ".dat") == Doc(Preserved(oldFile, authoritative)), "embedded legacy player is authoritative for UUID file");
        Expect(decoded("level.dat") == Level(4671, {{"Player", Compound(Preserved(oldInline, authoritative))}, {"Time", Int(100)}}), "embedded overlay leaves world time and other player state rolled back");
    }
    {
        const auto paths = fixture(); auto current = Player(A, 10); current.erase("UUID"); auto target = Player(A, 1); target.erase("UUID");
        Write(paths.first / "level.dat", Level(4671, {{"Player", Compound(current)}})); Write(paths.second / "level.dat", Level(4671, {{"Player", Compound(target)}}));
        Expect(prepare(paths), error.c_str()); Expect(proposals.size() == 1, "UUID-less legacy player stays embedded without guessed file identity");
        Expect(decoded("level.dat") == Level(4671, {{"Player", Compound(Preserved(target, current))}}), "UUID-less legacy fields are preserved");
    }
    {
        const auto paths = fixture(); auto current = Player(A, 18); auto old = Player(B, 4);
        Write(paths.first / "level.dat", Level(4786, {{"singleplayer_uuid", Uuid(A)}}));
        Write(paths.second / "level.dat", Level(4786, {{"singleplayer_uuid", Uuid(B)}, {"Time", Int(42)}}));
        Write(paths.first / "players/data" / (A + ".dat"), Doc(current)); Write(paths.second / "players/data" / (B + ".dat"), Doc(old));
        Expect(prepare(paths), error.c_str()); Expect(proposals.size() == 2, "modern newcomer plus world singleplayer reference staged");
        Expect(decoded("players/data/" + A + ".dat") == Doc(current), "26.1 players/data path is used");
        Expect(decoded("level.dat") == Level(4786, {{"singleplayer_uuid", Uuid(A)}, {"Time", Int(42)}}), "26.1 singleplayer_uuid synchronized without changing world time");
        Write(paths.first / "level.dat", Level(4786, {{"singleplayer_uuid", Uuid(B)}})); rejected(paths, "dangling singleplayer UUID rejected");
    }
    {
        const auto paths = fixture(); auto player = Player(A, 9); const auto uuid = Uuid(A).bytes.substr(4); player.erase("UUID");
        player["UUIDMost"] = {4, uuid.substr(0, 8)}; player["UUIDLeast"] = {4, uuid.substr(8)};
        Write(paths.first / "playerdata" / (A + ".dat"), Doc(player)); Expect(prepare(paths), error.c_str());
        player["UUID"] = Uuid(B); Write(paths.first / "playerdata" / (A + ".dat"), Doc(player)); rejected(paths, "modern and legacy UUID identity conflict rejected");
    }
    {
        const auto paths = fixture();
        fs::rename(paths.first / "level.dat", paths.first / "temporary"); fs::rename(paths.second / "level.dat", paths.second / "temporary");
        Write(paths.first / "server.properties", "level-name=custom/world\n"); Write(paths.second / "server.properties", "level-name=custom/world\n");
        Write(paths.first / "custom/world/level.dat", Level(4671)); Write(paths.second / "custom/world/level.dat", Level(4671));
        Write(paths.first / "custom/world/playerdata" / (A + ".dat"), Doc(Player(A, 8)));
        Expect(prepare(paths), error.c_str()); Expect(proposals.size() == 1 && proposals[0].relativePath.generic_string().starts_with("custom/world/playerdata/"), "server level-name selects nested player directory");
        Write(paths.first / "server.properties", "level-name=../outside\n"); rejected(paths, "unsafe server root rejected");
    }
    {
        const auto paths = fixture(); const auto folder = fs::path(u8"世界");
        fs::remove(paths.first / "level.dat"); fs::remove(paths.second / "level.dat");
        const auto utf8 = folder.generic_u8string(); const Bytes encoded(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        Write(paths.first / "server.properties", "\xef\xbb\xbflevel-name=" + encoded + "\n");
        Write(paths.second / "server.properties", "level-name=" + encoded + "\n");
        Write(paths.first / folder / "level.dat", Level(4671)); Write(paths.second / folder / "level.dat", Level(4671));
        Write(paths.first / folder / "playerdata" / (A + ".dat"), Doc(Player(A, 8)));
        Expect(prepare(paths), error.c_str());
        Expect(proposals.size() == 1 && proposals.front().relativePath == folder / "playerdata" / (A + ".dat"), "UTF-8 server world names and BOM round-trip portably");
    }
    {
        const auto paths = fixture(); Write(paths.first / "playerdata" / (A + ".dat"), Doc(Player(A, 9))); Write(paths.first / "playerdata" / (B + ".dat"), "broken");
        rejected(paths, "late corrupt player aborts earlier valid proposals");
        Expect(Read(paths.first / "playerdata" / (A + ".dat")) == Doc(Player(A, 9)) && !fs::exists(paths.second / "playerdata"), "failed prepare leaves both worlds untouched");
    }
    {
        const auto paths = fixture(); Write(paths.first / "playerdata" / (A + ".dat"), Doc(Player(B, 9))); rejected(paths, "filename and NBT UUID mismatch rejected");
        auto player = Player(A, 9); player["Health"] = String("bad"); Write(paths.first / "playerdata" / (A + ".dat"), Doc(player)); rejected(paths, "invalid selected field type rejected");
        Write(paths.first / "playerdata" / (A + ".dat"), Doc(Player(A, 9))); Write(paths.second / "level.dat", Level(4786)); rejected(paths, "cross-layout restore rejected");
        Write(paths.second / "level.dat", Level(4671)); Write(paths.first / "players/data" / (B + ".dat"), Doc(Player(B, 2))); rejected(paths, "mixed player layouts rejected");
    }
    {
        const auto paths = fixture(); Write(paths.first / "playerdata" / (A + ".dat"), Doc(Player(A, 9)));
        auto upper = A; for (auto& c : upper) if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        const auto upperPath = paths.first / "playerdata" / (upper + ".dat");
        if (!fs::exists(upperPath)) { Write(upperPath, Doc(Player(A, 8))); rejected(paths, "case duplicate UUID files rejected"); }
    }
    {
        const auto paths = fixture(); Write(paths.second / "playerdata" / (B + ".dat"), "broken"); rejected(paths, "corrupt target-only player rejected");
    }
    {
        const auto paths = fixture(); Write(paths.first / "PlayerData" / (A + ".dat"), Doc(Player(A, 9)));
        rejected(paths, "noncanonical player directory casing cannot silently omit players");
    }
    {
        const auto paths = fixture(); Write(paths.first / "playerdata" / (A + ".DAT"), Doc(Player(A, 9)));
        rejected(paths, "noncanonical player extension casing cannot silently omit players");
    }
    {
        const auto paths = fixture();
        const auto blob = Bytes(14 * 1024 * 1024, 'x');
        for (unsigned index = 0; index < 5; ++index) {
            auto uuid = B; uuid[0] = static_cast<char>('1' + index);
            Write(paths.second / "playerdata" / (uuid + ".dat"), Doc({{"UUID", Uuid(uuid)}, {"LargeBlob", {7, Be(blob.size(), 4) + blob}}}));
        }
        rejected(paths, "aggregate input and decoded NBT bytes are bounded");
    }
    {
        const auto paths = fixture(); const auto file = paths.first / "playerdata" / (A + ".dat");
        const std::vector<Bytes> malformed = {
            Doc({{"bad", {13, ""}}}),
            Bytes("\x0a\0\0", 3) + Named("same", Int(1)) + Named("same", Int(2)) + '\0',
            Doc({{"Nested", {10, Named("same", Int(1)) + Named("same", Int(2)) + '\0'}}}),
            Doc({{"list", {9, Bytes(1, 0) + Be(1, 4)}}}),
            Doc({{"list", {9, Bytes(1, 3) + Be(0xffffffffU, 4)}}}),
            Doc({{"array", {12, Be(0x7fffffffU, 4)}}}),
            Doc({{"list", {9, Bytes(1, 1) + Be(PlayerDataPreservation::MaximumTags + 1, 4)}}}),
            Doc(Player(A, 9)) + "trailing",
            Doc(Player(A, 9)).substr(0, 20),
            Doc({{Bytes("\xc1\x81", 2), Int(1)}})
        };
        for (const auto& bytes : malformed) { Write(file, bytes); rejected(paths, "malformed NBT rejected"); }
        const auto valid = Doc(Player(A, 9));
        for (std::size_t size = 0; size < valid.size(); ++size) {
            Write(file, valid.substr(0, size)); rejected(paths, "every truncated fixture prefix fails closed");
        }
        Tag nested = Int(1); for (std::size_t i = 0; i < PlayerDataPreservation::MaximumDepth + 1; ++i) nested = Compound({{"deep", nested}});
        Write(file, Doc({{"deep", nested}})); rejected(paths, "excessive NBT depth rejected");
        Write(file, "x"); fs::resize_file(file, PlayerDataPreservation::MaximumFileBytes + 1); rejected(paths, "oversized input rejected before allocation");
    }
    {
        const auto paths = fixture(); const auto file = paths.first / "playerdata" / (A + ".dat");
        Bytes oversized = FakeGzip(Doc(Player(A, 8)));
        const auto size = PlayerDataPreservation::MaximumFileBytes + 1;
        for (unsigned i = 0; i < 4; ++i) oversized[oversized.size() - 4 + i] = static_cast<char>(size >> (8 * i));
        Write(file, oversized); rejected(paths, "gzip trailer size is bounded before process decompression");
        if (real.empty()) {
            Write(file, FakeGzip(Doc(Player(A, 8)))); state->truncate = true; rejected(paths, "truncated gzip capture rejected"); state->truncate = false;
            state->timedOut = true; rejected(paths, "gzip timeout rejected"); state->timedOut = false;
            state->fail = true; rejected(paths, "gzip codec error rejected"); state->fail = false;
        }
    }
    {
        const auto paths = fixture(); Write(root / "outside.nbt", Doc(Player(A, 9))); fs::create_directories(paths.first / "playerdata");
        std::error_code ec; fs::create_symlink(root / "outside.nbt", paths.first / "playerdata" / (A + ".dat"), ec);
        if (!ec) rejected(paths, "player symlink rejected");
        else std::cout << "[SKIP] Symlink creation unavailable: " << ec.message() << '\n';
    }
    {
        const auto paths = fixture(); const auto outside = root / "hardlink-outside.nbt";
        const auto original = Doc(Player(A, 2)); Write(outside, original);
        fs::create_directories(paths.second / "playerdata");
        std::error_code ec; fs::create_hard_link(outside, paths.second / "playerdata" / (A + ".dat"), ec);
        if (!ec) {
            const auto level = Read(paths.second / "level.dat");
            proposals = {{"level.dat", "would overwrite"}, {fs::path("playerdata") / (A + ".dat"), "must not leak to alias"}};
            Expect(!PlayerDataPreservation::ApplyToStaging(proposals, paths.second, error), "hardlinked staged destination rejected");
            Expect(Read(outside) == original && Read(paths.second / "level.dat") == level,
                "hardlink rejection precedes every staged write and leaves external alias untouched");
            Write(paths.first / "playerdata" / (A + ".dat"), Doc(Player(A, 9)));
            rejected(paths, "preparation also refuses an aliased destination");
        } else std::cout << "[SKIP] Hardlink creation unavailable: " << ec.message() << '\n';
    }
    {
        const auto paths = fixture(); std::stop_source cancelled; cancelled.request_stop();
        Expect(!PlayerDataPreservation::Prepare(paths.first, paths.second, runner, proposals, error, cancelled.get_token()) && proposals.empty(), "cancelled preparation returns no results");
        proposals = {{"level.dat", "would overwrite"}, {"../escape", "bad"}};
        const auto original = Read(paths.second / "level.dat");
        Expect(!PlayerDataPreservation::ApplyToStaging(proposals, paths.second, error), "unsafe proposal destination rejected");
        Expect(Read(paths.second / "level.dat") == original, "all proposal paths validated before first write");
        proposals.assign(PlayerDataPreservation::MaximumProposals + 1, {"a.dat", "x"});
        Expect(!PlayerDataPreservation::ApplyToStaging(proposals, paths.second, error), "proposal count is bounded");
    }
    {
        const auto paths = fixture();
        proposals = {{"a.dat", Bytes(PlayerDataPreservation::MaximumTotalBytes / 2 + 1, 'x')},
            {"b.dat", Bytes(PlayerDataPreservation::MaximumTotalBytes / 2, 'x')}};
        Expect(!PlayerDataPreservation::ApplyToStaging(proposals, paths.second, error), "aggregate proposal byte limit rejected");
        Expect(!fs::exists(paths.second / "a.dat"), "aggregate limit checked before the first staged write");
        proposals.clear();
    }
    {
        const auto paths = fixture();
        fs::create_directories(paths.first / "playerdata");
        for (std::size_t index = 0; index <= PlayerDataPreservation::MaximumProposals; ++index) {
            auto uuid = A; const auto prefix = Be(index, 4); constexpr char hex[] = "0123456789abcdef";
            for (std::size_t i = 0; i < 4; ++i) { uuid[2 * i] = hex[static_cast<unsigned char>(prefix[i]) >> 4]; uuid[2 * i + 1] = hex[prefix[i] & 15]; }
            Write(paths.first / "playerdata" / (uuid + ".dat"), "");
        }
        rejected(paths, "player inventory count rejected before reading individual players");
    }
    if (!real.empty()) {
        const auto paths = fixture(); Write(paths.first / "playerdata" / (A + ".dat"), Doc(Player(A, 8)));
        Expect(prepare(paths), error.c_str());
        if (!proposals.empty()) {
            auto corrupt = proposals.front().content; corrupt[corrupt.size() - 8] ^= 1;
            Write(paths.first / "playerdata" / (A + ".dat"), corrupt);
            rejected(paths, "real gzip checksum corruption rejected");
        }
    }
    fs::remove_all(root);
    if (!failures) std::cout << "[PASS] Player data preservation " << (real.empty() ? "unit tests" : "real 7-Zip tests") << '\n';
    return failures ? 1 : 0;
}
