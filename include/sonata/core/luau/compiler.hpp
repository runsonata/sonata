#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

struct lua_State;

namespace sonata::luau {
class Bytecode {
public:
    using Byte = std::uint8_t;

    Bytecode() = default;

    // Construct from raw bytes.
    Bytecode(const void* data, std::size_t size);

    // Construct from Luau's std::string output.
    explicit Bytecode(const std::string& data);

    // Construct by taking ownership of a byte vector.
    explicit Bytecode(std::vector<Byte> data);

    // Access
    const Byte* data() const noexcept;
    Byte* data() noexcept;

    std::size_t size() const noexcept;
    bool empty() const noexcept;

    // Convenient conversion for Luau APIs.
    const char* luauData() const noexcept;

    // File IO
    bool save(const std::filesystem::path& path) const;
    static Bytecode load(const std::filesystem::path& path);

    // Direct access if needed.
    const std::vector<Byte>& bytes() const noexcept;
    std::vector<Byte>& bytes() noexcept;

    // Luau reports a compile error as "bytecode" that fails to load with the
    // message (":<line>: <message>"). Returns that message, or std::nullopt
    // for real bytecode.
    std::optional<std::string> compileError() const;

private:
    std::vector<Byte> m_bytes;
};

class Compiler {
public:
    // Same defaults as Luau's own (and therefore as "sn run").
    struct Options {
        int optimizationLevel = 1; // 0..2; 2 inlines functions, unrolls loops
        int debugLevel = 1;        // 0..2; 1 = line info for stack traces
    };

    Compiler() = default;
    explicit Compiler(Options options);

    // Never throws for bad source; see Bytecode::compileError().
    Bytecode compile(std::string_view source) const;

private:
    Options options_;
};
} // namespace sonata::luau