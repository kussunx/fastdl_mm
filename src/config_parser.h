#pragma once

#include <string>

namespace config {

std::string trim(std::string value);

// Drops a trailing "//" comment. One inside a quoted value belongs to the
// value, so paths and URLs survive.
std::string stripComment(const std::string& line);

// Splits `name value` or `name "value"`. False for blank and comment-only
// lines, and for anything malformed.
bool parseAssignment(const std::string& rawLine, std::string& name, std::string& value);

} // namespace config
