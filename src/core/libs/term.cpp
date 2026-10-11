// @sonata/term: the terminal and the standard streams.
//
//     local term = require("@sonata/term")
//
// Streams (this used to be @sonata/stdio)
//     term.stdin / term.stdout / term.stderr
//
//     stream:write(...)             --> stream?, string?   strings and numbers only; returns the stream so calls chain
//     stream:writeLine(...)         --> stream?, string?   like write, then a newline
//     stream:flush()                --> stream?, string?
//     stream:readLine(keepEnding)   --> string?, string?   next line, without its line ending unless keepEnding is
//                                                          true; nil at end of input
//     stream:read(count)            --> string?, string?   exactly "count" bytes (waits until they have all arrived);
//                                                          fewer only at EOF; nil at EOF
//     stream:readSome(maxCount)     --> string?, string?   up to "maxCount" bytes, returned as soon as at least one is
//                                                          available; nil at EOF
//     stream:readAll()              --> string?, string?   everything until EOF ("" when already at EOF)
//     stream:lines(keepEnding)      --> iterator           for line in term.stdin:lines() do ... end
//     stream:isTerminal()           --> boolean
//     stream:size()                 --> columns, rows      (nil if the stream is not a terminal)
//     term.prompt("Name: ")         --> string?, string?   writes to stdout, flushes, then reads a line from stdin
//     term.password("Password: ", mask?) --> string?      the same without echoing what is typed; "mask" (e.g. "*")
//                                                          is shown for each character instead. nil at EOF or Ctrl+C
//     term.size()                   --> columns, rows      of the terminal (nil if there is none)
//
//   I/O failures never raise: every method that can fail returns nil and a message instead (so "nil" alone means
//   end of input, "nil, message" means failure). Wrong arguments and using a stream in the wrong direction still
//   raise: write methods only work on stdout / stderr and read methods only on stdin. A line ends at "\n", "\r\n"
//   or a lone "\r". Reading waits without blocking other tasks (see @sonata/task); lines() is the exception, as
//   an iterator cannot let them run.
//
// Colors and styles
//     term.colorLevel(stream?)      --> 0 | 1 | 2 | 3       none, 16 colors, 256 colors, true color; for stdout unless
//                                                          another stream is given. Honours NO_COLOR, FORCE_COLOR,
//                                                          TERM and COLORTERM; 0 when the stream is not a terminal.
//     term.setColorLevel(level?)    forces a level for every stream; nil goes back to detecting it
//     term.style(text, ...)         --> string             text wrapped in the given styles (for stdout's level;
//                                                          plain text at level 0), e.g. term.style("ok", "bold", "green")
//         modifiers: "bold" "dim" "italic" "underline" "blink" "inverse" "hidden" "strikethrough"
//         colors:    "black" "red" "green" "yellow" "blue" "magenta" "cyan" "white" "gray",
//                    "brightRed" ... "brightWhite", and "bg" + any of them: "bgRed", "bgBrightBlue", ...
//         exact:     "#ff8800" / "#f80" (text), "bg:#ff8800" (background), "fg:208" / "bg:208" (256-color palette);
//                    turned into the nearest color the terminal can show
//     term.strip(text)              --> string             removes escape sequences
//     term.width(text)              --> number             columns it takes on screen (escapes take none, wide
//                                                          characters such as CJK and emoji take 2)
//
// Cursor and screen (written to stdout, and only if stdout is a terminal)
//     term.moveTo(column, row)      1-based          term.move(dx, dy)       relative
//     term.hideCursor()             term.showCursor()
//     term.saveCursor()             term.restoreCursor()
//     term.clear(what?)             "screen" (default: clears and moves home), "below", "above",
//                                   "line", "lineRight", "lineLeft"
//     term.altScreen(on)            switches to / from the alternate screen
//     term.setTitle(text)           term.bell()
//     term.cursorPosition()         --> column, row  (asks the terminal; nil if it does not answer)
//     term.ansi                     the same as strings, to build output yourself:
//         .moveTo(c, r), .move(dx, dy), .clear(what?), .style(...) (just the opening codes),
//         .hideCursor, .showCursor, .saveCursor, .restoreCursor, .altScreenOn, .altScreenOff, .reset
//
// Keyboard
//     term.rawMode(on)              --> boolean      previous state. Raw: no line editing, no echo, Ctrl+C arrives as
//                                                    a key instead of a signal. Restored when the program exits.
//     term.isRaw()                  --> boolean
//     term.readKey(timeout?)        --> Key?         waits for one key press (in raw mode for the duration if it is
//                                                    not on already); nil on timeout or end of input
//         Key = { name: string, char: string?, ctrl: boolean, alt: boolean, shift: boolean, sequence: string }
//         name: the character itself ("a", "A", "é", " ") or "enter" "tab" "backspace" "escape" "space"
//               "up" "down" "left" "right" "home" "end" "pageUp" "pageDown" "insert" "delete" "f1".."f12";
//               Ctrl+letter gives the letter with ctrl = true. char is set for printable keys.
//
// Output goes through C stdio, so it stays in order with print() and with child processes started by
// @sonata/process. Writing never flushes by itself: stdout follows the usual C buffering (line-buffered on a
// terminal), so call flush() when output must appear immediately. Input is read straight from the OS descriptor
// through one process-wide buffer instead of C's "stdin", so nothing else in the process should read C stdin;
// stdout is flushed before every blocking read so a prompt written with write() is visible.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <sonata/core/library.hpp>
#include <sonata/core/scheduler.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

using sched::Scheduler;

constexpr const char* kStreamMetatable = "sonata.term.stream";
constexpr const char* kStreamNames[] = {"stdin", "stdout", "stderr"};
constexpr std::size_t kInputChunkSize = 64 * 1024;
constexpr double kForever = std::numeric_limits<double>::infinity();
constexpr int kEscapeWaitMs = 30; // how long a lone ESC waits for the rest of a sequence

// A stream object is a userdata holding its descriptor number: 0, 1 or 2.
int checkStream(lua_State* L) {
    return *static_cast<const int*>(luaL_checkudata(L, 1, kStreamMetatable));
}

int checkWritable(lua_State* L) {
    const int fd = checkStream(L);
    luaL_argcheck(L, fd != 0, 1, "stdin cannot be written to");
    return fd;
}

int checkReadable(lua_State* L) {
    const int fd = checkStream(L);
    luaL_argcheck(L, fd == 0, 1, "only stdin can be read from");
    return fd;
}

std::FILE* fileOf(int fd) {
    return fd == 0 ? stdin : (fd == 1 ? stdout : stderr);
}

// platform layer //////////////////////////////

bool isTerminal(int fd) {
#if defined(_WIN32)
    return _isatty(fd) != 0;
#else
    return ::isatty(fd) != 0;
#endif
}

bool terminalSize(int fd, int& columns, int& rows) {
#if defined(_WIN32)
    const DWORD which = fd == 0 ? STD_INPUT_HANDLE : (fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(which), &info)) {
        return false;
    }
    columns = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return true;
#else
    winsize size;
    if (::ioctl(fd, TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
        return false;
    }
    columns = size.ws_col;
    rows = size.ws_row;
    return true;
#endif
}

const char* describeError(int err) {
    return err != 0 ? std::strerror(err) : "unknown error";
}

// Waits up to "timeoutMs" (-1 = forever) for stdin to have input. False on timeout.
bool waitForInput(int timeoutMs) {
#if defined(_WIN32)
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(in, &mode)) {
        return true; // a pipe or file: ReadFile decides
    }
    return WaitForSingleObject(in, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs)) == WAIT_OBJECT_0;
#else
    pollfd p{0, POLLIN, 0};
    for (;;) {
        const int rc = ::poll(&p, 1, timeoutMs);
        if (rc >= 0) {
            return rc > 0;
        }
        if (errno != EINTR) {
            return true; // let read() report the problem
        }
    }
#endif
}

#if defined(_WIN32)
// Appends UTF-16 code unit "unit" as UTF-8, pairing surrogates across calls.
void appendUtf16(std::string& out, wchar_t unit, wchar_t& pendingHigh) {
    std::uint32_t code = unit;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        pendingHigh = unit;
        return;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (pendingHigh == 0) {
            return;
        }
        code = 0x10000 + ((static_cast<std::uint32_t>(pendingHigh) - 0xD800) << 10) + (unit - 0xDC00);
        pendingHigh = 0;
    }
    char bytes[4];
    int n = 0;
    if (code < 0x80) {
        bytes[n++] = static_cast<char>(code);
    } else if (code < 0x800) {
        bytes[n++] = static_cast<char>(0xC0 | (code >> 6));
        bytes[n++] = static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        bytes[n++] = static_cast<char>(0xE0 | (code >> 12));
        bytes[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (code & 0x3F));
    } else {
        bytes[n++] = static_cast<char>(0xF0 | (code >> 18));
        bytes[n++] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (code & 0x3F));
    }
    out.append(bytes, static_cast<std::size_t>(n));
}

// Key presses from a console in raw mode, as the byte sequences a VT terminal sends.
// Returns -1 on failure, else the bytes appended (maybe 0: only non-key events).
long long readConsoleKeys(HANDLE in, std::string& out) {
    static wchar_t pendingHigh = 0;
    INPUT_RECORD records[64];
    DWORD count = 0;
    if (!ReadConsoleInputW(in, records, static_cast<DWORD>(std::size(records)), &count)) {
        return -1;
    }
    const std::size_t before = out.size();
    for (DWORD i = 0; i < count; ++i) {
        if (records[i].EventType != KEY_EVENT || !records[i].Event.KeyEvent.bKeyDown) {
            continue;
        }
        const KEY_EVENT_RECORD& key = records[i].Event.KeyEvent;
        for (WORD repeat = 0; repeat < std::max<WORD>(key.wRepeatCount, 1); ++repeat) {
            if (key.uChar.UnicodeChar != 0) {
                appendUtf16(out, key.uChar.UnicodeChar, pendingHigh);
                continue;
            }
            // Without VT input the special keys only come as virtual key codes.
            switch (key.wVirtualKeyCode) {
            case VK_UP: out += "\x1b[A"; break;
            case VK_DOWN: out += "\x1b[B"; break;
            case VK_RIGHT: out += "\x1b[C"; break;
            case VK_LEFT: out += "\x1b[D"; break;
            case VK_HOME: out += "\x1b[H"; break;
            case VK_END: out += "\x1b[F"; break;
            case VK_INSERT: out += "\x1b[2~"; break;
            case VK_DELETE: out += "\x1b[3~"; break;
            case VK_PRIOR: out += "\x1b[5~"; break;
            case VK_NEXT: out += "\x1b[6~"; break;
            case VK_F1: out += "\x1bOP"; break;
            case VK_F2: out += "\x1bOQ"; break;
            case VK_F3: out += "\x1bOR"; break;
            case VK_F4: out += "\x1bOS"; break;
            default: break;
            }
        }
    }
    return static_cast<long long>(out.size() - before);
}
#endif

// raw mode ///////////////////////////////////////////

struct TerminalModes {
    std::mutex mutex;
    bool saved = false;
    bool raw = false;
    bool restoreRegistered = false;
#if defined(_WIN32)
    DWORD original = 0;
#else
    termios original{};
#endif
};

TerminalModes& modes() {
    static TerminalModes* state = new TerminalModes(); // used by an atexit handler: never destroyed
    return *state;
}

enum class InputMode { Normal, Raw, NoEcho };

void restoreAtExit();

// Switches stdin's terminal mode. False if stdin is not a terminal. Caller holds modes().mutex.
bool applyMode(InputMode mode) {
    TerminalModes& m = modes();
#if defined(_WIN32)
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD current = 0;
    if (!GetConsoleMode(in, &current)) {
        return false;
    }
    if (!m.saved) {
        m.original = current;
        m.saved = true;
    }
    DWORD next = m.original;
    if (mode == InputMode::Raw) {
        next = (next & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) |
               ENABLE_VIRTUAL_TERMINAL_INPUT;
    } else if (mode == InputMode::NoEcho) {
        next &= ~static_cast<DWORD>(ENABLE_ECHO_INPUT);
    }
    if (!SetConsoleMode(in, next)) {
        if (mode != InputMode::Raw || !SetConsoleMode(in, next & ~static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_INPUT))) {
            return false;
        }
    }
#else
    termios current{};
    if (!::isatty(0) || ::tcgetattr(0, &current) != 0) {
        return false;
    }
    if (!m.saved) {
        m.original = current;
        m.saved = true;
    }
    termios next = m.original;
    if (mode == InputMode::Raw) {
        next.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN);
        next.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        next.c_cflag |= CS8;
        next.c_cc[VMIN] = 1;
        next.c_cc[VTIME] = 0;
    } else if (mode == InputMode::NoEcho) {
        next.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        next.c_lflag |= ECHONL;
    }
    if (::tcsetattr(0, TCSAFLUSH, &next) != 0) {
        return false;
    }
#endif
    if (!m.restoreRegistered) {
        m.restoreRegistered = true;
        std::atexit(restoreAtExit);
    }
    return true;
}

void restoreAtExit() {
    TerminalModes& m = modes();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.saved && m.raw) {
        applyMode(InputMode::Normal);
    }
}

// Puts the terminal in raw mode for one operation, unless it already is.
class TemporaryMode {
  public:
    explicit TemporaryMode(InputMode mode) {
        std::lock_guard<std::mutex> lock(modes().mutex);
        if (!modes().raw) {
            changed_ = applyMode(mode);
        }
    }
    ~TemporaryMode() {
        if (changed_) {
            std::lock_guard<std::mutex> lock(modes().mutex);
            applyMode(InputMode::Normal);
        }
    }
    TemporaryMode(const TemporaryMode&) = delete;
    TemporaryMode& operator=(const TemporaryMode&) = delete;

  private:
    bool changed_ = false;
};

// stdin buffer ///////////////////////////////////////
//
// One per process. Worker jobs fill it (only one at a time, under ioMutex); the
// VM's thread takes what it needs under "mutex". Data a cancelled read already
// fetched stays in the buffer for the next one.

struct StdinState {
    std::mutex ioMutex;
    std::mutex mutex;
    std::string buffer;
    bool eof = false; // an end of input not reported yet (a terminal can go on after Ctrl+D)
    int error = 0;    // a read failure not reported yet
};

StdinState& stdinState() {
    static StdinState* state = new StdinState(); // a worker may still be blocked in read() at exit
    return *state;
}

// One read from the OS into the buffer. Caller holds ioMutex but not mutex.
// With a timeout (ms, -1 = none): false if nothing arrived in time.
bool readMore(StdinState& state, int timeoutMs) {
    if (timeoutMs >= 0 && !waitForInput(timeoutMs)) {
        return false;
    }

    std::string chunk;
    long long got = 0;
    int err = 0;
#if defined(_WIN32)
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    bool rawConsole = false;
    {
        std::lock_guard<std::mutex> lock(modes().mutex);
        rawConsole = GetConsoleMode(in, &mode) && (mode & ENABLE_LINE_INPUT) == 0;
    }
    if (rawConsole) {
        got = readConsoleKeys(in, chunk);
        if (got < 0) {
            err = EIO;
        } else if (got == 0) {
            return true; // only non-key events: nothing to add, not the end either
        }
    } else {
        chunk.resize(kInputChunkSize);
        got = _read(0, chunk.data(), static_cast<unsigned>(chunk.size()));
        err = got < 0 ? errno : 0;
        chunk.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
    }
#else
    chunk.resize(kInputChunkSize);
    for (;;) {
        got = ::read(0, chunk.data(), chunk.size());
        if (got >= 0 || errno != EINTR) {
            break;
        }
    }
    err = got < 0 ? errno : 0;
    chunk.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
#endif

    std::lock_guard<std::mutex> lock(state.mutex);
    if (got > 0) {
        state.buffer += chunk;
    } else if (got == 0) {
        state.eof = true;
    } else {
        state.error = err != 0 ? err : EIO;
    }
    return true;
}

// Worker side of every read: reads until "enough" holds for the buffer, or input ends.
void fillUntil(const std::function<bool(const std::string&)>& enough) {
    StdinState& state = stdinState();
    std::lock_guard<std::mutex> io(state.ioMutex);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.eof || state.error != 0 || enough(state.buffer)) {
                return;
            }
        }
        std::fflush(stdout); // a pending prompt must be visible before we wait for input
        readMore(state, -1);
    }
}

// Where the first line ending in "text" is: [start, end) of the ending. False if there is none yet.
// A "\r" at the very end might be the start of "\r\n", unless the input has ended.
bool findLineEnd(const std::string& text, bool ended, std::size_t& start, std::size_t& end) {
    const std::size_t hit = text.find_first_of("\r\n");
    if (hit == std::string::npos) {
        return false;
    }
    start = hit;
    if (text[hit] == '\n') {
        end = hit + 1;
        return true;
    }
    if (hit + 1 < text.size()) {
        end = text[hit + 1] == '\n' ? hit + 2 : hit + 1;
        return true;
    }
    end = hit + 1;
    return ended;
}

enum class ReadKind { Line, Exact, Some, All };

// Takes what "kind" asks for from the buffer and pushes the results. -1 (nothing pushed) if
// the buffer cannot answer yet. Runs on the VM's thread.
int consume(lua_State* L, ReadKind kind, std::size_t count, bool keepEnding) {
    StdinState& state = stdinState();
    std::lock_guard<std::mutex> lock(state.mutex);
    std::string& buffer = state.buffer;
    const bool ended = state.eof || state.error != 0;

    switch (kind) {
    case ReadKind::Line: {
        std::size_t start = 0;
        std::size_t end = 0;
        if (findLineEnd(buffer, ended, start, end)) {
            lua_pushlstring(L, buffer.data(), keepEnding ? end : start);
            buffer.erase(0, end);
            return 1;
        }
        if (ended && !buffer.empty()) { // the last line had no ending
            lua_pushlstring(L, buffer.data(), buffer.size());
            buffer.clear();
            return 1;
        }
        break;
    }
    case ReadKind::Exact:
    case ReadKind::Some:
        if (count == 0) {
            lua_pushliteral(L, "");
            return 1;
        }
        if (!buffer.empty() && (kind == ReadKind::Some || buffer.size() >= count || ended)) {
            const std::size_t take = std::min(count, buffer.size());
            lua_pushlstring(L, buffer.data(), take);
            buffer.erase(0, take);
            return 1;
        }
        break;
    case ReadKind::All:
        if (ended) {
            if (state.error != 0 && buffer.empty()) {
                break;
            }
            lua_pushlstring(L, buffer.data(), buffer.size());
            buffer.clear();
            state.eof = false;
            return 1;
        }
        return -1;
    }

    if (!ended) {
        return -1;
    }
    // End of input (reported once) or a failure.
    if (state.error != 0) {
        const int err = state.error;
        state.error = 0;
        lua_pushnil(L);
        lua_pushfstring(L, "failed to read from stdin: %s", describeError(err));
        return 2;
    }
    state.eof = false;
    lua_pushnil(L);
    return 1;
}

bool hasEnough(const std::string& buffer, ReadKind kind, std::size_t count) {
    switch (kind) {
    case ReadKind::Line: {
        std::size_t start = 0;
        std::size_t end = 0;
        return findLineEnd(buffer, false, start, end);
    }
    case ReadKind::Exact:
        return buffer.size() >= count;
    case ReadKind::Some:
        return !buffer.empty() || count == 0;
    case ReadKind::All:
        return false;
    }
    return false;
}

// Reads from stdin without blocking other tasks.
int readStdin(lua_State* L, ReadKind kind, std::size_t count, bool keepEnding) {
    const int ready = consume(L, kind, count, keepEnding);
    if (ready >= 0) {
        return ready;
    }
    std::fflush(stdout);

    return Scheduler::of(L).await(L, [kind, count, keepEnding]() -> sched::Completion {
        fillUntil([kind, count](const std::string& buffer) { return hasEnough(buffer, kind, count); });
        return [kind, count, keepEnding](lua_State* T) {
            const int n = consume(T, kind, count, keepEnding);
            if (n >= 0) {
                return n;
            }
            lua_pushnil(T);
            return 1;
        };
    });
}

// keys ///////////////////////////////////////////////

struct Key {
    std::string name;
    std::string character; // printable keys only
    std::string sequence;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

std::size_t utf8Length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

// "1;5" style CSI parameters -> modifiers (2 = shift, 3 = alt, 5 = ctrl, combinations add up).
void applyModifiers(Key& key, int modifier) {
    if (modifier <= 1) {
        return;
    }
    const int bits = modifier - 1;
    key.shift = (bits & 1) != 0;
    key.alt = (bits & 2) != 0;
    key.ctrl = (bits & 4) != 0;
}

// Names one control byte.
void controlKey(Key& key, unsigned char byte) {
    switch (byte) {
    case '\r':
    case '\n':
        key.name = "enter";
        return;
    case '\t':
        key.name = "tab";
        return;
    case 0x7F:
    case 0x08:
        key.name = "backspace";
        return;
    case 0x1B:
        key.name = "escape";
        return;
    case 0x00:
        key.name = "space";
        key.ctrl = true;
        return;
    default:
        if (byte >= 1 && byte <= 26) {
            key.name = std::string(1, static_cast<char>('a' + byte - 1));
            key.ctrl = true;
        } else {
            key.name = std::string(1, static_cast<char>(byte));
        }
    }
}

// Decodes the key at the start of "input". Returns the bytes it used, or 0 if the
// sequence is incomplete ("ended": no more bytes are coming soon, take what is there).
std::size_t decodeKey(const std::string& input, bool ended, Key& key) {
    if (input.empty()) {
        return 0;
    }
    const auto byte = static_cast<unsigned char>(input[0]);

    if (byte == 0x1B) {
        if (input.size() == 1) {
            if (!ended) {
                return 0;
            }
            key.name = "escape";
            key.sequence = input.substr(0, 1);
            return 1;
        }
        const char second = input[1];
        if (second == '[' || second == 'O') {
            // CSI / SS3: parameters, then a final byte in 0x40..0x7E.
            std::size_t i = 2;
            while (i < input.size() && (static_cast<unsigned char>(input[i]) < 0x40 || static_cast<unsigned char>(input[i]) > 0x7E)) {
                ++i;
            }
            if (i >= input.size()) {
                if (!ended) {
                    return 0;
                }
                key.name = "escape";
                key.sequence = input.substr(0, 1);
                return 1;
            }
            const std::string params = input.substr(2, i - 2);
            const char final = input[i];
            key.sequence = input.substr(0, i + 1);

            int first = 0;
            int modifier = 0;
            std::sscanf(params.c_str(), "%d;%d", &first, &modifier);
            applyModifiers(key, modifier);

            switch (final) {
            case 'A': key.name = "up"; break;
            case 'B': key.name = "down"; break;
            case 'C': key.name = "right"; break;
            case 'D': key.name = "left"; break;
            case 'H': key.name = "home"; break;
            case 'F': key.name = "end"; break;
            case 'P': key.name = "f1"; break;
            case 'Q': key.name = "f2"; break;
            case 'R': key.name = "f3"; break;
            case 'S': key.name = "f4"; break;
            case 'Z':
                key.name = "tab";
                key.shift = true;
                break;
            case '~':
                switch (first) {
                case 1: case 7: key.name = "home"; break;
                case 2: key.name = "insert"; break;
                case 3: key.name = "delete"; break;
                case 4: case 8: key.name = "end"; break;
                case 5: key.name = "pageUp"; break;
                case 6: key.name = "pageDown"; break;
                case 11: key.name = "f1"; break;
                case 12: key.name = "f2"; break;
                case 13: key.name = "f3"; break;
                case 14: key.name = "f4"; break;
                case 15: key.name = "f5"; break;
                case 17: key.name = "f6"; break;
                case 18: key.name = "f7"; break;
                case 19: key.name = "f8"; break;
                case 20: key.name = "f9"; break;
                case 21: key.name = "f10"; break;
                case 23: key.name = "f11"; break;
                case 24: key.name = "f12"; break;
                default: key.name = "unknown"; break;
                }
                break;
            default:
                key.name = "unknown";
                break;
            }
            return i + 1;
        }

        // ESC + key = Alt + key.
        const std::size_t length = utf8Length(static_cast<unsigned char>(second));
        if (input.size() < 1 + length) {
            return ended ? 1 : 0;
        }
        const std::size_t used = decodeKey(input.substr(1, length), true, key);
        key.alt = true;
        key.sequence = input.substr(0, 1 + used);
        return 1 + used;
    }

    if (byte < 0x20 || byte == 0x7F) {
        controlKey(key, byte);
        key.sequence = input.substr(0, 1);
        return 1;
    }

    const std::size_t length = utf8Length(byte);
    if (input.size() < length) {
        return ended ? input.size() : 0;
    }
    key.sequence = input.substr(0, length);
    key.character = key.sequence;
    key.name = key.sequence == " " ? "space" : key.sequence;
    if (length == 1 && byte >= 'A' && byte <= 'Z') {
        key.shift = true;
    }
    return length;
}

// Worker side of readKey: waits up to "timeout" seconds for a complete key and takes it.
std::optional<Key> takeKey(double timeout) {
    StdinState& state = stdinState();
    std::lock_guard<std::mutex> io(state.ioMutex);
    TemporaryMode raw(InputMode::Raw);

    const bool forever = timeout >= kForever;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(forever ? 0.0 : timeout);
    bool waitedForRest = false;

    for (;;) {
        bool partial = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            Key key;
            const bool ended = waitedForRest || state.eof;
            const std::size_t used = decodeKey(state.buffer, ended, key);
            if (used > 0) {
                state.buffer.erase(0, used);
                return key;
            }
            if (state.eof || state.error != 0) {
                state.eof = false;
                state.error = 0;
                return std::nullopt;
            }
            partial = !state.buffer.empty();
        }
        if (partial) {
            // The start of a sequence: give the rest a moment to arrive.
            waitedForRest = !readMore(state, kEscapeWaitMs);
            continue;
        }

        int timeoutMs = -1;
        if (!forever) {
            const double left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                return std::nullopt;
            }
            timeoutMs = static_cast<int>(std::ceil(left * 1000.0));
        }
        std::fflush(stdout);
        if (!readMore(state, timeoutMs)) {
            return std::nullopt;
        }
        waitedForRest = false;
    }
}

void pushKey(lua_State* L, const Key& key) {
    lua_createtable(L, 0, 6);
    lua_pushlstring(L, key.name.data(), key.name.size());
    lua_setfield(L, -2, "name");
    if (!key.character.empty()) {
        lua_pushlstring(L, key.character.data(), key.character.size());
        lua_setfield(L, -2, "char");
    }
    lua_pushboolean(L, key.ctrl);
    lua_setfield(L, -2, "ctrl");
    lua_pushboolean(L, key.alt);
    lua_setfield(L, -2, "alt");
    lua_pushboolean(L, key.shift);
    lua_setfield(L, -2, "shift");
    lua_pushlstring(L, key.sequence.data(), key.sequence.size());
    lua_setfield(L, -2, "sequence");
}

// Worker side of term.password. "ok" is false at EOF or on Ctrl+C.
std::string readPassword(const std::string& mask, bool& ok) {
    ok = true;
    std::string password;

    if (!isTerminal(0)) { // nothing to hide: an ordinary line
        fillUntil([](const std::string& buffer) {
            std::size_t start = 0;
            std::size_t end = 0;
            return findLineEnd(buffer, false, start, end);
        });
        StdinState& state = stdinState();
        std::lock_guard<std::mutex> lock(state.mutex);
        std::size_t start = 0;
        std::size_t end = 0;
        if (findLineEnd(state.buffer, true, start, end)) {
            password = state.buffer.substr(0, start);
            state.buffer.erase(0, end);
        } else if (!state.buffer.empty()) {
            password.swap(state.buffer);
        } else {
            ok = false;
            state.eof = false;
        }
        return password;
    }

    for (;;) {
        const std::optional<Key> key = takeKey(kForever);
        if (!key || (key->ctrl && (key->name == "c" || (key->name == "d" && password.empty())))) {
            ok = false;
            break;
        }
        if (key->name == "enter") {
            break;
        }
        if (key->name == "backspace") {
            if (!password.empty()) {
                std::size_t cut = password.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(password[cut]) & 0xC0) == 0x80) {
                    --cut; // back to the first byte of the last UTF-8 character
                }
                password.erase(cut);
                if (!mask.empty()) {
                    std::fputs("\b \b", stdout);
                    std::fflush(stdout);
                }
            }
            continue;
        }
        if (!key->character.empty() && !key->ctrl && !key->alt) {
            password += key->character;
            if (!mask.empty()) {
                std::fputs(mask.c_str(), stdout);
                std::fflush(stdout);
            }
        }
    }
    std::fputs("\n", stdout);
    std::fflush(stdout);
    return password;
}

// colors ///////////////////////////////////////////////

std::atomic<int> gForcedLevel{-1};

bool envIs(const char* name, const char* value) {
    const char* found = std::getenv(name);
    return found != nullptr && std::strcmp(found, value) == 0;
}

#if defined(_WIN32)
// Lets the console interpret escape sequences. Remembers whether that worked.
bool enableVirtualTerminal(int fd) {
    static bool enabled[3] = {false, false, false};
    static bool tried[3] = {false, false, false};
    if (!tried[fd]) {
        tried[fd] = true;
        HANDLE out = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        DWORD mode = 0;
        enabled[fd] = GetConsoleMode(out, &mode) &&
                      ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
                       SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0);
    }
    return enabled[fd];
}
#endif

int detectColorLevel(int fd) {
    const int forced = gForcedLevel.load();
    if (forced >= 0) {
        return forced;
    }
    if (const char* noColor = std::getenv("NO_COLOR"); noColor != nullptr && *noColor != '\0') {
        return 0;
    }
    if (const char* force = std::getenv("FORCE_COLOR")) {
        if (std::strcmp(force, "0") == 0 || std::strcmp(force, "false") == 0) {
            return 0;
        }
        if (std::strcmp(force, "2") == 0) {
            return 2;
        }
        if (std::strcmp(force, "3") == 0) {
            return 3;
        }
        return 1;
    }
    if (fd == 0 || !isTerminal(fd)) {
        return 0;
    }
#if defined(_WIN32)
    return enableVirtualTerminal(fd) ? 3 : 0;
#else
    if (envIs("TERM", "dumb")) {
        return 0;
    }
    if (envIs("COLORTERM", "truecolor") || envIs("COLORTERM", "24bit")) {
        return 3;
    }
    const char* termName = std::getenv("TERM");
    if (termName != nullptr && std::strstr(termName, "256") != nullptr) {
        return 2;
    }
    return 1;
#endif
}

struct Rgb {
    int r, g, b;
};

// xterm's 256-color palette.
Rgb paletteColor(int index) {
    static const Rgb kBasic[16] = {{0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
                                   {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
                                   {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
                                   {92, 92, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
    if (index < 16) {
        return kBasic[index];
    }
    if (index < 232) {
        const int i = index - 16;
        const auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return {level(i / 36), level((i / 6) % 6), level(i % 6)};
    }
    const int gray = 8 + (index - 232) * 10;
    return {gray, gray, gray};
}

int distance(const Rgb& a, const Rgb& b) {
    return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
}

int nearestIndex(const Rgb& color, int first, int last) {
    int best = first;
    for (int i = first; i <= last; ++i) {
        if (distance(paletteColor(i), color) < distance(paletteColor(best), color)) {
            best = i;
        }
    }
    return best;
}

// SGR parameters for a color at the given level. index16: 0-15, or -1 for an exact color.
std::string colorCode(bool background, int level, const Rgb& rgb, int index256) {
    if (level >= 3 && index256 < 0) {
        return std::string(background ? "48;2;" : "38;2;") + std::to_string(rgb.r) + ";" + std::to_string(rgb.g) + ";" +
               std::to_string(rgb.b);
    }
    if (level >= 2) {
        const int index = index256 >= 0 ? index256 : nearestIndex(rgb, 16, 255);
        return std::string(background ? "48;5;" : "38;5;") + std::to_string(index);
    }
    const int index = nearestIndex(index256 >= 0 ? paletteColor(index256) : rgb, 0, 15);
    const int base = index < 8 ? (background ? 40 : 30) : (background ? 100 : 90);
    return std::to_string(base + index % 8);
}

struct StyleCode {
    std::string open;
    const char* close;
};

bool parseHex(std::string_view hex, Rgb& rgb) {
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int values[6];
    if (hex.size() != 3 && hex.size() != 6) {
        return false;
    }
    for (std::size_t i = 0; i < hex.size(); ++i) {
        values[i] = digit(hex[i]);
        if (values[i] < 0) {
            return false;
        }
    }
    if (hex.size() == 3) {
        rgb = {values[0] * 17, values[1] * 17, values[2] * 17};
    } else {
        rgb = {values[0] * 16 + values[1], values[2] * 16 + values[3], values[4] * 16 + values[5]};
    }
    return true;
}

// One style name -> its codes at "level". False if the name is unknown.
bool styleCode(std::string_view name, int level, StyleCode& out) {
    struct Modifier {
        const char* name;
        int open;
        const char* close;
    };
    static const Modifier kModifiers[] = {
        {"bold", 1, "22"},      {"dim", 2, "22"},     {"italic", 3, "23"}, {"underline", 4, "24"},
        {"blink", 5, "25"},     {"inverse", 7, "27"}, {"hidden", 8, "28"}, {"strikethrough", 9, "29"},
    };
    for (const Modifier& m : kModifiers) {
        if (name == m.name) {
            out = {std::to_string(m.open), m.close};
            return true;
        }
    }

    static const char* const kColors[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
    bool background = false;
    std::string_view color = name;
    if (color.size() > 2 && color.substr(0, 2) == "bg" && color[2] >= 'A' && color[2] <= 'Z') {
        background = true;
        color.remove_prefix(2);
    }
    const auto lowerFirst = [](std::string_view text) {
        std::string result(text);
        if (!result.empty()) {
            result[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[0])));
        }
        return result;
    };
    const std::string plain = lowerFirst(color);
    int index = -1;
    if (plain == "gray" || plain == "grey" || plain == "brightBlack") {
        index = 8;
    } else {
        const bool bright = plain.rfind("bright", 0) == 0;
        const std::string base = bright ? lowerFirst(std::string_view(plain).substr(6)) : plain;
        for (int i = 0; i < 8; ++i) {
            if (base == kColors[i]) {
                index = i + (bright ? 8 : 0);
            }
        }
    }
    if (index >= 0) { // the 16 standard colors work at every level
        const int base = index < 8 ? (background ? 40 : 30) : (background ? 100 : 90);
        out = {std::to_string(base + index % 8), background ? "49" : "39"};
        return true;
    }

    // "#rrggbb", "fg:#rrggbb", "bg:#rrggbb", "fg:208", "bg:208"
    std::string_view spec = name;
    background = false;
    if (spec.substr(0, 3) == "bg:") {
        background = true;
        spec.remove_prefix(3);
    } else if (spec.substr(0, 3) == "fg:") {
        spec.remove_prefix(3);
    } else if (spec.empty() || spec[0] != '#') {
        return false;
    }
    if (!spec.empty() && spec[0] == '#') {
        Rgb rgb{};
        if (!parseHex(spec.substr(1), rgb)) {
            return false;
        }
        out = {colorCode(background, std::max(level, 1), rgb, -1), background ? "49" : "39"};
        return true;
    }
    int number = 0;
    for (const char c : spec) {
        if (c < '0' || c > '9' || (number = number * 10 + (c - '0')) > 255) {
            return false;
        }
    }
    if (spec.empty()) {
        return false;
    }
    out = {colorCode(background, std::max(level, 1), paletteColor(number), number), background ? "49" : "39"};
    return true;
}

// Reads the style names at stack slots first..top. Raises on unknown ones.
std::vector<StyleCode> checkStyles(lua_State* L, int first, int level) {
    std::vector<StyleCode> codes;
    const int top = lua_gettop(L);
    for (int i = first; i <= top; ++i) {
        std::size_t length = 0;
        const char* name = luaL_checklstring(L, i, &length);
        StyleCode code;
        if (!styleCode(std::string_view(name, length), level, code)) {
            luaL_argerror(L, i, lua_pushfstring(L, "unknown style '%s'", name));
        }
        codes.push_back(std::move(code));
    }
    return codes;
}

// display width ///////////////////////////////////////////

// Removes CSI, OSC and two-byte escape sequences.
std::string stripEscapes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\x1b') {
            out += text[i];
            continue;
        }
        if (i + 1 >= text.size()) {
            break;
        }
        const char kind = text[i + 1];
        if (kind == '[') {
            std::size_t j = i + 2;
            while (j < text.size() && (static_cast<unsigned char>(text[j]) < 0x40 || static_cast<unsigned char>(text[j]) > 0x7E)) {
                ++j;
            }
            i = j;
        } else if (kind == ']') {
            std::size_t j = i + 2;
            while (j < text.size() && text[j] != '\a' && !(text[j] == '\x1b' && j + 1 < text.size() && text[j + 1] == '\\')) {
                ++j;
            }
            i = j < text.size() && text[j] == '\x1b' ? j + 1 : j;
        } else {
            i += 1;
        }
    }
    return out;
}

bool inRanges(std::uint32_t c, const std::uint32_t (*ranges)[2], std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (c >= ranges[i][0] && c <= ranges[i][1]) {
            return true;
        }
    }
    return false;
}

int codepointWidth(std::uint32_t c) {
    static const std::uint32_t kZero[][2] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0E31, 0x0E31},
        {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E},
        {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
    };
    static const std::uint32_t kWide[][2] = {
        {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},
        {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},
        {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},
        {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F5},   {0x26FA, 0x26FD},
        {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},
        {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
        {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xA960, 0xA97F},   {0xAC00, 0xD7A3},
        {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},
        {0x16FE0, 0x16FE4}, {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
        {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
        {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD},
    };
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) {
        return 0;
    }
    if (inRanges(c, kZero, std::size(kZero))) {
        return 0;
    }
    return inRanges(c, kWide, std::size(kWide)) ? 2 : 1;
}

double displayWidth(std::string_view text) {
    const std::string plain = stripEscapes(text);
    double width = 0;
    for (std::size_t i = 0; i < plain.size();) {
        const auto lead = static_cast<unsigned char>(plain[i]);
        const std::size_t length = utf8Length(lead);
        std::uint32_t c = lead;
        if (length > 1 && i + length <= plain.size()) {
            c = lead & (0xFF >> (length + 1));
            for (std::size_t k = 1; k < length; ++k) {
                c = (c << 6) | (static_cast<unsigned char>(plain[i + k]) & 0x3F);
            }
        }
        width += codepointWidth(c);
        i += length;
    }
    return width;
}

// bindings //////////////////////////////////////////////
//
// As in example.cpp: arguments are validated first, and the writers below hold no C++
// objects at all, so a Lua error can never skip a destructor.

// Pushes "nil, "failed to <action> <stream>: <reason>"".
int pushFailure(lua_State* L, const char* action, int fd, int err) {
    lua_pushnil(L);
    lua_pushfstring(L, "failed to %s %s: %s", action, kStreamNames[fd], describeError(err));
    return 2;
}

int writeValues(lua_State* L, bool newline) {
    const int fd = checkWritable(L);
    const int top = lua_gettop(L);

    for (int i = 2; i <= top; ++i) {
        luaL_checklstring(L, i, nullptr); // numbers are converted in place
    }

    std::FILE* file = fileOf(fd);
    bool failed = false;
    int err = 0;

    for (int i = 2; i <= top && !failed; ++i) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, i, &length);
        if (std::fwrite(text, 1, length, file) != length) {
            failed = true;
            err = errno;
        }
    }
    if (!failed && newline && std::fputc('\n', file) == EOF) {
        failed = true;
        err = errno;
    }
    if (failed) {
        std::clearerr(file); // leave the stream usable so the script can recover
        return pushFailure(L, "write to", fd, err);
    }

    lua_settop(L, 1);
    return 1; // the stream
}

// stream:write(...: string | number): stream?, string?
int streamWrite(lua_State* L) {
    return writeValues(L, false);
}

// stream:writeLine(...: string | number): stream?, string?
int streamWriteLine(lua_State* L) {
    return writeValues(L, true);
}

// stream:flush(): stream?, string?
int streamFlush(lua_State* L) {
    const int fd = checkWritable(L);
    std::FILE* file = fileOf(fd);

    if (std::fflush(file) != 0) {
        const int err = errno;
        std::clearerr(file);
        return pushFailure(L, "flush", fd, err);
    }

    lua_settop(L, 1);
    return 1;
}

// stream:readLine(keepEnding: boolean?): string?, string?
int streamReadLine(lua_State* L) {
    checkReadable(L);
    return readStdin(L, ReadKind::Line, 0, lua_toboolean(L, 2) != 0);
}

int readCount(lua_State* L, ReadKind kind) {
    checkReadable(L);
    const int count = luaL_checkinteger(L, 2);
    luaL_argcheck(L, count >= 0, 2, "count must not be negative");
    return readStdin(L, kind, static_cast<std::size_t>(count), false);
}

// stream:read(count: number): string?, string?
int streamRead(lua_State* L) {
    return readCount(L, ReadKind::Exact);
}

// stream:readSome(maxCount: number): string?, string?
int streamReadSome(lua_State* L) {
    return readCount(L, ReadKind::Some);
}

// stream:readAll(): string?, string?
int streamReadAll(lua_State* L) {
    checkReadable(L);
    return readStdin(L, ReadKind::All, 0, false);
}

// The iterator behind stream:lines(); upvalue 1 is keepEnding. It cannot yield, so it waits inline.
int linesNext(lua_State* L) {
    return readStdin(L, ReadKind::Line, 0, lua_toboolean(L, lua_upvalueindex(1)) != 0);
}

// stream:lines(keepEnding: boolean?): () -> string?
int streamLines(lua_State* L) {
    checkReadable(L);

    lua_pushboolean(L, lua_toboolean(L, 2));
    lua_pushcclosure(L, linesNext, "lines", 1);
    return 1;
}

// stream:isTerminal(): boolean
int streamIsTerminal(lua_State* L) {
    lua_pushboolean(L, isTerminal(checkStream(L)));
    return 1;
}

int pushSize(lua_State* L, int fd) {
    int columns = 0;
    int rows = 0;

    if (!terminalSize(fd, columns, rows)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushnumber(L, columns);
    lua_pushnumber(L, rows);
    return 2;
}

// stream:size(): (number, number)?
int streamSize(lua_State* L) {
    return pushSize(L, checkStream(L));
}

// tostring(stream): "term.stdout"
int streamToString(lua_State* L) {
    lua_pushfstring(L, "term.%s", kStreamNames[checkStream(L)]);
    return 1;
}

// term.prompt(message: string?): string?, string?
int termPrompt(lua_State* L) {
    std::size_t length = 0;
    const char* message = luaL_optlstring(L, 1, "", &length);

    if (std::fwrite(message, 1, length, stdout) != length || std::fflush(stdout) != 0) {
        const int err = errno;
        std::clearerr(stdout);
        return pushFailure(L, "write to", 1, err);
    }
    return readStdin(L, ReadKind::Line, 0, false);
}

// term.password(message: string?, mask: string?): string?
int termPassword(lua_State* L) {
    std::size_t length = 0;
    const char* message = luaL_optlstring(L, 1, "", &length);
    const char* maskText = luaL_optstring(L, 2, "");

    std::fwrite(message, 1, length, stdout);
    std::fflush(stdout);

    const std::string mask = maskText;
    return Scheduler::of(L).await(L, [mask]() -> sched::Completion {
        bool ok = true;
        auto password = std::make_shared<std::string>(readPassword(mask, ok));
        return [password, ok](lua_State* T) {
            if (ok) {
                lua_pushlstring(T, password->data(), password->size());
            } else {
                lua_pushnil(T);
            }
            return 1;
        };
    });
}

// term.size(): (number, number)?
int termSize(lua_State* L) {
    for (const int fd : {1, 2, 0}) {
        if (isTerminal(fd)) {
            return pushSize(L, fd);
        }
    }
    lua_pushnil(L);
    return 1;
}

// term.colorLevel(stream?): number
int termColorLevel(lua_State* L) {
    int fd = 1;
    if (!lua_isnoneornil(L, 1)) {
        fd = *static_cast<const int*>(luaL_checkudata(L, 1, kStreamMetatable));
    }
    lua_pushnumber(L, detectColorLevel(fd));
    return 1;
}

// term.setColorLevel(level: number?)
int termSetColorLevel(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        gForcedLevel = -1;
        return 0;
    }
    const int level = luaL_checkinteger(L, 1);
    luaL_argcheck(L, level >= 0 && level <= 3, 1, "level must be 0, 1, 2 or 3");
    gForcedLevel = level;
    return 0;
}

// term.style(text: string, ...: string): string
int termStyle(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const int level = detectColorLevel(1);

    {
        const std::vector<StyleCode> codes = checkStyles(L, 2, std::max(level, 1));
        if (level == 0 || codes.empty()) {
            lua_pushlstring(L, text, length);
            return 1;
        }
        std::string out = "\x1b[";
        for (std::size_t i = 0; i < codes.size(); ++i) {
            out += (i == 0 ? "" : ";") + codes[i].open;
        }
        out += 'm';
        out.append(text, length);
        out += "\x1b[";
        for (std::size_t i = codes.size(); i-- > 0;) {
            out += codes[i].close;
            out += i == 0 ? "m" : ";";
        }
        lua_pushlstring(L, out.data(), out.size());
    }
    return 1;
}

// term.ansi.style(...: string): string  (the opening codes only, at stdout's level, at least 16 colors)
int ansiStyle(lua_State* L) {
    const int level = std::max(detectColorLevel(1), 1);
    {
        const std::vector<StyleCode> codes = checkStyles(L, 1, level);
        std::string out = "\x1b[";
        for (std::size_t i = 0; i < codes.size(); ++i) {
            out += (i == 0 ? "" : ";") + codes[i].open;
        }
        out += codes.empty() ? "0m" : "m";
        lua_pushlstring(L, out.data(), out.size());
    }
    return 1;
}

// term.strip(text: string): string
int termStrip(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    {
        const std::string plain = stripEscapes(std::string_view(text, length));
        lua_pushlstring(L, plain.data(), plain.size());
    }
    return 1;
}

// term.width(text: string): number
int termWidth(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    lua_pushnumber(L, displayWidth(std::string_view(text, length)));
    return 1;
}

// Escape sequences ///////////////////////////////////////

const char* clearCode(lua_State* L, int index) {
    static const char* const kNames[] = {"screen", "below", "above", "line", "lineRight", "lineLeft", nullptr};
    static const char* const kCodes[] = {"\x1b[2J\x1b[H", "\x1b[0J", "\x1b[1J", "\x1b[2K", "\x1b[0K", "\x1b[1K"};
    return kCodes[luaL_checkoption(L, index, "screen", kNames)];
}

void pushMoveTo(lua_State* L) {
    const int column = luaL_checkinteger(L, 1);
    const int row = luaL_checkinteger(L, 2);
    luaL_argcheck(L, column >= 1, 1, "columns start at 1");
    luaL_argcheck(L, row >= 1, 2, "rows start at 1");
    lua_pushfstring(L, "\x1b[%d;%dH", row, column);
}

void pushMove(lua_State* L) {
    const int dx = luaL_optinteger(L, 1, 0);
    const int dy = luaL_optinteger(L, 2, 0);
    lua_pushliteral(L, "");
    if (dx != 0) {
        lua_pushfstring(L, "\x1b[%d%c", dx > 0 ? dx : -dx, dx > 0 ? 'C' : 'D');
        lua_concat(L, 2);
    }
    if (dy != 0) {
        lua_pushfstring(L, "\x1b[%d%c", dy > 0 ? dy : -dy, dy > 0 ? 'B' : 'A');
        lua_concat(L, 2);
    }
}

// Writes the string on top of the stack to stdout if it is a terminal, and pops it.
int emit(lua_State* L) {
    if (isTerminal(1)) {
#if defined(_WIN32)
        enableVirtualTerminal(1);
#endif
        std::size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        std::fwrite(text, 1, length, stdout);
        std::fflush(stdout);
    }
    lua_pop(L, 1);
    return 0;
}

int termMoveTo(lua_State* L) {
    pushMoveTo(L);
    return emit(L);
}

int termMove(lua_State* L) {
    pushMove(L);
    return emit(L);
}

int termClear(lua_State* L) {
    lua_pushstring(L, clearCode(L, 1));
    return emit(L);
}

template <const char* Code>
int emitFixed(lua_State* L) {
    lua_pushstring(L, Code);
    return emit(L);
}

constexpr char kHideCursor[] = "\x1b[?25l";
constexpr char kShowCursor[] = "\x1b[?25h";
constexpr char kSaveCursor[] = "\x1b" "7";
constexpr char kRestoreCursor[] = "\x1b" "8";
constexpr char kAltScreenOn[] = "\x1b[?1049h";
constexpr char kAltScreenOff[] = "\x1b[?1049l";
constexpr char kBell[] = "\a";
constexpr char kReset[] = "\x1b[0m";

// term.altScreen(on: boolean)
int termAltScreen(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    lua_pushstring(L, lua_toboolean(L, 1) ? kAltScreenOn : kAltScreenOff);
    return emit(L);
}

// term.setTitle(text: string)
int termSetTitle(lua_State* L) {
    std::size_t length = 0;
    const char* title = luaL_checklstring(L, 1, &length);
    lua_pushliteral(L, "\x1b]0;");
    lua_pushlstring(L, title, length);
    lua_pushliteral(L, "\a");
    lua_concat(L, 3);
    return emit(L);
}

int ansiMoveTo(lua_State* L) {
    pushMoveTo(L);
    return 1;
}

int ansiMove(lua_State* L) {
    pushMove(L);
    return 1;
}

int ansiClear(lua_State* L) {
    lua_pushstring(L, clearCode(L, 1));
    return 1;
}

// term.cursorPosition(): (number, number)?
int termCursorPosition(lua_State* L) {
    if (!isTerminal(0) || !isTerminal(1)) {
        lua_pushnil(L);
        return 1;
    }

    return Scheduler::of(L).await(L, []() -> sched::Completion {
        int row = 0;
        int column = 0;
        {
            StdinState& state = stdinState();
            std::lock_guard<std::mutex> io(state.ioMutex);
            TemporaryMode raw(InputMode::Raw);
#if defined(_WIN32)
            enableVirtualTerminal(1);
#endif
            std::fputs("\x1b[6n", stdout);
            std::fflush(stdout);

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    const std::size_t start = state.buffer.find("\x1b[");
                    if (start != std::string::npos) {
                        const std::size_t end = state.buffer.find('R', start);
                        if (end != std::string::npos &&
                            std::sscanf(state.buffer.c_str() + start + 2, "%d;%dR", &row, &column) == 2) {
                            state.buffer.erase(start, end - start + 1); // keep any keys typed around it
                            break;
                        }
                    }
                }
                const double left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0 || !readMore(state, static_cast<int>(std::ceil(left * 1000.0)))) {
                    break;
                }
            }
        }
        return [row, column](lua_State* T) {
            if (row <= 0) {
                lua_pushnil(T);
                return 1;
            }
            lua_pushnumber(T, column);
            lua_pushnumber(T, row);
            return 2;
        };
    });
}

// term.rawMode(on: boolean): boolean
int termRawMode(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    const bool on = lua_toboolean(L, 1) != 0;

    TerminalModes& m = modes();
    bool previous = false;
    bool ok = true;
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        previous = m.raw;
        if (on != m.raw) {
            ok = applyMode(on ? InputMode::Raw : InputMode::Normal);
            if (ok) {
                m.raw = on;
            }
        }
    }
    if (!ok) {
        luaL_error(L, "term.rawMode: stdin is not a terminal");
    }
    lua_pushboolean(L, previous);
    return 1;
}

// term.isRaw(): boolean
int termIsRaw(lua_State* L) {
    std::lock_guard<std::mutex> lock(modes().mutex);
    lua_pushboolean(L, modes().raw);
    return 1;
}

// term.readKey(timeout: number?): Key?
int termReadKey(lua_State* L) {
    double timeout = kForever;
    if (!lua_isnoneornil(L, 1)) {
        timeout = luaL_checknumber(L, 1);
        luaL_argcheck(L, timeout >= 0, 1, "timeout must not be negative");
    }

    return Scheduler::of(L).await(L, [timeout]() -> sched::Completion {
        auto key = std::make_shared<std::optional<Key>>(takeKey(timeout));
        return [key](lua_State* T) {
            if (*key) {
                pushKey(T, **key);
            } else {
                lua_pushnil(T);
            }
            return 1;
        };
    });
}

constexpr NativeFunction kFunctions[] = {
    {"prompt", termPrompt},
    {"password", termPassword},
    {"size", termSize},
    {"colorLevel", termColorLevel},
    {"setColorLevel", termSetColorLevel},
    {"style", termStyle},
    {"strip", termStrip},
    {"width", termWidth},
    {"moveTo", termMoveTo},
    {"move", termMove},
    {"clear", termClear},
    {"hideCursor", emitFixed<kHideCursor>},
    {"showCursor", emitFixed<kShowCursor>},
    {"saveCursor", emitFixed<kSaveCursor>},
    {"restoreCursor", emitFixed<kRestoreCursor>},
    {"altScreen", termAltScreen},
    {"setTitle", termSetTitle},
    {"bell", emitFixed<kBell>},
    {"cursorPosition", termCursorPosition},
    {"rawMode", termRawMode},
    {"isRaw", termIsRaw},
    {"readKey", termReadKey},
};

constexpr NativeFunction kAnsiFunctions[] = {
    {"moveTo", ansiMoveTo},
    {"move", ansiMove},
    {"clear", ansiClear},
    {"style", ansiStyle},
};

constexpr NativeFunction kStreamMethods[] = {
    {"write", streamWrite},
    {"writeLine", streamWriteLine},
    {"flush", streamFlush},
    {"readLine", streamReadLine},
    {"read", streamRead},
    {"readSome", streamReadSome},
    {"readAll", streamReadAll},
    {"lines", streamLines},
    {"isTerminal", streamIsTerminal},
    {"size", streamSize},
};

} // namespace

void openTerm(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 4);
    setFunctions(L, kFunctions);

    lua_createtable(L, 0, static_cast<int>(std::size(kAnsiFunctions)) + 8);
    setFunctions(L, kAnsiFunctions);
    const std::pair<const char*, const char*> constants[] = {
        {"hideCursor", kHideCursor},     {"showCursor", kShowCursor},     {"saveCursor", kSaveCursor},
        {"restoreCursor", kRestoreCursor}, {"altScreenOn", kAltScreenOn}, {"altScreenOff", kAltScreenOff},
        {"reset", kReset},
    };
    for (const auto& [name, code] : constants) {
        lua_pushstring(L, code);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "ansi");

    if (luaL_newmetatable(L, kStreamMetatable)) {
        lua_createtable(L, 0, static_cast<int>(std::size(kStreamMethods)));
        setFunctions(L, kStreamMethods);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, streamToString, "stream.__tostring");
        lua_setfield(L, -2, "__tostring");

        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");
    }

    for (int fd = 0; fd < 3; ++fd) {
        auto* slot = static_cast<int*>(lua_newuserdata(L, sizeof(int)));
        *slot = fd;
        lua_pushvalue(L, -2);
        lua_setmetatable(L, -2);
        lua_setfield(L, -3, kStreamNames[fd]);
    }
    lua_pop(L, 1); // the metatable
}

} // namespace sonata::lib::libs
