#pragma once
// Small, self-contained JSON value/parser, C++17. No WebView, CLR or third-party DLL.
// The model migrates legacy data and writes only the current settings schema.
#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace portable {
struct Json {
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;
    Storage value = nullptr;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool x) : value(x) {}
    Json(int x) : value(static_cast<double>(x)) {}
    Json(double x) : value(x) {}
    Json(const char* x) : value(std::string(x)) {}
    Json(const std::string& x) : value(x) {}
    Json(std::string&& x) : value(std::move(x)) {}
    Json(const Object& x) : value(x) {}
    Json(Object&& x) : value(std::move(x)) {}
    Json(const Array& x) : value(x) {}
    Json(Array&& x) : value(std::move(x)) {}
    static Json object() { return Json(Object{}); }
    static Json array() { return Json(Array{}); }

    bool isObject() const { return std::holds_alternative<Object>(value); }
    bool isArray() const { return std::holds_alternative<Array>(value); }
    Json& operator[](const std::string& key) {
        if (!isObject()) value = Object{};
        return std::get<Object>(value)[key];
    }
    const Json& get(const std::string& key) const {
        static const Json missing;
        if (!isObject()) return missing;
        const auto& obj = std::get<Object>(value);
        const auto it = obj.find(key);
        return it == obj.end() ? missing : it->second;
    }
    std::string str(const std::string& fallback = "") const {
        if (auto v = std::get_if<std::string>(&value)) return *v;
        return fallback;
    }
    bool boolean(bool fallback = false) const {
        if (auto v = std::get_if<bool>(&value)) return *v;
        return fallback;
    }
    int integer(int fallback = 0) const {
        if (auto v = std::get_if<double>(&value)) {
            if (std::isfinite(*v) && *v >= -2147483647.0 && *v <= 2147483647.0)
                return static_cast<int>(*v);
        }
        return fallback;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        }
    }

    static Json parse(const std::string& input) {
        struct Parser {
            const std::string& data;
            size_t i = 0;
            explicit Parser(const std::string& s) : data(s) {}
            void whitespace() {
                while (i < data.size() && (data[i] == ' ' || data[i] == '\n' ||
                        data[i] == '\r' || data[i] == '\t')) ++i;
            }
            bool take(char ch) {
                whitespace();
                if (i < data.size() && data[i] == ch) { ++i; return true; }
                return false;
            }
            void require(char ch) {
                if (!take(ch)) throw std::runtime_error("Invalid JSON delimiter");
            }
            uint32_t fourHex() {
                if (data.size() - i < 4) throw std::runtime_error("Invalid unicode escape");
                uint32_t cp = 0;
                for (int n = 0; n < 4; ++n) {
                    const char ch = data[i++];
                    cp <<= 4;
                    if (ch >= '0' && ch <= '9') cp |= ch - '0';
                    else if (ch >= 'a' && ch <= 'f') cp |= ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') cp |= ch - 'A' + 10;
                    else throw std::runtime_error("Invalid unicode digit");
                }
                return cp;
            }
            std::string string() {
                require('"');
                std::string out;
                while (i < data.size()) {
                    unsigned char ch = static_cast<unsigned char>(data[i++]);
                    if (ch == '"') return out;
                    if (ch < 0x20) throw std::runtime_error("Control character in JSON string");
                    if (ch != '\\') { out.push_back(static_cast<char>(ch)); continue; }
                    if (i >= data.size()) throw std::runtime_error("Invalid JSON escape");
                    switch (data[i++]) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        uint32_t cp = fourHex();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (data.size() - i < 6 || data[i] != '\\' || data[i + 1] != 'u')
                                throw std::runtime_error("Unpaired high surrogate");
                            i += 2;
                            const uint32_t low = fourHex();
                            if (low < 0xDC00 || low > 0xDFFF)
                                throw std::runtime_error("Invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            throw std::runtime_error("Unpaired low surrogate");
                        }
                        Json::appendUtf8(out, cp);
                        break;
                    }
                    default: throw std::runtime_error("Unknown JSON escape");
                    }
                }
                throw std::runtime_error("Unterminated JSON string");
            }
            Json value(unsigned depth=0) {
                if (depth>128) throw std::runtime_error("JSON nesting exceeds the supported limit");
                whitespace();
                if (i >= data.size()) throw std::runtime_error("Truncated JSON");
                if (data[i] == '"') return Json(string());
                if (take('{')) {
                    Object obj;
                    if (take('}')) return Json(std::move(obj));
                    do {
                        std::string key = string();
                        require(':');
                        obj[std::move(key)] = value(depth+1);
                        if (take('}')) return Json(std::move(obj));
                        require(',');
                    } while (true);
                }
                if (take('[')) {
                    Array arr;
                    if (take(']')) return Json(std::move(arr));
                    do {
                        arr.push_back(value(depth+1));
                        if (take(']')) return Json(std::move(arr));
                        require(',');
                    } while (true);
                }
                auto literal = [&](const char* word) {
                    const std::string w(word);
                    if (data.compare(i, w.size(), w) != 0)
                        throw std::runtime_error("Invalid JSON literal");
                    i += w.size();
                };
                if (data[i] == 't') { literal("true"); return Json(true); }
                if (data[i] == 'f') { literal("false"); return Json(false); }
                if (data[i] == 'n') { literal("null"); return Json(nullptr); }
                size_t start = i;
                if (data[i] == '-') ++i;
                if (i >= data.size()) throw std::runtime_error("Invalid JSON number");
                if (data[i] == '0') ++i;
                else {
                    if (data[i] < '1' || data[i] > '9') throw std::runtime_error("Invalid JSON number");
                    while (i < data.size() && data[i] >= '0' && data[i] <= '9') ++i;
                }
                if (i < data.size() && data[i] == '.') {
                    ++i;
                    if (i >= data.size() || data[i] < '0' || data[i] > '9')
                        throw std::runtime_error("Invalid JSON fraction");
                    while (i < data.size() && data[i] >= '0' && data[i] <= '9') ++i;
                }
                if (i < data.size() && (data[i] == 'e' || data[i] == 'E')) {
                    ++i;
                    if (i < data.size() && (data[i] == '+' || data[i] == '-')) ++i;
                    if (i >= data.size() || data[i] < '0' || data[i] > '9')
                        throw std::runtime_error("Invalid JSON exponent");
                    while (i < data.size() && data[i] >= '0' && data[i] <= '9') ++i;
                }
                const double number = std::strtod(data.c_str() + start, nullptr);
                if (!std::isfinite(number)) throw std::runtime_error("Invalid JSON floating-point value");
                return Json(number);
            }
        } parser(input);
        Json result = parser.value();
        parser.whitespace();
        if (parser.i != input.size()) throw std::runtime_error("Trailing JSON content");
        return result;
    }

    static void emitString(std::string& out, const std::string& text) {
        const char* hex = "0123456789abcdef";
        out.push_back('"');
        for (unsigned char ch : text) {
            switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[ch >> 4]);
                    out.push_back(hex[ch & 15]);
                } else out.push_back(static_cast<char>(ch));
            }
        }
        out.push_back('"');
    }
    void serialize(std::string& out) const {
        if (std::holds_alternative<std::nullptr_t>(value)) out += "null";
        else if (auto p = std::get_if<bool>(&value)) out += *p ? "true" : "false";
        else if (auto p = std::get_if<double>(&value)) {
            // All ordinary configuration integers have an identical
            // representation without a locale/stream allocation. Preserve
            // the existing floating-point path, including negative zero.
            if(*p>=-2147483648.0 && *p<=2147483647.0 &&
                std::trunc(*p)==*p && !(*p==0.0 && std::signbit(*p))) {
                char buffer[16];
                const auto result=std::to_chars(buffer,buffer+sizeof(buffer),static_cast<int>(*p));
                if(result.ec==std::errc{}) {out.append(buffer,result.ptr);return;}
            }
            std::ostringstream os;
            os.imbue(std::locale::classic());
            os << std::setprecision(15) << *p;
            out += os.str();
        } else if (auto p = std::get_if<std::string>(&value)) emitString(out, *p);
        else if (auto p = std::get_if<Object>(&value)) {
            out.push_back('{');
            bool first = true;
            for (const auto& it : *p) {
                if (!first) out.push_back(',');
                first = false;
                emitString(out, it.first);
                out.push_back(':');
                it.second.serialize(out);
            }
            out.push_back('}');
        } else if (auto p = std::get_if<Array>(&value)) {
            out.push_back('[');
            bool first = true;
            for (const auto& item : *p) {
                if (!first) out.push_back(',');
                first = false;
                item.serialize(out);
            }
            out.push_back(']');
        }
    }
    std::string dump() const {
        std::string out;
        serialize(out);
        return out;
    }
};
} // namespace portable
