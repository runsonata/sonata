#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace sonata::luau {

/*
 * DataValue is a small value tree for a *literal* Luau table: nil,
 * true/false, numbers, strings, and nested tables built out of those --
 * nothing that requires actually running Luau code (no variables, no
 * function calls, no operators besides unary '-' on a number literal).
 *
 * It's produced by parsing a .luau file whose entire body is
 *
 *     return { ... }
 *
 * (see DataFile below), but you can also build one by hand with table()/
 * array()/set()/push() -- handy for writing one back out with
 * DataFile::serialize()/save().
 *
 * This type has nothing project-specific about it; Project uses it to read
 * .sonata/project.luau, but it's just as suited to any other "config as
 * Luau" file.
 */
class DataValue {
public:
    enum class Kind {
        Nil,
        Boolean,
        Number,
        String,
        Table
    };

    /*
     * A literal table key: a name ("foo = ..." or "["foo"] = ...") or a
     * number ("[1] = ..."). Plain array-style items -- "{1, 2, 3}" -- have
     * no Key at all; see Entry::key.
     */
    struct Key {
        enum class Kind { String, Number } kind;
        std::string string;
        double number = 0.0;

        static Key ofString(std::string value);
        static Key ofNumber(double value);
    };

    // Forward-declared and defined just below the class: Entry holds a
    // DataValue by value, so it can only be completed once DataValue
    // itself is complete. std::vector (and therefore
    // std::variant<..., std::vector<Entry>>) is allowed to be a complete
    // type with an incomplete element type since C++17, which is what
    // makes this recursive definition legal despite the forward reference.
    struct Entry;
    using Entries = std::vector<Entry>;

    DataValue();                       // Nil
    DataValue(bool value);
    DataValue(double value);
    DataValue(std::string value);
    DataValue(const char* value);
    explicit DataValue(Entries entries);

    /* An empty table, ready for set()/push(). */
    static DataValue table();

    /* A table made only of positional entries: {values[0], values[1], ...}. */
    static DataValue array(std::vector<DataValue> values);

    [[nodiscard]] Kind kind() const noexcept;

    [[nodiscard]] bool isNil() const noexcept;
    [[nodiscard]] bool isBoolean() const noexcept;
    [[nodiscard]] bool isNumber() const noexcept;
    [[nodiscard]] bool isString() const noexcept;
    [[nodiscard]] bool isTable() const noexcept;

    /* True for a Table whose entries are all positional, e.g. {1, 2, 3}. */
    [[nodiscard]] bool isArray() const noexcept;

    /* Each as*() throws std::bad_variant_access if kind() doesn't match. */
    [[nodiscard]] bool asBoolean() const;
    [[nodiscard]] double asNumber() const;
    [[nodiscard]] const std::string& asString() const;
    [[nodiscard]] const Entries& asTable() const;

    /* Just the positional (array-part) values, in order. */
    [[nodiscard]] std::vector<DataValue> items() const;

    /*
     * Looks up a Table entry by string key ("foo" or "["foo"]").
     * Returns nullptr if this isn't a Table, or the key isn't present.
     */
    [[nodiscard]] const DataValue* find(const std::string& key) const;

    /* Table only; throws std::logic_error otherwise. */
    DataValue& set(std::string key, DataValue value);
    DataValue& push(DataValue value);

    /* Regenerates Luau source for just this value (no leading "return"). */
    [[nodiscard]] std::string serialize() const;

private:
    Kind kind_;
    std::variant<std::monostate, bool, double, std::string, Entries> data_;

    void serializeTo(std::string& out, int indent) const;
};

/*
 * One entry of a Table. "key" is set for named/indexed entries
 * ("foo = 1", "["foo"] = 1", "[1] = "x"") and unset for plain positional
 * entries ("{1, 2, 3}").
 */
struct DataValue::Entry {
    std::optional<DataValue::Key> key;
    DataValue value;
};

class DataFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/*
 * Reads/writes .luau files whose entire body is "return <table>".
 *
 * Deliberately doesn't touch a VM or lua_State -- it parses with Luau's own
 * Ast/Parser (the same front end the real compiler uses), so this works
 * without ever creating a lua_State, and can't run arbitrary code: only
 * literal values and table constructors are accepted.
 */
class DataFile {
public:
    /*
     * Parses "source". Throws DataFileError if it isn't exactly
     * "return <table constructor>", or if the table contains anything
     * beyond literal nil/boolean/number/string values and nested tables
     * (variables, calls, operators other than unary '-', ...).
     *
     * "chunkName" is only used for error messages.
     */
    static DataValue parse(
        const std::string& source,
        const std::string& chunkName = "=data"
    );

    static DataValue parseFile(const std::filesystem::path& path);

    /*
     * The "--!name" directives at the top of "source" (before any code), e.g.
     * { "strict" } for a file starting with "--!strict". Only the directive's
     * name is returned. Never throws for a syntax error; the directives that
     * could be read are returned.
     */
    static std::vector<std::string> directives(const std::string& source);

    /* "value" must be a Table; produces "return { ... }\n". */
    static std::string serialize(const DataValue& value);

    static void save(
        const DataValue& value,
        const std::filesystem::path& path
    );
};

} // namespace sonata::luau