#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

#include <zlib.h>

#include <sonata/core/bundle.hpp>

namespace fs = std::filesystem;

namespace sonata::bundle {

namespace {

constexpr char kTrailerMagic[8] = {'S', 'N', 'B', 'U', 'N', 'D', 'L', 'E'};
constexpr char kPayloadMagic[4] = {'S', 'N', 'P', 'K'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kTrailerSize = 32;
constexpr std::uint32_t kFlagCompressed = 1;

// Writing

void putU32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void putU64(std::string& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void putBytes(std::string& out, const void* data, std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw BundleError("an item is too large to bundle (over 4 GiB)");
    }
    putU32(out, static_cast<std::uint32_t>(size));
    out.append(static_cast<const char*>(data), size);
}

void putString(std::string& out, const std::string& value) {
    putBytes(out, value.data(), value.size());
}

void putCount(std::string& out, std::size_t count) {
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        throw BundleError("too many items to bundle");
    }
    putU32(out, static_cast<std::uint32_t>(count));
}

void putStrings(std::string& out, const std::vector<std::string>& values) {
    putCount(out, values.size());
    for (const std::string& value : values) {
        putString(out, value);
    }
}

std::string serialize(const Bundle& bundle) {
    std::string out(kPayloadMagic, sizeof(kPayloadMagic));
    putU32(out, kFormatVersion);

    putString(out, bundle.name);
    putString(out, bundle.version);
    putString(out, bundle.entry);
    putStrings(out, bundle.libraries);
    putString(out, bundle.libraryHint);

    putCount(out, bundle.aliases.size());
    for (const auto& [name, directory] : bundle.aliases) {
        putString(out, name);
        putString(out, directory);
    }

    putCount(out, bundle.packages.size());
    for (const Package& package : bundle.packages) {
        putString(out, package.root);
        putStrings(out, package.dependencies);
        putString(out, package.entrypoint);
    }

    putCount(out, bundle.modules.size());
    for (const Module& module : bundle.modules) {
        putString(out, module.path);
        putBytes(out, module.bytecode.data(), module.bytecode.size());
    }

    putCount(out, bundle.assets.size());
    for (const Asset& asset : bundle.assets) {
        putString(out, asset.package);
        putString(out, asset.path);
        putString(out, asset.data);
    }

    return out;
}

// Reading

class Reader {
public:
    explicit Reader(const std::string& data) : data_(data) {}

    std::uint32_t u32() {
        need(4);
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data_[pos_ + i])) << (8 * i);
        }
        pos_ += 4;
        return value;
    }

    std::string string() {
        const std::uint32_t size = u32();
        need(size);
        std::string value = data_.substr(pos_, size);
        pos_ += size;
        return value;
    }

    std::vector<std::string> strings() {
        const std::uint32_t count = u32();
        std::vector<std::string> values;
        for (std::uint32_t i = 0; i < count; ++i) {
            values.push_back(string());
        }
        return values;
    }

    void magic(const char* expected, std::size_t size) {
        need(size);
        if (std::memcmp(data_.data() + pos_, expected, size) != 0) {
            throw BundleError("the bundled program is damaged (bad payload header)");
        }
        pos_ += size;
    }

    bool done() const { return pos_ == data_.size(); }

private:
    void need(std::size_t size) const {
        if (data_.size() - pos_ < size) {
            throw BundleError("the bundled program is damaged (payload ends early)");
        }
    }

    const std::string& data_;
    std::size_t pos_ = 0;
};

Bundle deserialize(const std::string& payload) {
    Reader in(payload);
    in.magic(kPayloadMagic, sizeof(kPayloadMagic));

    const std::uint32_t version = in.u32();
    if (version != kFormatVersion) {
        throw BundleError("the bundled program uses format version " + std::to_string(version) +
                          ", but this runtime reads version " + std::to_string(kFormatVersion));
    }

    Bundle bundle;
    bundle.name = in.string();
    bundle.version = in.string();
    bundle.entry = in.string();
    bundle.libraries = in.strings();
    bundle.libraryHint = in.string();

    for (std::uint32_t i = 0, n = in.u32(); i < n; ++i) {
        std::string name = in.string();
        std::string directory = in.string();
        bundle.aliases.emplace_back(std::move(name), std::move(directory));
    }

    for (std::uint32_t i = 0, n = in.u32(); i < n; ++i) {
        Package package;
        package.root = in.string();
        package.dependencies = in.strings();
        package.entrypoint = in.string();
        bundle.packages.push_back(std::move(package));
    }

    for (std::uint32_t i = 0, n = in.u32(); i < n; ++i) {
        Module module;
        module.path = in.string();
        const std::string bytes = in.string();
        module.bytecode = luau::Bytecode(bytes.data(), bytes.size());
        bundle.modules.push_back(std::move(module));
    }

    for (std::uint32_t i = 0, n = in.u32(); i < n; ++i) {
        Asset asset;
        asset.package = in.string();
        asset.path = in.string();
        asset.data = in.string();
        bundle.assets.push_back(std::move(asset));
    }

    if (!in.done()) {
        throw BundleError("the bundled program is damaged (trailing data in payload)");
    }

    return bundle;
}

std::uint32_t crc(const std::string& data) {
    uLong value = crc32(0L, Z_NULL, 0);
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t chunk = std::min<std::size_t>(data.size() - pos, 1u << 30);
        value = crc32(value, reinterpret_cast<const Bytef*>(data.data() + pos), static_cast<uInt>(chunk));
        pos += chunk;
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t readU64(const unsigned char* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return value;
}

std::uint32_t readU32(const unsigned char* p) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    return value;
}

std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw BundleError("could not open " + path.generic_string());
    }

    std::string data;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0) {
        throw BundleError("could not read " + path.generic_string());
    }

    data.resize(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(data.data(), size)) {
        throw BundleError("could not read " + path.generic_string());
    }
    return data;
}

} // namespace

std::string encode(const Bundle& bundle, bool compress) {
    const std::string raw = serialize(bundle);
    std::string stored;
    std::uint32_t flags = 0;

    if (compress) {
        uLongf size = compressBound(static_cast<uLong>(raw.size()));
        stored.resize(size);
        const int status = compress2(reinterpret_cast<Bytef*>(stored.data()), &size,
                                     reinterpret_cast<const Bytef*>(raw.data()),
                                     static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION);
        if (status != Z_OK) {
            throw BundleError("could not compress the program (zlib error " + std::to_string(status) + ")");
        }
        stored.resize(size);
        flags |= kFlagCompressed;
    } else {
        stored = raw;
    }

    std::string out = stored;
    putU64(out, stored.size());
    putU64(out, raw.size());
    putU32(out, crc(stored));
    putU32(out, flags);
    out.append(kTrailerMagic, sizeof(kTrailerMagic));
    return out;
}

std::optional<Bundle> read(const fs::path& executable) {
    std::ifstream file(executable, std::ios::binary);
    if (!file) {
        throw BundleError("could not open " + executable.generic_string());
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(kTrailerSize)) {
        return std::nullopt;
    }

    unsigned char trailer[kTrailerSize];
    file.seekg(fileSize - static_cast<std::streamoff>(kTrailerSize));
    if (!file.read(reinterpret_cast<char*>(trailer), kTrailerSize)) {
        throw BundleError("could not read " + executable.generic_string());
    }

    if (std::memcmp(trailer + 24, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
        return std::nullopt;
    }

    const std::uint64_t storedSize = readU64(trailer);
    const std::uint64_t rawSize = readU64(trailer + 8);
    const std::uint32_t checksum = readU32(trailer + 16);
    const std::uint32_t flags = readU32(trailer + 20);

    if (storedSize > static_cast<std::uint64_t>(fileSize) - kTrailerSize ||
        rawSize > (std::uint64_t(1) << 40)) {
        throw BundleError("the bundled program is damaged (bad trailer)");
    }

    std::string stored(static_cast<std::size_t>(storedSize), '\0');
    file.seekg(fileSize - static_cast<std::streamoff>(kTrailerSize) - static_cast<std::streamoff>(storedSize));
    if (storedSize > 0 && !file.read(stored.data(), static_cast<std::streamsize>(storedSize))) {
        throw BundleError("could not read the bundled program from " + executable.generic_string());
    }

    if (crc(stored) != checksum) {
        throw BundleError("the bundled program is damaged (checksum mismatch)");
    }

    if ((flags & kFlagCompressed) == 0) {
        return deserialize(stored);
    }

    std::string raw(static_cast<std::size_t>(rawSize), '\0');
    uLongf size = static_cast<uLongf>(rawSize);
    const int status = uncompress(reinterpret_cast<Bytef*>(raw.data()), &size,
                                  reinterpret_cast<const Bytef*>(stored.data()), static_cast<uLong>(stored.size()));
    if (status != Z_OK || size != rawSize) {
        throw BundleError("the bundled program is damaged (could not decompress it)");
    }

    return deserialize(raw);
}

void write(const fs::path& runtime, const fs::path& output, const Bundle& bundle, bool compress) {
    if (read(runtime)) {
        throw BundleError(runtime.generic_string() + " is already a built program, not a plain runtime");
    }

    const std::string base = readFile(runtime);
    const std::string appended = encode(bundle, compress);

    std::error_code ec;
    if (output.has_parent_path()) {
        fs::create_directories(output.parent_path(), ec);
        if (ec) {
            throw BundleError("could not create " + output.parent_path().generic_string() + ": " + ec.message());
        }
    }

    // Write next to the target and rename over it, so a failure never leaves
    // a half-written program behind (and a running copy isn't overwritten in
    // place).
    fs::path temp = output;
    temp += ".partial";

    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw BundleError("could not write " + temp.generic_string());
        }
        file.write(base.data(), static_cast<std::streamsize>(base.size()));
        file.write(appended.data(), static_cast<std::streamsize>(appended.size()));
        if (!file) {
            file.close();
            fs::remove(temp, ec);
            throw BundleError("could not write " + temp.generic_string());
        }
    }

    fs::permissions(temp,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    fs::rename(temp, output, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw BundleError("could not write " + output.generic_string() + ": " + ec.message());
    }
}

} // namespace sonata::bundle
