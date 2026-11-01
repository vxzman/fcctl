#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A JSON DOM large enough for Firecracker VM configs and the small host file.
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool b = false;
    bool is_int = false;
    std::int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<Json> a;
    // Object keys in file order. Duplicate keys are rejected by the parser.
    std::vector<std::pair<std::string, Json>> o;

    const Json* find(std::string_view key) const;
    std::string str(std::string_view key) const;
    bool flag(std::string_view key, bool fallback) const;
    std::int64_t integer(std::string_view key, std::int64_t fallback) const;
};

Json parse_json(std::string_view text);
