#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <sonata/core/luau/datafile.hpp>
#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Parser.h"

namespace sonata::luau {

namespace {

[[noreturn]] void fail(const std::string& message)
{
    throw DataFileError(message);
}

std::string astString(const Luau::AstArray<char>& value)
{
    return std::string(value.data, value.size);
}

std::string formatNumber(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    return buf;
}

bool isIdentifier(const std::string& name)
{
    if (name.empty())
        return false;

    if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
        return false;

    for (char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    }

    return true;
}

void appendQuoted(std::string& out, const std::string& text)
{
    out += '"';

    for (char c : text)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(
                        buf,
                        sizeof(buf),
                        "\\%d",
                        static_cast<int>(static_cast<unsigned char>(c))
                    );
                    out += buf;
                }
                else
                {
                    out += c;
                }
        }
    }

    out += '"';
}

DataValue evaluateExpr(Luau::AstExpr* expr);

DataValue evaluateTable(Luau::AstExprTable* table)
{
    DataValue::Entries entries;
    entries.reserve(table->items.size);

    for (std::size_t i = 0; i < table->items.size; ++i)
    {
        const auto& item = table->items.data[i];

        DataValue::Entry entry;
        entry.value = evaluateExpr(item.value);

        if (item.kind != Luau::AstExprTable::Item::Kind::List)
        {
            Luau::AstExpr* keyExpr = item.key;

            if (auto* keyString = keyExpr->as<Luau::AstExprConstantString>())
            {
                entry.key = DataValue::Key::ofString(astString(keyString->value));
            }
            else if (auto* keyNumber = keyExpr->as<Luau::AstExprConstantNumber>())
            {
                entry.key = DataValue::Key::ofNumber(keyNumber->value);
            }
            else
            {
                fail(
                    "table keys must be literal strings or numbers, e.g. "
                    "foo = ..., [\"foo\"] = ..., [1] = ..."
                );
            }
        }

        entries.push_back(std::move(entry));
    }

    return DataValue(std::move(entries));
}

DataValue evaluateExpr(Luau::AstExpr* expr)
{
    // Parens only affect multret truncation, which doesn't matter here
    while (auto* group = expr->as<Luau::AstExprGroup>())
        expr = group->expr;

    if (expr->is<Luau::AstExprConstantNil>())
        return DataValue();

    if (auto* boolean = expr->as<Luau::AstExprConstantBool>())
        return DataValue(boolean->value);

    if (auto* number = expr->as<Luau::AstExprConstantNumber>())
        return DataValue(number->value);

    if (auto* string = expr->as<Luau::AstExprConstantString>())
        return DataValue(astString(string->value));

    if (auto* table = expr->as<Luau::AstExprTable>())
        return evaluateTable(table);

    if (auto* unary = expr->as<Luau::AstExprUnary>())
    {
        if (unary->op == Luau::AstExprUnary::Op::Minus)
        {
            DataValue operand = evaluateExpr(unary->expr);

            if (!operand.isNumber())
                fail("unary '-' can only be applied to a number literal");

            return DataValue(-operand.asNumber());
        }

        fail("only unary '-' is supported in data files (not 'not' or '#')");
    }

    fail(
        "unsupported expression in data file: only nil, true/false, numbers, "
        "strings, and table constructors made of those are allowed "
        "(no variables, calls, or operators other than unary '-')"
    );
}

} // namespace


// DataValue::Key
DataValue::Key DataValue::Key::ofString(std::string value)
{
    Key key;
    key.kind = Kind::String;
    key.string = std::move(value);
    return key;
}

DataValue::Key DataValue::Key::ofNumber(double value)
{
    Key key;
    key.kind = Kind::Number;
    key.number = value;
    return key;
}


// DataValue
DataValue::DataValue()
    : kind_(Kind::Nil), data_(std::monostate{})
{
}

DataValue::DataValue(bool value)
    : kind_(Kind::Boolean), data_(value)
{
}

DataValue::DataValue(double value)
    : kind_(Kind::Number), data_(value)
{
}

DataValue::DataValue(std::string value)
    : kind_(Kind::String), data_(std::move(value))
{
}

DataValue::DataValue(const char* value)
    : DataValue(std::string(value))
{
}

DataValue::DataValue(Entries entries)
    : kind_(Kind::Table), data_(std::move(entries))
{
}

DataValue DataValue::table()
{
    return DataValue(Entries{});
}

DataValue DataValue::array(std::vector<DataValue> values)
{
    Entries entries;
    entries.reserve(values.size());

    for (auto& v : values)
        entries.push_back(Entry{std::nullopt, std::move(v)});

    return DataValue(std::move(entries));
}

DataValue::Kind DataValue::kind() const noexcept { return kind_; }

bool DataValue::isNil() const noexcept     { return kind_ == Kind::Nil; }
bool DataValue::isBoolean() const noexcept { return kind_ == Kind::Boolean; }
bool DataValue::isNumber() const noexcept  { return kind_ == Kind::Number; }
bool DataValue::isString() const noexcept  { return kind_ == Kind::String; }
bool DataValue::isTable() const noexcept   { return kind_ == Kind::Table; }

bool DataValue::isArray() const noexcept
{
    if (kind_ != Kind::Table)
        return false;

    for (const auto& entry : std::get<Entries>(data_))
        if (entry.key)
            return false;

    return true;
}

bool DataValue::asBoolean() const
{
    return std::get<bool>(data_);
}

double DataValue::asNumber() const
{
    return std::get<double>(data_);
}

const std::string& DataValue::asString() const
{
    return std::get<std::string>(data_);
}

const DataValue::Entries& DataValue::asTable() const
{
    return std::get<Entries>(data_);
}

std::vector<DataValue> DataValue::items() const
{
    std::vector<DataValue> result;

    if (kind_ != Kind::Table)
        return result;

    for (const auto& entry : std::get<Entries>(data_))
        if (!entry.key)
            result.push_back(entry.value);

    return result;
}

const DataValue* DataValue::find(const std::string& key) const
{
    if (kind_ != Kind::Table)
        return nullptr;

    for (const auto& entry : std::get<Entries>(data_))
    {
        if (entry.key &&
            entry.key->kind == Key::Kind::String &&
            entry.key->string == key)
        {
            return &entry.value;
        }
    }

    return nullptr;
}

DataValue& DataValue::set(std::string key, DataValue value)
{
    if (kind_ != Kind::Table)
        throw std::logic_error("DataValue::set requires a Table value");

    auto& entries = std::get<Entries>(data_);

    for (auto& entry : entries)
    {
        if (entry.key &&
            entry.key->kind == Key::Kind::String &&
            entry.key->string == key)
        {
            entry.value = std::move(value);
            return *this;
        }
    }

    entries.push_back(Entry{Key::ofString(std::move(key)), std::move(value)});
    return *this;
}

DataValue& DataValue::push(DataValue value)
{
    if (kind_ != Kind::Table)
        throw std::logic_error("DataValue::push requires a Table value");

    std::get<Entries>(data_).push_back(Entry{std::nullopt, std::move(value)});
    return *this;
}

void DataValue::serializeTo(std::string& out, int indent) const
{
    switch (kind_)
    {
        case Kind::Nil:
            out += "nil";
            return;

        case Kind::Boolean:
            out += asBoolean() ? "true" : "false";
            return;

        case Kind::Number:
            out += formatNumber(asNumber());
            return;

        case Kind::String:
            appendQuoted(out, asString());
            return;

        case Kind::Table:
            break;
    }

    const Entries& entries = asTable();

    if (entries.empty())
    {
        out += "{}";
        return;
    }

    out += "{\n";
    const std::string pad(static_cast<std::size_t>(indent + 1) * 4, ' ');

    for (const auto& entry : entries)
    {
        out += pad;

        if (entry.key)
        {
            if (entry.key->kind == Key::Kind::String && isIdentifier(entry.key->string))
            {
                out += entry.key->string;
                out += " = ";
            }
            else if (entry.key->kind == Key::Kind::String)
            {
                out += '[';
                appendQuoted(out, entry.key->string);
                out += "] = ";
            }
            else
            {
                out += '[';
                out += formatNumber(entry.key->number);
                out += "] = ";
            }
        }

        entry.value.serializeTo(out, indent + 1);
        out += ",\n";
    }

    out += std::string(static_cast<std::size_t>(indent) * 4, ' ');
    out += "}";
}

std::string DataValue::serialize() const
{
    std::string out;
    serializeTo(out, 0);
    return out;
}

// ---------------------------------------------------------
// DataFile
// ---------------------------------------------------------

DataValue DataFile::parse(const std::string& source, const std::string& chunkName)
{
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions options;

    Luau::ParseResult result = Luau::Parser::parse(
        source.data(),
        source.size(),
        names,
        allocator,
        options
    );

    if (!result.errors.empty())
        fail(chunkName + ": " + result.errors.front().getMessage());

    Luau::AstStatBlock* root = result.root;

    if (!root || root->body.size != 1)
    {
        fail(
            chunkName +
            " must contain exactly one statement: 'return <table>' "
            "(nothing before or after it)"
        );
    }

    auto* ret = root->body.data[0]->as<Luau::AstStatReturn>();

    if (!ret)
        fail(chunkName + " must consist of a single 'return' statement");

    if (ret->list.size != 1)
        fail(chunkName + "'s return statement must return exactly one value");

    auto* tableExpr = ret->list.data[0]->as<Luau::AstExprTable>();

    if (!tableExpr)
        fail(chunkName + " must 'return' a table constructor, e.g. 'return { ... }'");

    return evaluateTable(tableExpr);
}

std::vector<std::string> DataFile::directives(const std::string& source)
{
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions options;

    const Luau::ParseResult result = Luau::Parser::parse(
        source.data(),
        source.size(),
        names,
        allocator,
        options
    );

    std::vector<std::string> found;

    for (const Luau::HotComment& comment : result.hotcomments)
    {
        if (!comment.header)
            continue;

        // "strict", "nonstrict", "optimize 2", ... -> the first word
        const std::string& content = comment.content;
        found.push_back(content.substr(0, content.find_first_of(" \t")));
    }

    return found;
}

DataValue DataFile::parseFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);

    if (!file)
        fail("could not open: " + path.string());

    std::ostringstream contents;
    contents << file.rdbuf();

    return parse(contents.str(), "=" + path.string());
}

std::string DataFile::serialize(const DataValue& value)
{
    if (!value.isTable())
        throw DataFileError("DataFile::serialize requires a top-level Table value");

    return "return " + value.serialize() + "\n";
}

void DataFile::save(const DataValue& value, const std::filesystem::path& path)
{
    std::ofstream file(path, std::ios::binary);

    if (!file)
        throw DataFileError("could not open for writing: " + path.string());

    file << serialize(value);
}

} // namespace sonata::luau