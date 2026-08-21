#pragma once

#include "CliTypes.h"

#include <cstddef>

inline constexpr std::size_t kMaximumControlCliEnvelopeBytes =
	3u * 1024u * 1024u;

nlohmann::json BuildCliEnvelope(const CliResult& result);
std::string SerializeCliEnvelope(const CliResult& result);
std::string SerializeCliEnvelopeForControlChannel(const CliResult& result);
bool ParseCliEnvelope(const std::string& payload, CliResult& result);
void RenderCliResult(const CliResult& result, bool jsonOutput);
