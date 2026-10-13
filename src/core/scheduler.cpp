#include <sonata/core/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "lua.h"
#include "lualib.h"

namespace sonata::sched {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kForever = std::numeric_limits<double>::infinity();
constexpr const char* kRegistryKey = "sonata.scheduler";
constexpr const char* kResultsKey = "sonata.scheduler.results"; // thread -> outcome record (weak keys)
constexpr double kMaxPollSeconds = 3600.0;                       // poll() sleeps in chunks of at most this
constexpr std::size_t kMaxWorkers = 512;
constexpr auto kWorkerIdleTime = std::chrono::seconds(30);

#ifdef _WIN32
using PollFd = WSAPOLLFD;
int pollAll(PollFd* fds, std::size_t n, int ms) { return WSAPoll(fds, static_cast<ULONG>(n), ms); }
#else
using PollFd = pollfd;
int pollAll(PollFd* fds, std::size_t n, int ms) { return ::poll(fds, static_cast<nfds_t>(n), ms); }
#endif

// The worker pool ////////////////////////////////////////////////////////////
//
// One per process, shared by every VM (jobs never touch Lua). It grows when all
// workers are busy, so a job that blocks for a long time (reading stdin, waiting
// for a child process) never holds up the others, and idle workers exit after a
// while. The pool is deliberately never destroyed: a worker may still be blocked
// in a system call when the process exits.
class Pool {
  public:
    static Pool& instance() {
        static Pool* pool = new Pool();
        return *pool;
    }

    void submit(std::function<void()> work) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(work));
        if (queue_.size() > idle_ && threads_ < kMaxWorkers) {
            ++threads_;
            std::thread([this] { loop(); }).detach();
        } else {
            wake_.notify_one();
        }
    }

  private:
    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            ++idle_;
            const bool got = wake_.wait_for(lock, kWorkerIdleTime, [this] { return !queue_.empty(); });
            --idle_;
            if (!got) {
                --threads_;
                return;
            }

            std::function<void()> work = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            work();
            work = nullptr; // destroy what the job captured before taking the lock again
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::size_t idle_ = 0;
    std::size_t threads_ = 0;
};

} // namespace

// Waking the loop from other threads ///////////////////////////////////////////

namespace detail {

// Something poll() can wait on that another thread can make readable: a pipe on
// POSIX, a UDP socket connected to itself on Windows (WSAPoll only takes sockets).
class Waker {
  public:
    Waker() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ != INVALID_SOCKET) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int length = sizeof addr;
            ::bind(socket_, reinterpret_cast<sockaddr*>(&addr), length);
            ::getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &length);
            ::connect(socket_, reinterpret_cast<sockaddr*>(&addr), length);
            u_long on = 1;
            ioctlsocket(socket_, FIONBIO, &on);
        }
#else
        int fds[2];
        if (::pipe(fds) == 0) {
            for (const int fd : fds) {
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            read_ = fds[0];
            write_ = fds[1];
        }
#endif
    }

    ~Waker() {
#ifdef _WIN32
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
#else
        if (read_ >= 0) {
            ::close(read_);
            ::close(write_);
        }
#endif
    }

    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    void signal() {
#ifdef _WIN32
        ::send(socket_, "x", 1, 0);
#else
        const ssize_t ignored = ::write(write_, "x", 1); // a full pipe is already signalled
        (void)ignored;
#endif
    }

    void drain() {
        char buffer[256];
#ifdef _WIN32
        while (::recv(socket_, buffer, sizeof buffer, 0) > 0) {
        }
#else
        while (::read(read_, buffer, sizeof buffer) > 0) {
        }
#endif
    }

    SocketHandle handle() const {
#ifdef _WIN32
        return static_cast<SocketHandle>(socket_);
#else
        return read_;
#endif
    }

  private:
#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
#else
    int read_ = -1;
    int write_ = -1;
#endif
};

// What other threads hand to the loop. Shared with the jobs in flight and with
// Remote handles, so it can outlive the scheduler.
struct Inbox {
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, Completion>> completions;
    std::vector<std::function<void(lua_State*)>> posts;
    Waker waker;
    bool closed = false;

    void complete(std::uint64_t id, Completion completion) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) {
                return; // "completion" is destroyed after the lock is released
            }
            completions.emplace_back(id, std::move(completion));
        }
        waker.signal();
    }

    bool post(std::function<void(lua_State*)> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) {
                return false;
            }
            posts.push_back(std::move(fn));
        }
        waker.signal();
        return true;
    }

    bool pending() {
        std::lock_guard<std::mutex> lock(mutex);
        return !completions.empty() || !posts.empty();
    }
};

} // namespace detail

Remote::Remote(std::shared_ptr<detail::Inbox> inbox) : inbox_(std::move(inbox)) {}

bool Remote::post(std::function<void(lua_State* L)> fn) {
    return inbox_->post(std::move(fn));
}

// The scheduler's state //////////////////////////////////////////////////////

namespace {

enum class Kind {
    Sleep, // task.wait: resumed with the time it took
    Job,   // await: resumed with the completion's values
    Io,    // waitIo: resumed with nothing
    Park,  // park: resumed with what unpark pushed
    Start, // task.delay: a new thread waiting for its first resume
};

// Something a thread waits for. A waiter of a yielded thread holds a registry
// reference that keeps the thread alive; an inline waiter's thread is running.
struct Waiter {
    std::uint64_t id = 0;
    lua_State* thread = nullptr;
    int ref = LUA_NOREF;
    Kind kind = Kind::Sleep;
    double deadline = kForever;
    double startedAt = 0.0;
    SocketHandle socket{};
    int events = 0;
    int startArgs = 0;              // Start: arguments already on the thread's stack
    lua_State* joinTarget = nullptr; // join: the thread being waited for
    bool yielded = false;
    bool done = false;
    bool raise = false;
    int nresults = 0;
};

struct Ready {
    lua_State* thread = nullptr;
    int ref = LUA_NOREF;
    int nargs = 0;
    bool raise = false;
    bool cancelled = false;
};

// After a failed resume the error value is on top of the dead thread's stack.
std::string describeError(lua_State* thread) {
    const int type = lua_type(thread, -1);
    if (type == LUA_TSTRING || type == LUA_TNUMBER) {
        std::size_t length = 0;
        const char* text = lua_tolstring(thread, -1, &length);
        return std::string(text, length);
    }
    return std::string("<error value of type ") + luaL_typename(thread, -1) + ">";
}

int toMillis(double seconds) {
    if (seconds <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::ceil(std::min(seconds, kMaxPollSeconds) * 1000.0));
}

} // namespace

struct Scheduler::State {
    lua_State* aux = nullptr; // a thread of our own, never run: scratch space for bookkeeping
    Clock::time_point origin = Clock::now();
    std::shared_ptr<detail::Inbox> inbox = std::make_shared<detail::Inbox>();
    std::shared_ptr<Remote> remote;

    std::uint64_t nextId = 1;
    std::unordered_map<std::uint64_t, Waiter> waiters;
    std::unordered_map<lua_State*, std::uint64_t> active; // thread -> the waiter it is suspended on
    std::multimap<double, std::uint64_t> deadlines;        // may hold stale entries; checked when due
    std::unordered_set<std::uint64_t> ioWaiters;
    std::size_t jobsInFlight = 0;
    std::deque<Ready> ready;
    std::unordered_set<lua_State*> timedOut;
    std::unordered_map<lua_State*, std::vector<lua_State*>> joiners; // target -> threads joining it
    std::unordered_map<lua_State*, std::vector<std::function<void(bool)>>> watchers;
    std::unordered_map<lua_State*, std::size_t> errorHandlers; // watchers that handle the thread's errors

    lua_State* current = nullptr;  // the task being resumed right now
    lua_State* pumpFrom = nullptr; // the thread waiting inline while the loop runs, if any
    lua_State* critical = nullptr;
    std::optional<std::string> fatal;
    bool stopping = false;
    ErrorHandler onError;

    double now() const { return std::chrono::duration<double>(Clock::now() - origin).count(); }

    // Waiters ///////////////////////////

    Waiter& newWaiter(lua_State* thread, Kind kind, double deadline) {
        const std::uint64_t id = nextId++;
        Waiter& waiter = waiters[id];
        waiter.id = id;
        waiter.thread = thread;
        waiter.kind = kind;
        waiter.deadline = deadline;
        waiter.startedAt = now();
        if (deadline < kForever) {
            deadlines.emplace(deadline, id);
        }
        return waiter;
    }

    // The condition a waiter was waiting for has happened (or its deadline has
    // passed). Pushes the values it resumes with; a yielded thread goes to the
    // ready queue, an inline one is picked up by the C function waiting on it.
    void complete(std::uint64_t id, const Push* push, bool byDeadline) {
        const auto it = waiters.find(id);
        if (it == waiters.end() || it->second.done) {
            return;
        }
        Waiter& w = it->second;
        w.done = true;

        if (const auto found = active.find(w.thread); found != active.end() && found->second == id) {
            active.erase(found);
        }
        if (w.kind == Kind::Io) {
            ioWaiters.erase(id);
        }
        if (w.kind == Kind::Job) {
            --jobsInFlight;
        }
        if (byDeadline && (w.kind == Kind::Io || w.kind == Kind::Park)) {
            timedOut.insert(w.thread);
            if (w.joinTarget != nullptr) {
                removeJoiner(w.joinTarget, w.thread);
            }
        }

        int n = 0;
        if (w.kind == Kind::Start) {
            n = w.startArgs;
        } else if (w.kind == Kind::Sleep) {
            if (lua_checkstack(w.thread, 1)) {
                lua_pushnumber(w.thread, now() - w.startedAt);
                n = 1;
            }
        } else if (push != nullptr && *push) {
            n = (*push)(w.thread);
            if (n == kRaise) {
                w.raise = true;
                n = 1;
                if (lua_type(w.thread, -1) == LUA_TSTRING && lua_checkstack(w.thread, 2)) {
                    luaL_where(w.thread, 1); // "file:line: ", as luaL_error would add
                    lua_insert(w.thread, -2);
                    lua_concat(w.thread, 2);
                }
            }
        }
        w.nresults = n;

        if (w.yielded) {
            ready.push_back(Ready{w.thread, w.ref, n, w.raise, false});
            waiters.erase(it);
        }
    }

    // Removes every trace of "thread" from the queues (it is being cancelled).
    void dropThread(lua_State* thread) {
        for (auto it = waiters.begin(); it != waiters.end();) {
            Waiter& w = it->second;
            if (w.thread != thread) {
                ++it;
                continue;
            }
            if (w.kind == Kind::Io) {
                ioWaiters.erase(w.id);
            }
            if (w.kind == Kind::Job && !w.done) {
                --jobsInFlight; // its completion will find no waiter and be dropped
            }
            if (w.joinTarget != nullptr) {
                removeJoiner(w.joinTarget, thread);
            }
            if (w.yielded) {
                lua_unref(aux, w.ref);
            }
            it = waiters.erase(it);
        }
        active.erase(thread);
        timedOut.erase(thread);

        for (Ready& entry : ready) {
            if (entry.thread == thread && !entry.cancelled) {
                entry.cancelled = true;
                lua_unref(aux, entry.ref);
                entry.ref = LUA_NOREF;
            }
        }
    }

    void removeJoiner(lua_State* target, lua_State* joiner) {
        const auto it = joiners.find(target);
        if (it == joiners.end()) {
            return;
        }
        auto& list = it->second;
        list.erase(std::remove(list.begin(), list.end(), joiner), list.end());
        if (list.empty()) {
            joiners.erase(it);
        }
    }

    bool unpark(lua_State* thread, std::uint64_t token, const Push* push) {
        const auto it = active.find(thread);
        if (it == active.end() || (token != 0 && it->second != token)) {
            return false;
        }
        const auto w = waiters.find(it->second);
        if (w == waiters.end() || w->second.kind != Kind::Park || w->second.done) {
            return false;
        }
        complete(it->second, push, false);
        return true;
    }

    // Outcome records ////////////////////////////

    // Pushes results[thread] (or nil) on aux.
    void pushRecord(lua_State* thread) const {
        lua_checkstack(aux, 3);
        lua_getfield(aux, LUA_REGISTRYINDEX, kResultsKey);
        lua_checkstack(thread, 1);
        lua_pushthread(thread);
        lua_xmove(thread, aux, 1);
        lua_rawget(aux, -2);
        lua_remove(aux, -2);
    }

    // Leaves aux as: results, thread, record (a new table with "status").
    void beginRecord(lua_State* thread, const char* status, int size) {
        lua_checkstack(aux, 6);
        lua_getfield(aux, LUA_REGISTRYINDEX, kResultsKey);
        lua_checkstack(thread, 1);
        lua_pushthread(thread);
        lua_xmove(thread, aux, 1);
        lua_createtable(aux, size, 2);
        lua_pushstring(aux, status);
        lua_setfield(aux, -2, "status");
    }

    void endRecord() {
        lua_rawset(aux, -3);
        lua_pop(aux, 1);
    }

    // A thread returned: its results are its whole stack.
    void recordResults(lua_State* thread) {
        const int n = lua_gettop(thread);
        beginRecord(thread, "done", n);
        lua_pushinteger(aux, n);
        lua_setfield(aux, -2, "n");
        for (int i = n; i >= 1; --i) {
            lua_xmove(thread, aux, 1); // the top value is result i
            lua_rawseti(aux, -2, i);
        }
        endRecord();
    }

    void recordError(lua_State* thread) {
        beginRecord(thread, "error", 0);
        lua_checkstack(thread, 1);
        lua_pushvalue(thread, -1);
        lua_xmove(thread, aux, 1);
        lua_setfield(aux, -2, "error");
        endRecord();
    }

    void recordCancelled(lua_State* thread) {
        beginRecord(thread, "cancelled", 0);
        endRecord();
    }

    // Running threads /////////////////////
    
    void report(lua_State* thread) {
        ThreadError error{thread, describeError(thread), lua_debugtrace(thread)};
        if (onError) {
            onError(error);
            return;
        }
        std::fprintf(stderr, "%s\n", error.message.c_str());
        if (!error.traceback.empty()) {
            std::fprintf(stderr, "stacktrace:\n%s", error.traceback.c_str());
        }
        std::fflush(stderr);
    }

    void wakeJoiners(lua_State* target) {
        const auto it = joiners.find(target);
        if (it == joiners.end()) {
            return;
        }
        const std::vector<lua_State*> list = std::move(it->second);
        joiners.erase(it);
        for (lua_State* joiner : list) {
            unpark(joiner, 0, nullptr);
        }
    }

    void fireWatchers(lua_State* target, bool failed) {
        const auto it = watchers.find(target);
        if (it == watchers.end()) {
            return;
        }
        const std::vector<std::function<void(bool)>> list = std::move(it->second);
        watchers.erase(it);
        errorHandlers.erase(target);
        for (const auto& fn : list) {
            fn(failed);
        }
    }

    // What happens after any resume of a task.
    void settle(lua_State* thread, int status) {
        if (status == LUA_YIELD || status == LUA_BREAK) {
            return;
        }

        if (status == LUA_OK) {
            recordResults(thread);
            lua_settop(thread, 0);
        } else {
            if (thread == critical) {
                std::string message = describeError(thread);
                message += "\nstacktrace:\n";
                message += lua_debugtrace(thread);
                fatal = std::move(message);
                stopping = true;
            } else if (joiners.find(thread) == joiners.end() && errorHandlers.find(thread) == errorHandlers.end()) {
                report(thread);
            }
            recordError(thread);
        }
        wakeJoiners(thread);
        fireWatchers(thread, status != LUA_OK);
    }

    int resumeNow(lua_State* thread, lua_State* from, int nargs, bool raise) {
        lua_State* previous = current;
        current = thread;
        const int status = raise ? lua_resumeerror(thread, from) : lua_resume(thread, from, nargs);
        current = previous;
        settle(thread, status);
        return status;
    }

    // The loop /////////////////////

    double nextDeadline() {
        while (!deadlines.empty()) {
            const auto [when, id] = *deadlines.begin();
            const auto it = waiters.find(id);
            if (it != waiters.end() && !it->second.done && it->second.deadline == when) {
                return when;
            }
            deadlines.erase(deadlines.begin());
        }
        return kForever;
    }

    // Waits (up to timeoutMs, -1 = forever) for the waker or a watched socket,
    // and returns the I/O waiters that are ready.
    std::vector<std::uint64_t> pollOnce(int timeoutMs) {
        std::vector<PollFd> fds;
        std::vector<std::uint64_t> owners;
        fds.reserve(ioWaiters.size() + 1);
        owners.reserve(ioWaiters.size() + 1);

        PollFd wake{};
        wake.fd = inbox->waker.handle();
        wake.events = POLLIN;
        fds.push_back(wake);
        owners.push_back(0);

        for (const std::uint64_t id : ioWaiters) {
            const Waiter& w = waiters.at(id);
            PollFd p{};
            p.fd = w.socket;
            p.events = static_cast<short>(((w.events & kReadable) ? POLLIN : 0) | ((w.events & kWritable) ? POLLOUT : 0));
            fds.push_back(p);
            owners.push_back(id);
        }

        std::vector<std::uint64_t> readyIds;
        if (pollAll(fds.data(), fds.size(), timeoutMs) <= 0) {
            return readyIds; // timed out, or interrupted by a signal: nothing to report
        }

        for (std::size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents != 0) {
                readyIds.push_back(owners[i]);
            }
        }
        return readyIds;
    }

    bool hasWork(double next, bool inboxPending) const {
        return !ready.empty() || jobsInFlight > 0 || !ioWaiters.empty() || next < kForever || inboxPending;
    }

    bool step(bool wait) {
        const double next = nextDeadline();
        const bool inboxPending = inbox->pending();
        if (!hasWork(next, inboxPending)) {
            return false;
        }

        int timeoutMs = 0;
        if (wait && ready.empty() && !inboxPending) {
            timeoutMs = next < kForever ? toMillis(next - now()) : -1;
        }

        // Sockets and wake-ups.
        const std::vector<std::uint64_t> ioReady = pollOnce(timeoutMs);
        inbox->waker.drain();
        for (const std::uint64_t id : ioReady) {
            complete(id, nullptr, false);
        }

        // Finished jobs and posted work.
        std::vector<std::pair<std::uint64_t, Completion>> completions;
        std::vector<std::function<void(lua_State*)>> posts;
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            completions.swap(inbox->completions);
            posts.swap(inbox->posts);
        }
        for (auto& [id, completion] : completions) {
            complete(id, &completion, false);
        }
        completions.clear();

        // Deadlines.
        const double t = now();
        while (!deadlines.empty() && deadlines.begin()->first <= t) {
            const auto [when, id] = *deadlines.begin();
            deadlines.erase(deadlines.begin());
            const auto it = waiters.find(id);
            if (it != waiters.end() && !it->second.done && it->second.deadline == when) {
                complete(id, nullptr, true);
            }
        }

        for (auto& post : posts) {
            const int top = lua_gettop(aux);
            post(aux);
            lua_settop(aux, top);
        }
        posts.clear();

        // Everything woken so far runs now; threads queued while these run wait for the next cycle.
        for (std::size_t count = ready.size(); count > 0 && !ready.empty(); --count) {
            const Ready entry = ready.front();
            ready.pop_front();
            if (entry.cancelled) {
                continue;
            }
            if (lua_costatus(aux, entry.thread) == LUA_COSUS) {
                resumeNow(entry.thread, pumpFrom, entry.nargs, entry.raise);
            }
            lua_unref(aux, entry.ref);
        }
        return true;
    }

    // Runs the loop until waiter "id" is done (it belongs to "L", which is
    // waiting inline). If nothing can end it, it ends now as timed out.
    void pumpUntilDone(lua_State* L, std::uint64_t id) {
        lua_State* savedFrom = pumpFrom;
        pumpFrom = L;
        for (;;) {
            const auto it = waiters.find(id);
            if (it == waiters.end() || it->second.done) {
                break;
            }
            if (!step(true)) {
                complete(id, nullptr, true);
                break;
            }
        }
        pumpFrom = savedFrom;
    }
};

// Scheduler ///////////////////////////////////////////////////////////////////

Scheduler* Scheduler::find(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRegistryKey);
    auto* box = static_cast<Scheduler**>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return box != nullptr ? *box : nullptr;
}

Scheduler& Scheduler::of(lua_State* L) {
    if (Scheduler* existing = find(L)) {
        return *existing;
    }

    auto** box = static_cast<Scheduler**>(lua_newuserdatadtor(L, sizeof(Scheduler*), [](lua_State*, void* data) {
        delete *static_cast<Scheduler**>(data); // no Lua calls in there: the VM is going away
    }));
    *box = nullptr;
    *box = new Scheduler(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kRegistryKey);
    return **box;
}

Scheduler::Scheduler(lua_State* L) : state_(std::make_unique<State>()) {
    State& s = *state_;

    // Created from the main thread so it has the program's globals, not those of whoever asked first.
    lua_State* main = lua_mainthread(L);
    s.aux = lua_newthread(main);
    lua_ref(main, -1);
    lua_pop(main, 1);

    lua_State* aux = s.aux;
    lua_createtable(aux, 0, 0);
    lua_createtable(aux, 0, 1);
    lua_pushstring(aux, "k");
    lua_setfield(aux, -2, "__mode");
    lua_setmetatable(aux, -2);
    lua_setfield(aux, LUA_REGISTRYINDEX, kResultsKey);

    s.remote = std::make_shared<Remote>(s.inbox);
}

Scheduler::~Scheduler() {
    std::vector<std::pair<std::uint64_t, Completion>> dropped;
    {
        std::lock_guard<std::mutex> lock(state_->inbox->mutex);
        state_->inbox->closed = true;
        dropped.swap(state_->inbox->completions);
        state_->inbox->posts.clear();
    }
}

double Scheduler::now() const {
    return state_->now();
}

namespace {

struct Prepared {
    lua_State* thread;
    int nargs;
};

// Turns "functionOrThread, ...args" (stack slots "first"..top) into a thread that is
// ready to be resumed with "nargs" values on its own stack. The thread is left on top
// of L's stack. Raises argument errors before anything else happens.
Prepared prepareThread(lua_State* L, int first) {
    luaL_argcheck(L, lua_isfunction(L, first) || lua_isthread(L, first), first, "function or thread expected");

    const int last = lua_gettop(L);
    const int count = last - first; // arguments after the function/thread
    luaL_checkstack(L, count + 2, "too many arguments");

    if (lua_isthread(L, first)) {
        lua_State* thread = lua_tothread(L, first);
        luaL_argcheck(L, lua_costatus(L, thread) == LUA_COSUS, first, "cannot schedule a thread that is not suspended");
        luaL_argcheck(L, lua_checkstack(thread, count), first, "too many arguments");

        for (int i = first + 1; i <= last; ++i) {
            lua_pushvalue(L, i);
        }
        lua_xmove(L, thread, count);
        lua_pushvalue(L, first);
        return {thread, count};
    }

    lua_State* thread = lua_newthread(L); // pushed onto L
    luaL_argcheck(L, lua_checkstack(thread, count + 1), first, "too many arguments");

    for (int i = first; i <= last; ++i) {
        lua_pushvalue(L, i);
    }
    lua_xmove(L, thread, count + 1); // the function, then its arguments
    return {thread, count};
}

} // namespace

lua_State* Scheduler::spawn(lua_State* L, int index) {
    const Prepared prepared = prepareThread(L, index);
    state_->resumeNow(prepared.thread, L, prepared.nargs, false);
    return prepared.thread; // still on top of L
}

lua_State* Scheduler::defer(lua_State* L, int index) {
    const Prepared prepared = prepareThread(L, index);
    lua_pushvalue(L, -1);
    const int ref = lua_ref(L, -1);
    lua_pop(L, 1);
    state_->ready.push_back(Ready{prepared.thread, ref, prepared.nargs, false, false});
    return prepared.thread;
}

lua_State* Scheduler::delay(lua_State* L, double seconds, int index) {
    const Prepared prepared = prepareThread(L, index);
    State& s = *state_;

    Waiter& w = s.newWaiter(prepared.thread, Kind::Start, s.now() + std::max(seconds, 0.0));
    w.startArgs = prepared.nargs;
    w.yielded = true;
    w.ref = lua_ref(L, -1);
    s.active[prepared.thread] = w.id;
    return prepared.thread;
}

int Scheduler::resume(lua_State* thread, lua_State* from, int nargs) {
    return state_->resumeNow(thread, from, nargs, false);
}

bool Scheduler::canSuspend(lua_State* L) const {
    return L == state_->current && lua_isyieldable(L);
}

namespace {

// Shared tail of every suspend function. See SUSPENDING in the header.
int suspendOn(Scheduler::State& s, bool canYield, lua_State* L, std::uint64_t id, Continuation k) {
    s.timedOut.erase(L); // left over from an earlier wait whose owner did not ask
    s.active[L] = id;
    Waiter& w = s.waiters.at(id);

    if (canYield) {
        w.yielded = true;
        lua_pushthread(L);
        w.ref = lua_ref(L, -1);
        lua_pop(L, 1);
        return lua_yield(L, 0);
    }

    s.pumpUntilDone(L, id);

    const auto it = s.waiters.find(id);
    const Waiter finished = it->second; // trivially copyable
    s.waiters.erase(it);
    s.active.erase(L);

    if (finished.raise) {
        lua_error(L); // the error value is on top
    }
    return k != nullptr ? kInline : finished.nresults;
}

} // namespace

int Scheduler::sleep(lua_State* L, double seconds) {
    State& s = *state_;
    const double start = s.now();
    const double deadline = seconds >= kForever ? kForever : start + std::max(seconds, 0.0);
    const std::uint64_t id = s.newWaiter(L, Kind::Sleep, deadline).id;
    return suspendOn(s, canSuspend(L), L, id, nullptr);
}

int Scheduler::await(lua_State* L, Job job, Continuation k) {
    State& s = *state_;
    const bool canYield = canSuspend(L);
    const std::uint64_t id = s.newWaiter(L, Kind::Job, kForever).id;
    ++s.jobsInFlight;

    Pool::instance().submit([inbox = s.inbox, id, work = std::move(job)]() mutable {
        Completion completion;
        try {
            completion = work();
        } catch (const std::exception& e) {
            std::string message = e.what();
            completion = [message](lua_State* thread) {
                lua_pushlstring(thread, message.data(), message.size());
                return kRaise;
            };
        } catch (...) {
            completion = [](lua_State* thread) {
                lua_pushstring(thread, "unknown error in a background job");
                return kRaise;
            };
        }
        work = nullptr;
        inbox->complete(id, std::move(completion));
    });

    return suspendOn(s, canYield, L, id, k);
}

int Scheduler::waitIo(lua_State* L, SocketHandle socket, int events, double deadline, Continuation k) {
    State& s = *state_;
    Waiter& w = s.newWaiter(L, Kind::Io, deadline);
    w.socket = socket;
    w.events = events;
    s.ioWaiters.insert(w.id);
    return suspendOn(s, canSuspend(L), L, w.id, k);
}

int Scheduler::park(lua_State* L, double deadline, Continuation k, std::uint64_t* token) {
    State& s = *state_;
    const std::uint64_t id = s.newWaiter(L, Kind::Park, deadline).id;
    if (token != nullptr) {
        *token = id;
    }
    return suspendOn(s, canSuspend(L), L, id, k);
}

bool Scheduler::unpark(lua_State* thread, std::uint64_t token, const Push& push) {
    return token != 0 && state_->unpark(thread, token, &push);
}

bool Scheduler::timedOut(lua_State* L) {
    return state_->timedOut.erase(L) != 0;
}

void Scheduler::wakeIo(SocketHandle socket) {
    State& s = *state_;
    std::vector<std::uint64_t> ids;
    for (const std::uint64_t id : s.ioWaiters) {
        if (s.waiters.at(id).socket == socket) {
            ids.push_back(id);
        }
    }
    for (const std::uint64_t id : ids) {
        s.complete(id, nullptr, false);
    }
}

Scheduler::Status Scheduler::status(lua_State* L, lua_State* thread) const {
    const State& s = *state_;

    s.pushRecord(thread);
    if (lua_istable(s.aux, -1)) {
        lua_getfield(s.aux, -1, "status");
        const std::string recorded = lua_tostring(s.aux, -1);
        lua_pop(s.aux, 2);
        return recorded == "done" ? Status::Done : recorded == "error" ? Status::Error : Status::Cancelled;
    }
    lua_pop(s.aux, 1);

    switch (lua_costatus(L, thread)) {
    case LUA_CORUN:
        return Status::Running;
    case LUA_CONOR:
        return Status::Normal;
    case LUA_COFIN:
        return Status::Done;
    case LUA_COERR:
        return Status::Error;
    default:
        break;
    }

    if (const auto it = s.active.find(thread); it != s.active.end()) {
        const auto w = s.waiters.find(it->second);
        return w != s.waiters.end() && w->second.kind == Kind::Start ? Status::Scheduled : Status::Waiting;
    }
    for (const Ready& entry : s.ready) {
        if (entry.thread == thread && !entry.cancelled) {
            return Status::Scheduled;
        }
    }
    return Status::Suspended;
}

void Scheduler::cancel(lua_State* L, lua_State* thread) {
    const int status = lua_costatus(L, thread);
    if (status == LUA_CORUN || status == LUA_CONOR) {
        luaL_error(L, "cannot cancel a thread that is running");
    }

    State& s = *state_;
    const bool alreadyFinished = finished(L, thread);
    s.dropThread(thread);

    // Closing the thread makes it permanently dead, whether or not we knew it.
    lua_resetthread(thread);

    if (!alreadyFinished) {
        s.recordCancelled(thread);
    }
    s.wakeJoiners(thread);
    s.fireWatchers(thread, !alreadyFinished);
}

bool Scheduler::watch(lua_State* L, lua_State* thread, std::function<void(bool failed)> fn, bool handlesErrors) {
    if (finished(L, thread)) {
        return false;
    }
    state_->watchers[thread].push_back(std::move(fn));
    if (handlesErrors) {
        ++state_->errorHandlers[thread];
    }
    return true;
}

bool Scheduler::finished(lua_State* L, lua_State* thread) const {
    const State& s = *state_;
    s.pushRecord(thread);
    const bool recorded = lua_istable(s.aux, -1);
    lua_pop(s.aux, 1);
    if (recorded) {
        return true;
    }
    const int status = lua_costatus(L, thread);
    return status == LUA_COFIN || status == LUA_COERR;
}

int Scheduler::join(lua_State* L, lua_State* thread, double deadline, Continuation k) {
    if (finished(L, thread)) {
        return kInline;
    }
    if (thread == L) {
        luaL_error(L, "a thread cannot join itself");
    }

    State& s = *state_;
    s.joiners[thread].push_back(L);
    const std::uint64_t id = s.newWaiter(L, Kind::Park, deadline).id;
    s.waiters.at(id).joinTarget = thread;
    return suspendOn(s, canSuspend(L), L, id, k);
}

int Scheduler::pushOutcome(lua_State* L, lua_State* thread) const {
    const State& s = *state_;
    lua_State* aux = s.aux;

    s.pushRecord(thread);
    if (!lua_istable(aux, -1)) {
        lua_pop(aux, 1);
        if (lua_costatus(L, thread) == LUA_COERR && lua_gettop(thread) > 0) {
            luaL_checkstack(L, 2, nullptr);
            lua_pushboolean(L, 0);
            lua_pushvalue(thread, -1);
            lua_xmove(thread, L, 1);
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    lua_getfield(aux, -1, "status");
    const std::string status = lua_tostring(aux, -1);
    lua_pop(aux, 1);

    if (status == "done") {
        lua_getfield(aux, -1, "n");
        const int n = static_cast<int>(lua_tointeger(aux, -1));
        lua_pop(aux, 1);
        luaL_checkstack(L, n + 1, "too many results");
        lua_checkstack(aux, 1);
        lua_pushboolean(L, 1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(aux, -1, i);
            lua_xmove(aux, L, 1);
        }
        lua_pop(aux, 1);
        return n + 1;
    }

    luaL_checkstack(L, 2, nullptr);
    lua_pushboolean(L, 0);
    if (status == "error") {
        lua_getfield(aux, -1, "error");
        lua_xmove(aux, L, 1);
    } else {
        lua_pushstring(L, "cancelled");
    }
    lua_pop(aux, 1);
    return 2;
}

lua_State* Scheduler::current() const {
    return state_->current;
}

std::size_t Scheduler::taskCount() const {
    const State& s = *state_;
    std::unordered_set<lua_State*> threads;
    for (const auto& [id, w] : s.waiters) {
        if (w.yielded) {
            threads.insert(w.thread);
        }
    }
    for (const Ready& entry : s.ready) {
        if (!entry.cancelled) {
            threads.insert(entry.thread);
        }
    }
    return threads.size();
}

void Scheduler::setErrorHandler(ErrorHandler handler) {
    state_->onError = std::move(handler);
}

std::shared_ptr<Remote> Scheduler::remote() {
    return state_->remote;
}

bool Scheduler::step(bool wait) {
    return state_->step(wait);
}

void Scheduler::run() {
    State& s = *state_;
    while (!s.stopping && s.step(true)) {
    }
    s.stopping = false;
}

void Scheduler::stop() {
    state_->stopping = true;
}

void Scheduler::setCritical(lua_State* thread) {
    state_->critical = thread;
}

const std::optional<std::string>& Scheduler::fatalError() const {
    return state_->fatal;
}

bool Scheduler::isWaiting(lua_State* thread) const {
    return state_->active.count(thread) != 0;
}

} // namespace sonata::sched
