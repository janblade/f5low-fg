// recipe_json.h - the small JSON reader for recipe/i0/parts.json (objects, arrays, strings, integers; no floats, true,
// false or null: a recipe has none). Shared by pack_build.cpp and intel_file_source.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace packbuild {

struct Json {
    enum Type { Null, Num, Str, Arr, Obj } type = Null;
    long long num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;  // in file order

    const Json* Get(const char* key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string Str_(const char* key) const {
        const Json* j = Get(key);
        return j && j->type == Str ? j->str : std::string();
    }
    long long Num_(const char* key, long long def = 0) const {
        const Json* j = Get(key);
        return j && j->type == Num ? j->num : def;
    }
};

class JsonParser {
public:
    JsonParser(const char* p, size_t n) : p_(p), e_(p + n) {}
    bool Parse(Json& out) { return Value(out, 0) && (Ws(), p_ == e_); }

private:
    void Ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool String(std::string& s) {
        if (p_ >= e_ || *p_ != '"') return false;
        ++p_;
        while (p_ < e_ && *p_ != '"') {
            char c = *p_++;
            if (c == '\\') {
                if (p_ >= e_) return false;
                char x = *p_++;
                switch (x) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        if (e_ - p_ < 4) return false;
                        unsigned v = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p_++;
                            v = v * 16 + (h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                        }
                        c = v < 0x80 ? static_cast<char>(v) : '?';
                        break;
                    }
                    default: c = x;  // " \ /
                }
            }
            s += c;
        }
        if (p_ >= e_) return false;
        ++p_;
        return true;
    }
    bool Value(Json& j, int depth) {
        if (depth > 16) return false;
        Ws();
        if (p_ >= e_) return false;
        if (*p_ == '{') {
            ++p_;
            j.type = Json::Obj;
            Ws();
            if (p_ < e_ && *p_ == '}') return ++p_, true;
            for (;;) {
                Ws();
                std::string k;
                if (!String(k)) return false;
                Ws();
                if (p_ >= e_ || *p_++ != ':') return false;
                Json v;
                if (!Value(v, depth + 1)) return false;
                j.obj.emplace_back(std::move(k), std::move(v));
                Ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') { ++p_; continue; }
                return *p_++ == '}';
            }
        }
        if (*p_ == '[') {
            ++p_;
            j.type = Json::Arr;
            Ws();
            if (p_ < e_ && *p_ == ']') return ++p_, true;
            for (;;) {
                Json v;
                if (!Value(v, depth + 1)) return false;
                j.arr.push_back(std::move(v));
                Ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') { ++p_; continue; }
                return *p_++ == ']';
            }
        }
        if (*p_ == '"') {
            j.type = Json::Str;
            return String(j.str);
        }
        if (*p_ == '-' || (*p_ >= '0' && *p_ <= '9')) {
            const bool neg = *p_ == '-';
            if (neg) ++p_;
            if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
            long long v = 0;
            while (p_ < e_ && *p_ >= '0' && *p_ <= '9') v = v * 10 + (*p_++ - '0');
            j.type = Json::Num;
            j.num = neg ? -v : v;
            return true;
        }
        return false;  // true/false/null/floats: not in a recipe
    }
    const char* p_;
    const char* e_;
};

inline bool ParseJson(const std::vector<uint8_t>& b, Json& j) {
    return JsonParser(reinterpret_cast<const char*>(b.data()), b.size()).Parse(j);
}

}  // namespace packbuild
