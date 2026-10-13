// Pure C++ half of @sonata/serde: a value tree plus parsers/encoders for the built-in formats.
// It contains no Lua API calls, so it can be tested standalone. Every error is thrown as `Fail`.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sonata::lib::serde {

struct Fail {
    std::string msg;
};

struct Node {
    enum Type { Null, Boolean, Number, String, Array, Map } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<std::string> keys; // Map only, parallel to kids
    std::vector<Node> kids;        // Array / Map

    static Node boolean(bool v) { Node x; x.type = Boolean; x.b = v; return x; }
    static Node number(double v) { Node x; x.type = Number; x.n = v; return x; }
    static Node string(std::string v) { Node x; x.type = String; x.s = std::move(v); return x; }
    static Node array() { Node x; x.type = Array; return x; }
    static Node map() { Node x; x.type = Map; return x; }

    bool coll() const { return type == Array || type == Map; }
    const Node* find(std::string_view k) const {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return &kids[i];
        return nullptr;
    }
    Node* find(std::string_view k) { return const_cast<Node*>(std::as_const(*this).find(k)); }
    Node& add(std::string k, Node v) {
        keys.push_back(std::move(k));
        kids.push_back(std::move(v));
        return kids.back();
    }
};

enum class Fmt { Yaml, Toml, Ini, Csv };

struct Opts {
    int indent = 2;       // yaml
    bool sort = true;     // sort map keys when converting Lua -> Node
    bool dropNull = false; // omit null map entries when converting Node -> Lua
    bool header = true;   // csv: first row is a header
    int typed = -1;       // ini/csv: -1 = format default, 0 = strings only, 1 = detect numbers/bools
    char delim = ',';     // csv
};

// ---------------------------------------------------------------- shared helpers

inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && isWs(s.front())) s.remove_prefix(1);
    while (!s.empty() && isWs(s.back())) s.remove_suffix(1);
    return s;
}

inline void utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) {
        o += char(cp);
    } else if (cp < 0x800) {
        o += char(0xC0 | (cp >> 6));
        o += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += char(0xE0 | (cp >> 12));
        o += char(0x80 | ((cp >> 6) & 0x3F));
        o += char(0x80 | (cp & 0x3F));
    } else {
        o += char(0xF0 | (cp >> 18));
        o += char(0x80 | ((cp >> 12) & 0x3F));
        o += char(0x80 | ((cp >> 6) & 0x3F));
        o += char(0x80 | (cp & 0x3F));
    }
}

// `i` points just past the backslash. Appends the decoded character; false on a bad escape.
inline bool unescapeOne(std::string_view s, std::size_t& i, std::string& o) {
    if (i >= s.size()) return false;
    const char c = s[i++];
    switch (c) {
    case 'n': o += '\n'; return true;
    case 't': o += '\t'; return true;
    case 'r': o += '\r'; return true;
    case 'b': o += '\b'; return true;
    case 'f': o += '\f'; return true;
    case 'e': o += '\x1b'; return true;
    case '0': o += '\0'; return true;
    case '"': case '\'': case '\\': case '/': case ' ': o += c; return true;
    case 'x': case 'u': case 'U': {
        const std::size_t len = c == 'x' ? 2 : c == 'u' ? 4 : 8;
        if (i + len > s.size()) return false;
        unsigned cp = 0;
        for (std::size_t k = 0; k < len; ++k) {
            const char h = s[i + k];
            unsigned d;
            if (isDigit(h)) d = unsigned(h - '0');
            else if (h >= 'a' && h <= 'f') d = unsigned(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') d = unsigned(h - 'A' + 10);
            else return false;
            cp = (cp << 4) | d;
        }
        i += len;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        utf8(o, cp);
        return true;
    }
    default: return false;
    }
}

inline void escapeInto(std::string& o, std::string_view s, char q = '"') {
    for (const unsigned char c : s) {
        switch (c) {
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        case '\r': o += "\\r"; break;
        default:
            if (c == static_cast<unsigned char>(q)) {
                o += '\\';
                o += char(c);
            } else if (c < 0x20 || c == 0x7f) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04X", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
}

// dialect 0: YAML/INI/CSV (.inf/.nan, no underscores). dialect 1: TOML (inf/nan, underscores, strict digits).
inline bool parseNumber(std::string_view t, double& out, int dialect = 0) {
    const bool signedNum = !t.empty() && (t[0] == '+' || t[0] == '-');
    const bool neg = signedNum && t[0] == '-';
    if (signedNum) t.remove_prefix(1);
    if (t.empty()) return false;
    if (t == std::string_view(dialect ? "inf" : ".inf")) { out = neg ? -INFINITY : INFINITY; return true; }
    if (t == std::string_view(dialect ? "nan" : ".nan")) { out = NAN; return true; }
    const bool us = dialect == 1;
    if (!signedNum && t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'o' || t[1] == 'b')) {
        const unsigned base = t[1] == 'x' ? 16 : t[1] == 'o' ? 8 : 2;
        double v = 0;
        bool prev = false;
        for (std::size_t k = 2; k < t.size(); ++k) {
            const char ch = t[k];
            if (ch == '_' && us && prev && k + 1 < t.size()) { prev = false; continue; }
            unsigned d;
            if (isDigit(ch)) d = unsigned(ch - '0');
            else if (ch >= 'a' && ch <= 'f') d = unsigned(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') d = unsigned(ch - 'A' + 10);
            else return false;
            if (d >= base) return false;
            v = v * base + d;
            prev = true;
        }
        out = v;
        return true;
    }
    std::string c;
    std::size_t k = 0;
    const auto digits = [&] {
        bool any = false;
        while (k < t.size()) {
            const char ch = t[k];
            if (isDigit(ch)) { c += ch; any = true; ++k; }
            else if (ch == '_' && us && any && k + 1 < t.size() && isDigit(t[k + 1])) ++k;
            else break;
        }
        return any;
    };
    const bool ip = digits();
    bool fp = false;
    if (k < t.size() && t[k] == '.') {
        c += '.';
        ++k;
        fp = digits();
        if (dialect == 1 && !fp) return false;
    }
    if (!ip && !fp) return false;
    if (dialect == 1 && !ip) return false;
    if (k < t.size() && (t[k] == 'e' || t[k] == 'E')) {
        c += 'e';
        ++k;
        if (k < t.size() && (t[k] == '+' || t[k] == '-')) c += t[k++];
        if (!digits()) return false;
    }
    if (k != t.size()) return false;
    out = std::strtod(c.c_str(), nullptr);
    if (neg) out = -out;
    return true;
}

// Shortest decimal text that round-trips; integral values print without a fraction. Finite input only.
inline std::string fmtNum(double d) {
    if (d == 0) return "0";
    char buf[48];
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        std::snprintf(buf, sizeof buf, "%.0f", d);
        return buf;
    }
    for (int p = 15; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    return buf;
}

inline std::string numText(double d, bool dotted) {
    if (std::isnan(d)) return dotted ? ".nan" : "nan";
    if (std::isinf(d)) return std::string(d < 0 ? "-" : "") + (dotted ? ".inf" : "inf");
    return fmtNum(d);
}

// Plain-scalar resolution (YAML 1.2 core-ish: yes/no/on/off stay strings).
inline Node typed(std::string_view t) {
    if (t.empty() || t == "~" || t == "null" || t == "Null" || t == "NULL") return {};
    if (t == "true" || t == "True" || t == "TRUE") return Node::boolean(true);
    if (t == "false" || t == "False" || t == "FALSE") return Node::boolean(false);
    double d;
    if (parseNumber(t, d)) return Node::number(d);
    return Node::string(std::string(t));
}

// Parses a "double" or 'single' quoted scalar at t[0]; `end` is set past the closing quote.
// Returns nullptr on success, else an error message.
inline const char* quoted(std::string_view t, std::size_t& end, std::string& out) {
    const char q = t[0];
    std::size_t i = 1;
    for (;;) {
        if (i >= t.size()) return "unterminated string";
        const char c = t[i];
        if (c == q) {
            if (q == '\'' && i + 1 < t.size() && t[i + 1] == '\'') { out += '\''; i += 2; continue; }
            end = i + 1;
            return nullptr;
        }
        if (q == '"' && c == '\\') {
            ++i;
            if (!unescapeOne(t, i, out)) return "invalid escape sequence";
            continue;
        }
        out += c;
        ++i;
    }
}

// A quote only opens a string at the start of an item (after [ { , : or at the start).
inline bool opensQuote(std::string_view t, std::size_t i) {
    while (i > 0 && t[i - 1] == ' ') --i;
    return i == 0 || std::strchr("[{,:", t[i - 1]) != nullptr;
}

// Cuts a trailing comment (a mark preceded by whitespace) and trims. `qa`: skip over quoted parts.
inline std::string_view stripComment(std::string_view t, bool qa = false, const char* marks = "#") {
    char q = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (q) {
            if (q == '"' && c == '\\') ++i;
            else if (c == q) q = 0;
        } else if (qa && (c == '"' || c == '\'') && opensQuote(t, i)) {
            q = c;
        } else if (c != '\0' && std::strchr(marks, c) && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t')) {
            return trim(t.substr(0, i));
        }
    }
    return trim(t);
}

// Text cursor with line/column error reporting.
struct Src {
    std::string_view s;
    std::size_t i = 0;
    int depth = 0;
    std::size_t lineBase = 0;

    bool eof() const { return i >= s.size(); }
    char peek(std::size_t k = 0) const { return i + k < s.size() ? s[i + k] : '\0'; }
    bool starts(std::string_view p) const { return s.substr(i, p.size()) == p; }
    [[noreturn]] void fail(const std::string& m) const {
        std::size_t line = lineBase + 1, col = 1;
        for (std::size_t k = 0; k < i && k < s.size(); ++k) {
            if (s[k] == '\n') { ++line; col = 1; }
            else ++col;
        }
        throw Fail{m + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")"};
    }
};

template <class P>
struct Deep {
    P& p;
    explicit Deep(P& x) : p(x) {
        if (++p.depth > 200) { --p.depth; p.fail("nesting too deep"); }
    }
    ~Deep() { --p.depth; }
};

// ---------------------------------------------------------------- YAML (practical subset)
// Supported: block maps/sequences, flow [..]/{..}, plain/quoted scalars, | and > block scalars
// (with - and + chomping), comments, "---". Not supported (reported as errors): anchors, aliases,
// tags, merge keys, multiple documents, complex keys, multi-line plain/quoted scalars.

struct Yaml {
    struct Ln { std::size_t indent; std::string_view text; std::size_t no; };
    std::vector<Ln> ls;
    std::size_t p = 0;
    int depth = 0;

    [[noreturn]] void fail(std::size_t no, const std::string& m) const {
        throw Fail{m + " (line " + std::to_string(no) + ")"};
    }
    [[noreturn]] void fail(const std::string& m) const { fail(p < ls.size() ? ls[p].no : 0, m); }

    static bool seqItem(std::string_view t) { return !t.empty() && t[0] == '-' && (t.size() == 1 || t[1] == ' '); }

    void load(std::string_view t) {
        std::size_t no = 0, pos = t.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
        while (pos <= t.size()) {
            const std::size_t e = t.find('\n', pos);
            std::string_view line = t.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            std::size_t ind = 0;
            while (ind < line.size() && line[ind] == ' ') ++ind;
            ls.push_back({ind, line.substr(ind), ++no});
            if (e == std::string_view::npos) break;
            pos = e + 1;
        }
    }

    // Next structural line (skips blanks and comment-only lines).
    Ln* cur() {
        while (p < ls.size()) {
            Ln& l = ls[p];
            if (l.text.empty() || l.text[0] == '#') { ++p; continue; }
            if (l.text[0] == '\t') fail(l.no, "tab indentation is not allowed");
            return &l;
        }
        return nullptr;
    }

    // Is `t` a "key: ..." line? Sets `colon` to the index of the separating colon.
    static bool keyEnd(std::string_view t, std::size_t& colon) {
        if (t.empty()) return false;
        std::size_t i = 0;
        if (t[0] == '"' || t[0] == '\'') {
            const char q = t[0];
            for (i = 1; i < t.size(); ++i) {
                if (q == '"' && t[i] == '\\') { ++i; continue; }
                if (t[i] == q) {
                    if (q == '\'' && i + 1 < t.size() && t[i + 1] == '\'') { ++i; continue; }
                    break;
                }
            }
            if (i >= t.size()) return false;
            ++i;
            while (i < t.size() && t[i] == ' ') ++i;
            if (i < t.size() && t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ')) { colon = i; return true; }
            return false;
        }
        if (t[0] == '[' || t[0] == '{' || t[0] == '#' || t[0] == '|' || t[0] == '>') return false;
        for (; i < t.size(); ++i) {
            if (t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ')) { colon = i; return true; }
            if (t[i] == '#' && i > 0 && t[i - 1] == ' ') return false;
        }
        return false;
    }

    Node node(std::size_t min) {
        Deep g(*this);
        Ln* l = cur();
        if (!l || l->indent < min) return {};
        std::size_t c;
        if (seqItem(l->text)) return seq(l->indent);
        if (keyEnd(l->text, c)) return map(l->indent);
        return inlineValue(l->text, l->indent, l->no);
    }

    Node seq(std::size_t ind) {
        Node out = Node::array();
        while (Ln* l = cur()) {
            if (l->indent != ind || !seqItem(l->text)) break;
            std::string_view rest = l->text.substr(1);
            std::size_t sp = 0;
            while (sp < rest.size() && rest[sp] == ' ') ++sp;
            rest = trim(rest.substr(sp));
            const std::size_t col = ind + 1 + sp, no = l->no;
            std::size_t c;
            if (rest.empty() || rest[0] == '#') {
                ++p;
                out.kids.push_back(node(ind + 1));
            } else if (seqItem(rest) || keyEnd(rest, c)) {
                l->indent = col; // re-read the remainder as if it were its own line
                l->text = rest;
                out.kids.push_back(node(col));
            } else {
                out.kids.push_back(inlineValue(rest, ind, no));
            }
        }
        return out;
    }

    Node map(std::size_t ind) {
        Node out = Node::map();
        while (Ln* l = cur()) {
            if (l->indent != ind) break;
            std::size_t c;
            if (!keyEnd(l->text, c)) fail(l->no, "expected 'key: value'");
            const std::size_t no = l->no;
            std::string key;
            if (l->text[0] == '"' || l->text[0] == '\'') {
                std::size_t e;
                if (const char* err = quoted(l->text, e, key)) fail(no, err);
            } else {
                key = std::string(trim(l->text.substr(0, c)));
            }
            if (key == "<<") fail(no, "merge keys are not supported");
            if (out.find(key)) fail(no, "duplicate key '" + key + "'");
            const std::string_view rest = trim(l->text.substr(c + 1));
            Node v;
            if (rest.empty() || rest[0] == '#') {
                ++p;
                Ln* n = cur();
                v = (n && n->indent == ind && seqItem(n->text)) ? seq(ind) : node(ind + 1);
            } else {
                v = inlineValue(rest, ind, no);
            }
            out.add(std::move(key), std::move(v));
        }
        return out;
    }

    // `r` is the trimmed value text of line ls[p]; consumes that line (and any it spans).
    Node inlineValue(std::string_view r, std::size_t parent, std::size_t no) {
        const char c = r[0];
        if (c == '|' || c == '>') { ++p; return block(r, parent, no); }
        if (c == '&' || c == '*' || c == '!') fail(no, "anchors, aliases and tags are not supported");
        if (c == '"' || c == '\'') {
            std::string v;
            std::size_t e = 0;
            if (const char* err = quoted(r, e, v)) fail(no, err);
            if (!stripComment(r.substr(e)).empty()) fail(no, "unexpected text after quoted string");
            ++p;
            return Node::string(std::move(v));
        }
        if (c == '[' || c == '{') {
            std::string buf(stripComment(r, true));
            ++p;
            while (!flowDone(buf)) {
                if (p >= ls.size()) fail(no, "unterminated flow collection");
                buf += ' ';
                buf += stripComment(ls[p].text, true);
                ++p;
            }
            Src s{buf};
            s.lineBase = no - 1;
            Node n = flow(s);
            ws(s);
            if (!s.eof()) s.fail("unexpected text after flow collection");
            return n;
        }
        ++p;
        return typed(stripComment(r));
    }

    Node block(std::string_view h, std::size_t parent, std::size_t no) {
        const bool fold = h[0] == '>';
        char chomp = 0;
        for (std::size_t k = 1; k < h.size(); ++k) {
            if (h[k] == '-' || h[k] == '+') chomp = h[k];
            else if (h[k] == ' ' || h[k] == '#') break;
            else fail(no, "unsupported block scalar header");
        }
        std::vector<std::string> body;
        std::size_t blockInd = std::string::npos;
        while (p < ls.size()) {
            const Ln& l = ls[p];
            if (l.text.empty()) { body.emplace_back(); ++p; continue; }
            if (blockInd == std::string::npos) {
                if (l.indent <= parent) break;
                blockInd = l.indent;
            }
            if (l.indent < blockInd) break;
            body.push_back(std::string(l.indent - blockInd, ' ') + std::string(l.text));
            ++p;
        }
        std::size_t trail = 0;
        while (!body.empty() && body.back().empty()) { body.pop_back(); ++trail; }
        std::string out;
        for (std::size_t i = 0; i < body.size(); ++i) {
            const std::string& b = body[i];
            if (i) {
                if (!fold || b.empty()) out += '\n';
                else if (!body[i - 1].empty()) out += (b[0] == ' ' || body[i - 1][0] == ' ') ? '\n' : ' ';
            }
            out += b;
        }
        if (!body.empty() && chomp != '-') out += '\n';
        if (chomp == '+') out.append(trail, '\n');
        return Node::string(std::move(out));
    }

    static bool flowDone(std::string_view b) {
        int d = 0;
        char q = 0;
        for (std::size_t i = 0; i < b.size(); ++i) {
            const char c = b[i];
            if (q) {
                if (q == '"' && c == '\\') ++i;
                else if (c == q) q = 0;
            } else if ((c == '"' || c == '\'') && opensQuote(b, i)) q = c;
            else if (c == '[' || c == '{') ++d;
            else if (c == ']' || c == '}') --d;
        }
        return d <= 0 && !q;
    }

    static void ws(Src& s) { while (!s.eof() && isWs(s.peek())) ++s.i; }
    static bool colonEnds(const Src& s) { return s.peek() == ':' && (isWs(s.peek(1)) || std::strchr(",]}", s.peek(1))); }

    static std::string flowKey(Src& s) {
        ws(s);
        if (s.peek() == '"' || s.peek() == '\'') {
            std::string k;
            std::size_t e;
            if (const char* err = quoted(s.s.substr(s.i), e, k)) s.fail(err);
            s.i += e;
            return k;
        }
        const std::size_t st = s.i;
        while (!s.eof() && s.peek() != ',' && s.peek() != '}' && !colonEnds(s)) ++s.i;
        const std::string_view t = trim(s.s.substr(st, s.i - st));
        if (t.empty()) s.fail("empty key");
        return std::string(t);
    }

    Node flow(Src& s) {
        Deep g(s);
        ws(s);
        const char c = s.peek();
        if (c == '[' || c == '{') {
            const bool isSeq = c == '[';
            const char close = isSeq ? ']' : '}';
            ++s.i;
            Node out = isSeq ? Node::array() : Node::map();
            for (;;) {
                ws(s);
                if (s.peek() == close) { ++s.i; return out; }
                if (isSeq) {
                    out.kids.push_back(flow(s));
                } else {
                    std::string key = flowKey(s);
                    ws(s);
                    Node v;
                    if (s.peek() == ':') {
                        ++s.i;
                        ws(s);
                        if (s.peek() != ',' && s.peek() != '}') v = flow(s);
                    }
                    if (out.find(key)) s.fail("duplicate key '" + key + "'");
                    out.add(std::move(key), std::move(v));
                }
                ws(s);
                if (s.peek() == ',') { ++s.i; continue; }
                if (s.peek() == close) { ++s.i; return out; }
                s.fail(isSeq ? "expected ',' or ']'" : "expected ',' or '}'");
            }
        }
        if (c == '"' || c == '\'') {
            std::string v;
            std::size_t e;
            if (const char* err = quoted(s.s.substr(s.i), e, v)) s.fail(err);
            s.i += e;
            return Node::string(std::move(v));
        }
        const std::size_t st = s.i;
        while (!s.eof()) {
            const char d = s.peek();
            if (d == ',' || d == ']' || d == '}' || d == '[' || d == '{' || colonEnds(s)) break;
            ++s.i;
        }
        const std::string_view t = trim(s.s.substr(st, s.i - st));
        if (t.empty()) s.fail("unexpected character");
        if (t[0] == '&' || t[0] == '*' || t[0] == '!') s.fail("anchors, aliases and tags are not supported");
        return typed(t);
    }

    Node parse(std::string_view t) {
        load(t);
        if (Ln* l = cur(); l && l->text == "---") ++p;
        Node out = node(0);
        if (Ln* x = cur())
            fail(x->no, x->text == "---" || x->text == "..." ? "multiple documents are not supported"
                                                             : "unexpected content (bad indentation or multi-line scalar?)");
        return out;
    }
};

inline bool yamlPlain(std::string_view s) {
    if (s.empty() || s.front() == ' ' || s.back() == ' ' || std::strchr("-?:,[]{}#&*!|>'\"%@`", s[0])) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c == ':' && (i + 1 == s.size() || s[i + 1] == ' ')) return false;
        if (c == '#' && s[i - 1] == ' ') return false;
    }
    std::string low(s);
    for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* b : {"y", "n", "yes", "no", "on", "off"})
        if (low == b) return false; // YAML 1.1 readers treat these as booleans
    return typed(s).type == Node::String; // must not re-read as null/bool/number
}

inline std::string yamlScalar(const Node& n) {
    switch (n.type) {
    case Node::Null: return "null";
    case Node::Boolean: return n.b ? "true" : "false";
    case Node::Number: return numText(n.n, true);
    case Node::String: {
        if (yamlPlain(n.s)) return n.s;
        std::string o = "\"";
        escapeInto(o, n.s);
        return o + '"';
    }
    case Node::Array: return "[]";
    default: return "{}";
    }
}

// `n` is a non-empty Array/Map. A nested collection under "- " is written one level deeper and its
// first indentation column is then overwritten by the dash (needs step >= 2).
inline void yamlEmit(const Node& n, std::size_t ind, std::string& o, std::size_t step) {
    const std::string pad(ind, ' ');
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        const Node& v = n.kids[i];
        const bool nested = v.coll() && !v.kids.empty();
        if (n.type == Node::Map) {
            o += pad + yamlScalar(Node::string(n.keys[i])) + ':';
            if (nested) {
                o += '\n';
                yamlEmit(v, ind + step, o, step);
            } else {
                o += ' ' + yamlScalar(v) + '\n';
            }
        } else if (nested) {
            const std::size_t at = o.size();
            yamlEmit(v, ind + step, o, step);
            o[at + ind] = '-';
        } else {
            o += pad + "- " + yamlScalar(v) + '\n';
        }
    }
}

inline std::string yamlEncode(const Node& n, std::size_t step) {
    std::string o;
    if (n.coll() && !n.kids.empty()) yamlEmit(n, 0, o, step);
    else o = yamlScalar(n) + "\n";
    return o;
}

// ---------------------------------------------------------------- TOML 1.0
// Date/times are kept as strings. Duplicate *table headers* are not rejected (duplicate keys are).

struct Toml : Src {
    void skipWs() { while (peek() == ' ' || peek() == '\t') ++i; }
    void skipAll() {
        for (;;) {
            while (!eof() && isWs(peek())) ++i;
            if (peek() == '#') { while (!eof() && peek() != '\n') ++i; }
            else break;
        }
    }
    void endLine() {
        skipWs();
        if (peek() == '#') while (!eof() && peek() != '\n') ++i;
        if (!eof() && peek() != '\n' && !(peek() == '\r' && peek(1) == '\n')) fail("expected end of line");
    }

    std::string str() { // at an opening quote: basic/literal, single/multi-line
        const char q = peek();
        const bool multi = starts(q == '"' ? "\"\"\"" : "'''");
        i += multi ? 3 : 1;
        if (multi) {
            if (peek() == '\r' && peek(1) == '\n') i += 2;
            else if (peek() == '\n') ++i;
        }
        std::string o;
        for (;;) {
            if (eof()) fail("unterminated string");
            const char c = peek();
            if (c == q) {
                if (!multi) { ++i; return o; }
                std::size_t k = 0;
                while (peek(k) == q) ++k;
                if (k >= 3) {
                    if (k > 5) fail("too many quotes");
                    o.append(k - 3, q);
                    i += k;
                    return o;
                }
                o.append(k, q);
                i += k;
                continue;
            }
            if (c == '\n' && !multi) fail("newline in string");
            if (q == '"' && c == '\\') {
                ++i;
                if (multi && isWs(peek())) { // line-ending backslash swallows the whitespace after it
                    std::size_t j = i;
                    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                    if (j < s.size() && (s[j] == '\n' || s[j] == '\r')) {
                        i = j;
                        while (!eof() && isWs(peek())) ++i;
                        continue;
                    }
                }
                if (!unescapeOne(s, i, o)) fail("invalid escape sequence");
                continue;
            }
            o += c;
            ++i;
        }
    }

    std::string key1() {
        if (peek() == '"' || peek() == '\'') return str();
        const std::size_t st = i;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == '-')) ++i;
        if (i == st) fail("expected a key");
        return std::string(s.substr(st, i - st));
    }
    std::vector<std::string> keyPath() {
        std::vector<std::string> path;
        for (;;) {
            skipWs();
            path.push_back(key1());
            skipWs();
            if (peek() == '.') ++i;
            else return path;
        }
    }

    void assign(Node& t, const std::vector<std::string>& path, Node v) {
        Node* c = &t;
        for (std::size_t k = 0; k + 1 < path.size(); ++k) {
            Node* n = c->find(path[k]);
            if (!n) n = &c->add(path[k], Node::map());
            if (n->type != Node::Map) fail("'" + path[k] + "' is not a table");
            c = n;
        }
        if (c->find(path.back())) fail("duplicate key '" + path.back() + "'");
        c->add(path.back(), std::move(v));
    }

    Node value() {
        Deep g(*this);
        const char c = peek();
        if (c == '"' || c == '\'') return Node::string(str());
        if (c == '[') {
            ++i;
            Node a = Node::array();
            for (;;) {
                skipAll();
                if (peek() == ']') { ++i; return a; }
                a.kids.push_back(value());
                skipAll();
                if (peek() == ',') { ++i; continue; }
                if (peek() == ']') { ++i; return a; }
                fail("expected ',' or ']'");
            }
        }
        if (c == '{') {
            ++i;
            Node t = Node::map();
            for (;;) {
                skipAll();
                if (peek() == '}') { ++i; return t; }
                const auto path = keyPath();
                skipWs();
                if (peek() != '=') fail("expected '='");
                ++i;
                skipWs();
                assign(t, path, value());
                skipAll();
                if (peek() == ',') { ++i; continue; }
                if (peek() == '}') { ++i; return t; }
                fail("expected ',' or '}'");
            }
        }
        const std::size_t st = i;
        while (!eof() && !isWs(peek()) && peek() != ',' && peek() != ']' && peek() != '}' && peek() != '#') ++i;
        std::string_view t = s.substr(st, i - st);
        if (t == "true") return Node::boolean(true);
        if (t == "false") return Node::boolean(false);
        if (t.size() >= 10 && t[4] == '-' && t[7] == '-' && peek() == ' ' && isDigit(peek(1))) { // "date time"
            while (!eof() && peek() != '\n' && peek() != ',' && peek() != ']' && peek() != '}' && peek() != '#') ++i;
            t = trim(s.substr(st, i - st));
        }
        double d;
        if (parseNumber(t, d, 1)) return Node::number(d);
        if (t.size() >= 5 && isDigit(t[0]) && (t[2] == ':' || t[4] == '-')) return Node::string(std::string(t));
        fail("invalid value");
    }

    Node parse() {
        Node root = Node::map();
        Node* cur = &root;
        if (s.substr(0, 3) == "\xEF\xBB\xBF") i = 3;
        for (;;) {
            skipAll();
            if (eof()) return root;
            if (peek() == '[') {
                const bool aot = peek(1) == '[';
                i += aot ? 2 : 1;
                const auto path = keyPath();
                skipWs();
                if (peek() != ']' || (aot && peek(1) != ']')) fail("expected ']'");
                i += aot ? 2 : 1;
                endLine();
                cur = &root;
                for (std::size_t k = 0; k < path.size(); ++k) {
                    const bool last = k + 1 == path.size();
                    Node* c = cur->find(path[k]);
                    if (!c) c = &cur->add(path[k], last && aot ? Node::array() : Node::map());
                    if (last && aot) {
                        if (c->type != Node::Array) fail("'" + path[k] + "' is not an array of tables");
                        c->kids.push_back(Node::map());
                        c = &c->kids.back();
                    } else if (c->type == Node::Array && !c->kids.empty()) {
                        c = &c->kids.back(); // descend into the latest array-of-tables element
                    }
                    if (c->type != Node::Map) fail("'" + path[k] + "' is not a table");
                    cur = c;
                }
            } else {
                const auto path = keyPath();
                skipWs();
                if (peek() != '=') fail("expected '='");
                ++i;
                skipWs();
                assign(*cur, path, value());
                endLine();
            }
        }
    }
};

inline std::string tomlKey(const std::string& k) {
    bool bare = !k.empty();
    for (const unsigned char c : k)
        if (!(std::isalnum(c) || c == '_' || c == '-')) bare = false;
    if (bare) return k;
    std::string o = "\"";
    escapeInto(o, k);
    return o + '"';
}

inline void tomlValue(const Node& n, std::string& o) {
    switch (n.type) {
    case Node::Null: throw Fail{"TOML cannot represent null inside an array"};
    case Node::Boolean: o += n.b ? "true" : "false"; break;
    case Node::Number: o += numText(n.n, false); break;
    case Node::String: o += '"'; escapeInto(o, n.s); o += '"'; break;
    case Node::Array:
        o += '[';
        for (std::size_t i = 0; i < n.kids.size(); ++i) {
            if (i) o += ", ";
            tomlValue(n.kids[i], o);
        }
        o += ']';
        break;
    case Node::Map: {
        o += '{';
        bool first = true;
        for (std::size_t i = 0; i < n.kids.size(); ++i) {
            if (n.kids[i].type == Node::Null) continue;
            o += first ? " " : ", ";
            first = false;
            o += tomlKey(n.keys[i]) + " = ";
            tomlValue(n.kids[i], o);
        }
        o += first ? "}" : " }";
        break;
    }
    }
}

inline bool tomlTables(const Node& a) {
    if (a.type != Node::Array || a.kids.empty()) return false;
    for (const Node& k : a.kids)
        if (k.type != Node::Map) return false;
    return true;
}

// Writes a table's plain entries first, then its sub-tables / arrays of tables (TOML requires that order).
inline void tomlTable(const Node& t, const std::string& path, std::string& o) {
    for (std::size_t i = 0; i < t.kids.size(); ++i) {
        const Node& v = t.kids[i];
        if (v.type == Node::Null || v.type == Node::Map || tomlTables(v)) continue; // null entries are omitted
        o += tomlKey(t.keys[i]) + " = ";
        tomlValue(v, o);
        o += '\n';
    }
    for (std::size_t i = 0; i < t.kids.size(); ++i) {
        const Node& v = t.kids[i];
        const bool sub = v.type == Node::Map, aot = tomlTables(v);
        if (!sub && !aot) continue;
        const std::string full = path.empty() ? tomlKey(t.keys[i]) : path + "." + tomlKey(t.keys[i]);
        if (sub) {
            if (!o.empty()) o += '\n';
            o += "[" + full + "]\n";
            tomlTable(v, full, o);
        } else {
            for (const Node& e : v.kids) {
                if (!o.empty()) o += '\n';
                o += "[[" + full + "]]\n";
                tomlTable(e, full, o);
            }
        }
    }
}

inline std::string tomlEncode(const Node& n) {
    if (n.type == Node::Array && n.kids.empty()) return "";
    if (n.type != Node::Map) throw Fail{"TOML root must be a table"};
    std::string o;
    tomlTable(n, "", o);
    return o;
}

// ---------------------------------------------------------------- INI

inline Node iniDecode(std::string_view t, bool typedVals) {
    if (t.substr(0, 3) == "\xEF\xBB\xBF") t.remove_prefix(3);
    Node root = Node::map();
    Node* cur = &root;
    std::size_t no = 0, pos = 0;
    while (pos <= t.size()) {
        const std::size_t e = t.find('\n', pos);
        std::string_view line = t.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
        pos = e == std::string_view::npos ? t.size() + 1 : e + 1;
        ++no;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        const auto fail = [&](const std::string& m) { throw Fail{m + " (line " + std::to_string(no) + ")"}; };
        if (line[0] == '[') {
            if (line.back() != ']') fail("unterminated section header");
            const std::string name(trim(line.substr(1, line.size() - 2)));
            Node* c = root.find(name);
            if (!c) c = &root.add(name, Node::map());
            else if (c->type != Node::Map) fail("section clashes with a key");
            cur = c;
            continue;
        }
        const std::size_t eq = line.find_first_of("=:");
        if (eq == std::string_view::npos) fail("expected 'key = value'");
        std::string key(trim(line.substr(0, eq)));
        std::string_view val = trim(line.substr(eq + 1));
        if (cur->find(key)) fail("duplicate key '" + key + "'");
        Node v;
        if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
            std::string str;
            std::size_t end = 0;
            if (const char* err = quoted(val, end, str)) fail(err);
            if (!stripComment(val.substr(end), false, ";#").empty()) fail("unexpected text after quoted value");
            v = Node::string(std::move(str));
        } else {
            val = stripComment(val, false, ";#");
            v = (typedVals && !val.empty()) ? typed(val) : Node::string(std::string(val));
        }
        cur->add(std::move(key), std::move(v));
    }
    return root;
}

inline std::string iniScalar(const Node& n) {
    switch (n.type) {
    case Node::Boolean: return n.b ? "true" : "false";
    case Node::Number: return numText(n.n, true);
    case Node::String: {
        const std::string& s = n.s;
        const bool q = !s.empty() && (isWs(s.front()) || isWs(s.back()) || s.find_first_of(";#\"'\\\n\r") != std::string::npos ||
                                      typed(s).type != Node::String);
        if (!q) return s;
        std::string o = "\"";
        escapeInto(o, s);
        return o + '"';
    }
    default: throw Fail{"INI only supports scalar values and one level of sections"};
    }
}

inline std::string iniEncode(const Node& n) {
    if (n.type == Node::Array && n.kids.empty()) return "";
    if (n.type != Node::Map) throw Fail{"INI root must be a table"};
    std::string o;
    const auto entry = [&](const std::string& k, const Node& v) {
        if (v.type == Node::Null) return; // null entries are omitted
        if (k.empty() || k.find_first_of("=:;#\n\r") != std::string::npos || k[0] == '[' || trim(k) != k)
            throw Fail{"invalid INI key '" + k + "'"};
        o += k + " = " + iniScalar(v) + '\n';
    };
    const auto section = [](const Node& v) { return v.type == Node::Map || (v.type == Node::Array && v.kids.empty()); };
    for (std::size_t i = 0; i < n.kids.size(); ++i)
        if (!section(n.kids[i])) entry(n.keys[i], n.kids[i]);
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        if (!section(n.kids[i])) continue;
        if (n.keys[i].find_first_of("]\n\r") != std::string::npos) throw Fail{"invalid INI section name"};
        if (!o.empty()) o += '\n';
        o += "[" + n.keys[i] + "]\n";
        for (std::size_t j = 0; j < n.kids[i].kids.size(); ++j) entry(n.kids[i].keys[j], n.kids[i].kids[j]);
    }
    return o;
}

// ---------------------------------------------------------------- CSV / TSV (RFC 4180 quoting)

inline Node csvDecode(std::string_view t, char d, bool header, bool typedVals) {
    std::size_t i = t.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string f;
    bool inQ = false, open = false; // open: this row already has a delimiter or quoted field
    const auto endRow = [&] {
        if (open || !f.empty()) {
            row.push_back(std::move(f));
            rows.push_back(std::move(row));
        }
        row.clear();
        f.clear();
        open = false;
    };
    for (; i < t.size(); ++i) {
        const char c = t[i];
        if (inQ) {
            if (c == '"') {
                if (i + 1 < t.size() && t[i + 1] == '"') { f += '"'; ++i; }
                else inQ = false;
            } else f += c;
        } else if (c == '"' && f.empty()) {
            inQ = true;
            open = true;
        } else if (c == d) {
            row.push_back(std::move(f));
            f.clear();
            open = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < t.size() && t[i + 1] == '\n') ++i;
            endRow();
        } else f += c;
    }
    if (inQ) throw Fail{"unterminated quoted field"};
    endRow();

    const auto cell = [&](const std::string& c) {
        return typedVals && !c.empty() ? typed(c) : Node::string(c);
    };
    Node out = Node::array();
    if (!header) {
        for (const auto& r : rows) {
            Node a = Node::array();
            for (const auto& c : r) a.kids.push_back(cell(c));
            out.kids.push_back(std::move(a));
        }
        return out;
    }
    if (rows.empty()) return out;
    Node seen = Node::map();
    for (const auto& h : rows[0]) {
        if (seen.find(h)) throw Fail{"duplicate column '" + h + "'"};
        seen.add(h, {});
    }
    for (std::size_t r = 1; r < rows.size(); ++r) {
        if (rows[r].size() > rows[0].size()) throw Fail{"row " + std::to_string(r + 1) + " has more fields than the header"};
        Node m = Node::map();
        for (std::size_t k = 0; k < rows[r].size(); ++k) m.add(rows[0][k], cell(rows[r][k]));
        out.kids.push_back(std::move(m));
    }
    return out;
}

inline std::string csvEncode(const Node& n, char d, bool header) {
    if (n.type != Node::Array) throw Fail{"CSV needs an array of tables or an array of arrays"};
    const bool maps = !n.kids.empty() && n.kids[0].type == Node::Map;
    for (const Node& r : n.kids)
        if (r.type != (maps ? Node::Map : Node::Array)) throw Fail{"CSV rows must be all tables or all arrays"};
    std::vector<std::string> cols;
    if (maps)
        for (const Node& r : n.kids)
            for (const std::string& k : r.keys)
                if (std::find(cols.begin(), cols.end(), k) == cols.end()) cols.push_back(k);
    std::string o;
    bool first = true;
    const auto put = [&](const Node* v) {
        if (!first) o += d;
        first = false;
        if (!v) return;
        std::string x;
        switch (v->type) {
        case Node::Null: return;
        case Node::Boolean: x = v->b ? "true" : "false"; break;
        case Node::Number: x = numText(v->n, true); break;
        case Node::String: x = v->s; break;
        default: throw Fail{"CSV cells must be scalars"};
        }
        if (x.find_first_of(std::string("\"\r\n") + d) == std::string::npos) { o += x; return; }
        o += '"';
        for (const char c : x) {
            if (c == '"') o += '"';
            o += c;
        }
        o += '"';
    };
    if (maps && header) {
        for (const std::string& c : cols) { const Node h = Node::string(c); put(&h); }
        o += '\n';
        first = true;
    }
    for (const Node& r : n.kids) {
        first = true;
        if (maps) for (const std::string& c : cols) put(r.find(c));
        else for (const Node& v : r.kids) put(&v);
        o += '\n';
    }
    return o;
}

// ---------------------------------------------------------------- dispatch

inline Node decodeText(Fmt f, std::string_view t, const Opts& o) {
    switch (f) {
    case Fmt::Yaml: { Yaml y; return y.parse(t); }
    case Fmt::Toml: { Toml p; p.s = t; return p.parse(); }
    case Fmt::Ini: return iniDecode(t, o.typed != 0);
    default: return csvDecode(t, o.delim, o.header, o.typed == 1);
    }
}

inline std::string encodeNode(Fmt f, const Node& n, const Opts& o) {
    switch (f) {
    case Fmt::Yaml: return yamlEncode(n, static_cast<std::size_t>(o.indent));
    case Fmt::Toml: return tomlEncode(n);
    case Fmt::Ini: return iniEncode(n);
    default: return csvEncode(n, o.delim, o.header);
    }
}

} // namespace sonata::lib::serde