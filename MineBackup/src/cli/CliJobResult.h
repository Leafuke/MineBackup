#pragma once

#include "JobModels.h"
#include "json.hpp"

#include <cstddef>

inline constexpr std::size_t kMaximumJobDiagnosticBytes = 2u * 1024u * 1024u;
inline constexpr std::size_t kMaximumSingleJobDiagnosticDetailBytes =
	256u * 1024u;
inline constexpr std::size_t kMaximumSingleJobDiagnosticEventIdBytes = 4u * 1024u;

nlohmann::json BuildJobRunData(const JobRunResult& run);
