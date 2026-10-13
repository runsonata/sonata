#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct lua_State;

namespace sonata::sched {

/*
 * THE SCHEDULER
 *
 * Every VM has one scheduler. It is created the first time something asks for
 * it, lives in the registry and is destroyed by lua_close(). It owns everything
 * that lets several Luau threads ("tasks") make progress at once:
 *
 *   - the ready queue: threads to resume during the next cycle
 *   - timers: task.wait / task.delay
 *   - a pool of worker threads for blocking work (DNS lookups, child processes,
 *     reading stdin, file locks, ...)
 *   - socket readiness waits (poll / WSAPoll)
 *   - the bookkeeping behind task.status / task.join / task.cancel
 *
 * @sonata/task is a thin layer over this class; net, process, term and fs use
 * it so that a task waiting for I/O never blocks the others.
 *
 * SUSPENDING
 *
 * The suspend functions (sleep, await, waitIo, park) only yield the calling
 * thread when the scheduler is the one running it: a task, or the program's
 * entry script. Anywhere else (a coroutine the script resumes by itself, a
 * metamethod, a module being required, a host that never runs the loop) the
 * wait happens right there on the C stack, and the scheduler's loop keeps
 * running in the meantime so other tasks still make progress. Code calling
 * them never has to care which of the two happened, except where noted.
 *
 * A wait that nothing can ever end (a park without a deadline while nothing
 * else is pending) ends at once and counts as timed out when it happens
 * inline. A suspended task in that situation simply never resumes: the loop
 * ends when the only thing left is waiting forever.
 *
 * RUNNING THE LOOP
 *
 * Hosts call run() after starting their entry thread (ModuleLoader::run()
 * does this). It returns once nothing is left that could ever happen: no
 * ready thread, no timer, no worker job, no socket wait. Threads waiting
 * forever (park without deadline, task.wait(math.huge)) do not keep it alive.
 */

// Pushes values onto a thread that is about to resume and returns how many. It
// may instead push one error value and return kRaise to raise it in the
// thread. Always runs on the VM's own OS thread.
using Push = std::function<int(lua_State* thread)>;

// A worker job's result: a Push that runs once the job has finished.
using Completion = Push;

// Work for a worker thread. It must not touch Lua in any way. What it returns
// runs on the VM's thread afterwards. Exceptions are turned into a Lua error.
using Job = std::function<Completion()>;

// A Luau continuation (lua_Continuation).
using Continuation = int (*)(lua_State* L, int status);

constexpr int kRaise = -1;

// Returned by the suspend functions that take a continuation when the wait
// happened inline (see SUSPENDING) and is already over: the caller carries on
// in the same C call instead of being resumed through its continuation.
constexpr int kInline = -2;

enum IoEvents : int {
    kReadable = 1,
    kWritable = 2,
};

#ifdef _WIN32
using SocketHandle = std::uintptr_t; // SOCKET
#else
using SocketHandle = int;
#endif

struct ThreadError {
    lua_State* thread;     // the thread that failed (dead; its stack is intact)
    std::string message;   // the error value; non-string errors are described by their type
    std::string traceback; // may be empty
};

using ErrorHandler = std::function<void(const ThreadError&)>;

namespace detail {
struct Inbox;
}

/*
 * A handle other OS threads (signal handlers, watchers, ...) use to hand work
 * to a VM's scheduler. Thread-safe, and safe to keep after the VM is gone:
 * post() then simply returns false.
 */
class Remote {
  public:
    explicit Remote(std::shared_ptr<detail::Inbox> inbox);

    // Runs "fn" on the VM's thread during the scheduler's next cycle. It gets
    // a thread that belongs to the scheduler, on which it may push values,
    // call functions and spawn tasks; its stack is reset afterwards. A posted
    // function does not keep the loop alive on its own.
    bool post(std::function<void(lua_State* L)> fn);

  private:
    std::shared_ptr<detail::Inbox> inbox_;
};

class Scheduler {
  public:
    enum class Status {
        Running,   // the thread that is running right now
        Normal,    // resumed another coroutine and waits for it
        Suspended, // yielded on its own; nothing will resume it
        Waiting,   // suspended on the scheduler (sleep, I/O, job, park)
        Scheduled, // queued to run (task.defer, task.delay, woken up)
        Done,      // returned
        Error,     // failed
        Cancelled, // stopped by cancel()
    };

    // The scheduler of the VM "L" belongs to, created on first use.
    static Scheduler& of(lua_State* L);

    // nullptr if the VM has none yet.
    static Scheduler* find(lua_State* L);

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    ~Scheduler();

    // Seconds on the scheduler's monotonic clock (0 = when it was created).
    double now() const;

    // ---- starting threads ------------------------------------------------
    //
    // The function or suspended thread at "index", with the values above it as
    // its arguments, becomes a task. The task's thread is left on top of L's
    // stack and returned. Argument errors are raised on L.

    lua_State* spawn(lua_State* L, int index);                 // runs now, until it first suspends
    lua_State* defer(lua_State* L, int index);                 // runs at the end of this cycle
    lua_State* delay(lua_State* L, double seconds, int index); // runs once "seconds" have passed

    // Resumes "thread" now with "nargs" values from its own stack, exactly as
    // the loop would (errors are reported, results kept for join). Returns the
    // lua_resume status.
    int resume(lua_State* thread, lua_State* from, int nargs);

    // ---- suspending the calling thread -------------------------------------
    //
    // Each of these must be the return value of the lua_CFunction calling it
    // ("return scheduler.sleep(L, 1);"), unless it returned kInline.

    // True if the suspend functions would yield L (rather than wait inline).
    bool canSuspend(lua_State* L) const;

    // Waits "seconds" (inf = forever); the result is how long it really took.
    int sleep(lua_State* L, double seconds);

    // Runs "job" on a worker thread and waits for it. The completion's values
    // are the results. With a continuation "k" (the C function must have been
    // pushed with the same k), Luau calls k once resumed, with the values on
    // top of the stack; an inline wait instead returns kInline with them
    // pushed.
    int await(lua_State* L, Job job, Continuation k = nullptr);

    // Waits until "socket" is ready for "events" (IoEvents bits), it is closed
    // (see wakeIo) or "deadline" (scheduler clock; inf = none) passes. Nothing
    // is pushed: try the operation again to find out which. Needs a
    // continuation; returns kInline for inline waits.
    int waitIo(lua_State* L, SocketHandle socket, int events, double deadline, Continuation k);

    // Waits until unpark() or "deadline". Afterwards timedOut(L) says which;
    // values given to unpark are on top of the stack. Needs a continuation;
    // returns kInline for inline waits.
    //
    // "token" receives (before the thread suspends) the number unpark() needs.
    // Store it with the thread in whatever queue the thread waits in: a stale
    // entry (the wait timed out, the thread was cancelled, or even collected
    // and its address reused) can then never wake the wrong wait.
    int park(lua_State* L, double deadline, Continuation k, std::uint64_t* token);

    // Wakes a parked thread with the values "push" puts on its stack. False if
    // that wait is over (it timed out or the thread was cancelled).
    bool unpark(lua_State* thread, std::uint64_t token, const Push& push);

    // Whether the last wait of L ended by its deadline. Reading it clears it.
    bool timedOut(lua_State* L);

    // Ends every waitIo() on "socket" now (call it before closing the socket).
    void wakeIo(SocketHandle socket);

    // ---- managing threads ------------------------------------------------

    Status status(lua_State* L, lua_State* thread) const;

    // Stops "thread" for good: drops everything scheduled for it and closes
    // it. Raises on L if the thread is running or normal.
    void cancel(lua_State* L, lua_State* thread);

    // Has "thread" returned, failed or been cancelled?
    bool finished(lua_State* L, lua_State* thread) const;

    // Waits until "thread" has finished (see park for k and kInline). If
    // timedOut(L) is false afterwards, pushOutcome() tells how it ended.
    int join(lua_State* L, lua_State* thread, double deadline, Continuation k);

    // Pushes how a finished thread ended: true, ...results | false, error.
    // A cancelled thread gives false, "cancelled". Returns the count.
    int pushOutcome(lua_State* L, lua_State* thread) const;

    // Calls "fn" once "thread" has finished; "failed" is true if it raised an
    // error or was cancelled. fn runs on the VM's thread between resumes and
    // must not call into Lua (unpark is fine). Returns false, without keeping
    // fn, if the thread has already finished.
    //
    // With "handlesErrors", the watcher takes care of the thread's error, so
    // it is not reported (like a thread somebody joins).
    bool watch(lua_State* L, lua_State* thread, std::function<void(bool failed)> fn, bool handlesErrors = true);

    // The task the scheduler is running right now, or nullptr.
    lua_State* current() const;

    // Threads that are scheduled or waiting.
    std::size_t taskCount() const;

    // Called when a task fails and nobody is joining it. Without a handler the
    // message and traceback go to stderr.
    void setErrorHandler(ErrorHandler handler);

    // A thread-safe way to reach this scheduler from other OS threads.
    std::shared_ptr<Remote> remote();

    // ---- running the loop ------------------------------------------------

    // One cycle: collects finished jobs, ready sockets and due timers, then
    // resumes what they woke. With "wait", sleeps until something can happen
    // first. Returns false (and does nothing) when nothing is left.
    bool step(bool wait = true);

    // Runs cycles until nothing is left, stop() is called or the critical
    // thread fails.
    void run();
    void stop();

    // When "thread" fails, run() stops and fatalError() holds its message and
    // stack trace instead of the error going to the error handler.
    void setCritical(lua_State* thread);
    const std::optional<std::string>& fatalError() const;

    // True while "thread" is suspended on the scheduler (as opposed to having
    // yielded on its own).
    bool isWaiting(lua_State* thread) const;

    struct State;

  private:
    explicit Scheduler(lua_State* L);

    std::unique_ptr<State> state_;
};

} // namespace sonata::sched
