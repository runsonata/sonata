// tests/test_lib_image.cpp: the @sonata/image built-in library.
#include "helper.hpp"
#include <doctest/doctest.h>
#include <string>
#include <string_view>

namespace {

// Every script starts with the library in scope, and "dir" (a fresh temp
// directory, with a trailing slash) for files.
std::string withImage(const TempDir& dir, std::string_view body) {
    return "local image = require(\"@sonata/image\")\nlocal dir = \"" + dir.path().generic_string() + "/\"\n" +
           std::string(body);
}

#define RUN_IMAGE(body)                                                                                                \
    do {                                                                                                               \
        TempDir dir;                                                                                                   \
        Exec exec(withImage(dir, body));                                                                               \
        REQUIRE_MESSAGE(exec.getStatus() == 0, exec.getError());                                                       \
    } while (false)

// Runs "body" and returns its error ("" if it succeeded).
std::string errorOf(std::string_view body) {
    TempDir dir;
    Exec exec(withImage(dir, body));
    return exec.getError();
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_SUITE("image library") {

    TEST_CASE("image: canvases, fields and pixels") {
        RUN_IMAGE(R"lua(
        assert(typeof(image.version) == "string" and #image.version > 0)
        local img = image.new(30, 20, "red")
        assert(typeof(img) == "Image")
        assert(img.width == 30 and img.height == 20 and img.frames == 1 and img.format == nil)
        assert(img.colorspace == "sRGB")
        assert(string.find(tostring(img), "^Image%(30x20"))

        local p = img:getPixel(29, 19)
        assert(p.r == 255 and p.g == 0 and p.b == 0 and p.a == 255)
        assert(image.new(2, 2):getPixel(0, 0).a == 0) -- transparent by default
        assert(image.new(2, 2, { r = 1, g = 2, b = 3 }):getPixel(1, 1).b == 3)

        img:setPixel(3, 4, "#00ff00")
        assert(image.hex(img:getPixel(3, 4)) == "#00ff00")
        assert(image.hex(img:getPixel(4, 4)) == "#ff0000")
        assert(image.new(2, 2, "white"):setPixel(0, 0, "#0000ff80"):getPixel(0, 0).a == 128)
    )lua");
    }

    TEST_CASE("image: raw pixels round-trip") {
        RUN_IMAGE(R"lua(
        local data = image.new(3, 2, "#10203040"):pixels()
        assert(buffer.len(data) == 3 * 2 * 4)
        assert(buffer.readu8(data, 0) == 0x10 and buffer.readu8(data, 3) == 0x40)

        local rgb = image.new(3, 2, "#102030"):pixels({ channels = "rgb", x = 1, width = 2 })
        assert(buffer.len(rgb) == 2 * 2 * 3)
        assert(buffer.len(image.new(4, 4, "white"):pixels({ channels = "gray" })) == 16)

        local copy = image.fromPixels(3, 2, data)
        assert(image.hex(copy:getPixel(2, 1)) == "#10203040")
        local fromString = image.fromPixels(1, 1, "\255\0\0", "rgb")
        assert(image.hex(fromString:getPixel(0, 0)) == "#ff0000")

        local ok, err = pcall(image.fromPixels, 2, 2, "abc")
        assert(not ok and string.find(err, "need 16"), err)
    )lua");
    }

    TEST_CASE("image: save, load, encode, decode and info") {
        RUN_IMAGE(R"lua(
        local img = image.new(40, 30, "blue"):drawRect(0, 0, 10, 10, { fill = "yellow" })
        assert(img:save(dir .. "a.png") == img)

        local png = image.load(dir .. "a.png")
        assert(png.format == "png" and png.width == 40 and png.height == 30)
        assert(png:signature() == img:signature())

        local info = image.info(dir .. "a.png")
        assert(info.width == 40 and info.height == 30 and info.format == "png" and info.frames == 1)

        local jpeg = img:encode("jpg", { quality = 90, progressive = true })
        assert(string.sub(jpeg, 1, 2) == "\xff\xd8")
        local decoded = image.decode(jpeg)
        assert(decoded.format == "jpeg" and decoded.width == 40)
        assert(image.info(buffer.fromstring(jpeg)).format == "jpeg")

        -- The format comes from options.format, else from the extension.
        img:save(dir .. "noext", { format = "gif" })
        assert(image.load(dir .. "noext").format == "gif")
        -- encode() defaults to the image's own format.
        assert(image.decode(png:encode()).format == "png")
        assert(image.decode(image.new(2, 2):encode()).format == "png")

        -- JPEG has no transparency: transparent pixels land on the background.
        local clear = image.new(4, 4)
        assert(image.hex(image.decode(clear:encode("jpg")):getPixel(0, 0)) == "#ffffff")
        local onBlack = image.decode(clear:encode("jpg", { background = "black" }))
        assert(onBlack:getPixel(0, 0).r < 8)

        -- Metadata
        img:setMetadata("comment", "made by sonata"):save(dir .. "c.png")
        assert(image.load(dir .. "c.png"):metadata()["comment"] == "made by sonata")
        assert(image.load(dir .. "c.png"):strip():metadata()["comment"] == nil)
    )lua");
    }

    TEST_CASE("image: read errors are clear") {
        CHECK(contains(errorOf("image.load(dir .. 'missing.png')"), "image.load: could not open"));
        CHECK(contains(errorOf("image.load(dir .. 'missing.png')"), "(no such file)"));
        CHECK(contains(errorOf("image.decode('not an image at all')"), "image.decode: not in an image format"));
        CHECK(contains(errorOf("image.decode('')"), "the data is empty"));
        CHECK(contains(errorOf("image.new(10, 10):save(dir .. 'x.notaformat')"), "Image:save:"));
        CHECK(contains(errorOf("image.new(10, 10):save(dir .. 'no/such/dir/x.png')"), "could not open"));
    }

    TEST_CASE("image: resize keeps or fits the aspect ratio") {
        RUN_IMAGE(R"lua(
        local img = image.new(200, 100, "white")
        local function size(i) return i.width .. "x" .. i.height end
        assert(size(img:clone():resize(100)) == "100x50")
        assert(size(img:clone():resize(nil, 25)) == "50x25")
        assert(size(img:clone():resize(50, 50)) == "50x50")
        assert(size(img:clone():resize(50, 50, { fit = "contain" })) == "50x25")
        assert(size(img:clone():resize(50, 50, { fit = "cover" })) == "50x50")
        assert(size(img:clone():resize(400, nil, { enlarge = false })) == "200x100")
        assert(size(img:clone():resize(100, nil, { filter = "point" })) == "100x50")
        assert(size(img:clone():thumbnail(1000)) == "200x100") -- thumbnails never enlarge
        assert(size(img:clone():thumbnail(20, 20)) == "20x10")
        assert(size(img:clone():scale(0.5)) == "100x50")
        assert(size(img:clone():scale(2, 1)) == "400x100")
        assert(img.width == 200) -- the original was cloned every time

        -- cover keeps the gravity's side
        local halves = image.append({ image.new(100, 100, "red"), image.new(100, 100, "blue") })
        assert(image.hex(halves:clone():resize(50, 50, { fit = "cover", gravity = "left" }):getPixel(25, 25)) == "#ff0000")
        assert(image.hex(halves:clone():resize(50, 50, { fit = "cover", gravity = "right" }):getPixel(25, 25)) == "#0000ff")
    )lua");
    }

    TEST_CASE("image: crop, extent, trim, rotate, flip") {
        RUN_IMAGE(R"lua(
        local img = image.append({ image.new(10, 10, "red"), image.new(10, 10, "blue") })
        assert(img.width == 20 and img.height == 10)

        local right = img:clone():crop(10, 0, 10, 10)
        assert(right.width == 10 and image.hex(right:getPixel(0, 0)) == "#0000ff")

        local padded = img:clone():extent(40, 30, { background = "white" })
        assert(padded.width == 40 and padded.height == 30)
        assert(image.hex(padded:getPixel(0, 0)) == "#ffffff")
        assert(image.hex(padded:getPixel(10, 10)) == "#ff0000") -- centered
        local corner = img:clone():extent(40, 30, { gravity = "bottom-right" })
        assert(corner:getPixel(0, 0).a == 0 and image.hex(corner:getPixel(39, 29)) == "#0000ff")
        assert(img:clone():extent(10, 10, { gravity = "left" }):getPixel(5, 5).r == 255) -- smaller: crops

        assert(padded:trim().width == 20 and padded.height == 10)

        local rotated = img:clone():rotate(90)
        assert(rotated.width == 10 and rotated.height == 20)
        assert(image.hex(rotated:getPixel(5, 2)) == "#ff0000")

        assert(image.hex(img:clone():mirror():getPixel(0, 0)) == "#0000ff")
        local column = image.append({ image.new(4, 4, "red"), image.new(4, 4, "blue") }, { vertical = true })
        assert(image.hex(column:flip():getPixel(0, 0)) == "#0000ff")
        assert(img:clone():transpose().width == 10)
        assert(img:clone():border(3, "green").width == 26)
        assert(image.hex(img:clone():border(3, "green"):getPixel(0, 0)) == "#008000")
    )lua");
    }

    TEST_CASE("image: color and tone operations") {
        RUN_IMAGE(R"lua(
        local function hex(i) return image.hex(i:getPixel(0, 0)) end
        local red = image.new(4, 4, "red")
        local gray = red:clone():grayscale():getPixel(0, 0)
        assert(gray.r == gray.g and gray.g == gray.b and gray.r > 0 and gray.r < 255)
        assert(hex(red:clone():negate()) == "#00ffff")
        assert(hex(image.new(2, 2, "#808080"):threshold(0.4)) == "#ffffff")
        assert(hex(image.new(2, 2, "#808080"):threshold(0.6)) == "#000000")
        assert(hex(red:clone():replaceColor("red", "lime")) == "#00ff00")
        assert(hex(image.new(2, 2, "#fe0000"):replaceColor("red", "lime", 0.05)) == "#00ff00")
        assert(red:clone():transparent("red"):getPixel(0, 0).a == 0)
        assert(hex(image.new(4, 4, "white"):floodFill(0, 0, "blue")) == "#0000ff")
        assert(red:clone():opacity(0.5):getPixel(0, 0).a == 128 or red:clone():opacity(0.5):getPixel(0, 0).a == 127)
        assert(hex(image.new(2, 2):flatten("white")) == "#ffffff")
        assert(image.new(2, 2, "red"):alpha("on").hasAlpha)
        assert(not image.new(2, 2):alpha("off").hasAlpha)
        assert(hex(image.new(2, 2, "white"):fx("u*0.5")) == "#808080")
        assert(hex(image.new(2, 2, "#ff8000"):channel("green")) == "#808080")
        assert(image.new(2, 2, "red"):toColorspace("cmyk").colorspace == "CMYK")
        assert(image.new(2, 2, "red"):modulate(100, 0):getPixel(0, 0).r == image.new(2, 2, "red"):modulate(100, 0):getPixel(0, 0).g)
        assert(image.new(64, 64):quantize(2) and image.generate("gradient:red-blue", 32, 32):quantize(4, { dither = false }):colorCount() <= 4)

        -- They all run, and chain.
        local img = image.generate("gradient:white-black", 32, 32)
        img:sepia():brightnessContrast(10, 10):gamma(1.1):level(0.05, 0.95):normalize():equalize()
            :autoLevel():autoGamma():contrast(2):contrast(-2):colorize("red", 0.2):tint("blue")
            :posterize(4, true):solarize():monochrome()
        assert(img.width == 32)
    )lua");
    }

    TEST_CASE("image: filters and effects run on every frame") {
        RUN_IMAGE(R"lua(
        local base = image.generate("rose:")
        for _, name in { "blur", "sharpen", "unsharp", "motionBlur", "rotationalBlur", "noise", "despeckle",
                         "median", "charcoal", "sketch", "oilPaint", "emboss", "edge", "swirl", "implode",
                         "wave", "spread", "vignette", "pixelate" } do
            local copy = base:clone()
            assert(copy[name](copy) == copy, name)
        end
        assert(base:clone():morphology("dilate", "disk:2").width == base.width)
        assert(base:clone():convolve({ 0, 0, 0, 0, 1, 0, 0, 0, 0 }):signature() == base:signature())
        assert(base:clone():convolve("3x3: 0,0,0 0,1,0 0,0,0"):signature() == base:signature())
        assert(base:clone():distort("srt", { 0.5, 0 }).width == base.width)
        -- Seam carving needs liblqr, which some builds (Homebrew's) leave out.
        local ok, resized = pcall(base.liquidRescale, base:clone(), 50, 40)
        assert(if ok then resized.width == 50 else string.find(resized, "(LQR)", 1, true), resized)

        local anim = image.animate({ image.new(8, 8, "red"), image.new(8, 8, "blue") })
        anim:blur(1):resize(4):drawRect(0, 0, 2, 2, { fill = "black" })
        assert(anim.frames == 2 and anim.width == 4)
        assert(image.hex(anim:frame(2):getPixel(0, 0)) == "#000000")
        assert(image.hex(anim:frame(2):getPixel(3, 3)) == "#0000ff")
    )lua");
    }

    TEST_CASE("image: compositing and drawing") {
        RUN_IMAGE(R"lua(
        local function hex(i, x, y) return image.hex(i:getPixel(x, y)) end
        local canvas = image.new(20, 20, "white")
        canvas:composite(image.new(4, 4, "red"), { x = 1, y = 2 })
        assert(hex(canvas, 1, 2) == "#ff0000" and hex(canvas, 0, 2) == "#ffffff")
        canvas:composite(image.new(4, 4, "blue"), { gravity = "bottom-right" })
        assert(hex(canvas, 19, 19) == "#0000ff" and hex(canvas, 15, 15) == "#ffffff")
        canvas:composite(image.new(2, 2, "black"), { gravity = "center", blend = "multiply" })
        assert(hex(canvas, 10, 10) == "#000000")
        local faded = image.new(4, 4, "white"):composite(image.new(4, 4, "black"), { opacity = 0.5 })
        assert(faded:getPixel(0, 0).r > 100 and faded:getPixel(0, 0).r < 150)

        local img = image.new(50, 50, "white")
        img:drawRect(10, 10, 10, 10, { fill = "red" })
        assert(hex(img, 10, 10) == "#ff0000" and hex(img, 19, 19) == "#ff0000" and hex(img, 20, 20) == "#ffffff")
        img:drawRect(30, 30, 10, 10, { stroke = "blue", antialias = false }) -- an outline stays hollow
        assert(hex(img, 30, 30) == "#0000ff" and hex(img, 35, 35) == "#ffffff")
        img:drawCircle(25, 5, 3, { fill = "lime" })
        assert(hex(img, 25, 5) == "#00ff00")
        img:drawLine(0, 49, 49, 49, { stroke = "black", antialias = false })
        assert(hex(img, 25, 49) == "#000000")
        img:drawPolygon({ { 0, 25 }, { 8, 25 }, { 4, 33 } }, { fill = "blue" })
        assert(hex(img, 4, 27) == "#0000ff")
        img:drawEllipse(40, 10, 5, 3):drawPolyline({ { x = 0, y = 0 }, { x = 5, y = 5 } }, { dash = { 1, 1 } })

        local text = image.new(200, 60, "white"):drawText("Hi", 5, 5, { size = 30, color = "black" })
        local ink = text:clone():trim()
        assert(ink.width < 100 and ink.height < 50) -- something was drawn near the corner
        assert(text:clone():crop(100, 0, 100, 60):colorCount() == 1) -- and nothing on the right

        local m = image.measureText("Hello", { size = 20 })
        assert(m.width > 20 and m.height > 10 and m.ascent > 0 and m.descent <= 0)
        local label = image.text("Hello", { size = 20, padding = 3, background = "white" })
        assert(label.width == math.ceil(m.width) + 6 and label.height == math.ceil(m.height) + 6)
        assert(image.measureText("Hello", { size = 20, weight = 700 }).width >= m.width)
    )lua");
    }

    TEST_CASE("image: analysis") {
        RUN_IMAGE(R"lua(
        local a = image.new(10, 10, "white")
        local b = a:clone():setPixel(5, 5, "black")
        local same = a:compare(a:clone())
        assert(same == 0)
        local distance, diff = a:compare(b)
        assert(distance > 0 and typeof(diff) == "Image")
        assert(a:signature() ~= b:signature())
        local ok, err = pcall(function() return a:compare(image.new(5, 5)) end)
        assert(not ok and string.find(err, "different sizes"), err)

        local hist = b:histogram()
        assert(#hist == 2 and hist[1].hex == "#ffffff" and hist[1].count == 99 and hist[2].count == 1)
        assert(math.abs(hist[1].fraction - 0.99) < 1e-9)
        assert(#b:histogram(1) == 1)
        assert(b:colorCount() == 2)

        local half = image.append({ image.new(30, 10, "red"), image.new(10, 10, "blue") })
        local main = half:dominantColors(2)
        assert(main[1].hex == "#ff0000" and main[2].hex == "#0000ff" and main[1].fraction > main[2].fraction)
        assert(image.hex(image.new(4, 4, "#336699"):averageColor()) == "#336699")
        local stats = image.new(4, 4, "white"):stats()
        assert(stats.mean == 1 and stats.max == 1 and stats.deviation == 0)
    )lua");
    }

    TEST_CASE("image: animations") {
        RUN_IMAGE(R"lua(
        local frames = { image.new(6, 6, "red"), image.new(6, 6, "lime"), image.new(6, 6, "blue") }
        local anim = image.animate(frames, { delay = 250, loop = 2 })
        assert(anim.frames == 3)
        assert(table.concat(anim:delays(), ",") == "250,250,250")
        anim:setDelay(100, 2)
        assert(table.concat(anim:delays(), ",") == "250,100,250")
        anim:save(dir .. "a.gif")

        local gif = image.load(dir .. "a.gif")
        assert(gif.format == "gif" and gif.frames == 3)
        assert(table.concat(gif:delays(), ",") == "250,100,250")
        assert(image.hex(gif:frame(3):getPixel(0, 0)) == "#0000ff")
        assert(image.load(dir .. "a.gif", { frame = 2 }).frames == 1)
        assert(image.info(dir .. "a.gif").frames == 3)
        assert(gif:clone():coalesce().frames == 3 and gif:clone():optimize().frames == 3)
        assert(gif:clone():flatten().frames == 1)
        assert(gif:setLoop(0) == gif)

        -- Formats with one image keep the first frame.
        assert(image.decode(gif:encode("png")).frames == 1)
        assert(not pcall(gif.frame, gif, 4))
    )lua");
    }

    TEST_CASE("image: grids, append and generated images") {
        RUN_IMAGE(R"lua(
        local tiles = {}
        for i = 1, 5 do table.insert(tiles, image.new(10, 10, "red")) end
        local grid = image.grid(tiles, { gap = 2 })
        assert(grid.width == 3 * 10 + 2 * 2 and grid.height == 2 * 10 + 2)
        assert(image.grid(tiles, { columns = 5 }).width == 50)
        assert(image.grid({ image.new(40, 40) }, { cellWidth = 10, cellHeight = 10 }).width == 10)

        local row = image.append({ image.new(10, 4), image.new(10, 8) }, { gap = 1, align = "end" })
        assert(row.width == 21 and row.height == 8)

        assert(image.generate("xc:red", 3, 2).width == 3)
        assert(image.generate("gradient:white-black", 4, 16):getPixel(0, 0).r > 200)
        assert(image.generate("pattern:checkerboard", 32, 32).height == 32)
        assert(image.generate("rose:").width == 70)
        assert(image.generate("label:Hello").width > 10)
        local ok, err = pcall(image.generate, "text:/etc/passwd")
        assert(not ok and string.find(err, "spec must start with"), err)
    )lua");
    }

    TEST_CASE("image: label text starting with @ is not a file name") {
        RUN_IMAGE(R"lua(
        local fs = require("@sonata/fs")
        fs.writeFile(dir .. "secret.txt", string.rep("this file must not be rendered ", 4))
        local label = image.generate("label:@" .. dir .. "secret.txt")
        local plain = image.generate("label:X" .. dir .. "secret.txt")
        assert(math.abs(label.width - plain.width) < 10, label.width .. " vs " .. plain.width)
    )lua");
    }

    TEST_CASE("image: helpers") {
        RUN_IMAGE(R"lua(
        local formats = image.formats()
        assert(table.find(formats, "png") and table.find(formats, "jpeg") and table.find(formats, "gif"))
        assert(#image.options("filter") > 10 and table.find(image.options("compose"), "Multiply"))
        assert(not table.find(image.options("filter"), "Undefined"))
        for _, kind in { "alpha", "channel", "colorspace", "distort", "kernel", "linecap", "linejoin",
                         "metric", "morphology", "noise" } do
            assert(#image.options(kind) > 0, kind)
        end
        assert(typeof(image.fonts()) == "table")

        local c = image.color("tomato")
        assert(c.r == 255 and c.g == 99 and c.b == 71 and c.a == 255)
        assert(image.color("none").a == 0)
        assert(image.hex({ r = 255, g = 0, b = 16 }) == "#ff0010")
        assert(image.hex({ r = 255, g = 0, b = 16, a = 128 }) == "#ff001080")
        assert(image.hex("rgb(0, 0, 255)") == "#0000ff")

        local limits = image.limits()
        assert(limits.threads > 0 and limits.width > 0)
        assert(image.limits({ threads = limits.threads }).threads == limits.threads)

        local img = image.new(2, 2):setResolution(300)
        local x, y = img:resolution()
        assert(x == 300 and y == 300)
    )lua");
    }

    TEST_CASE("image: argument errors name the function") {
        CHECK(contains(errorOf("image.new(10, 10):resize('x')"), "Image:resize: width must be a number, got string"));
        CHECK(contains(errorOf("image.new(10, 10):resize()"), "give a width, a height, or both"));
        CHECK(contains(errorOf("image.new(10, 10):resize(10.5)"), "whole number"));
        CHECK(contains(errorOf("image.new(10, 10):resize(5, nil, { fitt = 'cover' })"), "unknown option 'fitt'"));
        CHECK(contains(errorOf("image.new(10, 10):resize(5, nil, { filter = 'nope' })"),
                       "unknown filter 'nope' (image.options(\"filter\") lists them)"));
        CHECK(contains(errorOf("image.new(10, 10).resize(5)"), "use ':' to call methods"));
        CHECK(contains(errorOf("image.new(10, 10):getPixel(10, 0)"), "is outside the 10x10 image"));
        CHECK(contains(errorOf("image.new(10, 10, 'notacolor')"), "is not a color ImageMagick knows"));
        CHECK(contains(errorOf("image.new(10, 10, { r = 300, g = 0, b = 0 })"), "from 0 to 255"));
        CHECK(contains(errorOf("image.new(0, 10)"), "width must be a whole number from 1"));
        CHECK(contains(errorOf("image.new(10, 10):composite({})"), "other must be an Image"));
        CHECK(contains(errorOf("image.new(10, 10):drawText('x', 0, 0, { gravity = 'up' })"), "must be a gravity"));
        CHECK(contains(errorOf("image.new(10, 10):blur(-1)"), "sigma must be between"));
        CHECK(contains(errorOf("image.options('nope')"), "unknown kind 'nope'"));
        CHECK(contains(errorOf("image.new(4, 4):morphology('dilate', '@/etc/passwd')"), "aren't supported"));
    }

    TEST_CASE("image: closed images") {
        RUN_IMAGE(R"lua(
        local img = image.new(4, 4)
        img:close()
        img:close() -- twice is fine
        assert(tostring(img) == "Image(closed)")
        local ok, err = pcall(function() return img.width end)
        assert(not ok and string.find(err, "closed"), err)
        ok, err = pcall(function() return img:resize(2) end)
        assert(not ok and string.find(err, "Image:resize: self is a closed image"), err)
        ok, err = pcall(function() return image.new(4, 4):composite(img) end)
        assert(not ok and string.find(err, "other is a closed image"), err)
    )lua");
    }
}
