#pragma once

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

class ArchiveRunner;

// Preparation is read-only over caller-owned, quiescent world views. Apply only
// to a disposable restore staging tree, never to the live world. The caller
// owns the world lock, final transaction, and rollback on any failure.
namespace PlayerDataPreservation {
struct Proposal {
    std::filesystem::path relativePath;
    std::string content; // A complete gzip-compressed Java NBT document.
};
inline constexpr std::size_t MaximumFileBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t MaximumTotalBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t MaximumProposals = 4096;
inline constexpr std::size_t MaximumDepth = 64;
inline constexpr std::size_t MaximumTags = 262144;

// Supports raw and gzip Java NBT. Unsupported compression, duplicate names,
// malformed tags, ambiguous UUID/layout/path evidence, and symlinks fail closed.
// Limits cover input, decoded bytes, structure, inventory, and staged output.
// On failure proposals is empty; neither source tree is changed.
bool Prepare(const std::filesystem::path& currentRoot,
    const std::filesystem::path& targetRoot, const ArchiveRunner& runner,
    std::vector<Proposal>& proposals, std::string& error,
    std::stop_token stopToken = {}, bool lowPriority = false);

// All paths are validated before the first write. A write failure may leave
// this disposable staging tree partially changed; the live world is untouched.
bool ApplyToStaging(const std::vector<Proposal>& proposals,
    const std::filesystem::path& stagingRoot, std::string& error,
    std::stop_token stopToken = {});
}
