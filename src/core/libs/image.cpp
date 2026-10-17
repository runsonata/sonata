// @sonata/image: load, edit and save images, backed by ImageMagick (MagickWand,
// version 6 or 7).
//
//     local image = require("@sonata/image")
//
//     image.load("photo.jpg")
//         :autoOrient()
//         :resize(800)                         -- 800 wide, height keeps the aspect ratio
//         :drawText("© me", 10, 10, { gravity = "bottom-right", size = 18, color = "white" })
//         :save("photo.webp", { quality = 85 })
//
// Creating images (every function returns an Image):
//     image.load(path, options?)              a file (every frame of a GIF, ...)
//     image.decode(data, options?)            bytes (string or buffer)
//     image.new(width, height, background?)   a blank canvas (transparent by default)
//     image.generate(spec, width?, height?)   an ImageMagick pseudo-image: "gradient:red-blue",
//                                             "plasma:", "pattern:checkerboard", "xc:red", ...
//     image.fromPixels(width, height, data, channels?)   raw 8-bit pixels ("rgba" by default)
//     image.text(text, style?)                text on a canvas that just fits it
//     image.animate(frames, options?)         frames -> one animated image
//     image.append(images, options?)          side by side (or stacked with vertical = true)
//     image.grid(images, options?)            a contact sheet
// Other functions:
//     image.info(pathOrData)  image.measureText(text, style?)  image.color(c)  image.hex(c)
//     image.formats()  image.fonts()  image.options(kind)  image.limits(limits?)  image.version
//
// Image methods change the image in place, apply to every frame, and return the image, so
// they chain. Use img:clone() to keep the original. Fields: width, height, format, frames,
// depth, colorspace, hasAlpha.
//
// Conventions: pixel coordinates start at 0 (top-left); colors are an ImageMagick color
// string ("red", "#ff000080", "rgba(255,0,0,0.5)", "none") or { r, g, b, a? } with 0-255
// channels; fuzz, thresholds and levels are fractions from 0 to 1; option names such as
// filters ("lanczos"), blend operators ("multiply") or gravities ("bottom-right") are
// case-insensitive, and image.options(kind) lists them.
//
// Errors raise; messages start with the function name ("Image:resize: ...").

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if __has_include(<MagickWand/MagickWand.h>)
#include <MagickWand/MagickWand.h>
#else
#include <wand/MagickWand.h>
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#if MagickLibVersion >= 0x700
#define SONATA_IM7 1
#else
#define SONATA_IM7 0
#endif

namespace sonata::lib::libs {

namespace {

constexpr const char* kImageMeta = "sonata.image.Image";
constexpr double kQuantum = static_cast<double>(QuantumRange);
constexpr size_t kMaxDimension = 1u << 20; // per side; ImageMagick's own limits still apply

// ImageMagick 6 / 7 differences //////////////////////////////////////////////

namespace im {

#if SONATA_IM7
using Filter = FilterType;
using Distort = DistortMethod;
using Layer = LayerMethod;
using AlphaOption = AlphaChannelOption;
constexpr CommandOption kAlphaOptions = MagickAlphaChannelOptions;
#else
using Filter = FilterTypes;
using Distort = DistortImageMethod;
using Layer = ImageLayerMethod;
using AlphaOption = AlphaChannelType;
constexpr CommandOption kAlphaOptions = MagickAlphaOptions;
#endif

MagickBooleanType resize(MagickWand* w, size_t columns, size_t rows, Filter filter) {
#if SONATA_IM7
    return MagickResizeImage(w, columns, rows, filter);
#else
    return MagickResizeImage(w, columns, rows, filter, 1.0);
#endif
}

MagickBooleanType composite(MagickWand* w, const MagickWand* source, CompositeOperator op, ssize_t x, ssize_t y) {
#if SONATA_IM7
    return MagickCompositeImage(w, source, op, MagickTrue, x, y);
#else
    return MagickCompositeImage(w, source, op, x, y);
#endif
}

MagickBooleanType oilPaint(MagickWand* w, double radius) {
#if SONATA_IM7
    return MagickOilPaintImage(w, radius, radius / 2.0);
#else
    return MagickOilPaintImage(w, radius);
#endif
}

MagickBooleanType swirl(MagickWand* w, double degrees) {
#if SONATA_IM7
    return MagickSwirlImage(w, degrees, UndefinedInterpolatePixel);
#else
    return MagickSwirlImage(w, degrees);
#endif
}

MagickBooleanType implode(MagickWand* w, double amount) {
#if SONATA_IM7
    return MagickImplodeImage(w, amount, UndefinedInterpolatePixel);
#else
    return MagickImplodeImage(w, amount);
#endif
}

MagickBooleanType wave(MagickWand* w, double amplitude, double wavelength) {
#if SONATA_IM7
    return MagickWaveImage(w, amplitude, wavelength, UndefinedInterpolatePixel);
#else
    return MagickWaveImage(w, amplitude, wavelength);
#endif
}

MagickBooleanType spread(MagickWand* w, double radius) {
#if SONATA_IM7
    return MagickSpreadImage(w, UndefinedInterpolatePixel, radius);
#else
    return MagickSpreadImage(w, radius);
#endif
}

MagickBooleanType addNoise(MagickWand* w, NoiseType type, double attenuate) {
#if SONATA_IM7
    return MagickAddNoiseImage(w, type, attenuate);
#else
    char value[64];
    std::snprintf(value, sizeof(value), "%g", attenuate);
    MagickSetImageArtifact(w, "attenuate", value);
    return MagickAddNoiseImage(w, type);
#endif
}

MagickBooleanType border(MagickWand* w, const PixelWand* color, size_t width, size_t height) {
#if SONATA_IM7
    return MagickBorderImage(w, color, width, height, OverCompositeOp);
#else
    return MagickBorderImage(w, color, width, height);
#endif
}

MagickBooleanType separate(MagickWand* w, ChannelType channel) {
#if SONATA_IM7
    return MagickSeparateImage(w, channel);
#else
    return MagickSeparateImageChannel(w, channel);
#endif
}

MagickBooleanType mean(MagickWand* w, double* mean, double* deviation) {
#if SONATA_IM7
    return MagickGetImageMean(w, mean, deviation);
#else
    return MagickGetImageChannelMean(w, CompositeChannels, mean, deviation);
#endif
}

MagickBooleanType quantize(MagickWand* w, size_t colors, ColorspaceType colorspace, bool dither) {
#if SONATA_IM7
    return MagickQuantizeImage(w, colors, colorspace, 0, dither ? RiemersmaDitherMethod : NoDitherMethod, MagickFalse);
#else
    return MagickQuantizeImage(w, colors, colorspace, 0, dither ? MagickTrue : MagickFalse, MagickFalse);
#endif
}

MagickBooleanType posterize(MagickWand* w, size_t levels, bool dither) {
#if SONATA_IM7
    return MagickPosterizeImage(w, levels, dither ? RiemersmaDitherMethod : NoDitherMethod);
#else
    return MagickPosterizeImage(w, levels, dither ? MagickTrue : MagickFalse);
#endif
}

MagickBooleanType floodFill(MagickWand* w, const PixelWand* fill, double fuzz, ssize_t x, ssize_t y) {
#if SONATA_IM7
    return MagickFloodfillPaintImage(w, fill, fuzz, nullptr, x, y, MagickFalse);
#else
    return MagickFloodfillPaintImage(w, DefaultChannels, fill, fuzz, nullptr, x, y, MagickFalse);
#endif
}

KernelInfo* kernel(const char* text) {
#if SONATA_IM7
    ExceptionInfo* exception = AcquireExceptionInfo();
    KernelInfo* kernel = AcquireKernelInfo(text, exception);
    DestroyExceptionInfo(exception);
    return kernel;
#else
    return AcquireKernelInfo(text);
#endif
}

} // namespace im

// RAII for ImageMagick's handles ///////////////////////////////////////////

struct WandDeleter {
    void operator()(MagickWand* w) const { DestroyMagickWand(w); }
    void operator()(PixelWand* p) const { DestroyPixelWand(p); }
    void operator()(DrawingWand* d) const { DestroyDrawingWand(d); }
    void operator()(KernelInfo* k) const { DestroyKernelInfo(k); }
};
using Wand = std::unique_ptr<MagickWand, WandDeleter>;
using Pixel = std::unique_ptr<PixelWand, WandDeleter>;
using Drawing = std::unique_ptr<DrawingWand, WandDeleter>;
using Kernel = std::unique_ptr<KernelInfo, WandDeleter>;

// Memory ImageMagick hands out (strings, blobs) and must take back.
struct MagickString {
    char* text = nullptr;
    explicit MagickString(char* t) : text(t) {}
    ~MagickString() {
        if (text) {
            MagickRelinquishMemory(text);
        }
    }
    MagickString(const MagickString&) = delete;
    MagickString& operator=(const MagickString&) = delete;
    std::string str() const { return text ? std::string(text) : std::string(); }
};

// The userdata behind an Image. "wand" is null once the image is closed.
struct Image {
    MagickWand* wand = nullptr;

    explicit Image(MagickWand* w) : wand(w) {}
    ~Image() { close(); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    void close() {
        if (wand) {
            DestroyMagickWand(wand);
            wand = nullptr;
        }
    }
    // Takes ownership of "next" and drops the current images.
    void replace(Wand next) {
        close();
        wand = next.release();
    }
};

// Errors ////////////////////////////////////////////////////////////////////
//
// Bindings run their body in run(), where every problem is a C++ exception
// (Failure). run() turns it into a Lua error only after the body's objects,
// wands included, were destroyed, so nothing leaks however Luau was built.

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class F>
int run(lua_State* L, F&& body) {
    char message[1024] = {0};
    int results = 0;
    try {
        results = body();
    } catch (const Failure& e) {
        std::snprintf(message, sizeof(message), "%s", e.what());
    } catch (const std::bad_alloc&) {
        std::snprintf(message, sizeof(message), "image: out of memory");
    } catch (const std::exception& e) {
        std::snprintf(message, sizeof(message), "image: %s", e.what());
    }
    if (message[0] != '\0') {
        luaL_error(L, "%s", message);
    }
    return results;
}

// ImageMagick's message, without the " @ error/file.c/Function/line" suffix.
std::string describe(MagickWand* w) {
    ExceptionType severity = UndefinedException;
    MagickString description(MagickGetException(w, &severity));
    MagickClearException(w);
    std::string text = description.str();
    if (const std::size_t at = text.find(" @ "); at != std::string::npos) {
        text.resize(at);
    }
    return text.empty() ? std::string("ImageMagick reported a failure") : text;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string typeName(lua_State* L, int index) {
    return luaL_typename(L, index);
}

std::filesystem::path pathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Arguments /////////////////////////////////////////////////////////////////

struct Context;

// A value somewhere on the stack, and how to name it in an error.
struct Value {
    const Context& c;
    int index;
    std::string what;

    bool missing() const;
    int type() const;
    [[noreturn]] void fail(std::string_view problem) const;
    [[noreturn]] void expected(const char* type) const;

    double number() const;
    double number(double fallback) const;
    double number(double fallback, double min, double max) const;
    std::optional<double> optNumber() const;
    size_t size() const; // a whole number >= 1, for pixel dimensions
    ssize_t coordinate() const;
    std::string string() const;
    std::optional<std::string> optString() const;
    bool boolean(bool fallback) const;
    Pixel color() const;
    Pixel color(const char* fallback) const;
};

// One call of a binding: the Lua state and the name used in messages.
struct Context {
    lua_State* L;
    const char* name;

    [[noreturn]] void fail(std::string_view problem) const {
        throw Failure(std::string(name) + ": " + std::string(problem));
    }
    [[noreturn]] void magick(MagickWand* w) const { fail(describe(w)); }

    Value arg(int index, std::string what) const { return Value{*this, index, std::move(what)}; }

    // Raises the message of a failed ImageMagick call.
    void check(MagickBooleanType ok, MagickWand* w) const {
        if (ok == MagickFalse) {
            magick(w);
        }
    }
    Wand check(MagickWand* result, MagickWand* from) const {
        if (result == nullptr) {
            magick(from);
        }
        return Wand(result);
    }

    // The open Image at "index".
    Image& image(int index, const char* what = "self") const {
        bool ours = false;
        if (lua_type(L, index) == LUA_TUSERDATA && lua_getmetatable(L, index)) {
            luaL_getmetatable(L, kImageMeta);
            ours = lua_rawequal(L, -1, -2) != 0;
            lua_pop(L, 2);
        }
        if (!ours) {
            if (std::string_view(what) == "self") {
                fail("expected an Image as self, got " + typeName(L, index) + " (use ':' to call methods)");
            }
            fail(std::string(what) + " must be an Image, got " + typeName(L, index));
        }
        Image* image = static_cast<Image*>(lua_touserdata(L, index));
        if (image->wand == nullptr) {
            fail(std::string(what) + " is a closed image");
        }
        MagickClearException(image->wand);
        return *image;
    }
};

bool Value::missing() const { return lua_isnoneornil(c.L, index); }
int Value::type() const { return lua_type(c.L, index); }
void Value::fail(std::string_view problem) const { c.fail(what + " " + std::string(problem)); }
void Value::expected(const char* type) const {
    fail(std::string("must be ") + type + ", got " + typeName(c.L, index));
}

double Value::number() const {
    if (type() != LUA_TNUMBER) {
        expected("a number");
    }
    const double value = lua_tonumber(c.L, index);
    if (!std::isfinite(value)) {
        fail("must be a finite number");
    }
    return value;
}
double Value::number(double fallback) const { return missing() ? fallback : number(); }
double Value::number(double fallback, double min, double max) const {
    const double value = number(fallback);
    if (value < min || value > max) {
        char range[128];
        std::snprintf(range, sizeof(range), "must be between %g and %g, got %g", min, max, value);
        fail(range);
    }
    return value;
}
std::optional<double> Value::optNumber() const {
    if (missing()) {
        return std::nullopt;
    }
    return number();
}
size_t Value::size() const {
    const double value = number();
    if (value < 1 || value > static_cast<double>(kMaxDimension) || value != std::floor(value)) {
        fail("must be a whole number from 1 to " + std::to_string(kMaxDimension));
    }
    return static_cast<size_t>(value);
}
ssize_t Value::coordinate() const {
    const double value = number();
    if (std::fabs(value) > static_cast<double>(kMaxDimension) * 4) {
        fail("is out of range");
    }
    return static_cast<ssize_t>(std::lround(value));
}
std::string Value::string() const {
    if (type() != LUA_TSTRING) {
        expected("a string");
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(c.L, index, &length);
    return std::string(text, length);
}
std::optional<std::string> Value::optString() const {
    if (missing()) {
        return std::nullopt;
    }
    return string();
}
bool Value::boolean(bool fallback) const {
    if (missing()) {
        return fallback;
    }
    if (type() != LUA_TBOOLEAN) {
        expected("a boolean");
    }
    return lua_toboolean(c.L, index) != 0;
}

// Reads { r, g, b, a? } (0-255) or any color string ImageMagick knows.
Pixel Value::color() const {
    Pixel pixel(NewPixelWand());
    if (type() == LUA_TSTRING) {
        const std::string text = string();
        if (PixelSetColor(pixel.get(), text.c_str()) == MagickFalse) {
            fail("is not a color ImageMagick knows: '" + text + "'");
        }
        return pixel;
    }
    if (type() != LUA_TTABLE) {
        expected("a color (a string or { r, g, b, a? })");
    }
    double channels[4] = {0, 0, 0, 255};
    const char* names[4] = {"r", "g", "b", "a"};
    for (int i = 0; i < 4; ++i) {
        lua_rawgetfield(c.L, index, names[i]);
        const int t = lua_type(c.L, -1);
        const double v = t == LUA_TNUMBER ? lua_tonumber(c.L, -1) : -1;
        lua_pop(c.L, 1);
        if (t == LUA_TNIL && i == 3) {
            continue;
        }
        if (t != LUA_TNUMBER || !(v >= 0 && v <= 255)) {
            fail(std::string("must have a number from 0 to 255 as '") + names[i] + "'");
        }
        channels[i] = v;
    }
    PixelSetRed(pixel.get(), channels[0] / 255.0);
    PixelSetGreen(pixel.get(), channels[1] / 255.0);
    PixelSetBlue(pixel.get(), channels[2] / 255.0);
    PixelSetAlpha(pixel.get(), channels[3] / 255.0);
    return pixel;
}
Pixel Value::color(const char* fallback) const {
    if (!missing()) {
        return color();
    }
    Pixel pixel(NewPixelWand());
    PixelSetColor(pixel.get(), fallback);
    return pixel;
}

// An optional options table. Unknown names are an error, so typos don't go
// unnoticed. field() puts the value in a scratch slot that the next field()
// call reuses, so read one field completely before asking for the next.
class Options {
public:
    Options(const Context& c, int index, std::initializer_list<const char*> known)
        : c_(c), index_(index < 0 ? lua_gettop(c.L) + index + 1 : index) {
        const int type = lua_type(c.L, index_);
        if (type == LUA_TNONE || type == LUA_TNIL) {
            index_ = 0;
            return;
        }
        if (type != LUA_TTABLE) {
            c.fail(std::string("options must be a table, got ") + lua_typename(c.L, type));
        }
        lua_pushnil(c.L);
        while (lua_next(c.L, index_) != 0) {
            lua_pop(c.L, 1);
            if (lua_type(c.L, -1) != LUA_TSTRING) {
                lua_pop(c.L, 1);
                c.fail("option names must be strings");
            }
            const std::string key = lua_tostring(c.L, -1);
            if (std::none_of(known.begin(), known.end(), [&](const char* k) { return key == k; })) {
                lua_pop(c.L, 1);
                c.fail("unknown option '" + key + "'");
            }
        }
        // A scratch slot that field() fills.
        lua_pushnil(c.L);
        slot_ = lua_gettop(c.L);
    }

    bool given() const { return index_ != 0; }

    // options[key], in the scratch slot (nil without options).
    Value field(const char* key) const {
        if (index_ == 0) { // just past the top: reads as none
            return Value{c_, lua_gettop(c_.L) + 1, std::string("option '") + key + "'"};
        }
        lua_rawgetfield(c_.L, index_, key);
        lua_replace(c_.L, slot_);
        return Value{c_, slot_, std::string("option '") + key + "'"};
    }

private:
    const Context& c_;
    int index_;
    int slot_ = 0;
};

// Names ImageMagick knows ("lanczos", "multiply", ...) ////////////////////////

// Case and '-', '_', ' ' don't matter, so "linear-dodge" finds LinearDodge.
ssize_t parseOption(const Context& c, CommandOption kind, const std::string& value, const char* what,
                    const char* listName) {
    std::string squashed;
    for (const char ch : value) {
        if (ch != '-' && ch != '_' && ch != ' ') {
            squashed.push_back(ch);
        }
    }
    const ssize_t parsed = squashed.empty() ? -1 : ParseCommandOption(kind, MagickFalse, squashed.c_str());
    if (parsed < 0) {
        c.fail(std::string("unknown ") + what + " '" + value + "' (image.options(\"" + listName + "\") lists them)");
    }
    return parsed;
}

struct OptionKind {
    const char* name;
    CommandOption kind;
};

constexpr OptionKind kOptionKinds[] = {
    {"alpha", im::kAlphaOptions},
    {"channel", MagickChannelOptions},
    {"colorspace", MagickColorspaceOptions},
    {"compose", MagickComposeOptions},
    {"distort", MagickDistortOptions},
    {"filter", MagickFilterOptions},
    {"kernel", MagickKernelOptions},
    {"linecap", MagickLineCapOptions},
    {"linejoin", MagickLineJoinOptions},
    {"metric", MagickMetricOptions},
    {"morphology", MagickMorphologyOptions},
    {"noise", MagickNoiseOptions},
};

template <class T>
T parseEnum(const Context& c, CommandOption kind, const std::string& value, const char* what, const char* list) {
    return static_cast<T>(parseOption(c, kind, value, what, list));
}

// Gravity: where something goes inside a larger area.
struct Gravity {
    int column = 0; // 0 left, 1 center, 2 right
    int row = 0;    // 0 top, 1 middle, 2 bottom

    // Position of something "inner" wide inside "outer", nudged by "offset"
    // (inward from the edge it sticks to, as ImageMagick does it).
    static double place(int where, double outer, double inner, double offset) {
        switch (where) {
            case 0: return offset;
            case 1: return (outer - inner) / 2.0 + offset;
            default: return outer - inner - offset;
        }
    }
    double x(double outer, double inner, double offset = 0) const { return place(column, outer, inner, offset); }
    double y(double outer, double inner, double offset = 0) const { return place(row, outer, inner, offset); }

    GravityType magick() const {
        static constexpr GravityType kTypes[3][3] = {
            {NorthWestGravity, NorthGravity, NorthEastGravity},
            {WestGravity, CenterGravity, EastGravity},
            {SouthWestGravity, SouthGravity, SouthEastGravity},
        };
        return kTypes[row][column];
    }
};

std::optional<Gravity> readGravity(const Value& v) {
    const std::optional<std::string> text = v.optString();
    if (!text) {
        return std::nullopt;
    }
    std::string name;
    for (const char ch : lowercase(*text)) {
        if (ch != '-' && ch != '_' && ch != ' ') {
            name.push_back(ch);
        }
    }
    struct Named {
        const char* a;
        const char* b;
        Gravity g;
    };
    static constexpr Named kNames[] = {
        {"topleft", "northwest", {0, 0}},    {"top", "north", {1, 0}},          {"topright", "northeast", {2, 0}},
        {"left", "west", {0, 1}},            {"center", "centre", {1, 1}},      {"right", "east", {2, 1}},
        {"bottomleft", "southwest", {0, 2}}, {"bottom", "south", {1, 2}},       {"bottomright", "southeast", {2, 2}},
    };
    for (const Named& n : kNames) {
        if (name == n.a || name == n.b) {
            return n.g;
        }
    }
    v.fail("must be a gravity like \"center\", \"top-left\" or \"bottom-right\", got '" + *text + "'");
}

// Frames ////////////////////////////////////////////////////////////////////

// Runs "op" on every frame; "op" returns ImageMagick's MagickBooleanType.
template <class F>
void eachFrame(const Context& c, MagickWand* w, F&& op) {
    MagickResetIterator(w);
    while (MagickNextImage(w) != MagickFalse) {
        c.check(op(), w);
    }
    MagickResetIterator(w);
}

// Replaces every frame with what "op" makes of it (a new single-image wand).
template <class F>
Wand mapFrames(const Context& c, MagickWand* w, F&& op) {
    Wand out(NewMagickWand());
    MagickResetIterator(w);
    while (MagickNextImage(w) != MagickFalse) {
        Wand frame = c.check(op(), w);
        MagickSetLastIterator(out.get());
        c.check(MagickAddImage(out.get(), frame.get()), out.get());
    }
    MagickResetIterator(out.get());
    return out;
}

void first(MagickWand* w) {
    MagickResetIterator(w);
}

size_t frameCount(MagickWand* w) {
    return MagickGetNumberImages(w);
}

// A copy of the first frame alone.
Wand firstFrame(const Context& c, MagickWand* w) {
    first(w);
    return c.check(MagickGetImage(w), w);
}

// Removes the virtual canvas left behind by crops (ImageMagick's +repage).
void repage(MagickWand* w) {
    MagickResetImagePage(w, nullptr);
}

// Lua side //////////////////////////////////////////////////////////////////

void pushImage(lua_State* L, Wand wand) {
    MagickResetIterator(wand.get());
    MagickWand* raw = wand.release();
    void* memory = lua_newuserdatadtor(L, sizeof(Image), [](lua_State*, void* p) { static_cast<Image*>(p)->~Image(); });
    new (memory) Image(raw);
    luaL_getmetatable(L, kImageMeta);
    lua_setmetatable(L, -2);
}

// Returns the image at index 1 (self) to Luau, for chaining.
int self(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}

void setNumber(lua_State* L, const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

int channel255(double fraction) {
    return static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 255.0));
}

void pushColor(lua_State* L, PixelWand* p) {
    lua_createtable(L, 0, 4);
    setNumber(L, "r", channel255(PixelGetRed(p)));
    setNumber(L, "g", channel255(PixelGetGreen(p)));
    setNumber(L, "b", channel255(PixelGetBlue(p)));
    setNumber(L, "a", channel255(PixelGetAlpha(p)));
}

std::string hexOf(PixelWand* p) {
    char text[16];
    const int a = channel255(PixelGetAlpha(p));
    if (a == 255) {
        std::snprintf(text, sizeof(text), "#%02x%02x%02x", channel255(PixelGetRed(p)), channel255(PixelGetGreen(p)),
                      channel255(PixelGetBlue(p)));
    } else {
        std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", channel255(PixelGetRed(p)),
                      channel255(PixelGetGreen(p)), channel255(PixelGetBlue(p)), a);
    }
    return text;
}

std::string formatOf(MagickWand* w) {
    first(w);
    return lowercase(MagickString(MagickGetImageFormat(w)).str());
}

// Reading and writing ///////////////////////////////////////////////////////

struct ReadOptions {
    std::optional<std::string> format;
    std::optional<double> density;
    std::optional<size_t> frame;
    std::vector<std::pair<std::string, std::string>> defines;
};

// Each "define" is an ImageMagick coder option, such as "jpeg:size" = "200x200".
void readDefines(const Value& v, std::vector<std::pair<std::string, std::string>>& out) {
    if (v.missing()) {
        return;
    }
    if (v.type() != LUA_TTABLE) {
        v.expected("a table of strings");
    }
    lua_State* L = v.c.L;
    lua_pushnil(L);
    while (lua_next(L, v.index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER &&
                                               lua_type(L, -1) != LUA_TBOOLEAN)) {
            lua_pop(L, 2);
            v.fail("must map names to strings");
        }
        std::string value;
        if (lua_type(L, -1) == LUA_TBOOLEAN) {
            value = lua_toboolean(L, -1) ? "true" : "false";
        } else {
            lua_pushvalue(L, -1); // tostring on a copy, so lua_next's key stays put
            value = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        out.emplace_back(lua_tostring(L, -2), std::move(value));
        lua_pop(L, 1);
    }
}

ReadOptions readReadOptions(const Context& c, int index) {
    const Options o(c, index, {"format", "density", "frame", "define"});
    ReadOptions r;
    r.format = o.field("format").optString();
    r.density = o.field("density").optNumber();
    if (r.density && !(*r.density > 0 && *r.density <= 10000)) {
        c.fail("option 'density' must be between 0 and 10000");
    }
    const Value frame = o.field("frame");
    if (!frame.missing()) {
        r.frame = frame.size();
    }
    readDefines(o.field("define"), r.defines);
    return r;
}

bool isAlnum(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-';
    });
}

// The extension of "path", lowercase, without the dot ("" if there is none).
std::string extensionOf(const std::string& path) {
    const std::string ext = pathOf(path).extension().string();
    return ext.size() > 1 ? lowercase(ext.substr(1)) : std::string();
}

// Decodes "data". "nameHint" (a file name) lets formats without a signature,
// like TGA, be found by their extension; it is never opened.
Wand readBlob(const Context& c, const void* data, size_t length, const ReadOptions& options,
              const std::string& extension, bool ping) {
    if (length == 0) {
        c.fail("the data is empty");
    }
    Wand w(NewMagickWand());
    if (options.density) {
        MagickSetResolution(w.get(), *options.density, *options.density);
    }
    for (const auto& [key, value] : options.defines) {
        MagickSetOption(w.get(), key.c_str(), value.c_str());
    }
    // Only a synthetic name: a real path like "a[1].gif" would be parsed for
    // frame numbers and sizes.
    if (options.format) {
        if (!isAlnum(*options.format)) {
            c.fail("option 'format' must be a format name such as \"png\"");
        }
        MagickSetFilename(w.get(), (*options.format + ":image").c_str());
    } else if (isAlnum(extension)) {
        MagickSetFilename(w.get(), ("image." + extension).c_str());
    }

    const MagickBooleanType ok =
        ping ? MagickPingImageBlob(w.get(), data, length) : MagickReadImageBlob(w.get(), data, length);
    if (ok == MagickFalse) {
        const std::string problem = describe(w.get());
        if (problem.find("no decode delegate") != std::string::npos) {
            c.fail("not in an image format this ImageMagick can read" +
                   (options.format ? " (as " + *options.format + ")" : std::string()));
        }
        c.fail(problem);
    }

    if (options.frame) {
        if (*options.frame > frameCount(w.get())) {
            c.fail("option 'frame' is " + std::to_string(*options.frame) + ", but the image has " +
                   std::to_string(frameCount(w.get())) + " frame(s)");
        }
        MagickSetIteratorIndex(w.get(), static_cast<ssize_t>(*options.frame - 1));
        w = c.check(MagickGetImage(w.get()), w.get());
    }
    MagickResetIterator(w.get());
    return w;
}

std::string readFile(const Context& c, const std::string& path) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(pathOf(path), ec);
        c.fail("could not open '" + path + "'" + (exists ? "" : " (no such file)"));
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        c.fail("could not read '" + path + "'");
    }
    return data;
}

void writeFile(const Context& c, const std::string& path, const unsigned char* data, size_t length) {
    std::ofstream out(pathOf(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        c.fail("could not open '" + path + "' for writing");
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(length));
    out.close();
    if (!out) {
        c.fail("could not write '" + path + "'");
    }
}

// Raw bytes from a string or a buffer argument.
std::string_view bytesOf(const Value& v) {
    std::size_t length = 0;
    if (v.type() == LUA_TBUFFER) {
        const void* data = lua_tobuffer(v.c.L, v.index, &length);
        return {static_cast<const char*>(data), length};
    }
    if (v.type() == LUA_TSTRING) {
        const char* data = lua_tolstring(v.c.L, v.index, &length);
        return {data, length};
    }
    v.expected("a string or a buffer");
}

struct EncodeOptions {
    std::optional<double> quality;
    bool lossless = false;
    bool progressive = false;
    bool strip = false;
    Pixel background;
    std::vector<std::pair<std::string, std::string>> defines;
};

EncodeOptions readEncodeOptions(const Context& c, int index) {
    const Options o(c, index, {"quality", "lossless", "progressive", "strip", "background", "define"});
    EncodeOptions e;
    e.quality = o.field("quality").optNumber();
    if (e.quality && !(*e.quality >= 1 && *e.quality <= 100)) {
        c.fail("option 'quality' must be between 1 and 100");
    }
    e.lossless = o.field("lossless").boolean(false);
    e.progressive = o.field("progressive").boolean(false);
    e.strip = o.field("strip").boolean(false);
    e.background = o.field("background").color("white");
    readDefines(o.field("define"), e.defines);
    return e;
}

bool hasAlpha(MagickWand* w) {
    return MagickGetImageAlphaChannel(w) != MagickFalse;
}

// The bytes of "source" in "format", leaving "source" untouched.
std::string encode(const Context& c, MagickWand* source, std::string format, const EncodeOptions& options) {
    format = lowercase(format);
    if (!isAlnum(format)) {
        c.fail("'" + format + "' is not an image format name");
    }
    // A clone shares the pixels until something changes them.
    Wand w(CloneMagickWand(source));

    // Formats without transparency get the image on a background instead of
    // whatever color the transparent pixels happen to have.
    static constexpr std::string_view kOpaque[] = {"jpg", "jpeg", "jpe", "jfif", "bmp", "bmp3", "ppm", "pnm", "pgm"};
    const bool opaque = std::find(std::begin(kOpaque), std::end(kOpaque), format) != std::end(kOpaque);
    first(w.get());
    if (opaque && hasAlpha(w.get())) {
        eachFrame(c, w.get(), [&] {
            MagickSetImageBackgroundColor(w.get(), options.background.get());
            return MagickSetImageAlphaChannel(w.get(), static_cast<im::AlphaOption>(RemoveAlphaChannel));
        });
    }

    for (const auto& [key, value] : options.defines) {
        MagickSetOption(w.get(), key.c_str(), value.c_str());
    }
    if (options.lossless) {
        MagickSetOption(w.get(), "webp:lossless", "true");
        MagickSetOption(w.get(), "jxl:lossless", "true");
        MagickSetOption(w.get(), "heic:lossless", "true");
    }
    MagickSetFormat(w.get(), format.c_str());
    eachFrame(c, w.get(), [&] {
        if (options.quality) {
            MagickSetImageCompressionQuality(w.get(), static_cast<size_t>(std::lround(*options.quality)));
        }
        if (options.progressive) {
            MagickSetImageInterlaceScheme(w.get(), PlaneInterlace);
        }
        if (options.strip) {
            MagickStripImage(w.get());
        }
        return MagickSetImageFormat(w.get(), format.c_str());
    });

    size_t length = 0;
    MagickResetIterator(w.get());
    unsigned char* data = MagickGetImagesBlob(w.get(), &length);
    if (data == nullptr || length == 0) {
        if (data) {
            MagickRelinquishMemory(data);
        }
        c.magick(w.get());
    }
    std::string bytes(reinterpret_cast<const char*>(data), length);
    MagickRelinquishMemory(data);
    return bytes;
}

// Canvases //////////////////////////////////////////////////////////////////

Wand canvas(const Context& c, size_t width, size_t height, const PixelWand* background) {
    Wand w(NewMagickWand());
    c.check(MagickNewImage(w.get(), width, height, background), w.get());
    return w;
}

Pixel colorNamed(const char* name) {
    Pixel p(NewPixelWand());
    PixelSetColor(p.get(), name);
    return p;
}

// Multiplies every pixel's alpha by "amount" (ImageMagick's dissolve).
Wand faded(const Context& c, MagickWand* frame, double amount) {
    const size_t width = MagickGetImageWidth(frame);
    const size_t height = MagickGetImageHeight(frame);
    Wand out = canvas(c, width, height, colorNamed("none").get());
    char percent[32];
    std::snprintf(percent, sizeof(percent), "%g", amount * 100.0);
    MagickSetImageArtifact(out.get(), "compose:args", percent);
    c.check(im::composite(out.get(), frame, DissolveCompositeOp, 0, 0), out.get());
    return out;
}

// Builds without a font list (Homebrew's, for one) can't draw text with no
// font set ("unable to read font `'"), so those get a font file the system is
// known to have. Empty when ImageMagick has fonts of its own.
const std::string& fallbackFont() {
    static const std::string font = [] {
        size_t count = 0;
        char** fonts = MagickQueryFonts("*", &count);
        if (fonts) {
            for (size_t i = 0; i < count; ++i) {
                MagickRelinquishMemory(fonts[i]);
            }
            MagickRelinquishMemory(fonts);
        }
        if (count > 0) {
            return std::string();
        }
        static constexpr const char* kCandidates[] = {
            "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
            "/System/Library/Fonts/Helvetica.ttc",          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",          "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        };
        for (const char* path : kCandidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error)) {
                return std::string(path);
            }
        }
        return std::string();
    }();
    return font;
}

// Drawing styles ////////////////////////////////////////////////////////////

enum class Shape { Filled, Line, Text };

struct Style {
    Drawing draw;
    double angle = 0;           // text only
    double radius = 0;          // rectangles only
    std::optional<Gravity> gravity;
};

// Text and shape options, applied to a fresh DrawingWand.
Style readStyle(const Context& c, int index, Shape shape) {
    Style s;
    s.draw.reset(NewDrawingWand());
    DrawingWand* d = s.draw.get();

    if (shape == Shape::Text) {
        const Options o(c, index,
                        {"color", "size", "font", "family", "weight", "italic", "stroke", "strokeWidth",
                         "gravity", "angle", "background", "antialias", "kerning", "lineSpacing", "wordSpacing"});
        DrawSetFillColor(d, o.field("color").color("black").get());
        DrawSetFontSize(d, o.field("size").number(16, 0.1, 10000));
        if (std::optional<std::string> font = o.field("font").optString()) {
            if (DrawSetFont(d, font->c_str()) == MagickFalse) {
                c.fail("could not use the font '" + *font + "'");
            }
        } else if (!fallbackFont().empty()) {
            DrawSetFont(d, fallbackFont().c_str());
        }
        if (std::optional<std::string> family = o.field("family").optString()) {
            DrawSetFontFamily(d, family->c_str());
        }
        // A weight or style makes ImageMagick search its font list, so they are
        // only set when asked for; the style is always pinned, or the search
        // may settle on an italic face.
        if (!o.field("weight").missing()) {
            DrawSetFontWeight(d, static_cast<size_t>(o.field("weight").number(400, 100, 900)));
        }
        DrawSetFontStyle(d, o.field("italic").boolean(false) ? ItalicStyle : NormalStyle);
        if (!o.field("stroke").missing()) {
            DrawSetStrokeColor(d, o.field("stroke").color().get());
            DrawSetStrokeWidth(d, o.field("strokeWidth").number(1, 0, 10000));
        }
        s.gravity = readGravity(o.field("gravity"));
        if (!s.gravity) {
            s.gravity = Gravity{0, 0};
        }
        DrawSetGravity(d, s.gravity->magick());
        s.angle = o.field("angle").number(0);
        if (!o.field("background").missing()) {
            DrawSetTextUnderColor(d, o.field("background").color().get());
        }
        const bool antialias = o.field("antialias").boolean(true);
        DrawSetTextAntialias(d, antialias ? MagickTrue : MagickFalse);
        DrawSetTextKerning(d, o.field("kerning").number(0));
        DrawSetTextInterlineSpacing(d, o.field("lineSpacing").number(0));
        DrawSetTextInterwordSpacing(d, o.field("wordSpacing").number(0));
        return s;
    }

    const Options o(c, index, {"fill", "stroke", "strokeWidth", "antialias", "radius", "dash", "lineCap", "lineJoin"});
    const bool hasFill = !o.field("fill").missing();
    const bool hasStroke = !o.field("stroke").missing();

    // Defaults: shapes are filled black; an outline alone (stroke, no fill)
    // stays hollow; lines are stroked black.
    if (shape == Shape::Line) {
        DrawSetFillColor(d, o.field("fill").color("none").get());
        DrawSetStrokeColor(d, o.field("stroke").color("black").get());
    } else {
        DrawSetFillColor(d, o.field("fill").color(hasStroke && !hasFill ? "none" : "black").get());
        DrawSetStrokeColor(d, o.field("stroke").color("none").get());
    }
    DrawSetStrokeWidth(d, o.field("strokeWidth").number(1, 0, 10000));
    const MagickBooleanType antialias = o.field("antialias").boolean(true) ? MagickTrue : MagickFalse;
    DrawSetStrokeAntialias(d, antialias);
    s.radius = o.field("radius").number(0, 0, 1e6);

    const Value dash = o.field("dash");
    if (!dash.missing()) {
        if (dash.type() != LUA_TTABLE) {
            dash.expected("an array of numbers");
        }
        std::vector<double> lengths;
        const int n = lua_objlen(c.L, dash.index);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(c.L, dash.index, i);
            const bool ok = lua_type(c.L, -1) == LUA_TNUMBER && lua_tonumber(c.L, -1) >= 0;
            lengths.push_back(lua_tonumber(c.L, -1));
            lua_pop(c.L, 1);
            if (!ok) {
                dash.fail("must be an array of non-negative numbers");
            }
        }
        DrawSetStrokeDashArray(d, lengths.size(), lengths.data());
    }
    if (std::optional<std::string> cap = o.field("lineCap").optString()) {
        DrawSetStrokeLineCap(d, parseEnum<LineCap>(c, MagickLineCapOptions, *cap, "line cap", "lineCap"));
    }
    if (std::optional<std::string> join = o.field("lineJoin").optString()) {
        DrawSetStrokeLineJoin(d, parseEnum<LineJoin>(c, MagickLineJoinOptions, *join, "line join", "lineJoin"));
    }
    return s;
}

// Points: { {x, y}, ... } or { {x = .., y = ..}, ... }.
std::vector<PointInfo> readPoints(const Value& v, int minimum) {
    if (v.type() != LUA_TTABLE) {
        v.expected("an array of points");
    }
    lua_State* L = v.c.L;
    std::vector<PointInfo> points;
    const int n = lua_objlen(L, v.index);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, v.index, i);
        bool ok = lua_type(L, -1) == LUA_TTABLE;
        PointInfo p{0, 0};
        if (ok) {
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            if (lua_type(L, -2) != LUA_TNUMBER) {
                lua_pop(L, 2);
                lua_rawgetfield(L, -1, "x");
                lua_rawgetfield(L, -2, "y");
            }
            ok = lua_type(L, -2) == LUA_TNUMBER && lua_type(L, -1) == LUA_TNUMBER;
            p.x = lua_tonumber(L, -2);
            p.y = lua_tonumber(L, -1);
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        if (!ok) {
            v.fail("must be an array of points ({ x, y } or { x = x, y = y })");
        }
        points.push_back(p);
    }
    if (static_cast<int>(points.size()) < minimum) {
        v.fail("needs at least " + std::to_string(minimum) + " points");
    }
    return points;
}

// Text measurements: [4] width, [5] height, [2] ascent, [3] descent.
struct TextMetrics {
    double width, height, ascent, descent;
};

TextMetrics measure(const Context& c, const DrawingWand* d, const std::string& text) {
    Wand w = canvas(c, 1, 1, colorNamed("none").get());
    double* metrics = MagickQueryMultilineFontMetrics(w.get(), d, text.c_str());
    if (metrics == nullptr) {
        c.magick(w.get());
    }
    const TextMetrics m{metrics[4], metrics[5], metrics[2], metrics[3]};
    MagickRelinquishMemory(metrics);
    return m;
}

// "label:@file" and "caption:@file" would render the file's contents; "\\@"
// is a literal '@' there. (Drawn text never reads files.)
std::string literalLabel(std::string text) {
    if (!text.empty() && text[0] == '@') {
        text.insert(text.begin(), '\\');
    }
    return text;
}

// Module functions //////////////////////////////////////////////////////////

// image.load(path, options?): Image
int imageLoad(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.load"};
        const std::string path = c.arg(1, "path").string();
        const ReadOptions options = readReadOptions(c, 2);
        const std::string data = readFile(c, path);
        Wand w = readBlob(c, data.data(), data.size(), options, extensionOf(path), false);
        pushImage(L, std::move(w));
        return 1;
    });
}

// image.decode(data, options?): Image
int imageDecode(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.decode"};
        const std::string_view data = bytesOf(c.arg(1, "data"));
        const ReadOptions options = readReadOptions(c, 2);
        Wand w = readBlob(c, data.data(), data.size(), options, "", false);
        pushImage(L, std::move(w));
        return 1;
    });
}

// image.info(pathOrData): { width, height, format, frames }
// A string is a path; a buffer holds the image's bytes.
int imageInfo(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.info"};
        const Value source = c.arg(1, "source");
        Wand w;
        if (source.type() == LUA_TBUFFER) {
            const std::string_view data = bytesOf(source);
            w = readBlob(c, data.data(), data.size(), {}, "", true);
        } else {
            const std::string path = source.string();
            const std::string data = readFile(c, path);
            w = readBlob(c, data.data(), data.size(), {}, extensionOf(path), true);
        }
        first(w.get());
        lua_createtable(L, 0, 4);
        setNumber(L, "width", static_cast<double>(MagickGetImageWidth(w.get())));
        setNumber(L, "height", static_cast<double>(MagickGetImageHeight(w.get())));
        setNumber(L, "frames", static_cast<double>(frameCount(w.get())));
        const std::string format = formatOf(w.get());
        lua_pushlstring(L, format.data(), format.size());
        lua_setfield(L, -2, "format");
        return 1;
    });
}

// image.new(width, height, background?): Image
int imageNew(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.new"};
        const size_t width = c.arg(1, "width").size();
        const size_t height = c.arg(2, "height").size();
        const Pixel background = c.arg(3, "background").color("none");
        pushImage(L, canvas(c, width, height, background.get()));
        return 1;
    });
}

// image.generate(spec, width?, height?): Image
int imageGenerate(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.generate"};
        const std::string spec = c.arg(1, "spec").string();
        const std::optional<double> width = c.arg(2, "width").optNumber();
        const std::optional<double> height = c.arg(3, "height").optNumber();

        // Only coders that make pixels out of nothing; the rest would read files.
        static constexpr std::string_view kAllowed[] = {
            "xc", "canvas", "gradient", "radial-gradient", "plasma", "pattern", "label", "caption",
            "hald", "rose", "logo", "wizard", "granite", "netscape",
        };
        const std::size_t colon = spec.find(':');
        const std::string coder = lowercase(spec.substr(0, colon == std::string::npos ? spec.size() : colon));
        if (colon == std::string::npos ||
            std::find(std::begin(kAllowed), std::end(kAllowed), coder) == std::end(kAllowed)) {
            std::string list;
            for (const std::string_view name : kAllowed) {
                list += (list.empty() ? "" : ", ") + std::string(name) + ":";
            }
            c.fail("spec must start with one of " + list + " (got '" + spec + "')");
        }
        const std::string rest = spec.substr(colon + 1);

        Wand w(NewMagickWand());
        if (width || height) {
            const size_t columns = width ? c.arg(2, "width").size() : 0;
            const size_t rows = height ? c.arg(3, "height").size() : 0;
            MagickSetSize(w.get(), columns, rows);
        }
        if ((coder == "label" || coder == "caption") && !fallbackFont().empty()) {
            MagickSetFont(w.get(), fallbackFont().c_str());
        }
        const std::string full = coder + ":" + ((coder == "label" || coder == "caption") ? literalLabel(rest) : rest);
        c.check(MagickReadImage(w.get(), full.c_str()), w.get());
        pushImage(L, std::move(w));
        return 1;
    });
}

// Channel layouts for raw pixels: "rgba" -> "RGBA", "gray" -> "I".
std::string channelMap(const Context& c, std::string channels) {
    channels = lowercase(channels);
    if (channels == "gray" || channels == "grey") {
        return "I";
    }
    if (channels == "graya" || channels == "greya") {
        return "IA";
    }
    std::string map;
    for (const char ch : channels) {
        if (std::string_view("rgbacmyk").find(ch) == std::string_view::npos) {
            c.fail("channels must be letters from \"rgbacmyk\" (like \"rgba\" or \"bgr\") or \"gray\", got '" +
                   channels + "'");
        }
        map.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (map.empty() || map.size() > 5) {
        c.fail("channels must have from 1 to 5 letters");
    }
    return map;
}

// image.fromPixels(width, height, data, channels?): Image
int imageFromPixels(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.fromPixels"};
        const size_t width = c.arg(1, "width").size();
        const size_t height = c.arg(2, "height").size();
        const std::string_view data = bytesOf(c.arg(3, "data"));
        const std::string map = channelMap(c, c.arg(4, "channels").optString().value_or("rgba"));

        const unsigned long long expected = static_cast<unsigned long long>(width) * height * map.size();
        if (data.size() != expected) {
            c.fail("data has " + std::to_string(data.size()) + " bytes, but " + std::to_string(width) + "x" +
                   std::to_string(height) + " " + map + " pixels need " + std::to_string(expected));
        }
        const bool alpha = map.find('A') != std::string::npos;
        Wand w = canvas(c, width, height, colorNamed(alpha ? "none" : "black").get());
        c.check(MagickImportImagePixels(w.get(), 0, 0, width, height, map.c_str(), CharPixel, data.data()), w.get());
        pushImage(L, std::move(w));
        return 1;
    });
}

// image.measureText(text, style?): { width, height, ascent, descent }
int imageMeasureText(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.measureText"};
        const std::string text = c.arg(1, "text").string();
        const Style style = readStyle(c, 2, Shape::Text);
        const TextMetrics m = measure(c, style.draw.get(), text);
        lua_createtable(L, 0, 4);
        setNumber(L, "width", m.width);
        setNumber(L, "height", m.height);
        setNumber(L, "ascent", m.ascent);
        setNumber(L, "descent", m.descent);
        return 1;
    });
}

void annotate(const Context& c, MagickWand* w, const Style& style, double x, double y, const std::string& text) {
    eachFrame(c, w, [&] { return MagickAnnotateImage(w, style.draw.get(), x, y, style.angle, text.c_str()); });
}

// image.text(text, style?): Image. Style also takes background (the canvas
// color, transparent by default) and padding (pixels around the text).
int imageText(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.text"};
        const std::string text = c.arg(1, "text").string();

        // background and padding belong to the canvas, the rest to the text.
        Pixel background = colorNamed("none");
        double padding = 0;
        if (lua_type(L, 2) == LUA_TTABLE) {
            lua_createtable(L, 0, 0);
            const int copy = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, 2) != 0) {
                const bool canvasKey = lua_type(L, -2) == LUA_TSTRING &&
                                       (std::strcmp(lua_tostring(L, -2), "background") == 0 ||
                                        std::strcmp(lua_tostring(L, -2), "padding") == 0);
                if (canvasKey) {
                    lua_pop(L, 1);
                } else {
                    lua_pushvalue(L, -2);
                    lua_insert(L, -2);
                    lua_rawset(L, copy);
                }
            }
            lua_rawgetfield(L, 2, "background");
            background = Value{c, lua_gettop(L), "option 'background'"}.color("none");
            lua_pop(L, 1);
            lua_rawgetfield(L, 2, "padding");
            padding = Value{c, lua_gettop(L), "option 'padding'"}.number(0, 0, 10000);
            lua_pop(L, 1);
            lua_replace(L, 2);
        }
        Style style = readStyle(c, 2, Shape::Text);
        style.gravity = Gravity{0, 0};
        DrawSetGravity(style.draw.get(), NorthWestGravity);

        const TextMetrics m = measure(c, style.draw.get(), text);
        const auto side = [&](double extent) {
            return static_cast<size_t>(std::clamp(std::ceil(extent + 2 * padding), 1.0, double(kMaxDimension)));
        };
        Wand w = canvas(c, side(m.width), side(m.height), background.get());
        annotate(c, w.get(), style, padding, padding, text);
        pushImage(L, std::move(w));
        return 1;
    });
}

// Collects the Images in the array at "index".
std::vector<MagickWand*> readImages(const Context& c, int index, const char* what) {
    if (lua_type(c.L, index) != LUA_TTABLE) {
        c.fail(std::string(what) + " must be an array of Images");
    }
    std::vector<MagickWand*> wands;
    const int n = lua_objlen(c.L, index);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(c.L, index, i);
        const std::string name = std::string(what) + "[" + std::to_string(i) + "]";
        wands.push_back(c.image(lua_gettop(c.L), name.c_str()).wand);
        lua_pop(c.L, 1);
    }
    if (wands.empty()) {
        c.fail(std::string(what) + " is empty");
    }
    return wands;
}

size_t centiseconds(const Context& c, double milliseconds) {
    if (!(milliseconds >= 0 && milliseconds <= 1e9)) {
        c.fail("delay must be between 0 and 1e9 milliseconds");
    }
    return static_cast<size_t>(std::lround(milliseconds / 10.0));
}

// image.animate(frames, options?): Image  options: delay (ms), loop (0 = forever)
int imageAnimate(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.animate"};
        const std::vector<MagickWand*> frames = readImages(c, 1, "frames");
        const Options o(c, 2, {"delay", "loop"});
        const std::optional<double> delay = o.field("delay").optNumber();
        const double loop = o.field("loop").number(0, 0, 65535);

        Wand out(NewMagickWand());
        for (MagickWand* frame : frames) {
            if (frameCount(out.get()) > 0) {
                MagickSetLastIterator(out.get());
            }
            c.check(MagickAddImage(out.get(), frame), out.get());
        }
        eachFrame(c, out.get(), [&] {
            if (delay) {
                MagickSetImageDelay(out.get(), centiseconds(c, *delay));
            }
            return MagickSetImageIterations(out.get(), static_cast<size_t>(loop));
        });
        pushImage(L, std::move(out));
        return 1;
    });
}

struct Placed {
    MagickWand* frame;
    double x, y;
};

Wand compose(const Context& c, size_t width, size_t height, const PixelWand* background,
             const std::vector<Placed>& parts) {
    Wand out = canvas(c, width, height, background);
    for (const Placed& p : parts) {
        c.check(im::composite(out.get(), p.frame, OverCompositeOp, std::lround(p.x), std::lround(p.y)), out.get());
    }
    return out;
}

// image.append(images, options?): Image
// options: vertical, gap (pixels), background, align ("start" | "center" | "end")
int imageAppend(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.append"};
        const std::vector<MagickWand*> images = readImages(c, 1, "images");
        const Options o(c, 2, {"vertical", "gap", "background", "align"});
        const bool vertical = o.field("vertical").boolean(false);
        const double gap = o.field("gap").number(0, 0, kMaxDimension);
        const Pixel background = o.field("background").color("none");
        const std::string align = lowercase(o.field("align").optString().value_or("start"));
        int where = 0;
        if (align == "center" || align == "middle") {
            where = 1;
        } else if (align == "end") {
            where = 2;
        } else if (align != "start") {
            c.fail("option 'align' must be \"start\", \"center\" or \"end\"");
        }

        std::vector<Wand> firsts;
        double along = 0, across = 0;
        for (MagickWand* w : images) {
            firsts.push_back(firstFrame(c, w));
            const double width = static_cast<double>(MagickGetImageWidth(firsts.back().get()));
            const double height = static_cast<double>(MagickGetImageHeight(firsts.back().get()));
            along += (vertical ? height : width) + (firsts.size() > 1 ? gap : 0);
            across = std::max(across, vertical ? width : height);
        }
        std::vector<Placed> parts;
        double at = 0;
        for (const Wand& w : firsts) {
            const double width = static_cast<double>(MagickGetImageWidth(w.get()));
            const double height = static_cast<double>(MagickGetImageHeight(w.get()));
            const double offset = Gravity::place(where, across, vertical ? width : height, 0);
            parts.push_back(vertical ? Placed{w.get(), offset, at} : Placed{w.get(), at, offset});
            at += (vertical ? height : width) + gap;
        }
        const auto total = [](double v) { return static_cast<size_t>(std::clamp(v, 1.0, double(kMaxDimension))); };
        Wand out = vertical ? compose(c, total(across), total(along), background.get(), parts)
                            : compose(c, total(along), total(across), background.get(), parts);
        pushImage(L, std::move(out));
        return 1;
    });
}

// image.grid(images, options?): Image
// options: columns (default: a square-ish grid), gap, background, cell = { width, height }
int imageGrid(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.grid"};
        const std::vector<MagickWand*> images = readImages(c, 1, "images");
        const Options o(c, 2, {"columns", "gap", "background", "cellWidth", "cellHeight"});
        const size_t count = images.size();
        size_t columns = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
        if (!o.field("columns").missing()) {
            columns = std::min(o.field("columns").size(), count);
        }
        const size_t rows = (count + columns - 1) / columns;
        const double gap = o.field("gap").number(0, 0, kMaxDimension);
        const Pixel background = o.field("background").color("none");

        std::vector<Wand> firsts;
        double cellWidth = 0, cellHeight = 0;
        for (MagickWand* w : images) {
            firsts.push_back(firstFrame(c, w));
            cellWidth = std::max(cellWidth, static_cast<double>(MagickGetImageWidth(firsts.back().get())));
            cellHeight = std::max(cellHeight, static_cast<double>(MagickGetImageHeight(firsts.back().get())));
        }
        if (!o.field("cellWidth").missing()) {
            cellWidth = static_cast<double>(o.field("cellWidth").size());
        }
        if (!o.field("cellHeight").missing()) {
            cellHeight = static_cast<double>(o.field("cellHeight").size());
        }

        // Images bigger than their cell are scaled down to fit it.
        std::vector<Placed> parts;
        for (size_t i = 0; i < firsts.size(); ++i) {
            MagickWand* w = firsts[i].get();
            double width = static_cast<double>(MagickGetImageWidth(w));
            double height = static_cast<double>(MagickGetImageHeight(w));
            const double scale = std::min({1.0, cellWidth / width, cellHeight / height});
            if (scale < 1.0) {
                width = std::max(1.0, std::round(width * scale));
                height = std::max(1.0, std::round(height * scale));
                c.check(im::resize(w, static_cast<size_t>(width), static_cast<size_t>(height), UndefinedFilter), w);
            }
            const double x = static_cast<double>(i % columns) * (cellWidth + gap) + (cellWidth - width) / 2.0;
            const double y = static_cast<double>(i / columns) * (cellHeight + gap) + (cellHeight - height) / 2.0;
            parts.push_back({w, x, y});
        }
        const auto total = [](double v) { return static_cast<size_t>(std::clamp(v, 1.0, double(kMaxDimension))); };
        Wand out = compose(c, total(columns * cellWidth + (columns - 1) * gap),
                           total(rows * cellHeight + (rows - 1) * gap), background.get(), parts);
        pushImage(L, std::move(out));
        return 1;
    });
}

constexpr size_t kTerminated = static_cast<size_t>(-1);

// Pushes the "count" names in "list" (or up to a null entry, with
// kTerminated) as a sorted array, without duplicates.
void pushNames(lua_State* L, char** list, size_t count, bool lower, bool skipUndefined) {
    std::vector<std::string> names;
    for (size_t i = 0; list && (count == kTerminated ? list[i] != nullptr : i < count); ++i) {
        std::string name = lower ? lowercase(list[i]) : std::string(list[i]);
        if (!(skipUndefined && lowercase(name) == "undefined")) {
            names.push_back(std::move(name));
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return lowercase(a) < lowercase(b);
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushlstring(L, names[i].data(), names[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

void relinquishList(char** list, size_t count) {
    if (list == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        MagickRelinquishMemory(list[i]);
    }
    MagickRelinquishMemory(list);
}

// image.formats(): { string }  every format this ImageMagick reads or writes
int imageFormats(lua_State* L) {
    return run(L, [&] {
        size_t count = 0;
        char** formats = MagickQueryFormats("*", &count);
        if (formats) {
            pushNames(L, formats, count, true, false);
        } else {
            lua_createtable(L, 0, 0);
        }
        relinquishList(formats, count);
        return 1;
    });
}

// image.fonts(): { string }  font names usable as style.font
int imageFonts(lua_State* L) {
    return run(L, [&] {
        size_t count = 0;
        char** fonts = MagickQueryFonts("*", &count);
        if (fonts) {
            pushNames(L, fonts, count, false, false);
        } else {
            lua_createtable(L, 0, 0);
        }
        relinquishList(fonts, count);
        return 1;
    });
}

// image.options(kind): { string }  the names ImageMagick accepts for "kind"
int imageOptions(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.options"};
        const std::string kind = lowercase(c.arg(1, "kind").string());
        for (const OptionKind& k : kOptionKinds) {
            if (kind == k.name) {
                char** list = GetCommandOptions(k.kind);
                pushNames(L, list, kTerminated, false, true);
                if (list) {
                    DestroyStringList(list);
                }
                return 1;
            }
        }
        std::string known;
        for (const OptionKind& k : kOptionKinds) {
            known += (known.empty() ? "" : ", ") + std::string(k.name);
        }
        c.fail("unknown kind '" + kind + "' (known: " + known + ")");
    });
}

// image.color(color): Color   parses any color into { r, g, b, a }
int imageColor(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.color"};
        const Pixel p = c.arg(1, "color").color();
        pushColor(L, p.get());
        return 1;
    });
}

// image.hex(color): string   "#rrggbb", or "#rrggbbaa" when not opaque
int imageHex(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.hex"};
        const Pixel p = c.arg(1, "color").color();
        const std::string hex = hexOf(p.get());
        lua_pushlstring(L, hex.data(), hex.size());
        return 1;
    });
}

struct Limit {
    const char* name;
    ResourceType type;
};

constexpr Limit kLimits[] = {
    {"memory", MemoryResource}, {"disk", DiskResource},     {"map", MapResource},
    {"area", AreaResource},     {"width", WidthResource},   {"height", HeightResource},
    {"threads", ThreadResource}, {"time", TimeResource},    {"files", FileResource},
};

// image.limits(limits?): Limits   sets (some of) ImageMagick's resource limits,
// returns all of them. memory, disk, map: bytes; area: pixels; time: seconds.
int imageLimits(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "image.limits"};
        const Options o(c, 1, {"memory", "disk", "map", "area", "width", "height", "threads", "time", "files"});
        for (const Limit& limit : kLimits) {
            const std::optional<double> value = o.field(limit.name).optNumber();
            if (value) {
                if (*value < 0 || *value > 9.0e18) {
                    c.fail(std::string("option '") + limit.name + "' must be a non-negative number");
                }
                MagickSetResourceLimit(limit.type, static_cast<MagickSizeType>(*value));
            }
        }
        lua_createtable(L, 0, static_cast<int>(std::size(kLimits)));
        for (const Limit& limit : kLimits) {
            setNumber(L, limit.name, static_cast<double>(MagickGetResourceLimit(limit.type)));
        }
        return 1;
    });
}

// Image: fields and conversion ////////////////////////////////////////////

// img:clone(): Image
int imgClone(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:clone"};
        Image& img = c.image(1);
        pushImage(L, c.check(CloneMagickWand(img.wand), img.wand));
        return 1;
    });
}

// img:close()  frees the pixels now instead of at garbage collection.
int imgClose(lua_State* L) {
    const Context c{L, "Image:close"};
    if (lua_type(L, 1) == LUA_TUSERDATA) {
        if (lua_getmetatable(L, 1)) {
            luaL_getmetatable(L, kImageMeta);
            const bool ours = lua_rawequal(L, -1, -2) != 0;
            lua_pop(L, 2);
            if (ours) {
                static_cast<Image*>(lua_touserdata(L, 1))->close();
                return 0;
            }
        }
    }
    return run(L, [&]() -> int { c.image(1); return 0; });
}

// img:save(path, options?): Image  The format comes from options.format,
// else the extension, else the image's own format.
int imgSave(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:save"};
        Image& img = c.image(1);
        const std::string path = c.arg(2, "path").string();
        lua_settop(L, 3);
        std::optional<std::string> format;
        if (lua_type(L, 3) == LUA_TTABLE) {
            lua_rawgetfield(L, 3, "format");
            format = Value{c, lua_gettop(L), "option 'format'"}.optString();
            lua_pop(L, 1);
            // The remaining options are the encoder's.
            lua_createtable(L, 0, 0);
            lua_pushnil(L);
            while (lua_next(L, 3) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && std::strcmp(lua_tostring(L, -2), "format") == 0) {
                    lua_pop(L, 1);
                    continue;
                }
                lua_pushvalue(L, -2);
                lua_insert(L, -2);
                lua_rawset(L, 4);
            }
            lua_replace(L, 3);
        }
        const EncodeOptions options = readEncodeOptions(c, 3);
        if (!format) {
            format = extensionOf(path);
        }
        if (format->empty()) {
            format = formatOf(img.wand);
        }
        if (format->empty()) {
            c.fail("can't tell the format of '" + path + "'; give it an extension or options.format");
        }
        const std::string bytes = encode(c, img.wand, *format, options);
        writeFile(c, path, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        return self(L);
    });
}

// img:encode(format?, options?): string
int imgEncode(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:encode"};
        Image& img = c.image(1);
        std::string format = c.arg(2, "format").optString().value_or("");
        const EncodeOptions options = readEncodeOptions(c, 3);
        if (format.empty()) {
            format = formatOf(img.wand);
        }
        if (format.empty()) {
            format = "png";
        }
        const std::string bytes = encode(c, img.wand, format, options);
        lua_pushlstring(L, bytes.data(), bytes.size());
        return 1;
    });
}

struct Region {
    ssize_t x = 0, y = 0;
    size_t width = 0, height = 0;
};

// A rectangle inside the first frame, from options x, y, width, height.
Region readRegion(const Context& c, const Options& o, MagickWand* w) {
    first(w);
    const size_t imageWidth = MagickGetImageWidth(w);
    const size_t imageHeight = MagickGetImageHeight(w);
    Region r;
    r.x = o.field("x").missing() ? 0 : o.field("x").coordinate();
    r.y = o.field("y").missing() ? 0 : o.field("y").coordinate();
    if (r.x < 0 || r.y < 0 || static_cast<size_t>(r.x) >= imageWidth || static_cast<size_t>(r.y) >= imageHeight) {
        c.fail("the region starts outside the image");
    }
    r.width = o.field("width").missing() ? imageWidth - static_cast<size_t>(r.x) : o.field("width").size();
    r.height = o.field("height").missing() ? imageHeight - static_cast<size_t>(r.y) : o.field("height").size();
    if (static_cast<size_t>(r.x) + r.width > imageWidth || static_cast<size_t>(r.y) + r.height > imageHeight) {
        c.fail("the region ends outside the image");
    }
    return r;
}

// img:pixels(options?): buffer   8-bit samples, row by row, of the first frame
// options: channels ("rgba"), x, y, width, height
int imgPixels(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:pixels"};
        Image& img = c.image(1);
        const Options o(c, 2, {"channels", "x", "y", "width", "height"});
        const std::string map = channelMap(c, o.field("channels").optString().value_or("rgba"));
        const Region r = readRegion(c, o, img.wand);
        const unsigned long long length = static_cast<unsigned long long>(r.width) * r.height * map.size();
        if (length > (1ull << 31)) {
            c.fail("the region is too big for one buffer");
        }
        void* data = lua_newbuffer(L, static_cast<size_t>(length));
        first(img.wand);
        c.check(MagickExportImagePixels(img.wand, r.x, r.y, r.width, r.height, map.c_str(), CharPixel, data),
                img.wand);
        return 1;
    });
}

// The first frame's pixel at (x, y), checked to be inside it.
void checkPoint(const Context& c, MagickWand* w, ssize_t x, ssize_t y) {
    first(w);
    if (x < 0 || y < 0 || static_cast<size_t>(x) >= MagickGetImageWidth(w) ||
        static_cast<size_t>(y) >= MagickGetImageHeight(w)) {
        c.fail("(" + std::to_string(x) + ", " + std::to_string(y) + ") is outside the " +
               std::to_string(MagickGetImageWidth(w)) + "x" + std::to_string(MagickGetImageHeight(w)) + " image");
    }
}

// img:getPixel(x, y): Color
int imgGetPixel(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:getPixel"};
        Image& img = c.image(1);
        const ssize_t x = c.arg(2, "x").coordinate();
        const ssize_t y = c.arg(3, "y").coordinate();
        checkPoint(c, img.wand, x, y);
        Pixel p(NewPixelWand());
        c.check(MagickGetImagePixelColor(img.wand, x, y, p.get()), img.wand);
        pushColor(L, p.get());
        return 1;
    });
}

// img:setPixel(x, y, color): Image   (every frame)
int imgSetPixel(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:setPixel"};
        Image& img = c.image(1);
        const ssize_t x = c.arg(2, "x").coordinate();
        const ssize_t y = c.arg(3, "y").coordinate();
        const Pixel color = c.arg(4, "color").color();
        checkPoint(c, img.wand, x, y);
        const double rgba[4] = {PixelGetRed(color.get()), PixelGetGreen(color.get()), PixelGetBlue(color.get()),
                                PixelGetAlpha(color.get())};
        eachFrame(c, img.wand, [&] {
            if (rgba[3] < 1.0 && !hasAlpha(img.wand)) {
                MagickSetImageAlphaChannel(img.wand, static_cast<im::AlphaOption>(OpaqueAlphaChannel));
            }
            const char* map = hasAlpha(img.wand) ? "RGBA" : "RGB";
            return MagickImportImagePixels(img.wand, x, y, 1, 1, map, DoublePixel, rgba);
        });
        return self(L);
    });
}

// img:frame(index): Image   a copy of one frame (1-based)
int imgFrame(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:frame"};
        Image& img = c.image(1);
        const size_t index = c.arg(2, "index").size();
        if (index > frameCount(img.wand)) {
            c.fail("index " + std::to_string(index) + " is past the last frame (" +
                   std::to_string(frameCount(img.wand)) + ")");
        }
        MagickSetIteratorIndex(img.wand, static_cast<ssize_t>(index - 1));
        Wand frame = c.check(MagickGetImage(img.wand), img.wand);
        MagickResetIterator(img.wand);
        pushImage(L, std::move(frame));
        return 1;
    });
}

// img:metadata(pattern?): { [string]: string }   EXIF and other properties
int imgMetadata(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:metadata"};
        Image& img = c.image(1);
        const std::string pattern = c.arg(2, "pattern").optString().value_or("*");
        first(img.wand);
        // EXIF is parsed lazily; asking for it once makes it show up below.
        MagickString(MagickGetImageProperty(img.wand, "exif:*"));
        size_t count = 0;
        char** names = MagickGetImageProperties(img.wand, pattern.c_str(), &count);
        lua_createtable(L, 0, static_cast<int>(count));
        for (size_t i = 0; names && i < count; ++i) {
            MagickString value(MagickGetImageProperty(img.wand, names[i]));
            if (value.text) {
                lua_pushstring(L, value.text);
                lua_setfield(L, -2, names[i]);
            }
        }
        relinquishList(names, count);
        MagickClearException(img.wand);
        return 1;
    });
}

// img:setMetadata(name, value?): Image   nil removes it
int imgSetMetadata(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:setMetadata"};
        Image& img = c.image(1);
        const std::string name = c.arg(2, "name").string();
        const std::optional<std::string> value = c.arg(3, "value").optString();
        eachFrame(c, img.wand, [&] {
            if (value) {
                return MagickSetImageProperty(img.wand, name.c_str(), value->c_str());
            }
            MagickDeleteImageProperty(img.wand, name.c_str());
            return MagickTrue;
        });
        return self(L);
    });
}

// img:strip(): Image   removes profiles and comments (EXIF, ICC, ...)
int imgStrip(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:strip"};
        Image& img = c.image(1);
        eachFrame(c, img.wand, [&] { return MagickStripImage(img.wand); });
        return self(L);
    });
}

// img:signature(): string   SHA-256 of the first frame's pixels
int imgSignature(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:signature"};
        Image& img = c.image(1);
        // ImageMagick hashes the pixels as stored: at the image's own depth,
        // with or without an alpha channel. So the same pixels read back from a
        // 2-bit PNG would hash differently without normalizing both.
        Wand w = firstFrame(c, img.wand);
        c.check(MagickSetImageDepth(w.get(), 16), w.get());
        c.check(MagickSetImageAlphaChannel(w.get(), static_cast<im::AlphaOption>(SetAlphaChannel)), w.get());
        const std::string signature = MagickString(MagickGetImageSignature(w.get())).str();
        if (signature.empty()) {
            c.magick(w.get());
        }
        lua_pushlstring(L, signature.data(), signature.size());
        return 1;
    });
}

// img:resolution(): (number, number)   pixels per inch
int imgResolution(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:resolution"};
        Image& img = c.image(1);
        first(img.wand);
        double x = 0, y = 0;
        c.check(MagickGetImageResolution(img.wand, &x, &y), img.wand);
        if (MagickGetImageUnits(img.wand) == PixelsPerCentimeterResolution) {
            x *= 2.54;
            y *= 2.54;
        }
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        return 2;
    });
}

// img:setResolution(x, y?): Image   pixels per inch
int imgSetResolution(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:setResolution"};
        Image& img = c.image(1);
        const double x = c.arg(2, "x").number(0, 0.01, 1e6);
        const double y = c.arg(3, "y").number(x, 0.01, 1e6);
        eachFrame(c, img.wand, [&] {
            MagickSetImageUnits(img.wand, PixelsPerInchResolution);
            return MagickSetImageResolution(img.wand, x, y);
        });
        return self(L);
    });
}

// Image: geometry ///////////////////////////////////////////////////////////

enum class Fit { Fill, Contain, Cover };

// resize / thumbnail: (width?, height?, options?)
int resizeLike(lua_State* L, const char* name, bool thumbnail) {
    return run(L, [&] {
        const Context c{L, name};
        Image& img = c.image(1);
        const std::optional<double> width = c.arg(2, "width").optNumber();
        const std::optional<double> height = c.arg(3, "height").optNumber();
        if (!width && !height) {
            c.fail("give a width, a height, or both");
        }
        if (width) c.arg(2, "width").size();
        if (height) c.arg(3, "height").size();

        const Options o(c, 4, {"fit", "filter", "gravity", "enlarge"});
        const std::string fitName = lowercase(o.field("fit").optString().value_or(thumbnail ? "contain" : "fill"));
        Fit fit = Fit::Fill;
        if (fitName == "contain" || fitName == "inside") {
            fit = Fit::Contain;
        } else if (fitName == "cover") {
            fit = Fit::Cover;
        } else if (fitName != "fill") {
            c.fail("option 'fit' must be \"fill\", \"contain\" or \"cover\"");
        }
        im::Filter filter = UndefinedFilter;
        if (std::optional<std::string> name = o.field("filter").optString()) {
            filter = parseEnum<im::Filter>(c, MagickFilterOptions, *name, "filter", "filter");
        }
        const Gravity gravity = readGravity(o.field("gravity")).value_or(Gravity{1, 1});
        const bool enlarge = o.field("enlarge").boolean(!thumbnail);

        eachFrame(c, img.wand, [&] {
            const double w0 = static_cast<double>(MagickGetImageWidth(img.wand));
            const double h0 = static_cast<double>(MagickGetImageHeight(img.wand));
            constexpr double inf = std::numeric_limits<double>::infinity();
            double sx = width ? *width / w0 : inf;
            double sy = height ? *height / h0 : inf;
            if (!width) sx = sy;
            if (!height) sy = sx;
            if (fit == Fit::Contain) {
                sx = sy = std::min(sx, sy);
            } else if (fit == Fit::Cover && width && height) {
                sx = sy = std::max(sx, sy);
            }
            if (!enlarge) {
                sx = std::min(sx, 1.0);
                sy = std::min(sy, 1.0);
            }
            const auto side = [](double v) {
                return static_cast<size_t>(std::clamp(std::round(v), 1.0, double(kMaxDimension)));
            };
            const size_t tw = side(w0 * sx);
            const size_t th = side(h0 * sy);
            MagickBooleanType ok = MagickTrue;
            if (tw != static_cast<size_t>(w0) || th != static_cast<size_t>(h0)) {
                ok = thumbnail ? MagickThumbnailImage(img.wand, tw, th) : im::resize(img.wand, tw, th, filter);
            }
            if (ok && fit == Fit::Cover && width && height) {
                const size_t cw = std::min(tw, static_cast<size_t>(*width));
                const size_t ch = std::min(th, static_cast<size_t>(*height));
                const ssize_t x = std::lround(gravity.x(double(tw), double(cw)));
                const ssize_t y = std::lround(gravity.y(double(th), double(ch)));
                ok = MagickCropImage(img.wand, cw, ch, x, y);
                repage(img.wand);
            }
            return ok;
        });
        return self(L);
    });
}

// img:resize(width?, height?, options?): Image
// With one size the other keeps the aspect ratio. options.fit: "fill" (exactly
// width x height), "contain" (fit inside), "cover" (fill, then crop the rest
// away around options.gravity); filter; enlarge (false: never upscale).
int imgResize(lua_State* L) {
    return resizeLike(L, "Image:resize", false);
}

// img:thumbnail(width?, height?, options?): Image   resize for previews: fast,
// strips metadata, fits inside by default and never enlarges.
int imgThumbnail(lua_State* L) {
    return resizeLike(L, "Image:thumbnail", true);
}

// img:scale(factor, factorY?): Image   0.5 halves, 2 doubles
int imgScale(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:scale"};
        Image& img = c.image(1);
        const double fx = c.arg(2, "factor").number(1, 1e-6, 1e6);
        const double fy = c.arg(3, "factorY").number(fx, 1e-6, 1e6);
        eachFrame(c, img.wand, [&] {
            const auto side = [](double v) {
                return static_cast<size_t>(std::clamp(std::round(v), 1.0, double(kMaxDimension)));
            };
            return im::resize(img.wand, side(double(MagickGetImageWidth(img.wand)) * fx),
                              side(double(MagickGetImageHeight(img.wand)) * fy), UndefinedFilter);
        });
        return self(L);
    });
}

// img:crop(x, y, width, height): Image
int imgCrop(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:crop"};
        Image& img = c.image(1);
        const ssize_t x = c.arg(2, "x").coordinate();
        const ssize_t y = c.arg(3, "y").coordinate();
        const size_t width = c.arg(4, "width").size();
        const size_t height = c.arg(5, "height").size();
        eachFrame(c, img.wand, [&] {
            const ssize_t w0 = static_cast<ssize_t>(MagickGetImageWidth(img.wand));
            const ssize_t h0 = static_cast<ssize_t>(MagickGetImageHeight(img.wand));
            if (x >= w0 || y >= h0 || x + static_cast<ssize_t>(width) <= 0 || y + static_cast<ssize_t>(height) <= 0) {
                c.fail("the crop area is outside the image");
            }
            const MagickBooleanType ok = MagickCropImage(img.wand, width, height, x, y);
            repage(img.wand);
            return ok;
        });
        return self(L);
    });
}

// img:extent(width, height, options?): Image   pads (or crops) to exactly
// width x height. options: gravity ("center"), background ("none").
int imgExtent(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:extent"};
        Image& img = c.image(1);
        const size_t width = c.arg(2, "width").size();
        const size_t height = c.arg(3, "height").size();
        const Options o(c, 4, {"gravity", "background"});
        const Gravity gravity = readGravity(o.field("gravity")).value_or(Gravity{1, 1});
        const Pixel background = o.field("background").color("none");
        eachFrame(c, img.wand, [&] {
            const double w0 = static_cast<double>(MagickGetImageWidth(img.wand));
            const double h0 = static_cast<double>(MagickGetImageHeight(img.wand));
            // Where the canvas starts, in the image's coordinates.
            const ssize_t x = -std::lround(gravity.x(double(width), w0));
            const ssize_t y = -std::lround(gravity.y(double(height), h0));
            MagickSetImageBackgroundColor(img.wand, background.get());
            if (PixelGetAlpha(background.get()) < 1.0 && !hasAlpha(img.wand)) {
                MagickSetImageAlphaChannel(img.wand, static_cast<im::AlphaOption>(OpaqueAlphaChannel));
            }
            const MagickBooleanType ok = MagickExtentImage(img.wand, width, height, x, y);
            repage(img.wand);
            return ok;
        });
        return self(L);
    });
}

// img:trim(fuzz?): Image   removes borders of the corner color
int imgTrim(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:trim"};
        Image& img = c.image(1);
        const double fuzz = c.arg(2, "fuzz").number(0, 0, 1);
        eachFrame(c, img.wand, [&] {
            const MagickBooleanType ok = MagickTrimImage(img.wand, fuzz * kQuantum);
            repage(img.wand);
            return ok;
        });
        return self(L);
    });
}

// img:rotate(degrees, background?): Image   clockwise; the corners get background ("none")
int imgRotate(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:rotate"};
        Image& img = c.image(1);
        const double degrees = c.arg(2, "degrees").number();
        const Pixel background = c.arg(3, "background").color("none");
        eachFrame(c, img.wand, [&] {
            const MagickBooleanType ok = MagickRotateImage(img.wand, background.get(), degrees);
            repage(img.wand);
            return ok;
        });
        return self(L);
    });
}

// Methods with no arguments that run one MagickWand call on every frame.
#define SONATA_IMAGE_SIMPLE(fn, label, call)                                                                          \
    int fn(lua_State* L) {                                                                                             \
        return run(L, [&] {                                                                                            \
            const Context c{L, label};                                                                                 \
            Image& img = c.image(1);                                                                                   \
            eachFrame(c, img.wand, [&] { return call(img.wand); });                                                    \
            return self(L);                                                                                            \
        });                                                                                                            \
    }

SONATA_IMAGE_SIMPLE(imgFlip, "Image:flip", MagickFlipImage)                 // upside down
SONATA_IMAGE_SIMPLE(imgMirror, "Image:mirror", MagickFlopImage)             // left-right
SONATA_IMAGE_SIMPLE(imgTranspose, "Image:transpose", MagickTransposeImage)
SONATA_IMAGE_SIMPLE(imgTransverse, "Image:transverse", MagickTransverseImage)
SONATA_IMAGE_SIMPLE(imgAutoOrient, "Image:autoOrient", MagickAutoOrientImage)
SONATA_IMAGE_SIMPLE(imgNormalize, "Image:normalize", MagickNormalizeImage)
SONATA_IMAGE_SIMPLE(imgEqualize, "Image:equalize", MagickEqualizeImage)
SONATA_IMAGE_SIMPLE(imgAutoLevel, "Image:autoLevel", MagickAutoLevelImage)
SONATA_IMAGE_SIMPLE(imgAutoGamma, "Image:autoGamma", MagickAutoGammaImage)
SONATA_IMAGE_SIMPLE(imgDespeckle, "Image:despeckle", MagickDespeckleImage)

#undef SONATA_IMAGE_SIMPLE

// img:shear(x, y?, background?): Image   degrees
int imgShear(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:shear"};
        Image& img = c.image(1);
        const double x = c.arg(2, "x").number(0, -89.9, 89.9);
        const double y = c.arg(3, "y").number(0, -89.9, 89.9);
        const Pixel background = c.arg(4, "background").color("none");
        eachFrame(c, img.wand, [&] {
            const MagickBooleanType ok = MagickShearImage(img.wand, background.get(), x, y);
            repage(img.wand);
            return ok;
        });
        return self(L);
    });
}

// img:roll(x, y): Image   shifts the pixels, wrapping around
int imgRoll(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:roll"};
        Image& img = c.image(1);
        const ssize_t x = c.arg(2, "x").coordinate();
        const ssize_t y = c.arg(3, "y").coordinate();
        eachFrame(c, img.wand, [&] { return MagickRollImage(img.wand, x, y); });
        return self(L);
    });
}

// img:border(size, color?): Image   adds size pixels on every side
int imgBorder(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:border"};
        Image& img = c.image(1);
        const double size = c.arg(2, "size").number(0, 0, kMaxDimension);
        const Pixel color = c.arg(3, "color").color("black");
        const size_t n = static_cast<size_t>(std::lround(size));
        eachFrame(c, img.wand, [&] { return im::border(img.wand, color.get(), n, n); });
        return self(L);
    });
}

// img:distort(method, arguments, options?): Image   options: bestFit, background
// "perspective", "arc", "barrel", "polar", "srt", ... (image.options("distort"))
int imgDistort(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:distort"};
        Image& img = c.image(1);
        const im::Distort method =
            parseEnum<im::Distort>(c, MagickDistortOptions, c.arg(2, "method").string(), "distortion", "distort");
        const Value args = c.arg(3, "arguments");
        if (args.type() != LUA_TTABLE) {
            args.expected("an array of numbers");
        }
        std::vector<double> numbers;
        const int n = lua_objlen(L, 3);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 3, i);
            const bool ok = lua_type(L, -1) == LUA_TNUMBER;
            numbers.push_back(lua_tonumber(L, -1));
            lua_pop(L, 1);
            if (!ok) {
                args.fail("must be an array of numbers");
            }
        }
        const Options o(c, 4, {"bestFit", "background"});
        const bool bestFit = o.field("bestFit").boolean(false);
        const bool hasBackground = !o.field("background").missing();
        const Pixel background = o.field("background").color("none");
        eachFrame(c, img.wand, [&] {
            if (hasBackground) {
                MagickSetImageBackgroundColor(img.wand, background.get());
                MagickSetImageVirtualPixelMethod(img.wand, BackgroundVirtualPixelMethod);
            }
            const MagickBooleanType ok = MagickDistortImage(img.wand, method, numbers.size(), numbers.data(),
                                                            bestFit ? MagickTrue : MagickFalse);
            repage(img.wand);
            return ok;
        });
        return self(L);
    });
}

// img:liquidRescale(width, height): Image   content-aware resize (seam carving)
int imgLiquidRescale(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:liquidRescale"};
        Image& img = c.image(1);
        const size_t width = c.arg(2, "width").size();
        const size_t height = c.arg(3, "height").size();
        eachFrame(c, img.wand, [&] { return MagickLiquidRescaleImage(img.wand, width, height, 0, 0); });
        return self(L);
    });
}

// img:deskew(threshold?): Image   straightens scanned text (threshold 0-1, default 0.4)
int imgDeskew(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:deskew"};
        Image& img = c.image(1);
        const double threshold = c.arg(2, "threshold").number(0.4, 0, 1);
        eachFrame(c, img.wand, [&] {
            const MagickBooleanType ok = MagickDeskewImage(img.wand, threshold * kQuantum);
            repage(img.wand);
            return ok;
        });
        return self(L);
    });
}

// Image: color and tone /////////////////////////////////////////////////////

// img:grayscale(): Image
int imgGrayscale(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:grayscale"};
        Image& img = c.image(1);
        // Back to sRGB afterwards, so later drawing in color still works.
        eachFrame(c, img.wand, [&] {
            return MagickTransformImageColorspace(img.wand, GRAYColorspace) &&
                           MagickTransformImageColorspace(img.wand, sRGBColorspace)
                       ? MagickTrue
                       : MagickFalse;
        });
        return self(L);
    });
}

// img:negate(): Image
int imgNegate(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:negate"};
        Image& img = c.image(1);
        eachFrame(c, img.wand, [&] { return MagickNegateImage(img.wand, MagickFalse); });
        return self(L);
    });
}

// img:sepia(threshold?): Image   0-1, default 0.8
int imgSepia(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:sepia"};
        Image& img = c.image(1);
        const double threshold = c.arg(2, "threshold").number(0.8, 0, 1);
        eachFrame(c, img.wand, [&] { return MagickSepiaToneImage(img.wand, threshold * kQuantum); });
        return self(L);
    });
}

// img:brightnessContrast(brightness, contrast?): Image   each -100..100
int imgBrightnessContrast(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:brightnessContrast"};
        Image& img = c.image(1);
        const double brightness = c.arg(2, "brightness").number(0, -100, 100);
        const double contrast = c.arg(3, "contrast").number(0, -100, 100);
        eachFrame(c, img.wand, [&] { return MagickBrightnessContrastImage(img.wand, brightness, contrast); });
        return self(L);
    });
}

// img:modulate(brightness?, saturation?, hue?): Image   percentages, 100 = unchanged
int imgModulate(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:modulate"};
        Image& img = c.image(1);
        const double brightness = c.arg(2, "brightness").number(100, 0, 10000);
        const double saturation = c.arg(3, "saturation").number(100, 0, 10000);
        const double hue = c.arg(4, "hue").number(100, 0, 200);
        eachFrame(c, img.wand, [&] { return MagickModulateImage(img.wand, brightness, saturation, hue); });
        return self(L);
    });
}

// img:gamma(value): Image   > 1 brightens, < 1 darkens
int imgGamma(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:gamma"};
        Image& img = c.image(1);
        const double gamma = c.arg(2, "value").number(1, 0.01, 100);
        eachFrame(c, img.wand, [&] { return MagickGammaImage(img.wand, gamma); });
        return self(L);
    });
}

// img:level(black?, white?, gamma?): Image   black/white 0-1: they become the new 0 and 1
int imgLevel(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:level"};
        Image& img = c.image(1);
        const double black = c.arg(2, "black").number(0, 0, 1);
        const double white = c.arg(3, "white").number(1, 0, 1);
        const double gamma = c.arg(4, "gamma").number(1, 0.01, 100);
        eachFrame(c, img.wand, [&] { return MagickLevelImage(img.wand, black * kQuantum, gamma, white * kQuantum); });
        return self(L);
    });
}

// img:contrast(strength?, midpoint?): Image   sigmoidal; strength > 0 adds contrast,
// < 0 removes it (default 3); midpoint 0-1 (0.5)
int imgContrast(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:contrast"};
        Image& img = c.image(1);
        const double strength = c.arg(2, "strength").number(3, -100, 100);
        const double midpoint = c.arg(3, "midpoint").number(0.5, 0, 1);
        if (strength != 0) {
            eachFrame(c, img.wand, [&] {
                return MagickSigmoidalContrastImage(img.wand, strength > 0 ? MagickTrue : MagickFalse,
                                                    std::fabs(strength), midpoint * kQuantum);
            });
        }
        return self(L);
    });
}

// "0.3" -> a PixelWand of rgb(30%,30%,30%), how colorize/tint take their amount.
Pixel blendOf(double amount) {
    char text[96];
    const double percent = amount * 100.0;
    std::snprintf(text, sizeof(text), "rgb(%g%%,%g%%,%g%%)", percent, percent, percent);
    return colorNamed(text);
}

// img:colorize(color, amount?): Image   blends every pixel toward color (0-1, default 0.5)
int imgColorize(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:colorize"};
        Image& img = c.image(1);
        const Pixel color = c.arg(2, "color").color();
        const Pixel blend = blendOf(c.arg(3, "amount").number(0.5, 0, 1));
        eachFrame(c, img.wand, [&] { return MagickColorizeImage(img.wand, color.get(), blend.get()); });
        return self(L);
    });
}

// img:tint(color, amount?): Image   tints the midtones (0-1, default 0.5)
int imgTint(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:tint"};
        Image& img = c.image(1);
        const Pixel color = c.arg(2, "color").color();
        const Pixel blend = blendOf(c.arg(3, "amount").number(0.5, 0, 1));
        eachFrame(c, img.wand, [&] { return MagickTintImage(img.wand, color.get(), blend.get()); });
        return self(L);
    });
}

// img:threshold(value?): Image   pixels above value (0-1, default 0.5) go white, the rest black
int imgThreshold(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:threshold"};
        Image& img = c.image(1);
        const double value = c.arg(2, "value").number(0.5, 0, 1);
        eachFrame(c, img.wand, [&] { return MagickThresholdImage(img.wand, value * kQuantum); });
        return self(L);
    });
}

// img:posterize(levels, dither?): Image   levels per channel
int imgPosterize(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:posterize"};
        Image& img = c.image(1);
        const size_t levels = static_cast<size_t>(c.arg(2, "levels").number(4, 2, 256));
        const bool dither = c.arg(3, "dither").boolean(false);
        eachFrame(c, img.wand, [&] { return im::posterize(img.wand, levels, dither); });
        return self(L);
    });
}

// img:quantize(colors, options?): Image   reduces to at most "colors" colors
// options: dither (true), colorspace
int imgQuantize(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:quantize"};
        Image& img = c.image(1);
        const size_t colors = static_cast<size_t>(c.arg(2, "colors").number(256, 1, 65536));
        const Options o(c, 3, {"dither", "colorspace"});
        const bool dither = o.field("dither").boolean(true);
        ColorspaceType colorspace = UndefinedColorspace;
        if (std::optional<std::string> name = o.field("colorspace").optString()) {
            colorspace = parseEnum<ColorspaceType>(c, MagickColorspaceOptions, *name, "colorspace", "colorspace");
        }
        eachFrame(c, img.wand, [&] { return im::quantize(img.wand, colors, colorspace, dither); });
        return self(L);
    });
}

// img:monochrome(): Image   pure black and white, dithered
int imgMonochrome(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:monochrome"};
        Image& img = c.image(1);
        eachFrame(c, img.wand, [&] { return MagickSetImageType(img.wand, BilevelType); });
        return self(L);
    });
}

// img:solarize(threshold?): Image   0-1, default 0.5
int imgSolarize(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:solarize"};
        Image& img = c.image(1);
        const double threshold = c.arg(2, "threshold").number(0.5, 0, 1);
        eachFrame(c, img.wand, [&] { return MagickSolarizeImage(img.wand, threshold * kQuantum); });
        return self(L);
    });
}

// img:replaceColor(from, to, fuzz?): Image   fuzz 0-1: how close counts as the same color
int imgReplaceColor(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:replaceColor"};
        Image& img = c.image(1);
        const Pixel from = c.arg(2, "from").color();
        const Pixel to = c.arg(3, "to").color();
        const double fuzz = c.arg(4, "fuzz").number(0, 0, 1);
        eachFrame(c, img.wand, [&] {
            if (PixelGetAlpha(to.get()) < 1.0 && !hasAlpha(img.wand)) {
                MagickSetImageAlphaChannel(img.wand, static_cast<im::AlphaOption>(OpaqueAlphaChannel));
            }
            return MagickOpaquePaintImage(img.wand, from.get(), to.get(), fuzz * kQuantum, MagickFalse);
        });
        return self(L);
    });
}

// img:transparent(color, fuzz?): Image   makes color (and colors within fuzz) transparent
int imgTransparent(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:transparent"};
        Image& img = c.image(1);
        const Pixel color = c.arg(2, "color").color();
        const double fuzz = c.arg(3, "fuzz").number(0, 0, 1);
        eachFrame(c, img.wand, [&] {
            return MagickTransparentPaintImage(img.wand, color.get(), 0.0, fuzz * kQuantum, MagickFalse);
        });
        return self(L);
    });
}

// img:floodFill(x, y, color, fuzz?): Image   paint bucket
int imgFloodFill(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:floodFill"};
        Image& img = c.image(1);
        const ssize_t x = c.arg(2, "x").coordinate();
        const ssize_t y = c.arg(3, "y").coordinate();
        const Pixel color = c.arg(4, "color").color();
        const double fuzz = c.arg(5, "fuzz").number(0, 0, 1);
        checkPoint(c, img.wand, x, y);
        eachFrame(c, img.wand, [&] {
            if (PixelGetAlpha(color.get()) < 1.0 && !hasAlpha(img.wand)) {
                MagickSetImageAlphaChannel(img.wand, static_cast<im::AlphaOption>(OpaqueAlphaChannel));
            }
            return im::floodFill(img.wand, color.get(), fuzz * kQuantum, x, y);
        });
        return self(L);
    });
}

// img:alpha(mode): Image   "on", "off", "remove" (onto the background color),
// "opaque", "transparent", "extract", ... (image.options("alpha"))
int imgAlpha(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:alpha"};
        Image& img = c.image(1);
        std::string mode = lowercase(c.arg(2, "mode").string());
#if !SONATA_IM7
        // ImageMagick 6 calls them activate / deactivate. 7 has both, but its
        // deactivate keeps the channel (just ignored), where off removes it.
        if (mode == "on") mode = "activate";
        if (mode == "off") mode = "deactivate";
#endif
        const im::AlphaOption option = parseEnum<im::AlphaOption>(c, im::kAlphaOptions, mode, "alpha mode", "alpha");
        eachFrame(c, img.wand, [&] { return MagickSetImageAlphaChannel(img.wand, option); });
        return self(L);
    });
}

// img:opacity(amount): Image   multiplies every pixel's alpha by amount (0-1)
int imgOpacity(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:opacity"};
        Image& img = c.image(1);
        const double amount = c.arg(2, "amount").number(1, 0, 1);
        if (amount < 1.0) {
            img.replace(mapFrames(c, img.wand, [&] { return faded(c, img.wand, amount).release(); }));
        }
        return self(L);
    });
}

// img:flatten(background?): Image   merges every frame (layer) onto background
// ("white"), which also removes transparency
int imgFlatten(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:flatten"};
        Image& img = c.image(1);
        const Pixel background = c.arg(2, "background").color("white");
        eachFrame(c, img.wand, [&] { return MagickSetImageBackgroundColor(img.wand, background.get()); });
        first(img.wand);
        img.replace(c.check(MagickMergeImageLayers(img.wand, static_cast<im::Layer>(FlattenLayer)), img.wand));
        return self(L);
    });
}

// img:toColorspace(name): Image   converts: "gray", "cmyk", "lab", "hsl", "srgb", ...
int imgToColorspace(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:toColorspace"};
        Image& img = c.image(1);
        const ColorspaceType colorspace = parseEnum<ColorspaceType>(
            c, MagickColorspaceOptions, c.arg(2, "name").string(), "colorspace", "colorspace");
        eachFrame(c, img.wand, [&] { return MagickTransformImageColorspace(img.wand, colorspace); });
        return self(L);
    });
}

// img:channel(name): Image   keeps one channel ("red", "alpha", ...) as a grayscale image
int imgChannel(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:channel"};
        Image& img = c.image(1);
        const std::string name = c.arg(2, "name").string();
        const ssize_t channel = ParseChannelOption(name.c_str());
        if (channel < 0 || name.find(',') != std::string::npos) {
            c.fail("unknown channel '" + name + "' (image.options(\"channel\") lists them)");
        }
        eachFrame(c, img.wand, [&] { return im::separate(img.wand, static_cast<ChannelType>(channel)); });
        return self(L);
    });
}

// img:fx(expression): Image   an ImageMagick FX expression per pixel, e.g. "u*0.5"
int imgFx(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:fx"};
        Image& img = c.image(1);
        const std::string expression = c.arg(2, "expression").string();
        if (!expression.empty() && expression[0] == '@') {
            c.fail("expressions starting with '@' (read from a file) aren't supported");
        }
        img.replace(mapFrames(c, img.wand, [&] { return MagickFxImage(img.wand, expression.c_str()); }));
        return self(L);
    });
}

// Image: filters and effects ////////////////////////////////////////////////

// img:blur(sigma?, radius?): Image   sigma = strength (default 2), radius 0 = automatic
int imgBlur(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:blur"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(2, 0, 1000);
        const double radius = c.arg(3, "radius").number(0, 0, 1000);
        eachFrame(c, img.wand, [&] { return MagickBlurImage(img.wand, radius, sigma); });
        return self(L);
    });
}

// img:sharpen(sigma?, radius?): Image
int imgSharpen(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:sharpen"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(1, 0, 1000);
        const double radius = c.arg(3, "radius").number(0, 0, 1000);
        eachFrame(c, img.wand, [&] { return MagickSharpenImage(img.wand, radius, sigma); });
        return self(L);
    });
}

// img:unsharp(sigma?, amount?, threshold?, radius?): Image   the photographer's sharpen
int imgUnsharp(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:unsharp"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(1, 0, 1000);
        const double amount = c.arg(3, "amount").number(1, 0, 100);
        const double threshold = c.arg(4, "threshold").number(0.05, 0, 1);
        const double radius = c.arg(5, "radius").number(0, 0, 1000);
        eachFrame(c, img.wand, [&] { return MagickUnsharpMaskImage(img.wand, radius, sigma, amount, threshold); });
        return self(L);
    });
}

// img:motionBlur(sigma?, angle?, radius?): Image   angle in degrees
int imgMotionBlur(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:motionBlur"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(5, 0, 1000);
        const double angle = c.arg(3, "angle").number(0);
        const double radius = c.arg(4, "radius").number(0, 0, 1000);
        eachFrame(c, img.wand, [&] { return MagickMotionBlurImage(img.wand, radius, sigma, angle); });
        return self(L);
    });
}

// img:rotationalBlur(angle?): Image   degrees (default 10)
int imgRotationalBlur(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:rotationalBlur"};
        Image& img = c.image(1);
        const double angle = c.arg(2, "angle").number(10, 0, 360);
        eachFrame(c, img.wand, [&] { return MagickRotationalBlurImage(img.wand, angle); });
        return self(L);
    });
}

// img:noise(type?, amount?): Image   "gaussian" (default), "impulse", "poisson", ...
int imgNoise(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:noise"};
        Image& img = c.image(1);
        const NoiseType type = parseEnum<NoiseType>(c, MagickNoiseOptions,
                                                    c.arg(2, "type").optString().value_or("gaussian"), "noise", "noise");
        const double amount = c.arg(3, "amount").number(1, 0, 100);
        eachFrame(c, img.wand, [&] { return im::addNoise(img.wand, type, amount); });
        return self(L);
    });
}

// img:median(size?): Image   removes noise while keeping edges (size in pixels, default 3)
int imgMedian(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:median"};
        Image& img = c.image(1);
        const size_t size = static_cast<size_t>(c.arg(2, "size").number(3, 1, 100));
        eachFrame(c, img.wand, [&] { return MagickStatisticImage(img.wand, MedianStatistic, size, size); });
        return self(L);
    });
}

// img:charcoal(sigma?, radius?): Image
int imgCharcoal(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:charcoal"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(1, 0, 1000);
        const double radius = c.arg(3, "radius").number(0, 0, 1000);
        eachFrame(c, img.wand, [&] { return MagickCharcoalImage(img.wand, radius, sigma); });
        return self(L);
    });
}

// img:sketch(sigma?, angle?, radius?): Image   a pencil sketch
int imgSketch(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:sketch"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(5, 0, 1000);
        const double angle = c.arg(3, "angle").number(45);
        const double radius = c.arg(4, "radius").number(0, 0, 1000);
        eachFrame(c, img.wand, [&] { return MagickSketchImage(img.wand, radius, sigma, angle); });
        return self(L);
    });
}

// img:oilPaint(radius?): Image
int imgOilPaint(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:oilPaint"};
        Image& img = c.image(1);
        const double radius = c.arg(2, "radius").number(3, 0, 100);
        eachFrame(c, img.wand, [&] { return im::oilPaint(img.wand, radius); });
        return self(L);
    });
}

// img:emboss(sigma?, radius?): Image
int imgEmboss(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:emboss"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(1, 0, 1000);
        const double radius = c.arg(3, "radius").number(0, 0, 1000);
        eachFrame(c, img.wand, [&] { return MagickEmbossImage(img.wand, radius, sigma); });
        return self(L);
    });
}

// img:edge(radius?): Image   highlights edges
int imgEdge(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:edge"};
        Image& img = c.image(1);
        const double radius = c.arg(2, "radius").number(1, 0, 100);
        eachFrame(c, img.wand, [&] { return MagickEdgeImage(img.wand, radius); });
        return self(L);
    });
}

// img:swirl(degrees?): Image
int imgSwirl(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:swirl"};
        Image& img = c.image(1);
        const double degrees = c.arg(2, "degrees").number(90, -3600, 3600);
        eachFrame(c, img.wand, [&] { return im::swirl(img.wand, degrees); });
        return self(L);
    });
}

// img:implode(amount?): Image   > 0 pulls in, < 0 pushes out
int imgImplode(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:implode"};
        Image& img = c.image(1);
        const double amount = c.arg(2, "amount").number(0.5, -100, 100);
        eachFrame(c, img.wand, [&] { return im::implode(img.wand, amount); });
        return self(L);
    });
}

// img:wave(amplitude?, wavelength?): Image   the image grows by 2 * amplitude vertically
int imgWave(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:wave"};
        Image& img = c.image(1);
        const double amplitude = c.arg(2, "amplitude").number(10, 0, 10000);
        const double wavelength = c.arg(3, "wavelength").number(100, 0.01, 100000);
        eachFrame(c, img.wand, [&] { return im::wave(img.wand, amplitude, wavelength); });
        return self(L);
    });
}

// img:spread(radius?): Image   scatters pixels randomly
int imgSpread(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:spread"};
        Image& img = c.image(1);
        const double radius = c.arg(2, "radius").number(3, 0, 1000);
        eachFrame(c, img.wand, [&] { return im::spread(img.wand, radius); });
        return self(L);
    });
}

// img:vignette(sigma?, color?, x?, y?): Image   darkens (color, default black)
// the edges; x, y inset the ellipse
int imgVignette(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:vignette"};
        Image& img = c.image(1);
        const double sigma = c.arg(2, "sigma").number(10, 0, 10000);
        const Pixel color = c.arg(3, "color").color("black");
        const ssize_t x = c.arg(4, "x").missing() ? 0 : c.arg(4, "x").coordinate();
        const ssize_t y = c.arg(5, "y").missing() ? 0 : c.arg(5, "y").coordinate();
        eachFrame(c, img.wand, [&] {
            MagickSetImageBackgroundColor(img.wand, color.get());
            return MagickVignetteImage(img.wand, 0, sigma, x, y);
        });
        return self(L);
    });
}

// img:pixelate(size?): Image   blocks of size x size pixels (default 8)
int imgPixelate(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:pixelate"};
        Image& img = c.image(1);
        const double size = c.arg(2, "size").number(8, 1, 10000);
        eachFrame(c, img.wand, [&] {
            const size_t width = MagickGetImageWidth(img.wand);
            const size_t height = MagickGetImageHeight(img.wand);
            const size_t sw = std::max<size_t>(1, static_cast<size_t>(std::lround(double(width) / size)));
            const size_t sh = std::max<size_t>(1, static_cast<size_t>(std::lround(double(height) / size)));
            return MagickScaleImage(img.wand, sw, sh) && MagickSampleImage(img.wand, width, height) ? MagickTrue
                                                                                                    : MagickFalse;
        });
        return self(L);
    });
}

void runKernel(const Context& c, Image& img, MorphologyMethod method, const std::string& text, ssize_t iterations) {
    if (!text.empty() && text[0] == '@') {
        c.fail("kernels starting with '@' (read from a file) aren't supported");
    }
    const Kernel kernel(im::kernel(text.c_str()));
    if (!kernel) {
        c.fail("'" + text + "' is not a kernel ImageMagick understands");
    }
    eachFrame(c, img.wand, [&] { return MagickMorphologyImage(img.wand, method, iterations, kernel.get()); });
}

// img:morphology(method, kernel, iterations?): Image
// method: "dilate", "erode", "open", "close", "edge", ... (image.options("morphology"))
// kernel: ImageMagick's syntax, "disk:3", "square:2", "diamond", ...
int imgMorphology(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:morphology"};
        Image& img = c.image(1);
        const MorphologyMethod method = parseEnum<MorphologyMethod>(
            c, MagickMorphologyOptions, c.arg(2, "method").string(), "morphology method", "morphology");
        const std::string kernel = c.arg(3, "kernel").string();
        const ssize_t iterations = static_cast<ssize_t>(c.arg(4, "iterations").number(1, -1, 1000));
        runKernel(c, img, method, kernel, iterations);
        return self(L);
    });
}

// img:convolve(kernel): Image   a square array of numbers (row by row) or
// ImageMagick kernel text such as "3x3: 0,-1,0 -1,5,-1 0,-1,0"
int imgConvolve(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:convolve"};
        Image& img = c.image(1);
        const Value arg = c.arg(2, "kernel");
        std::string text;
        if (arg.type() == LUA_TTABLE) {
            const int n = lua_objlen(L, 2);
            const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(n))));
            if (n == 0 || side * side != n) {
                arg.fail("must have a square number of values (9 for 3x3, 25 for 5x5, ...)");
            }
            text = std::to_string(side) + "x" + std::to_string(side) + ":";
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, 2, i);
                const bool ok = lua_type(L, -1) == LUA_TNUMBER;
                char value[64];
                std::snprintf(value, sizeof(value), "%s%.17g", i == 1 ? " " : ",", lua_tonumber(L, -1));
                lua_pop(L, 1);
                if (!ok) {
                    arg.fail("must be an array of numbers");
                }
                text += value;
            }
        } else {
            text = arg.string();
        }
        runKernel(c, img, ConvolveMorphology, text, 1);
        return self(L);
    });
}

// Image: compositing and drawing ////////////////////////////////////////////

// img:composite(other, options?): Image   draws other's first frame on every frame
// options: x, y (offsets, default 0), gravity (what x, y are relative to,
// default top-left), blend ("over"; image.options("compose")), opacity (0-1)
int imgComposite(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:composite"};
        Image& img = c.image(1);
        Image& other = c.image(2, "other");
        const Options o(c, 3, {"x", "y", "gravity", "blend", "opacity"});
        const double x = o.field("x").number(0);
        const double y = o.field("y").number(0);
        const Gravity gravity = readGravity(o.field("gravity")).value_or(Gravity{0, 0});
        CompositeOperator blend = OverCompositeOp;
        if (std::optional<std::string> name = o.field("blend").optString()) {
            blend = parseEnum<CompositeOperator>(c, MagickComposeOptions, *name, "blend mode", "compose");
        }
        const double opacity = o.field("opacity").number(1, 0, 1);

        Wand source = firstFrame(c, other.wand);
        if (opacity < 1.0) {
            source = faded(c, source.get(), opacity);
        }
        const double sw = static_cast<double>(MagickGetImageWidth(source.get()));
        const double sh = static_cast<double>(MagickGetImageHeight(source.get()));
        eachFrame(c, img.wand, [&] {
            const double w = static_cast<double>(MagickGetImageWidth(img.wand));
            const double h = static_cast<double>(MagickGetImageHeight(img.wand));
            return im::composite(img.wand, source.get(), blend, std::lround(gravity.x(w, sw, x)),
                                 std::lround(gravity.y(h, sh, y)));
        });
        return self(L);
    });
}

void drawOnFrames(const Context& c, Image& img, const Style& style) {
    eachFrame(c, img.wand, [&] { return MagickDrawImage(img.wand, style.draw.get()); });
}

// img:drawRect(x, y, width, height, style?): Image   style.radius rounds the corners
int imgDrawRect(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:drawRect"};
        Image& img = c.image(1);
        const double x = c.arg(2, "x").number();
        const double y = c.arg(3, "y").number();
        const double width = c.arg(4, "width").number(0, 0, 1e7);
        const double height = c.arg(5, "height").number(0, 0, 1e7);
        const Style style = readStyle(c, 6, Shape::Filled);
        // The right and bottom edges are inclusive in ImageMagick.
        if (style.radius > 0) {
            DrawRoundRectangle(style.draw.get(), x, y, x + width - 1, y + height - 1, style.radius, style.radius);
        } else {
            DrawRectangle(style.draw.get(), x, y, x + width - 1, y + height - 1);
        }
        drawOnFrames(c, img, style);
        return self(L);
    });
}

// img:drawCircle(x, y, radius, style?): Image   centered on (x, y)
int imgDrawCircle(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:drawCircle"};
        Image& img = c.image(1);
        const double x = c.arg(2, "x").number();
        const double y = c.arg(3, "y").number();
        const double radius = c.arg(4, "radius").number(0, 0, 1e7);
        const Style style = readStyle(c, 5, Shape::Filled);
        DrawCircle(style.draw.get(), x, y, x + radius, y);
        drawOnFrames(c, img, style);
        return self(L);
    });
}

// img:drawEllipse(x, y, radiusX, radiusY, style?): Image
int imgDrawEllipse(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:drawEllipse"};
        Image& img = c.image(1);
        const double x = c.arg(2, "x").number();
        const double y = c.arg(3, "y").number();
        const double rx = c.arg(4, "radiusX").number(0, 0, 1e7);
        const double ry = c.arg(5, "radiusY").number(0, 0, 1e7);
        const Style style = readStyle(c, 6, Shape::Filled);
        DrawEllipse(style.draw.get(), x, y, rx, ry, 0, 360);
        drawOnFrames(c, img, style);
        return self(L);
    });
}

// img:drawLine(x1, y1, x2, y2, style?): Image
int imgDrawLine(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:drawLine"};
        Image& img = c.image(1);
        const double x1 = c.arg(2, "x1").number();
        const double y1 = c.arg(3, "y1").number();
        const double x2 = c.arg(4, "x2").number();
        const double y2 = c.arg(5, "y2").number();
        const Style style = readStyle(c, 6, Shape::Line);
        DrawLine(style.draw.get(), x1, y1, x2, y2);
        drawOnFrames(c, img, style);
        return self(L);
    });
}

// img:drawPolygon(points, style?): Image   closed and filled
int imgDrawPolygon(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:drawPolygon"};
        Image& img = c.image(1);
        const std::vector<PointInfo> points = readPoints(c.arg(2, "points"), 3);
        const Style style = readStyle(c, 3, Shape::Filled);
        DrawPolygon(style.draw.get(), points.size(), points.data());
        drawOnFrames(c, img, style);
        return self(L);
    });
}

// img:drawPolyline(points, style?): Image   an open path of lines
int imgDrawPolyline(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:drawPolyline"};
        Image& img = c.image(1);
        const std::vector<PointInfo> points = readPoints(c.arg(2, "points"), 2);
        const Style style = readStyle(c, 3, Shape::Line);
        DrawPolyline(style.draw.get(), points.size(), points.data());
        drawOnFrames(c, img, style);
        return self(L);
    });
}

// img:drawText(text, x?, y?, style?): Image   (x, y) is the top-left corner of
// the text, or an offset from style.gravity's edge ("bottom-right", ...)
int imgDrawText(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:drawText"};
        Image& img = c.image(1);
        const std::string text = c.arg(2, "text").string();
        const double x = c.arg(3, "x").number(0);
        const double y = c.arg(4, "y").number(0);
        const Style style = readStyle(c, 5, Shape::Text);
        annotate(c, img.wand, style, x, y, text);
        return self(L);
    });
}

// Image: analysis ///////////////////////////////////////////////////////////

// img:compare(other, metric?): (number, Image)   how different the first frames
// are (0 = identical; "rmse" by default, image.options("metric")), and an
// image highlighting the differences. Sizes must match.
int imgCompare(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:compare"};
        Image& img = c.image(1);
        Image& other = c.image(2, "other");
        const MetricType metric = parseEnum<MetricType>(
            c, MagickMetricOptions, c.arg(3, "metric").optString().value_or("rmse"), "metric", "metric");
        Wand a = firstFrame(c, img.wand);
        Wand b = firstFrame(c, other.wand);
        if (MagickGetImageWidth(a.get()) != MagickGetImageWidth(b.get()) ||
            MagickGetImageHeight(a.get()) != MagickGetImageHeight(b.get())) {
            c.fail("the images have different sizes");
        }
        double distortion = 0;
        Wand diff = c.check(MagickCompareImages(a.get(), b.get(), metric, &distortion), a.get());
        lua_pushnumber(L, distortion);
        pushImage(L, std::move(diff));
        return 2;
    });
}

// img:stats(): { mean, deviation, min, max }   over every channel, 0-1
int imgStats(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:stats"};
        Image& img = c.image(1);
        first(img.wand);
        double mean = 0, deviation = 0, min = 0, max = 0;
        c.check(im::mean(img.wand, &mean, &deviation), img.wand);
        c.check(MagickGetImageRange(img.wand, &min, &max), img.wand);
        lua_createtable(L, 0, 4);
        setNumber(L, "mean", mean / kQuantum);
        setNumber(L, "deviation", deviation / kQuantum);
        setNumber(L, "min", min / kQuantum);
        setNumber(L, "max", max / kQuantum);
        return 1;
    });
}

struct ColorCount {
    Pixel color;
    size_t count;
};

// The first frame's colors and how many pixels have each, most common first.
std::vector<ColorCount> histogram(const Context& c, MagickWand* w) {
    size_t n = 0;
    PixelWand** wands = MagickGetImageHistogram(w, &n);
    if (wands == nullptr) {
        c.magick(w);
    }
    std::vector<ColorCount> colors;
    colors.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        colors.push_back({Pixel(wands[i]), static_cast<size_t>(PixelGetColorCount(wands[i]))});
    }
    MagickRelinquishMemory(wands); // the wands themselves are owned by "colors" now
    std::stable_sort(colors.begin(), colors.end(),
                     [](const ColorCount& a, const ColorCount& b) { return a.count > b.count; });
    return colors;
}

void pushColorCounts(lua_State* L, const std::vector<ColorCount>& colors, size_t limit, double total) {
    const size_t n = std::min(limit, colors.size());
    lua_createtable(L, static_cast<int>(n), 0);
    for (size_t i = 0; i < n; ++i) {
        lua_createtable(L, 0, 4);
        pushColor(L, colors[i].color.get());
        lua_setfield(L, -2, "color");
        const std::string hex = hexOf(colors[i].color.get());
        lua_pushlstring(L, hex.data(), hex.size());
        lua_setfield(L, -2, "hex");
        setNumber(L, "count", static_cast<double>(colors[i].count));
        setNumber(L, "fraction", total > 0 ? static_cast<double>(colors[i].count) / total : 0);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

// img:histogram(limit?): { { color, hex, count, fraction } }   most common first
int imgHistogram(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:histogram"};
        Image& img = c.image(1);
        const double limit = c.arg(2, "limit").number(1e12, 1, 1e12);
        first(img.wand);
        const double total = double(MagickGetImageWidth(img.wand)) * double(MagickGetImageHeight(img.wand));
        const std::vector<ColorCount> colors = histogram(c, img.wand);
        pushColorCounts(L, colors, static_cast<size_t>(limit), total);
        return 1;
    });
}

// img:dominantColors(count?): { { color, hex, count, fraction } }   the main
// colors of the first frame (default 5), most common first
int imgDominantColors(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:dominantColors"};
        Image& img = c.image(1);
        const size_t count = static_cast<size_t>(c.arg(2, "count").number(5, 1, 256));
        Wand w = firstFrame(c, img.wand);
        const double width = static_cast<double>(MagickGetImageWidth(w.get()));
        const double height = static_cast<double>(MagickGetImageHeight(w.get()));
        const double scale = std::min(1.0, 256.0 / std::max(width, height)); // fast and good enough
        if (scale < 1.0) {
            c.check(MagickScaleImage(w.get(), std::max<size_t>(1, size_t(width * scale)),
                                     std::max<size_t>(1, size_t(height * scale))),
                    w.get());
        }
        c.check(im::quantize(w.get(), count, sRGBColorspace, false), w.get());
        const double total = double(MagickGetImageWidth(w.get())) * double(MagickGetImageHeight(w.get()));
        const std::vector<ColorCount> colors = histogram(c, w.get());
        pushColorCounts(L, colors, count, total);
        return 1;
    });
}

// img:averageColor(): Color
int imgAverageColor(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:averageColor"};
        Image& img = c.image(1);
        Wand w = firstFrame(c, img.wand);
        c.check(MagickScaleImage(w.get(), 1, 1), w.get());
        Pixel p(NewPixelWand());
        c.check(MagickGetImagePixelColor(w.get(), 0, 0, p.get()), w.get());
        pushColor(L, p.get());
        return 1;
    });
}

// img:colorCount(): number   distinct colors in the first frame
int imgColorCount(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:colorCount"};
        Image& img = c.image(1);
        first(img.wand);
        lua_pushnumber(L, static_cast<double>(MagickGetImageColors(img.wand)));
        return 1;
    });
}

// Image: animation //////////////////////////////////////////////////////////

// img:coalesce(): Image   turns optimized animation frames into full frames
int imgCoalesce(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:coalesce"};
        Image& img = c.image(1);
        first(img.wand);
        img.replace(c.check(MagickCoalesceImages(img.wand), img.wand));
        return self(L);
    });
}

// img:optimize(): Image   stores only what changes between frames (smaller GIFs)
int imgOptimize(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:optimize"};
        Image& img = c.image(1);
        if (frameCount(img.wand) > 1) {
            first(img.wand);
            img.replace(c.check(MagickOptimizeImageLayers(img.wand), img.wand));
        }
        return self(L);
    });
}

// img:setDelay(milliseconds, frame?): Image   every frame, or just one (1-based)
int imgSetDelay(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:setDelay"};
        Image& img = c.image(1);
        const size_t delay = centiseconds(c, c.arg(2, "milliseconds").number());
        if (!c.arg(3, "frame").missing()) {
            const size_t frame = c.arg(3, "frame").size();
            if (frame > frameCount(img.wand)) {
                c.fail("frame " + std::to_string(frame) + " is past the last frame");
            }
            MagickSetIteratorIndex(img.wand, static_cast<ssize_t>(frame - 1));
            c.check(MagickSetImageDelay(img.wand, delay), img.wand);
            MagickResetIterator(img.wand);
        } else {
            eachFrame(c, img.wand, [&] { return MagickSetImageDelay(img.wand, delay); });
        }
        return self(L);
    });
}

// img:delays(): { number }   each frame's delay in milliseconds
int imgDelays(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:delays"};
        Image& img = c.image(1);
        lua_createtable(L, static_cast<int>(frameCount(img.wand)), 0);
        int i = 0;
        eachFrame(c, img.wand, [&] {
            const double ticks = static_cast<double>(std::max<ssize_t>(1, MagickGetImageTicksPerSecond(img.wand)));
            lua_pushnumber(L, std::round(static_cast<double>(MagickGetImageDelay(img.wand)) * 1000.0 / ticks));
            lua_rawseti(L, -2, ++i);
            return MagickTrue;
        });
        return 1;
    });
}

// img:setLoop(count): Image   0 loops forever
int imgSetLoop(lua_State* L) {
    return run(L, [&] {
        const Context c{L, "Image:setLoop"};
        Image& img = c.image(1);
        const size_t count = static_cast<size_t>(c.arg(2, "count").number(0, 0, 65535));
        eachFrame(c, img.wand, [&] { return MagickSetImageIterations(img.wand, count); });
        return self(L);
    });
}

// Metatable /////////////////////////////////////////////////////////////////

constexpr NativeFunction kMethods[] = {
    // conversion
    {"clone", imgClone},
    {"close", imgClose},
    {"save", imgSave},
    {"encode", imgEncode},
    {"pixels", imgPixels},
    {"getPixel", imgGetPixel},
    {"setPixel", imgSetPixel},
    {"frame", imgFrame},
    {"metadata", imgMetadata},
    {"setMetadata", imgSetMetadata},
    {"strip", imgStrip},
    {"signature", imgSignature},
    {"resolution", imgResolution},
    {"setResolution", imgSetResolution},
    // geometry
    {"resize", imgResize},
    {"thumbnail", imgThumbnail},
    {"scale", imgScale},
    {"crop", imgCrop},
    {"extent", imgExtent},
    {"trim", imgTrim},
    {"rotate", imgRotate},
    {"flip", imgFlip},
    {"mirror", imgMirror},
    {"transpose", imgTranspose},
    {"transverse", imgTransverse},
    {"autoOrient", imgAutoOrient},
    {"shear", imgShear},
    {"roll", imgRoll},
    {"border", imgBorder},
    {"distort", imgDistort},
    {"liquidRescale", imgLiquidRescale},
    {"deskew", imgDeskew},
    // color
    {"grayscale", imgGrayscale},
    {"negate", imgNegate},
    {"sepia", imgSepia},
    {"brightnessContrast", imgBrightnessContrast},
    {"modulate", imgModulate},
    {"gamma", imgGamma},
    {"level", imgLevel},
    {"normalize", imgNormalize},
    {"equalize", imgEqualize},
    {"autoLevel", imgAutoLevel},
    {"autoGamma", imgAutoGamma},
    {"contrast", imgContrast},
    {"colorize", imgColorize},
    {"tint", imgTint},
    {"threshold", imgThreshold},
    {"posterize", imgPosterize},
    {"quantize", imgQuantize},
    {"monochrome", imgMonochrome},
    {"solarize", imgSolarize},
    {"replaceColor", imgReplaceColor},
    {"transparent", imgTransparent},
    {"floodFill", imgFloodFill},
    {"alpha", imgAlpha},
    {"opacity", imgOpacity},
    {"flatten", imgFlatten},
    {"toColorspace", imgToColorspace},
    {"channel", imgChannel},
    {"fx", imgFx},
    // filters
    {"blur", imgBlur},
    {"sharpen", imgSharpen},
    {"unsharp", imgUnsharp},
    {"motionBlur", imgMotionBlur},
    {"rotationalBlur", imgRotationalBlur},
    {"noise", imgNoise},
    {"despeckle", imgDespeckle},
    {"median", imgMedian},
    {"charcoal", imgCharcoal},
    {"sketch", imgSketch},
    {"oilPaint", imgOilPaint},
    {"emboss", imgEmboss},
    {"edge", imgEdge},
    {"swirl", imgSwirl},
    {"implode", imgImplode},
    {"wave", imgWave},
    {"spread", imgSpread},
    {"vignette", imgVignette},
    {"pixelate", imgPixelate},
    {"morphology", imgMorphology},
    {"convolve", imgConvolve},
    // compositing and drawing
    {"composite", imgComposite},
    {"drawRect", imgDrawRect},
    {"drawCircle", imgDrawCircle},
    {"drawEllipse", imgDrawEllipse},
    {"drawLine", imgDrawLine},
    {"drawPolygon", imgDrawPolygon},
    {"drawPolyline", imgDrawPolyline},
    {"drawText", imgDrawText},
    // analysis
    {"compare", imgCompare},
    {"stats", imgStats},
    {"histogram", imgHistogram},
    {"dominantColors", imgDominantColors},
    {"averageColor", imgAverageColor},
    {"colorCount", imgColorCount},
    // animation
    {"coalesce", imgCoalesce},
    {"optimize", imgOptimize},
    {"setDelay", imgSetDelay},
    {"delays", imgDelays},
    {"setLoop", imgSetLoop},
};

// __index: methods first (upvalue 1), then the read-only fields.
int imageIndex(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1) || lua_type(L, 2) != LUA_TSTRING) {
        return 1;
    }
    lua_pop(L, 1);

    const std::string_view key = lua_tostring(L, 2);
    MagickWand* w = static_cast<Image*>(lua_touserdata(L, 1))->wand;
    if (w == nullptr) {
        luaL_error(L, "Image.%s: the image is closed", key.data());
    }
    first(w);
    if (key == "width") {
        lua_pushnumber(L, static_cast<double>(MagickGetImageWidth(w)));
    } else if (key == "height") {
        lua_pushnumber(L, static_cast<double>(MagickGetImageHeight(w)));
    } else if (key == "frames") {
        lua_pushnumber(L, static_cast<double>(frameCount(w)));
    } else if (key == "depth") {
        lua_pushnumber(L, static_cast<double>(MagickGetImageDepth(w)));
    } else if (key == "hasAlpha") {
        lua_pushboolean(L, hasAlpha(w));
    } else if (key == "format") {
        char* format = MagickGetImageFormat(w);
        if (format && *format) {
            for (char* p = format; *p; ++p) {
                *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
            }
            lua_pushstring(L, format);
        } else {
            lua_pushnil(L);
        }
        if (format) {
            MagickRelinquishMemory(format);
        }
    } else if (key == "colorspace") {
        const char* name = CommandOptionToMnemonic(MagickColorspaceOptions, MagickGetImageColorspace(w));
        lua_pushstring(L, name ? name : "undefined");
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int imageToString(lua_State* L) {
    Image* img = static_cast<Image*>(lua_touserdata(L, 1));
    if (img == nullptr || img->wand == nullptr) {
        lua_pushstring(L, "Image(closed)");
        return 1;
    }
    first(img->wand);
    char* format = MagickGetImageFormat(img->wand);
    const size_t frames = frameCount(img->wand);
    lua_pushfstring(L, "Image(%dx%d%s%s%s)", static_cast<int>(MagickGetImageWidth(img->wand)),
                    static_cast<int>(MagickGetImageHeight(img->wand)), format && *format ? " " : "",
                    format ? format : "",
                    frames > 1 ? (", " + std::to_string(frames) + " frames").c_str() : "");
    if (format) {
        MagickRelinquishMemory(format);
    }
    return 1;
}

constexpr NativeFunction kFunctions[] = {
    {"load", imageLoad},
    {"decode", imageDecode},
    {"info", imageInfo},
    {"new", imageNew},
    {"generate", imageGenerate},
    {"fromPixels", imageFromPixels},
    {"text", imageText},
    {"measureText", imageMeasureText},
    {"animate", imageAnimate},
    {"append", imageAppend},
    {"grid", imageGrid},
    {"formats", imageFormats},
    {"fonts", imageFonts},
    {"options", imageOptions},
    {"color", imageColor},
    {"hex", imageHex},
    {"limits", imageLimits},
};

// "ImageMagick 6.9.12-98 Q16 x86_64 ..." -> "6.9.12-98"
std::string versionNumber() {
    size_t number = 0;
    const std::string full = MagickGetVersion(&number);
    const std::size_t start = full.find(' ');
    if (start == std::string::npos) {
        return full;
    }
    const std::size_t end = full.find(' ', start + 1);
    return full.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
}

} // namespace

void openImage(lua_State* L) {
    static std::once_flag genesis;
    std::call_once(genesis, [] { MagickWandGenesis(); });

    if (luaL_newmetatable(L, kImageMeta)) {
        lua_createtable(L, 0, static_cast<int>(std::size(kMethods)));
        setFunctions(L, kMethods);
        lua_setreadonly(L, -1, 1);
        lua_pushcclosure(L, imageIndex, "Image.__index", 1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, imageToString, "Image.__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "Image");
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 1);
    setFunctions(L, kFunctions);
    const std::string version = versionNumber();
    lua_pushlstring(L, version.data(), version.size());
    lua_setfield(L, -2, "version");
}

} // namespace sonata::lib::libs
