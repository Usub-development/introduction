#ifndef LOGGINGUTILS_H
#define LOGGINGUTILS_H

#include <source_location>
#include <string>

inline std::string make_location_string(const std::source_location& loc = std::source_location::current()) {
    using namespace std::string_literals;
    return std::string(loc.file_name()) + "(" +
           std::to_string(loc.line()) + ":" +
           std::to_string(loc.column()) + ") `" +
           loc.function_name() + "`";
}

#endif //LOGGINGUTILS_H
