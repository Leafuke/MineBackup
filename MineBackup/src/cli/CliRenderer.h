#pragma once

#include "CliTypes.h"

nlohmann::json BuildCliEnvelope(const CliResult& result);
std::string SerializeCliEnvelope(const CliResult& result);
bool ParseCliEnvelope(const std::string& payload, CliResult& result);
void RenderCliResult(const CliResult& result, bool jsonOutput);
