// @sonata/compress: compression, password protection, streaming and checksums.
//
//     local compress = require("@sonata/compress")
//
//     -- one-shot (level: a number, or "fast" | "default" | "best", meaning the same in every format)
//     compress.compress(text)                         --> gzip, default level
//     compress.compress(text, "zstd", "best")         --> format, level
//     compress.compress(text, nil, nil, "hunter2")    --> password-protected (format/level may stay nil)
//     compress.decompress(blob)                       --> text (format sniffed from magic bytes)
//     compress.decompress(blob, "deflate", 1e6)       --> explicit format, output capped at 1e6 bytes
//     compress.decompress(blob, nil, nil, "hunter2")  --> password-protected blob (format is stored in it)
//
//     -- streaming: feed chunks of any size; each call returns the output that is ready
//     local write, finish = compress.encoder("gzip", 6)
//     local blob = write("chunk 1") .. write("chunk 2") .. finish()
//     local write, finish = compress.decoder("gzip", maxSize?)   -- finish() validates the stream
//
//     -- inspection
//     compress.detect(blob)       --> "gzip" | "zlib" | "zstd" | nil   (encrypted blob: its inner format)
//     compress.isEncrypted(blob)  --> boolean
//     compress.formats()          --> { "gzip", "zlib", "deflate", "zstd" }
//     compress.levels("zstd")     --> 1, 22, 3   (min, max, default)
//     compress.crc32("abc")       --> 891568578
//     compress.adler32("abc")     --> 38600999
//     compress.crc32("c", compress.crc32("ab"))   --> 891568578  (optional seed = incremental)
//
// Formats: gzip (default), zlib and deflate (raw) always; zstd with SONATA_COMPRESS_ZSTD.
// maxSize (default 256 MiB, max 1 GiB) caps the output of every decompress()/write()/finish()
// call: bomb protection. Raw deflate has no magic bytes, so it must be named when decompressing.
// Output is byte-identical on every OS (the gzip header carries no timestamp or OS id).
// Passwords need SONATA_COMPRESS_CRYPTO (libsodium); streaming does not support them.
//

#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include <zlib.h>
#ifdef SONATA_COMPRESS_ZSTD
#include <zstd.h>
#endif
#ifdef SONATA_COMPRESS_CRYPTO
#include <sodium.h>
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"


namespace sonata::lib::libs {

namespace {

constexpr double kDefaultMaxSize = 256.0 * 1024 * 1024;
constexpr double kHardMaxSize = 1024.0 * 1024 * 1024; // Luau strings cannot be larger anyway

// ---------------------------------------------------------------------------
// Engines: one incremental implementation per format serves both the one-shot
// functions and the streaming ones. They report failure by RETURNING a message
// (nullptr = success) instead of raising, so no C++ object is alive when a Lua
// error is thrown.
// ---------------------------------------------------------------------------

class Engine {
public:
    virtual ~Engine() = default;
    // Consumes `in` and appends whatever output is ready to `out`.
    virtual const char* write(std::string_view in, std::string& out) = 0;
    // Ends the stream: appends the rest and checks that it is complete.
    virtual const char* finish(std::string& out) = 0;
};

// `limit` (decoders only) caps the output of each write()/finish() call.
using Open = std::unique_ptr<Engine> (*)(bool encode, int level, int variant, std::size_t limit);

struct Codec {
    const char* name;
    int minLevel;
    int maxLevel;
    int defaultLevel;
    int variant; // codec-specific knob; for the zlib family this is windowBits
    Open open;
};

class ZlibEngine final : public Engine {
public:
    ZlibEngine(bool encode, int level, int wbits, std::size_t limit) : encode_(encode), wbits_(wbits), limit_(limit) {
        if (encode) {
            ready_ = deflateInit2(&z_, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) == Z_OK;
            header_.os = 255; // "unknown" instead of the build platform's code => same bytes everywhere
            if (ready_ && wbits > 15) {
                deflateSetHeader(&z_, &header_);
            }
        } else {
            ready_ = inflateInit2(&z_, wbits) == Z_OK;
        }
    }

    ~ZlibEngine() override {
        if (!ready_) {
            return;
        }
        if (encode_) {
            deflateEnd(&z_);
        } else {
            inflateEnd(&z_);
        }
    }

    bool ready() const { return ready_; }

    const char* write(std::string_view in, std::string& out) override { return run(in, Z_NO_FLUSH, out); }

    const char* finish(std::string& out) override {
        if (const char* error = run({}, Z_FINISH, out)) {
            return error;
        }
        return ended_ ? nullptr : (encode_ ? "compression failed" : "truncated data");
    }

private:
    const char* run(std::string_view in, int flush, std::string& out) {
        z_.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
        z_.avail_in = static_cast<uInt>(in.size()); // Lua strings are < 1 GiB: fits zlib's 32-bit uInt

        Bytef buffer[16 * 1024];
        std::size_t produced = 0;
        for (;;) {
            z_.next_out = buffer;
            z_.avail_out = sizeof(buffer);
            const int rc = encode_ ? deflate(&z_, flush) : inflate(&z_, Z_NO_FLUSH);
            const std::size_t n = sizeof(buffer) - z_.avail_out;
            out.append(reinterpret_cast<const char*>(buffer), n);
            produced += n;
            if (!encode_ && produced > limit_) {
                return "output exceeds maxSize";
            }

            if (rc == Z_STREAM_END) {
                ended_ = true;
                // Like gunzip: back-to-back gzip members form one stream.
                if (!encode_ && wbits_ > 15 && z_.avail_in > 0 && z_.next_in[0] == 0x1f && inflateReset(&z_) == Z_OK) {
                    ended_ = false;
                    continue;
                }
                return nullptr;
            }
            if (rc != Z_OK && rc != Z_BUF_ERROR) { // Z_BUF_ERROR just means "needs more input"
                return rc == Z_MEM_ERROR ? "out of memory" : "corrupt data";
            }
            if (z_.avail_out != 0) {
                return nullptr; // input consumed and output drained
            }
        }
    }

    z_stream z_{};
    gz_header header_{};
    bool encode_;
    int wbits_;
    std::size_t limit_;
    bool ready_ = false;
    bool ended_ = false;
};

std::unique_ptr<Engine> openZlib(bool encode, int level, int wbits, std::size_t limit) {
    auto engine = std::make_unique<ZlibEngine>(encode, level, wbits, limit);
    if (!engine->ready()) {
        return nullptr;
    }
    return engine;
}

#ifdef SONATA_COMPRESS_ZSTD
class ZstdEngine final : public Engine {
public:
    ZstdEngine(bool encode, int level, std::size_t limit) : encode_(encode), limit_(limit) {
        if (encode) {
            c_ = ZSTD_createCCtx();
            if (c_) {
                ZSTD_CCtx_setParameter(c_, ZSTD_c_compressionLevel, level);
                ZSTD_CCtx_setParameter(c_, ZSTD_c_checksumFlag, 1);
            }
        } else {
            d_ = ZSTD_createDCtx();
        }
    }

    ~ZstdEngine() override {
        ZSTD_freeCCtx(c_); // both accept null
        ZSTD_freeDCtx(d_);
    }

    bool ready() const { return c_ || d_; }

    const char* write(std::string_view in, std::string& out) override {
        return encode_ ? encodeStep(in, ZSTD_e_continue, out) : decodeStep(in, out);
    }

    const char* finish(std::string& out) override {
        if (encode_) {
            return encodeStep({}, ZSTD_e_end, out);
        }
        return frameDone_ ? nullptr : "truncated data";
    }

private:
    const char* encodeStep(std::string_view in, ZSTD_EndDirective mode, std::string& out) {
        ZSTD_inBuffer src{in.data(), in.size(), 0};
        char buffer[16 * 1024];
        for (;;) {
            ZSTD_outBuffer dst{buffer, sizeof(buffer), 0};
            const std::size_t rc = ZSTD_compressStream2(c_, &dst, &src, mode);
            if (ZSTD_isError(rc)) {
                return ZSTD_getErrorName(rc);
            }
            out.append(buffer, dst.pos);
            if (mode == ZSTD_e_end ? rc == 0 : src.pos == src.size) {
                return nullptr;
            }
        }
    }

    // Also accepts concatenated frames; trailing garbage is an error.
    const char* decodeStep(std::string_view in, std::string& out) {
        ZSTD_inBuffer src{in.data(), in.size(), 0};
        char buffer[16 * 1024];
        std::size_t produced = 0;
        bool full = false; // output buffer filled: more may be pending even with no input left
        while (src.pos < src.size || full) {
            ZSTD_outBuffer dst{buffer, sizeof(buffer), 0};
            const std::size_t rc = ZSTD_decompressStream(d_, &dst, &src);
            if (ZSTD_isError(rc)) {
                return ZSTD_getErrorName(rc);
            }
            out.append(buffer, dst.pos);
            produced += dst.pos;
            if (produced > limit_) {
                return "output exceeds maxSize";
            }
            frameDone_ = rc == 0;
            full = dst.pos == dst.size && rc != 0;
        }
        return nullptr;
    }

    ZSTD_CCtx* c_ = nullptr;
    ZSTD_DCtx* d_ = nullptr;
    bool encode_;
    std::size_t limit_;
    bool frameDone_ = false;
};

std::unique_ptr<Engine> openZstd(bool encode, int level, int, std::size_t limit) {
    auto engine = std::make_unique<ZstdEngine>(encode, level, limit);
    if (!engine->ready()) {
        return nullptr;
    }
    return engine;
}
#endif

// The first row is the default format.
constexpr Codec kCodecs[] = {
    {"gzip", 0, 9, 6, 15 + 16, openZlib},
    {"zlib", 0, 9, 6, 15, openZlib},
    {"deflate", 0, 9, 6, -15, openZlib},
#ifdef SONATA_COMPRESS_ZSTD
    {"zstd", 1, 22, 3, 0, openZstd},
#endif
};

const Codec* findCodec(std::string_view name) {
    for (const Codec& codec : kCodecs) {
        if (std::string_view(codec.name) == name) {
            return &codec;
        }
    }
    return nullptr;
}

// Magic-byte sniffing. Raw deflate has none, so it is never detected.
const Codec* sniff(std::string_view d) {
    if (d.size() < 4) {
        return nullptr;
    }
    const auto b = [&](std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(d[i])); };

    if (b(0) == 0x1f && b(1) == 0x8b) {
        return findCodec("gzip");
    }
    if (b(0) == 0x28 && b(1) == 0xb5 && b(2) == 0x2f && b(3) == 0xfd) {
        return findCodec("zstd");
    }
    // zlib: CM = 8, CINFO <= 7, and the 16-bit header is a multiple of 31.
    if ((b(0) & 0x0f) == 8 && (b(0) >> 4) <= 7 && (b(0) * 256 + b(1)) % 31 == 0) {
        return findCodec("zlib");
    }
    return nullptr;
}

// Whole input through a fresh engine.
const char* convert(const Codec& codec, bool encode, int level, std::size_t limit, std::string_view in, std::string& out) {
    const auto engine = codec.open(encode, level, codec.variant, limit);
    if (!engine) {
        return "codec init failed";
    }
    if (const char* error = engine->write(in, out)) {
        return error;
    }
    return engine->finish(out);
}

// ---------------------------------------------------------------------------
// Password protection. Container, version 1:
//   "SCX\x01" | format name length (1 byte) | format name | salt (16) | nonce (24) | ciphertext + tag (16)
// The key is Argon2id(password, salt). The cipher is XChaCha20-Poly1305 over the COMPRESSED bytes
// (compress first, then encrypt), with the whole header as authenticated data.
// ---------------------------------------------------------------------------

constexpr std::string_view kMagic("SCX\x01", 4);
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kNonceBytes = 24;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kKeyBytes = 32;

struct Container {
    std::string_view format;
    std::string_view header; // everything before the ciphertext
    std::string_view salt;
    std::string_view nonce;
    std::string_view body; // ciphertext + tag
};

// True if `blob` starts with a well-formed container header (needs no crypto library).
bool parseContainer(std::string_view blob, Container& c) {
    if (blob.size() <= kMagic.size() || blob.substr(0, kMagic.size()) != kMagic) {
        return false;
    }
    const std::size_t nameAt = kMagic.size() + 1;
    const std::size_t nameLength = static_cast<unsigned char>(blob[kMagic.size()]);
    const std::size_t headerLength = nameAt + nameLength + kSaltBytes + kNonceBytes;
    if (blob.size() < headerLength) {
        return false;
    }
    c.format = blob.substr(nameAt, nameLength);
    c.salt = blob.substr(nameAt + nameLength, kSaltBytes);
    c.nonce = blob.substr(nameAt + nameLength + kSaltBytes, kNonceBytes);
    c.header = blob.substr(0, headerLength);
    c.body = blob.substr(headerLength);
    return true;
}

#ifdef SONATA_COMPRESS_CRYPTO
static_assert(kSaltBytes == crypto_pwhash_SALTBYTES);
static_assert(kNonceBytes == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(kTagBytes == crypto_aead_xchacha20poly1305_ietf_ABYTES);
static_assert(kKeyBytes == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

struct Key {
    unsigned char bytes[kKeyBytes];
    ~Key() { sodium_memzero(bytes, sizeof(bytes)); }
};

const char* deriveKey(std::string_view password, const unsigned char* salt, unsigned char* key) {
    // MODERATE = 3 passes over 256 MiB. Change both constants (and the container version) to tune.
    if (crypto_pwhash(key, kKeyBytes, password.data(), password.size(), salt, crypto_pwhash_OPSLIMIT_MODERATE,
                      crypto_pwhash_MEMLIMIT_MODERATE, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return "key derivation failed (out of memory?)";
    }
    return nullptr;
}

const char* encrypt(std::string_view format, std::string_view password, std::string_view packed, std::string& out) {
    if (sodium_init() < 0) {
        return "crypto init failed";
    }
    const std::size_t headerLength = kMagic.size() + 1 + format.size() + kSaltBytes + kNonceBytes;
    out.assign(headerLength + packed.size() + kTagBytes, '\0');

    auto* p = reinterpret_cast<unsigned char*>(out.data());
    std::memcpy(p, kMagic.data(), kMagic.size());
    p[kMagic.size()] = static_cast<unsigned char>(format.size());
    std::memcpy(p + kMagic.size() + 1, format.data(), format.size());
    unsigned char* salt = p + kMagic.size() + 1 + format.size();
    unsigned char* nonce = salt + kSaltBytes;
    randombytes_buf(salt, kSaltBytes + kNonceBytes); // adjacent: one call fills both

    Key key;
    if (const char* error = deriveKey(password, salt, key.bytes)) {
        return error;
    }
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(p + headerLength, &written,
                                                   reinterpret_cast<const unsigned char*>(packed.data()), packed.size(),
                                                   p, headerLength, nullptr, nonce, key.bytes) != 0) {
        return "encryption failed";
    }
    return nullptr;
}

const char* decrypt(const Container& c, std::string_view password, std::string& packed) {
    constexpr const char* kWrong = "wrong password or corrupted data";
    if (sodium_init() < 0) {
        return "crypto init failed";
    }
    if (c.body.size() < kTagBytes) {
        return kWrong;
    }
    Key key;
    if (const char* error = deriveKey(password, reinterpret_cast<const unsigned char*>(c.salt.data()), key.bytes)) {
        return error;
    }
    packed.resize(c.body.size() - kTagBytes);
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(packed.data()), &written, nullptr,
            reinterpret_cast<const unsigned char*>(c.body.data()), c.body.size(),
            reinterpret_cast<const unsigned char*>(c.header.data()), c.header.size(),
            reinterpret_cast<const unsigned char*>(c.nonce.data()), key.bytes) != 0) {
        return kWrong;
    }
    packed.resize(written);
    return nullptr;
}
#else
constexpr const char* kNoCrypto = "password support is not built in (SONATA_COMPRESS_CRYPTO)";

const char* encrypt(std::string_view, std::string_view, std::string_view, std::string&) { return kNoCrypto; }
const char* decrypt(const Container&, std::string_view, std::string&) { return kNoCrypto; }
#endif

// ---------------------------------------------------------------------------
// Lua bindings
// ---------------------------------------------------------------------------

// Resolves the optional `format` argument at `idx`; `fallback` is used when it is absent.
const Codec* argCodec(lua_State* L, int idx, const Codec* fallback) {
    const char* name = luaL_optlstring(L, idx, nullptr, nullptr);
    const Codec* codec = name ? findCodec(name) : fallback;
    luaL_argcheck(L, codec != nullptr, idx, name ? "unknown format" : "unknown format (pass one explicitly)");
    return codec;
}

// A number in the format's range, or "fast" | "default" | "best" (same meaning in every format).
int argLevel(lua_State* L, int idx, const Codec& codec) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        const std::string_view name = lua_tostring(L, idx);
        if (name == "fast") {
            return codec.minLevel == 0 ? 1 : codec.minLevel; // level 0 means "store", not "fast"
        }
        if (name == "default") {
            return codec.defaultLevel;
        }
        luaL_argcheck(L, name == "best", idx, "level must be a number, \"fast\", \"default\" or \"best\"");
        return codec.maxLevel;
    }
    const int level = luaL_optinteger(L, idx, codec.defaultLevel);
    luaL_argcheck(L, level >= codec.minLevel && level <= codec.maxLevel, idx, "level out of range for this format");
    return level;
}

std::size_t argMaxSize(lua_State* L, int idx) {
    const double maxSize = luaL_optnumber(L, idx, kDefaultMaxSize);
    luaL_argcheck(L, maxSize >= 0 && maxSize <= kHardMaxSize, idx, "maxSize must be between 0 and 1073741824");
    return static_cast<std::size_t>(maxSize);
}

// Optional non-empty password; data() == nullptr means "none".
std::string_view argPassword(lua_State* L, int idx) {
    std::size_t length = 0;
    const char* password = luaL_optlstring(L, idx, nullptr, &length);
    luaL_argcheck(L, password == nullptr || length > 0, idx, "password must not be empty");
    return {password, length};
}

// Runs `job` against a fresh output buffer, then pushes the result or raises its error.
// The buffer is destroyed BEFORE raising, so no destructor is ever skipped.
template <class Job>
int respond(lua_State* L, Job job) {
    const char* error = nullptr;
    {
        std::string out;
        error = job(out);
        if (!error) {
            lua_pushlstring(L, out.data(), out.size());
            return 1;
        }
    }
    luaL_error(L, "%s", error);
}

// compress.compress(data: string, format: string?, level: number | string?, password: string?): string
int compressEncode(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const std::string_view input(data, length);
    const Codec* codec = argCodec(L, 2, &kCodecs[0]);
    const int level = argLevel(L, 3, *codec);
    const std::string_view password = argPassword(L, 4);

    return respond(L, [&](std::string& out) -> const char* {
        if (!password.data()) {
            return convert(*codec, true, level, 0, input, out);
        }
        std::string packed;
        if (const char* error = convert(*codec, true, level, 0, input, packed)) {
            return error;
        }
        return encrypt(codec->name, password, packed, out);
    });
}

// compress.decompress(data: string, format: string?, maxSize: number?, password: string?): string
int compressDecode(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const std::string_view input(data, length);
    const bool explicitFormat = !lua_isnoneornil(L, 2);
    const std::size_t limit = argMaxSize(L, 3);
    const std::string_view password = argPassword(L, 4);

    // An explicit format without a password means "these bytes ARE that format", even if they
    // happen to start with the container magic. Encrypted blobs carry their own format.
    Container box;
    const bool encrypted = (password.data() || !explicitFormat) && parseContainer(input, box);
    const Codec* codec = encrypted ? findCodec(box.format) : argCodec(L, 2, sniff(input));

    return respond(L, [&](std::string& out) -> const char* {
        if (!encrypted) {
            return password.data() ? "data is not password-protected" : convert(*codec, false, 0, limit, input, out);
        }
        if (!password.data()) {
            return "password required";
        }
        if (!codec) {
            return "unsupported format inside encrypted data";
        }
        std::string packed;
        if (const char* error = decrypt(box, password, packed)) {
            return error;
        }
        return convert(*codec, false, 0, limit, packed, out);
    });
}

// compress.detect(data: string): string?
int compressDetect(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const std::string_view view(data, length);

    Container box;
    if (parseContainer(view, box)) {
        lua_pushlstring(L, box.format.data(), box.format.size());
    } else if (const Codec* codec = sniff(view)) {
        lua_pushstring(L, codec->name);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// compress.isEncrypted(data: string): boolean
int compressIsEncrypted(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);

    Container box;
    lua_pushboolean(L, parseContainer(std::string_view(data, length), box));
    return 1;
}

// compress.formats(): { string }
int compressFormats(lua_State* L) {
    lua_createtable(L, static_cast<int>(std::size(kCodecs)), 0);
    int index = 0;
    for (const Codec& codec : kCodecs) {
        lua_pushstring(L, codec.name);
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}

// compress.levels(format: string?): (number, number, number)   -- min, max, default
int compressLevels(lua_State* L) {
    const Codec* codec = argCodec(L, 1, &kCodecs[0]);

    lua_pushnumber(L, codec->minLevel);
    lua_pushnumber(L, codec->maxLevel);
    lua_pushnumber(L, codec->defaultLevel);
    return 3;
}

// compress.crc32 / compress.adler32(data: string, seed: number?): number
template <bool Adler>
int compressChecksum(lua_State* L) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 1, &length);
    const double seed = luaL_optnumber(L, 2, Adler ? 1 : 0);

    luaL_argcheck(L, seed >= 0 && seed <= 4294967295.0, 2, "seed must be a 32-bit unsigned integer");

    const auto* bytes = reinterpret_cast<const Bytef*>(data);
    const auto start = static_cast<uLong>(seed);
    const auto size = static_cast<uInt>(length);

    lua_pushnumber(L, static_cast<double>(Adler ? adler32(start, bytes, size) : crc32(start, bytes, size)));
    return 1;
}

// Streams: a userdata (freed by the GC) shared as upvalue 1 by the write and finish closures.
struct Stream {
    std::unique_ptr<Engine> engine;
    bool finished = false;
    bool failed = false;
};

// write(chunk: string): string   /   finish(): string
template <bool Finish>
int streamStep(lua_State* L) {
    auto* stream = static_cast<Stream*>(lua_touserdata(L, lua_upvalueindex(1)));
    std::size_t length = 0;
    const char* chunk = Finish ? "" : luaL_checklstring(L, 1, &length);

    if (stream->failed) {
        luaL_error(L, "stream failed earlier and cannot be used anymore");
    }
    if (stream->finished) {
        luaL_error(L, "stream is already finished");
    }

    return respond(L, [&](std::string& out) -> const char* {
        const char* error = Finish ? stream->engine->finish(out) : stream->engine->write({chunk, length}, out);
        stream->failed = error != nullptr;
        stream->finished = Finish && !error;
        return error;
    });
}

// compress.encoder(format: string?, level: number | string?): ((string) -> string, () -> string)
// compress.decoder(format: string?, maxSize: number?): ((string) -> string, () -> string)
template <bool Encode>
int streamOpen(lua_State* L) {
    const Codec* codec = argCodec(L, 1, &kCodecs[0]);
    const int level = Encode ? argLevel(L, 2, *codec) : 0;
    const std::size_t limit = Encode ? 0 : argMaxSize(L, 2);

    // Create the userdata first and fill it afterwards: no C++ object is alive across a Lua call.
    auto* stream = new (lua_newuserdatadtor(L, sizeof(Stream), [](lua_State*, void* p) {
        static_cast<Stream*>(p)->~Stream();
    })) Stream();

    stream->engine = codec->open(Encode, level, codec->variant, limit);
    if (!stream->engine) {
        luaL_error(L, "codec init failed");
    }

    lua_pushvalue(L, -1); // stream, stream
    lua_pushcclosure(L, streamStep<false>, "write", 1); // stream, write
    lua_pushvalue(L, -2); // stream, write, stream
    lua_pushcclosure(L, streamStep<true>, "finish", 1); // stream, write, finish
    return 2;
}

constexpr NativeFunction kFunctions[] = {
    {"compress", compressEncode},
    {"decompress", compressDecode},
    {"encoder", streamOpen<true>},
    {"decoder", streamOpen<false>},
    {"detect", compressDetect},
    {"isEncrypted", compressIsEncrypted},
    {"formats", compressFormats},
    {"levels", compressLevels},
    {"crc32", compressChecksum<false>},
    {"adler32", compressChecksum<true>},
};

} // namespace

// The opener: runs once per VM and pushes exactly one value (the module table).
void openCompress(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 1);
    setFunctions(L, kFunctions);

    lua_pushnumber(L, 2);
    lua_setfield(L, -2, "version");
}

} // namespace sonata::lib::libs