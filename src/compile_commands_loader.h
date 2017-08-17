#pragma once

#include "project.h"

#include <string>
#include <unordered_set>
#include <vector>

bool TryLoadFromCompileCommandsJson(
    std::vector<Project::Entry>* result,
    std::unordered_set<std::string>* angle_includes,
    std::unordered_set<std::string>* quote_includes,
    std::string project_directory,
    const std::vector<std::string>& extra_flags,
    const std::string& compile_commands_content);

void LoadFromDirectoryListing(
    std::vector<Project::Entry>* result,
    std::unordered_set<std::string>* angle_includes,
    std::unordered_set<std::string>* quote_includes,
    std::string project_directory,
    const std::vector<std::string>& extra_flags);