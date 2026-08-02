#include "ReadOnlyMappedFile.h"
#include "imgui.h"

#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "[FAIL] " << message << '\n';
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    using minebackup::infra::ReadOnlyMappedFile;

    std::error_code error;
    ReadOnlyMappedFile missing;
    Expect(!missing.Open(fs::path(MINEBACKUP_TEST_FONT_PATH).concat(".missing"), error)
            && error && !missing.IsOpen(),
        "mapping a missing file should fail without retaining resources");

    ReadOnlyMappedFile mapping;
    error.clear();
    Expect(mapping.Open(MINEBACKUP_TEST_FONT_PATH, error) && !error,
        "the bundled font should be memory mapped");
    Expect(mapping.IsOpen() && mapping.Data() != nullptr && mapping.Size() > 100,
        "a successful mapping should expose the complete non-empty file");

    ReadOnlyMappedFile moved(std::move(mapping));
    Expect(!mapping.IsOpen() && moved.IsOpen(),
        "move construction should transfer mapping ownership");
    ReadOnlyMappedFile assigned;
    assigned = std::move(moved);
    Expect(!moved.IsOpen() && assigned.IsOpen(),
        "move assignment should transfer mapping ownership");

    ImGui::CreateContext();
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<void*>(assigned.Data()),
        static_cast<int>(assigned.Size()),
        20.0f,
        &config);
    Expect(font != nullptr, "ImGui should accept the read-only mapped font source");

    // ImGui 1.92 retains source bytes for dynamic glyph rasterization.
    ImGui::DestroyContext();
    assigned.Close();
    assigned.Close();
    Expect(!assigned.IsOpen() && assigned.Data() == nullptr && assigned.Size() == 0,
        "closing a mapping repeatedly should be harmless");

    if (failures != 0) {
        std::cerr << failures << " font mapping test(s) failed\n";
        return 1;
    }
    std::cout << "Font mapping tests passed\n";
    return 0;
}
