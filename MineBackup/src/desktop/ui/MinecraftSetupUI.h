#pragma once

#include "BatchReadinessService.h"

const char* MinecraftReadinessIssueLabel(const ReadinessIssue& issue);
void DrawMinecraftReadinessIssues(const BatchReadinessResult& readiness);
