#include "LegacyIniConfigCodecTests.h"

#include "LegacyIniConfigCodec.h"

#include <limits>

void RunLegacyIniConfigCodecTests(TestContext& test) {
    int integer = 0;
    test.Expect(LegacyIniConfigCodec::TryParseInt(L"42", 0, 100, integer)
            && integer == 42,
        "INI integer parser should accept bounded values");
    test.Expect(!LegacyIniConfigCodec::TryParseInt(L"42x", 0, 100, integer),
        "INI integer parser should reject trailing text");
    test.Expect(!LegacyIniConfigCodec::TryParseInt(L"99999999999999999999", 0, 100, integer),
        "INI integer parser should reject overflow");
    test.Expect(!LegacyIniConfigCodec::TryParseInt(L"-1", 0, 100, integer),
        "INI integer parser should enforce ranges");

    float scale = 0.0f;
    test.Expect(LegacyIniConfigCodec::TryParseFloat(L"1.25", 0.25f, 4.0f, scale)
            && scale == 1.25f,
        "INI float parser should accept finite bounded values");
    test.Expect(!LegacyIniConfigCodec::TryParseFloat(L"nan", 0.25f, 4.0f, scale),
        "INI float parser should reject non-finite values");

    const auto tokens = LegacyIniConfigCodec::Split(L"a,b,", L',');
    test.Expect(tokens.size() == 3 && tokens[2].empty(),
        "INI delimiter parser should preserve trailing empty fields");

    std::vector<LegacyIniConfigCodec::Diagnostic> diagnostics{
        {LegacyIniConfigCodec::DiagnosticSeverity::Warning, 1, L"General", L"Theme", "warning", ""},
        {LegacyIniConfigCodec::DiagnosticSeverity::Fatal, 2, L"Config1", L"ZipLevel", "fatal", ""}};
    test.Expect(LegacyIniConfigCodec::HasFatalDiagnostics(diagnostics),
        "INI diagnostics should expose fatal parse failures");
}

