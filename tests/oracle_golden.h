#pragma once

// ============================================================================
// A minimal JSON reader for the oracle goldens.
//
// The goldens in `tools/d3_particle_oracle/golden/` were recorded by running the
// real Diablo III binary under Unicorn; this header is what lets `ctest` replay
// them on a machine with neither the emulator nor `main.elf`. That is the whole
// point of the format, so it is deliberately plain: objects, arrays, numbers,
// strings, bools, null, and nothing else.
//
// Numbers are parsed with `strtod`. Python writes a double's shortest
// round-tripping decimal, and every value in a golden is a float widened to a
// double, so `static_cast<float>(strtod(...))` recovers the original bits — which
// is what makes a bit-exact comparison possible through a text file.
// ============================================================================

#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wdx_golden {

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<ValuePtr> arr;
    std::map<std::string, ValuePtr> obj;

    bool IsNull() const { return kind == Kind::Null; }
    /// A JSON `true`/`false`. Python's `bool` lands here, not in `Num()`, so a
    /// recorded flag needs this and not `I()`.
    bool B() const {
        if (kind == Kind::Bool)
            return b;
        if (kind == Kind::Number)
            return num != 0.0;
        throw std::runtime_error("golden: expected a bool");
    }
    double Num() const {
        if (kind != Kind::Number)
            throw std::runtime_error("golden: expected a number");
        return num;
    }
    float F() const { return static_cast<float>(Num()); }
    int I() const { return static_cast<int>(Num()); }
    unsigned U() const { return static_cast<unsigned>(static_cast<long long>(Num())); }
    const std::string& S() const {
        if (kind != Kind::String)
            throw std::runtime_error("golden: expected a string");
        return str;
    }
    const std::vector<ValuePtr>& A() const {
        if (kind != Kind::Array)
            throw std::runtime_error("golden: expected an array");
        return arr;
    }
    const Value& operator[](const std::string& key) const {
        auto it = obj.find(key);
        if (it == obj.end())
            throw std::runtime_error("golden: missing key '" + key + "'");
        return *it->second;
    }
    bool Has(const std::string& key) const { return obj.find(key) != obj.end(); }
    const Value& operator[](std::size_t i) const { return *A().at(i); }
    std::size_t Size() const { return A().size(); }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    ValuePtr Parse() {
        ValuePtr v = ParseValue();
        SkipWs();
        return v;
    }

private:
    const std::string& s_;
    std::size_t p_ = 0;

    void SkipWs() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r'))
            ++p_;
    }
    [[noreturn]] void Fail(const std::string& what) const {
        throw std::runtime_error("golden: " + what + " at offset " + std::to_string(p_));
    }
    void Expect(char c) {
        SkipWs();
        if (p_ >= s_.size() || s_[p_] != c)
            Fail(std::string("expected '") + c + "'");
        ++p_;
    }

    ValuePtr ParseValue() {
        SkipWs();
        if (p_ >= s_.size())
            Fail("unexpected end");
        const char c = s_[p_];
        if (c == '{')
            return ParseObject();
        if (c == '[')
            return ParseArray();
        if (c == '"')
            return ParseString();
        if (c == 't' || c == 'f')
            return ParseBool();
        if (c == 'n') {
            if (s_.compare(p_, 4, "null") != 0)
                Fail("bad literal");
            p_ += 4;
            return std::make_shared<Value>();
        }
        return ParseNumber();
    }

    ValuePtr ParseObject() {
        auto v = std::make_shared<Value>();
        v->kind = Value::Kind::Object;
        Expect('{');
        SkipWs();
        if (p_ < s_.size() && s_[p_] == '}') {
            ++p_;
            return v;
        }
        for (;;) {
            SkipWs();
            ValuePtr key = ParseString();
            Expect(':');
            v->obj[key->str] = ParseValue();
            SkipWs();
            if (p_ < s_.size() && s_[p_] == ',') {
                ++p_;
                continue;
            }
            Expect('}');
            return v;
        }
    }

    ValuePtr ParseArray() {
        auto v = std::make_shared<Value>();
        v->kind = Value::Kind::Array;
        Expect('[');
        SkipWs();
        if (p_ < s_.size() && s_[p_] == ']') {
            ++p_;
            return v;
        }
        for (;;) {
            v->arr.push_back(ParseValue());
            SkipWs();
            if (p_ < s_.size() && s_[p_] == ',') {
                ++p_;
                continue;
            }
            Expect(']');
            return v;
        }
    }

    ValuePtr ParseString() {
        auto v = std::make_shared<Value>();
        v->kind = Value::Kind::String;
        Expect('"');
        std::string out;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\' && p_ < s_.size()) {
                const char e = s_[p_++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    // The goldens are ASCII; decode the escape and keep the low
                    // byte rather than pulling in a UTF-8 encoder.
                    if (p_ + 4 > s_.size())
                        Fail("bad \\u escape");
                    const std::string hex = s_.substr(p_, 4);
                    p_ += 4;
                    c = static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16) & 0xFF);
                    break;
                }
                default: c = e; break;
                }
            }
            out.push_back(c);
        }
        Expect('"');
        v->str = std::move(out);
        return v;
    }

    ValuePtr ParseBool() {
        auto v = std::make_shared<Value>();
        v->kind = Value::Kind::Bool;
        if (s_.compare(p_, 4, "true") == 0) {
            v->b = true;
            p_ += 4;
        } else if (s_.compare(p_, 5, "false") == 0) {
            v->b = false;
            p_ += 5;
        } else {
            Fail("bad literal");
        }
        return v;
    }

    ValuePtr ParseNumber() {
        auto v = std::make_shared<Value>();
        v->kind = Value::Kind::Number;
        const char* start = s_.c_str() + p_;
        char* end = nullptr;
        v->num = std::strtod(start, &end);
        if (end == start)
            Fail("bad number");
        p_ += static_cast<std::size_t>(end - start);
        return v;
    }
};

inline ValuePtr Load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("golden: cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    return Parser(text).Parse();
}

} // namespace wdx_golden
