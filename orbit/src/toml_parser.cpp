#include "toml_parser.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace toml {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string strip_comment(const std::string& s) {
    bool in_q = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '"' && (i == 0 || s[i-1] != '\\')) in_q = !in_q;
        if (!in_q && s[i] == '#') return s.substr(0, i);
    }
    return s;
}

static std::string parse_string_value(const std::string& raw) {
    std::string r = trim(raw);
    if (r.size() >= 2 && r.front() == '"' && r.back() == '"')
        return r.substr(1, r.size() - 2);
    if (r.size() >= 2 && r.front() == '\'' && r.back() == '\'')
        return r.substr(1, r.size() - 2);
    return r;
}

// Forward declare so parse_inline_table and parse_array can be mutually aware
static Value parse_value(const std::string& raw_val);

// Parse inline table: { key = "val", key2 = 123 }
static Value parse_inline_table(const std::string& raw) {
    Value val;
    val.type = Value::Type::Table;

    std::string inner = trim(raw);
    // Strip outer braces
    if (!inner.empty() && inner.front() == '{') inner = inner.substr(1);
    if (!inner.empty() && inner.back()  == '}') inner = inner.substr(0, inner.size() - 1);
    inner = trim(inner);
    if (inner.empty()) return val;

    // Split by comma respecting quotes and nested braces/brackets
    std::vector<std::string> pairs;
    int depth = 0;
    bool in_q = false;
    std::string cur;
    for (size_t i = 0; i < inner.size(); i++) {
        char c = inner[i];
        if (c == '"' && (i == 0 || inner[i-1] != '\\')) in_q = !in_q;
        if (!in_q) {
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') depth--;
            else if (c == ',' && depth == 0) {
                pairs.push_back(trim(cur));
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    if (!trim(cur).empty()) pairs.push_back(trim(cur));

    for (const auto& pair : pairs) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(pair.substr(0, eq));
        std::string v = trim(pair.substr(eq + 1));
        // Strip quotes from key
        if (k.size() >= 2 && k.front() == '"' && k.back() == '"')
            k = k.substr(1, k.size() - 2);
        val.tbl[k] = parse_value(v);
    }
    return val;
}

static Value parse_array(const std::string& raw) {
    Value val;
    val.type = Value::Type::Array;
    std::string inner = trim(raw);
    if (inner.front() == '[') inner = inner.substr(1);
    if (inner.back()  == ']') inner = inner.substr(0, inner.size() - 1);
    inner = trim(inner);
    if (inner.empty()) return val;

    std::vector<std::string> items;
    int depth = 0;
    bool in_q = false;
    std::string cur;
    for (size_t i = 0; i < inner.size(); i++) {
        char c = inner[i];
        if (c == '"' && (i == 0 || inner[i-1] != '\\')) in_q = !in_q;
        if (!in_q) {
            if (c == '[' || c == '{') depth++;
            else if (c == ']' || c == '}') depth--;
            else if (c == ',' && depth == 0) {
                items.push_back(trim(cur));
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    if (!trim(cur).empty()) items.push_back(trim(cur));

    for (const auto& item : items) {
        std::string t = trim(item);
        if (t.empty()) continue;
        val.arr.push_back(parse_value(t));
    }
    return val;
}

// Parse a single TOML value from a raw string
static Value parse_value(const std::string& raw_val) {
    std::string r = trim(raw_val);
    if (r.empty())                              return Value::from_string("");
    if (r == "true")                            return Value::from_bool(true);
    if (r == "false")                           return Value::from_bool(false);
    if (r.front() == '{')                       return parse_inline_table(r);
    if (r.front() == '[')                       return parse_array(r);
    if (r.front() == '"' || r.front() == '\'') return Value::from_string(parse_string_value(r));
    // Try integer
    try {
        size_t pos;
        long long i = std::stoll(r, &pos);
        if (pos == r.size()) return Value::from_int(i);
    } catch (...) {}
    return Value::from_string(r);
}

Table parse_string(const std::string& content) {
    Table root;
    Table* current = &root;

    std::istringstream ss(content);
    std::string line;

    while (std::getline(ss, line)) {
        line = trim(strip_comment(line));
        if (line.empty()) continue;

        // Section header [section] or [[array_table]]
        if (line.front() == '[') {
            bool is_array_table = (line.size() > 1 && line[1] == '[');
            size_t a = is_array_table ? 2 : 1;
            size_t b = is_array_table ? line.rfind("]]") : line.rfind(']');
            if (b == std::string::npos) continue;
            std::string section = trim(line.substr(a, b - a));

            Table* t = &root;
            std::istringstream path_ss(section);
            std::string seg;
            while (std::getline(path_ss, seg, '.')) {
                seg = trim(seg);
                if (t->find(seg) == t->end())
                    (*t)[seg].type = Value::Type::Table;
                t = &(*t)[seg].tbl;
            }
            current = t;
            continue;
        }

        // key = value
        // Find '=' not inside quotes
        size_t eq = std::string::npos;
        {
            bool in_q = false;
            for (size_t i = 0; i < line.size(); i++) {
                if (line[i] == '"' && (i == 0 || line[i-1] != '\\')) in_q = !in_q;
                if (!in_q && line[i] == '=') { eq = i; break; }
            }
        }
        if (eq == std::string::npos) continue;

        std::string key     = trim(line.substr(0, eq));
        std::string raw_val = trim(line.substr(eq + 1));

        if (key.size() >= 2 && key.front() == '"' && key.back() == '"')
            key = key.substr(1, key.size() - 2);

        (*current)[key] = parse_value(raw_val);
    }
    return root;
}

Table parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open file: " + path);
    std::ostringstream buf;
    buf << f.rdbuf();
    return parse_string(buf.str());
}

std::string get_string(const Table& t, const std::string& key, const std::string& def) {
    auto it = t.find(key);
    if (it == t.end()) return def;
    return it->second.as_string();
}

long long get_int(const Table& t, const std::string& key, long long def) {
    auto it = t.find(key);
    if (it == t.end()) return def;
    if (it->second.type == Value::Type::Integer) return it->second.i;
    return def;
}

bool get_bool(const Table& t, const std::string& key, bool def) {
    auto it = t.find(key);
    if (it == t.end()) return def;
    if (it->second.type == Value::Type::Bool) return it->second.b;
    return def;
}

std::vector<std::string> get_string_array(const Table& t, const std::string& key) {
    std::vector<std::string> result;
    auto it = t.find(key);
    if (it == t.end()) return result;
    if (it->second.type != Value::Type::Array) return result;
    for (const auto& v : it->second.arr)
        result.push_back(v.as_string());
    return result;
}

const Table* get_subtable(const Table& t, const std::string& key) {
    auto it = t.find(key);
    if (it == t.end()) return nullptr;
    if (it->second.type != Value::Type::Table) return nullptr;
    return &it->second.tbl;
}

} // namespace toml