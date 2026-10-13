// tests/temp_dir.hpp
#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sonata/core/luau/vm.hpp>
#include <sonata/core/luau/compiler.hpp>
#include <sonata/core/module.hpp>
#include "lua.h"

class TempDir {
  public:
    TempDir() {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        for (int i = 0; i < 100; ++i) {
            auto p = std::filesystem::temp_directory_path() /
                     ("sonata-test-" + std::to_string(gen()));
            if (std::filesystem::create_directory(
                    p)) { // false if it already exists
                path_ = std::filesystem::canonical(p);
                return;
            }
        }
        throw std::runtime_error("could not create temp directory");
    }

    ~TempDir() {
        std::error_code ec; // never throw from a destructor
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const std::filesystem::path &path() const { return path_; }
    std::filesystem::path operator/(const std::filesystem::path &rel) const {
        return path_ / rel;
    }

    // Create a file (and any parent dirs) with the given content.
    std::filesystem::path write(const std::filesystem::path &rel,
                                std::string_view content) const {
        auto p = path_ / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
        return p;
    }

    static std::string read(const std::filesystem::path &p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

  private:
    std::filesystem::path path_;
};

#ifdef _WIN32
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define FILENO _fileno
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define FILENO fileno
#endif

class StdoutCapture {
    int saved_;
    FILE *tmp_;
    bool done_ = false;

  public:
    StdoutCapture() {
        fflush(stdout);
        tmp_ = tmpfile();
        saved_ = DUP(FILENO(stdout));
        DUP2(FILENO(tmp_), FILENO(stdout));
    }

    std::string finish() {
        fflush(stdout);
        DUP2(saved_, FILENO(stdout)); // restore real stdout
        CLOSE(saved_);
        done_ = true;

        std::string out;
        rewind(tmp_);
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, tmp_)) > 0)
            out.append(buf, n);
        fclose(tmp_);
        return out;
    }

    ~StdoutCapture() {
        if (!done_)
            finish();
    }
};

// Runs a chunk on the VM's main thread (not as a task: waits happen inline).
class Exec {
public:
    Exec(std::string_view code) {
        loader.install(vm);
        StdoutCapture capture;
        lua_State* L = vm.state();
        const sonata::luau::Bytecode bytecode = comp.compile(code);
        status = luau_load(L, "=test", bytecode.luauData(), bytecode.size(), 0);
        if (status == 0) {
            status = lua_pcall(L, 0, 0, 0);
        }
        if (status != 0) {
            const char* message = lua_tostring(L, -1);
            error = message ? message : "(error value is not a string)";
            lua_pop(L, 1);
        }
        output = capture.finish();
    }

    const std::string& getOutput() const { return output; }
    const std::string& getError() const { return error; }
    int getStatus() const { return status; }

private:
    sonata::luau::ModuleLoader loader{std::make_unique<sonata::luau::MemorySource>()};
    sonata::luau::VM vm;
    sonata::luau::Compiler comp;
    std::string output;
    std::string error;
    int status = 0;
};