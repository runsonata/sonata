--[[
	Type definitions for @sonata/image, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Load, edit and save images, backed by ImageMagick.

	```luau
	image.load("photo.jpg")
		:autoOrient()
		:resize(800)
		:drawText("© me", 10, 10, { gravity = "bottom-right", size = 18, color = "white" })
		:save("photo.webp", { quality = 85 })
	```

	Methods change the image in place, apply to every frame of an animation, and return the
	image so they chain; `img:clone()` keeps the original. Pixel coordinates start at 0 in the
	top-left corner. Fuzz, thresholds and levels are fractions from 0 to 1. Names such as
	filters (`"lanczos"`), blend modes (`"multiply"`) and gravities (`"bottom-right"`) are
	case-insensitive; `image.options(kind)` lists the ones ImageMagick knows.
]=]
local image = {}

--- An ImageMagick color string (`"red"`, `"#ff000080"`, `"rgba(255,0,0,0.5)"`, `"none"`) or channels from 0 to 255.
export type ColorInput = string | { r: number, g: number, b: number, a: number? }

--- A color with every channel from 0 to 255 (`a` 255 is opaque).
export type Color = { r: number, g: number, b: number, a: number }

--- Where something goes inside a larger area.
export type Gravity =
	"top-left"
	| "top"
	| "top-right"
	| "left"
	| "center"
	| "right"
	| "bottom-left"
	| "bottom"
	| "bottom-right"
	| string

export type ReadOptions = {
	--- The format, for data without a recognizable signature (`"tga"`, `"rgba"`, ...).
	format: string?,
	--- Pixels per inch for vector formats (SVG, PDF). Higher is sharper.
	density: number?,
	--- Only keep this frame (1-based) of a multi-frame image.
	frame: number?,
	--- ImageMagick coder options, such as `{ ["jpeg:size"] = "400x400" }`.
	define: { [string]: string | number | boolean }?,
}

export type EncodeOptions = {
	--- 1 to 100, for lossy formats (JPEG, WebP, AVIF, ...); also PNG's compression level.
	quality: number?,
	--- Lossless WebP, JPEG XL or HEIC.
	lossless: boolean?,
	--- Progressive JPEG, interlaced PNG or GIF.
	progressive: boolean?,
	--- Leave out metadata (EXIF, ICC profiles, comments).
	strip: boolean?,
	--- What transparent pixels become in formats without transparency, such as JPEG. Default white.
	background: ColorInput?,
	--- ImageMagick coder options, such as `{ ["png:compression-level"] = 9 }`.
	define: { [string]: string | number | boolean }?,
}

export type SaveOptions = EncodeOptions & {
	--- The format; by default the path's extension decides.
	format: string?,
}

export type ResizeOptions = {
	--- With both sizes: `"fill"` stretches to exactly width x height (default for `resize`),
	--- `"contain"` fits inside (default for `thumbnail`), `"cover"` fills the box and crops the rest.
	fit: ("fill" | "contain" | "cover")?,
	--- The resampling filter (`"lanczos"`, `"mitchell"`, `"point"`, ...; `image.options("filter")`).
	filter: string?,
	--- With `fit = "cover"`: which part to keep. Default `"center"`.
	gravity: Gravity?,
	--- `false` never makes the image bigger. Default `true` (`false` for `thumbnail`).
	enlarge: boolean?,
}

--- Style for shapes. Without fill or stroke a shape is filled black; with only a stroke it stays hollow.
export type ShapeStyle = {
	fill: ColorInput?,
	stroke: ColorInput?,
	--- Default 1.
	strokeWidth: number?,
	--- Default true.
	antialias: boolean?,
	--- Rounded corners for `drawRect`.
	radius: number?,
	--- Dash and gap lengths, such as `{ 6, 3 }`.
	dash: { number }?,
	--- `"butt"`, `"round"` or `"square"`.
	lineCap: string?,
	--- `"miter"`, `"round"` or `"bevel"`.
	lineJoin: string?,
}

export type TextStyle = {
	--- Default black.
	color: ColorInput?,
	--- In points. Default 16.
	size: number?,
	--- A font file path, or an ImageMagick font name (`image.fonts()`).
	font: string?,
	--- A font family name, such as `"DejaVu Sans"`.
	family: string?,
	--- 100 to 900; 700 is bold.
	weight: number?,
	italic: boolean?,
	stroke: ColorInput?,
	strokeWidth: number?,
	--- What `x` and `y` are relative to. Default `"top-left"`.
	gravity: Gravity?,
	--- Rotation in degrees.
	angle: number?,
	--- A box color behind the text.
	background: ColorInput?,
	antialias: boolean?,
	--- Extra space between letters.
	kerning: number?,
	--- Extra space between lines.
	lineSpacing: number?,
	--- Extra space between words.
	wordSpacing: number?,
}

export type CompositeOptions = {
	--- Offset in pixels, from the gravity's edge. Default 0.
	x: number?,
	y: number?,
	--- Default `"top-left"`.
	gravity: Gravity?,
	--- How the pixels mix: `"over"` (default), `"multiply"`, `"screen"`, `"overlay"`, ... (`image.options("compose")`).
	blend: string?,
	--- 0 to 1. Default 1.
	opacity: number?,
}

export type ColorCount = {
	color: Color,
	--- `"#rrggbb"`, or `"#rrggbbaa"` when not opaque.
	hex: string,
	--- Pixels with this color.
	count: number,
	--- Share of all the pixels, 0 to 1.
	fraction: number,
}

export type Info = {
	width: number,
	height: number,
	format: string,
	frames: number,
}

export type TextMetrics = {
	width: number,
	height: number,
	--- Pixels above the baseline.
	ascent: number,
	--- Pixels below the baseline (negative).
	descent: number,
}

export type Limits = {
	--- Bytes of memory for pixels before ImageMagick moves them to disk.
	memory: number?,
	--- Bytes of disk for pixels.
	disk: number?,
	--- Bytes of memory-mapped files.
	map: number?,
	--- Pixels in one image.
	area: number?,
	--- Widest image.
	width: number?,
	--- Tallest image.
	height: number?,
	threads: number?,
	--- Seconds an operation may take.
	time: number?,
	files: number?,
}

export type Image = {
	--- Width of the first frame, in pixels.
	width: number,
	--- Height of the first frame, in pixels.
	height: number,
	--- The format it was read from (`"png"`, `"jpeg"`, ...); `nil` for new images.
	format: string?,
	--- Number of frames (1 unless animated or multi-page).
	frames: number,
	--- Bits per channel.
	depth: number,
	--- `"sRGB"`, `"Gray"`, `"CMYK"`, ...
	colorspace: string,
	hasAlpha: boolean,

	-- Conversion and pixels

	--- An independent copy. Cheap: pixels are shared until one of them changes.
	clone: (self: Image) -> Image,
	--- Frees the pixels now instead of at garbage collection. Using the image afterwards raises.
	close: (self: Image) -> (),
	--- Writes the image. The format comes from `options.format` or the extension. Formats without
	--- transparency (JPEG) put transparent images on `options.background` (white).
	save: (self: Image, path: string, options: SaveOptions?) -> Image,
	--- The image's bytes in `format` (default: its own format, else PNG).
	encode: (self: Image, format: string?, options: EncodeOptions?) -> string,
	--- Raw 8-bit samples of the first frame, row by row. `channels` defaults to `"rgba"`
	--- (`"rgb"`, `"bgra"`, `"gray"`, ...); the region defaults to the whole image.
	pixels: (
		self: Image,
		options: { channels: string?, x: number?, y: number?, width: number?, height: number? }?
	) -> buffer,
	--- The first frame's pixel at (x, y).
	getPixel: (self: Image, x: number, y: number) -> Color,
	setPixel: (self: Image, x: number, y: number, color: ColorInput) -> Image,
	--- A copy of frame `index` (1-based).
	frame: (self: Image, index: number) -> Image,
	--- EXIF and other properties of the first frame, optionally matching a pattern such as `"exif:*"`.
	metadata: (self: Image, pattern: string?) -> { [string]: string },
	--- Sets a property such as `"comment"`; `nil` removes it.
	setMetadata: (self: Image, name: string, value: string?) -> Image,
	--- Removes EXIF, ICC profiles and comments.
	strip: (self: Image) -> Image,
	--- SHA-256 of the first frame's pixels (hex): equal pixels, equal signature.
	signature: (self: Image) -> string,
	--- Pixels per inch.
	resolution: (self: Image) -> (number, number),
	setResolution: (self: Image, x: number, y: number?) -> Image,

	-- Geometry

	--- Resizes. With one size the other keeps the aspect ratio: `img:resize(800)`, `img:resize(nil, 600)`.
	resize: (self: Image, width: number?, height: number?, options: ResizeOptions?) -> Image,
	--- Like `resize` for previews: faster, strips metadata, fits inside and never enlarges by default.
	thumbnail: (self: Image, width: number?, height: number?, options: ResizeOptions?) -> Image,
	--- Resizes by a factor: 0.5 halves, 2 doubles.
	scale: (self: Image, factor: number, factorY: number?) -> Image,
	crop: (self: Image, x: number, y: number, width: number, height: number) -> Image,
	--- Pads (or crops) to exactly width x height. Default gravity `"center"`, background `"none"`.
	extent: (
		self: Image,
		width: number,
		height: number,
		options: { gravity: Gravity?, background: ColorInput? }?
	) -> Image,
	--- Cuts away borders that have the corner's color (within `fuzz`, 0 to 1).
	trim: (self: Image, fuzz: number?) -> Image,
	--- Clockwise. The uncovered corners get `background` (default transparent).
	rotate: (self: Image, degrees: number, background: ColorInput?) -> Image,
	--- Upside down.
	flip: (self: Image) -> Image,
	--- Left-right.
	mirror: (self: Image) -> Image,
	--- Mirrors along the top-left to bottom-right diagonal.
	transpose: (self: Image) -> Image,
	--- Mirrors along the top-right to bottom-left diagonal.
	transverse: (self: Image) -> Image,
	--- Rotates the way the camera's EXIF orientation says, then resets it.
	autoOrient: (self: Image) -> Image,
	--- Shears by `x` and `y` degrees.
	shear: (self: Image, x: number, y: number?, background: ColorInput?) -> Image,
	--- Shifts the pixels, wrapping around the edges.
	roll: (self: Image, x: number, y: number) -> Image,
	--- Adds `size` pixels of `color` (default black) on every side.
	border: (self: Image, size: number, color: ColorInput?) -> Image,
	--- ImageMagick's distortions: `"perspective"`, `"arc"`, `"barrel"`, `"polar"`, `"srt"`, ...
	--- (`image.options("distort")`), with their numeric arguments.
	distort: (
		self: Image,
		method: string,
		arguments: { number },
		options: { bestFit: boolean?, background: ColorInput? }?
	) -> Image,
	--- Content-aware resize (seam carving).
	liquidRescale: (self: Image, width: number, height: number) -> Image,
	--- Straightens a scanned page. Threshold 0 to 1, default 0.4.
	deskew: (self: Image, threshold: number?) -> Image,

	-- Color and tone

	grayscale: (self: Image) -> Image,
	negate: (self: Image) -> Image,
	--- Threshold 0 to 1, default 0.8.
	sepia: (self: Image, threshold: number?) -> Image,
	--- Each from -100 to 100.
	brightnessContrast: (self: Image, brightness: number, contrast: number?) -> Image,
	--- Percentages; 100 leaves a property unchanged, 0 saturation is gray.
	modulate: (self: Image, brightness: number?, saturation: number?, hue: number?) -> Image,
	--- Above 1 brightens, below 1 darkens.
	gamma: (self: Image, value: number) -> Image,
	--- `black` and `white` (0 to 1) become the new darkest and brightest values.
	level: (self: Image, black: number?, white: number?, gamma: number?) -> Image,
	--- Stretches the values to use the full range.
	normalize: (self: Image) -> Image,
	equalize: (self: Image) -> Image,
	autoLevel: (self: Image) -> Image,
	autoGamma: (self: Image) -> Image,
	--- Sigmoidal contrast: positive strength adds contrast, negative removes it (default 3).
	contrast: (self: Image, strength: number?, midpoint: number?) -> Image,
	--- Blends every pixel toward `color` by `amount` (0 to 1, default 0.5).
	colorize: (self: Image, color: ColorInput, amount: number?) -> Image,
	--- Tints the midtones toward `color`.
	tint: (self: Image, color: ColorInput, amount: number?) -> Image,
	--- Pixels above `value` (0 to 1, default 0.5) become white, the rest black.
	threshold: (self: Image, value: number?) -> Image,
	--- `levels` per channel.
	posterize: (self: Image, levels: number, dither: boolean?) -> Image,
	--- At most `colors` colors.
	quantize: (self: Image, colors: number, options: { dither: boolean?, colorspace: string? }?) -> Image,
	--- Pure black and white, dithered.
	monochrome: (self: Image) -> Image,
	solarize: (self: Image, threshold: number?) -> Image,
	--- Replaces `from` (and colors within `fuzz`, 0 to 1) with `to`.
	replaceColor: (self: Image, from: ColorInput, to: ColorInput, fuzz: number?) -> Image,
	--- Makes `color` (and colors within `fuzz`) transparent.
	transparent: (self: Image, color: ColorInput, fuzz: number?) -> Image,
	--- Paint bucket at (x, y).
	floodFill: (self: Image, x: number, y: number, color: ColorInput, fuzz: number?) -> Image,
	--- `"on"`, `"off"`, `"remove"`, `"opaque"`, `"transparent"`, `"extract"`, ... (`image.options("alpha")`).
	alpha: (self: Image, mode: string) -> Image,
	--- Multiplies every pixel's alpha by `amount` (0 to 1).
	opacity: (self: Image, amount: number) -> Image,
	--- Merges every frame onto `background` (default white), removing transparency.
	flatten: (self: Image, background: ColorInput?) -> Image,
	--- Converts to `"gray"`, `"cmyk"`, `"lab"`, `"hsl"`, `"srgb"`, ... (`image.options("colorspace")`).
	toColorspace: (self: Image, name: string) -> Image,
	--- Keeps one channel (`"red"`, `"alpha"`, ...) as a grayscale image.
	channel: (self: Image, name: string) -> Image,
	--- An ImageMagick FX expression for every pixel, such as `"u*0.5"`.
	fx: (self: Image, expression: string) -> Image,

	-- Filters and effects

	--- `sigma` is the strength (default 2); `radius` 0 picks one.
	blur: (self: Image, sigma: number?, radius: number?) -> Image,
	sharpen: (self: Image, sigma: number?, radius: number?) -> Image,
	--- The photographer's sharpen.
	unsharp: (self: Image, sigma: number?, amount: number?, threshold: number?, radius: number?) -> Image,
	motionBlur: (self: Image, sigma: number?, angle: number?, radius: number?) -> Image,
	rotationalBlur: (self: Image, angle: number?) -> Image,
	--- `"gaussian"` (default), `"impulse"`, `"poisson"`, `"uniform"`, ... (`image.options("noise")`).
	noise: (self: Image, type: string?, amount: number?) -> Image,
	despeckle: (self: Image) -> Image,
	--- Removes noise while keeping edges.
	median: (self: Image, size: number?) -> Image,
	charcoal: (self: Image, sigma: number?, radius: number?) -> Image,
	sketch: (self: Image, sigma: number?, angle: number?, radius: number?) -> Image,
	oilPaint: (self: Image, radius: number?) -> Image,
	emboss: (self: Image, sigma: number?, radius: number?) -> Image,
	edge: (self: Image, radius: number?) -> Image,
	swirl: (self: Image, degrees: number?) -> Image,
	--- Positive pulls toward the center, negative pushes out.
	implode: (self: Image, amount: number?) -> Image,
	--- The image grows by 2 * amplitude vertically.
	wave: (self: Image, amplitude: number?, wavelength: number?) -> Image,
	spread: (self: Image, radius: number?) -> Image,
	--- Fades the edges to `color` (default black).
	vignette: (self: Image, sigma: number?, color: ColorInput?, x: number?, y: number?) -> Image,
	--- Blocks of size x size pixels (default 8).
	pixelate: (self: Image, size: number?) -> Image,
	--- `"dilate"`, `"erode"`, `"open"`, `"close"`, `"edge"`, ... with an ImageMagick kernel such as `"disk:3"`.
	morphology: (self: Image, method: string, kernel: string, iterations: number?) -> Image,
	--- A square array of numbers (row by row), or ImageMagick kernel text.
	convolve: (self: Image, kernel: { number } | string) -> Image,

	-- Compositing and drawing

	--- Draws `other`'s first frame on top.
	composite: (self: Image, other: Image, options: CompositeOptions?) -> Image,
	--- `style.radius` rounds the corners.
	drawRect: (self: Image, x: number, y: number, width: number, height: number, style: ShapeStyle?) -> Image,
	--- Centered on (x, y).
	drawCircle: (self: Image, x: number, y: number, radius: number, style: ShapeStyle?) -> Image,
	drawEllipse: (self: Image, x: number, y: number, radiusX: number, radiusY: number, style: ShapeStyle?) -> Image,
	drawLine: (self: Image, x1: number, y1: number, x2: number, y2: number, style: ShapeStyle?) -> Image,
	--- Points as `{ x, y }` or `{ x = x, y = y }`.
	drawPolygon: (self: Image, points: { { number } | { x: number, y: number } }, style: ShapeStyle?) -> Image,
	drawPolyline: (self: Image, points: { { number } | { x: number, y: number } }, style: ShapeStyle?) -> Image,
	--- (x, y) is the top-left corner of the text, or an offset from `style.gravity`'s edge.
	drawText: (self: Image, text: string, x: number?, y: number?, style: TextStyle?) -> Image,

	-- Analysis (first frame)

	--- How different two same-sized images are (0 = identical; `"rmse"` by default,
	--- `image.options("metric")`), and an image that highlights the differences.
	compare: (self: Image, other: Image, metric: string?) -> (number, Image),
	--- Over every channel, 0 to 1.
	stats: (self: Image) -> { mean: number, deviation: number, min: number, max: number },
	--- Every color and its pixel count, most common first.
	histogram: (self: Image, limit: number?) -> { ColorCount },
	--- The main colors (default 5), most common first.
	dominantColors: (self: Image, count: number?) -> { ColorCount },
	averageColor: (self: Image) -> Color,
	--- Number of distinct colors.
	colorCount: (self: Image) -> number,

	-- Animation

	--- Turns optimized animation frames into complete frames (do this before editing a GIF).
	coalesce: (self: Image) -> Image,
	--- Stores only what changes between frames, for smaller GIFs.
	optimize: (self: Image) -> Image,
	--- For every frame, or just `frame` (1-based).
	setDelay: (self: Image, milliseconds: number, frame: number?) -> Image,
	--- Each frame's delay, in milliseconds.
	delays: (self: Image) -> { number },
	--- 0 loops forever.
	setLoop: (self: Image, count: number) -> Image,
}

--- Reads an image file. Animated and multi-page files keep every frame.
function image.load(path: string, options: ReadOptions?): Image
	return nil :: any
end

--- Reads an image from its bytes.
function image.decode(data: string | buffer, options: ReadOptions?): Image
	return nil :: any
end

--- Size, format and frame count, without decoding the pixels. A string is a path; a buffer holds the bytes.
function image.info(source: string | buffer): Info
	return nil :: any
end

--- A blank canvas, transparent unless `background` is given.
function image.new(width: number, height: number, background: ColorInput?): Image
	return nil :: any
end

--[=[
	An image ImageMagick makes from a description: `"xc:red"` (a solid color),
	`"gradient:red-blue"`, `"radial-gradient:white-black"`, `"plasma:"`, `"pattern:checkerboard"`,
	`"label:Text"`, `"caption:Wrapped text"`, `"hald:8"`, and the built-in `"rose:"`, `"logo:"`,
	`"wizard:"`, `"granite:"`, `"netscape:"`. Gradients and patterns need a size.
]=]
function image.generate(spec: string, width: number?, height: number?): Image
	return nil :: any
end

--- An image from raw 8-bit samples, row by row. `channels` defaults to `"rgba"`.
function image.fromPixels(width: number, height: number, data: buffer | string, channels: string?): Image
	return nil :: any
end

--- Text on a canvas that just fits it. `background` is the canvas color (default transparent).
function image.text(text: string, style: (TextStyle & { padding: number? })?): Image
	return nil :: any
end

--- The size text would have when drawn with `style`.
function image.measureText(text: string, style: TextStyle?): TextMetrics
	return nil :: any
end

--- One animation out of every frame of `frames`. `delay` is in milliseconds; `loop` 0 repeats forever.
function image.animate(frames: { Image }, options: { delay: number?, loop: number? }?): Image
	return nil :: any
end

--- The first frames side by side, or stacked with `vertical`.
function image.append(
	images: { Image },
	options: {
		vertical: boolean?,
		gap: number?,
		background: ColorInput?,
		align: ("start" | "center" | "end")?,
	}?
): Image
	return nil :: any
end

--- A contact sheet: the first frames in a grid of equal cells (images too big for a cell shrink to fit).
function image.grid(
	images: { Image },
	options: {
		columns: number?,
		gap: number?,
		background: ColorInput?,
		cellWidth: number?,
		cellHeight: number?,
	}?
): Image
	return nil :: any
end

--- Every format this ImageMagick reads or writes, lowercase.
function image.formats(): { string }
	return nil :: any
end

--- Font names usable as `style.font`.
function image.fonts(): { string }
	return nil :: any
end

--- The names ImageMagick accepts for `kind`.
function image.options(
	kind: "alpha"
		| "channel"
		| "colorspace"
		| "compose"
		| "distort"
		| "filter"
		| "kernel"
		| "linecap"
		| "linejoin"
		| "metric"
		| "morphology"
		| "noise"
): { string }
	return nil :: any
end

--- Parses any color into channels from 0 to 255.
function image.color(color: ColorInput): Color
	return nil :: any
end

--- `"#rrggbb"`, or `"#rrggbbaa"` when the color isn't opaque.
function image.hex(color: ColorInput): string
	return nil :: any
end

--- Sets some of ImageMagick's resource limits (for the whole process) and returns all of them.
function image.limits(limits: Limits?): Limits
	return nil :: any
end

--- The ImageMagick version, such as `"7.1.1-43"`.
image.version = (nil :: any) :: string

return image
