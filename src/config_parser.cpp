#include "config_parser.h"

#include <cstddef>
#include <sstream>

namespace config {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string stripComment(const std::string& line) {
    bool quoted = false;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] == '"') {
            quoted = !quoted;
        } else if (!quoted && line[i] == '/' && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

bool parseAssignment(const std::string& rawLine, std::string& name, std::string& value) {
    const std::string line = trim(stripComment(rawLine));
    if (line.empty()) return false;

    std::istringstream input(line);
    if (!(input >> name)) return false;
    std::string remainder;
    std::getline(input, remainder);
    remainder = trim(std::move(remainder));
    if (remainder.empty()) return false;
    if (remainder.front() == '"') {
        const auto closing = remainder.find('"', 1);
        if (closing == std::string::npos) return false;
        value = remainder.substr(1, closing - 1);
        if (!trim(remainder.substr(closing + 1)).empty()) return false;
    } else {
        const auto whitespace = remainder.find_first_of(" \t");
        value = whitespace == std::string::npos
            ? remainder : remainder.substr(0, whitespace);
    }
    return !value.empty();
}

} // namespace config
