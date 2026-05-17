#pragma once
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <stdexcept>

namespace toml {

// Simple value types supported
struct Value {
    enum class Type { String, Integer, Float, Bool, Array, Table };
    Type type = Type::String;

    std::string              s;
    long long                i = 0;
    double                   f = 0.0;
    bool                     b = false;
    std::vector<Value>       arr;
    std::map<std::string, Value> tbl;

    static Value from_string(const std::string& v) {
        Value val; val.type = Type::String; val.s = v; return val;
    }
    static Value from_int(long long v) {
        Value val; val.type = Type::Integer; val.i = v; return val;
    }
    static Value from_bool(bool v) {
        Value val; val.type = Type::Bool; val.b = v; return val;
    }

    std::string as_string() const {
        if (type == Type::String)  return s;
        if (type == Type::Integer) return std::to_string(i);
        if (type == Type::Bool)    return b ? "true" : "false";
        return "";
    }

    bool exists() const { return type != Type::String || !s.empty() || type != Type::String; }
};

using Table = std::map<std::string, Value>;

// Parse a TOML file and return a nested Table
Table parse_file(const std::string& path);
Table parse_string(const std::string& content);

// Convenience getters (return empty/default on missing key)
std::string        get_string (const Table& t, const std::string& key, const std::string& def = "");
long long          get_int    (const Table& t, const std::string& key, long long def = 0);
bool               get_bool   (const Table& t, const std::string& key, bool def = false);
std::vector<std::string> get_string_array(const Table& t, const std::string& key);

// Access nested table: get_table(t, "package") returns t["package"].tbl
const Table* get_subtable(const Table& t, const std::string& key);

} // namespace toml
