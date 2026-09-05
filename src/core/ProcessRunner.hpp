#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ProcessRunner {

std::string executableDirectory();
std::optional<std::string> findExecutable(const std::string& name);
// Search PATH only, skipping helpers inside the current TPlay bundle.
std::optional<std::string> findSystemExecutable(const std::string& name);
int run(const std::vector<std::string>& args, std::string* output = nullptr);
int runWithCombinedOutput(const std::vector<std::string>& args, std::string* output);
bool launchDetached(const std::vector<std::string>& args);
std::string trim(std::string value);

}  // namespace ProcessRunner
