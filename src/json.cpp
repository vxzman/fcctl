#include "json.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>

namespace {

class Parser {
public:
    explicit Parser(std::string_view in) : in_(in) {}

    Json parse() {
        skip();
        Json value = parse_value();
        skip();
        if (i_ != in_.size()) {
            error("trailing data");
        }
        return value;
    }

private:
    std::string_view in_;
    std::size_t i_ = 0;

    [[noreturn]] void error(const std::string& message) const {
        throw std::runtime_error("JSON: " + message + " at offset " + std::to_string(i_));
    }

    char peek() const { return i_ < in_.size() ? in_[i_] : '\0'; }

    char get() { return i_ < in_.size() ? in_[i_++] : '\0'; }

    void skip() {
        while (std::isspace(static_cast<unsigned char>(peek())) != 0) {
            ++i_;
        }
    }

    Json parse_value() {
        skip();
        const char c = peek();
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == '"') {
            Json value;
            value.type = Json::Type::String;
            value.s = parse_string();
            return value;
        }
        if (c == 't' || c == 'f') {
            return parse_bool();
        }
        if (c == 'n') {
            return parse_null();
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            return parse_number();
        }
        error("expected a value");
    }

    Json parse_object() {
        get();
        Json value;
        value.type = Json::Type::Object;
        skip();
        if (peek() == '}') {
            get();
            return value;
        }
        while (true) {
            skip();
            if (peek() != '"') {
                error("expected an object key");
            }
            std::string key = parse_string();
            for (const auto& item : value.o) {
                if (item.first == key) {
                    error("duplicate key \"" + key + "\"");
                }
            }
            skip();
            if (get() != ':') {
                error("expected ':'");
            }
            value.o.emplace_back(std::move(key), parse_value());
            skip();
            const char c = get();
            if (c == '}') {
                return value;
            }
            if (c != ',') {
                error("expected ',' or '}'");
            }
        }
    }

    Json parse_array() {
        get();
        Json value;
        value.type = Json::Type::Array;
        skip();
        if (peek() == ']') {
            get();
            return value;
        }
        while (true) {
            value.a.push_back(parse_value());
            skip();
            const char c = get();
            if (c == ']') {
                return value;
            }
            if (c != ',') {
                error("expected ',' or ']'");
            }
        }
    }

    std::string parse_string() {
        if (get() != '"') {
            error("expected a string");
        }
        std::string out;
        while (true) {
            if (i_ >= in_.size()) {
                error("unterminated string");
            }
            const char c = get();
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (i_ >= in_.size()) {
                    error("unterminated escape");
                }
                const char e = get();
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    out.push_back(static_cast<char>(parse_hex4()));
                    break;
                default:
                    error("bad escape");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                error("raw control character in string");
            }
            out.push_back(c);
        }
    }

    // Firecracker configs only need ASCII escapes. Code points above 0xFF are rejected.
    int parse_hex4() {
        int value = 0;
        for (int n = 0; n < 4; ++n) {
            if (i_ >= in_.size()) {
                error("short unicode escape");
            }
            const char c = get();
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value += c - '0';
            } else if (c >= 'a' && c <= 'f') {
                value += c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                value += c - 'A' + 10;
            } else {
                error("bad unicode escape");
            }
        }
        if (value > 0xFF) {
            error("unicode escape above U+00FF is not supported");
        }
        return value;
    }

    Json parse_bool() {
        Json value;
        value.type = Json::Type::Bool;
        if (in_.substr(i_, 4) == "true") {
            i_ += 4;
            value.b = true;
            return value;
        }
        if (in_.substr(i_, 5) == "false") {
            i_ += 5;
            value.b = false;
            return value;
        }
        error("expected true or false");
    }

    Json parse_null() {
        if (in_.substr(i_, 4) != "null") {
            error("expected null");
        }
        i_ += 4;
        return Json{};
    }

    Json parse_number() {
        const std::size_t start = i_;
        if (peek() == '-') {
            get();
        }
        if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
            error("expected a number");
        }
        if (peek() == '0') {
            get();
        } else {
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                get();
            }
        }
        bool fractional = false;
        if (peek() == '.') {
            fractional = true;
            get();
            if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                error("expected a digit after '.'");
            }
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                get();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            fractional = true;
            get();
            if (peek() == '+' || peek() == '-') {
                get();
            }
            if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                error("expected an exponent");
            }
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                get();
            }
        }
        const std::string token(in_.substr(start, i_ - start));
        Json value;
        value.type = Json::Type::Number;
        if (!fractional) {
            errno = 0;
            char* end = nullptr;
            const long long parsed = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                value.is_int = true;
                value.i = parsed;
                value.d = static_cast<double>(parsed);
                return value;
            }
        }
        errno = 0;
        char* end = nullptr;
        value.d = std::strtod(token.c_str(), &end);
        if (errno != 0 || !end || *end != '\0') {
            error("number out of range");
        }
        return value;
    }
};

}  // namespace

const Json* Json::find(std::string_view key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& item : o) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

std::string Json::str(std::string_view key) const {
    const Json* value = find(key);
    if (value == nullptr) {
        return {};
    }
    if (value->type != Type::String) {
        throw std::runtime_error("JSON: \"" + std::string(key) + "\" is not a string");
    }
    return value->s;
}

bool Json::flag(std::string_view key, bool fallback) const {
    const Json* value = find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->type != Type::Bool) {
        throw std::runtime_error("JSON: \"" + std::string(key) + "\" is not a bool");
    }
    return value->b;
}

std::int64_t Json::integer(std::string_view key, std::int64_t fallback) const {
    const Json* value = find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->type != Type::Number || !value->is_int) {
        throw std::runtime_error("JSON: \"" + std::string(key) + "\" is not an integer");
    }
    return value->i;
}

Json parse_json(std::string_view text) {
    return Parser(text).parse();
}
