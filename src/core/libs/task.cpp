// @sonata/task: tasks (lightweight threads) and the tools to coordinate them.
//
//     local task = require("@sonata/task")
//
// Tasks are Luau threads run by the VM's scheduler (sonata/core/scheduler.hpp).
// Only one runs at a time; a task gives the others a turn whenever it waits:
// task.wait, a socket read, a child process, a channel, ... The program keeps
// running until every task has finished or is waiting for something that can
// never happen.
//
// Starting and stopping
//     task.spawn(fn, ...)           --> thread   runs fn(...) right now, until it first waits
//     task.defer(fn, ...)           --> thread   runs fn(...) at the end of the current cycle
//     task.delay(seconds, fn, ...)  --> thread   runs fn(...) once "seconds" have passed
//     task.wait(seconds?)           --> number   waits (default 0 = until the next cycle) and
//                                                returns how long it really waited
//     task.cancel(thread)                        stops a task for good, wherever it is waiting
//
//   Wherever "fn" is expected, a suspended coroutine works too: it is resumed with
//   the extra arguments instead of a new thread being created.
//
// Managing tasks
//     task.running()                --> thread?  the task running right now (nil outside one)
//     task.status(thread)           --> "running" | "normal" | "suspended" | "waiting" |
//                                       "scheduled" | "done" | "error" | "cancelled"
//     task.join(thread, timeout?)   --> true, ...results | false, error | nil, "timed out"
//                                       waits for a task to finish (errors are returned, not raised)
//     task.all({ fn | thread })     --> { result }   runs the functions as tasks and waits for all;
//                                       each entry is that task's first result. The first error
//                                       is raised, and the tasks still running are cancelled.
//     task.race({ fn | thread })    --> index, ...results   the first task to finish wins; the
//                                       others are cancelled. Raises if the winner failed.
//     task.timeout(seconds, fn, ...) --> true, ...results | false, error
//                                       runs fn as a task; cancels it after "seconds"
//                                       (the error is then "timed out")
//     task.count()                  --> number   tasks that are scheduled or waiting
//     task.onError(handler?)        handler(message, traceback, thread) is called (as a new
//                                   task) when a task fails and nobody joins it. nil restores
//                                   the default: print to stderr.
//
// Coordination (all waits take an optional timeout in seconds)
//     task.channel(capacity?)       --> Channel   capacity 0 (default: unbounded) = every send
//                                                 waits for a receiver
//         ch:send(value, timeout?)  --> true | false, "closed" | false, "timed out"
//         ch:recv(timeout?)         --> true, value | false, "closed" | false, "timed out"
//         ch:trySend(value)         --> boolean   never waits
//         ch:tryRecv()              --> true, value | false
//         ch:close()                wakes everyone; values already sent can still be received
//         ch:isClosed(), ch:len() (#ch), ch:capacity()
//     task.mutex()                  --> Mutex
//         m:lock(timeout?) --> boolean,  m:tryLock() --> boolean,  m:unlock(),  m:isLocked()
//         Not reentrant; only the thread that locked it may unlock it.
//     task.semaphore(permits)       --> Semaphore
//         s:acquire(timeout?) --> boolean,  s:tryAcquire() --> boolean,  s:release(),  s:available()
//     task.event()                  --> Event: a flag tasks can wait for
//         e:wait(timeout?) --> boolean,  e:set() (wakes every waiter),  e:reset(),  e:isSet()
//     task.group()                  --> Group: tasks managed together
//         g:spawn(fn, ...) --> thread    g:wait(timeout?) --> ok, { error }   (nil, "timed out")
//         g:cancel()                     g:count() --> tasks still running
//
// Waiting inside something that cannot yield (a metamethod, a for-loop iterator, a
// coroutine you resume yourself, a module's top level) still works: the wait runs
// the scheduler right there until it is over. Prefer waiting directly in tasks.
//
// Errors in tasks never stop the program: they are reported (see task.onError)
// and the task is dead. An error in the program's main script still ends it.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>

#include <sonata/core/library.hpp>
#include <sonata/core/scheduler.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

using sched::Scheduler;

constexpr double kForever = std::numeric_limits<double>::infinity();
constexpr const char* kChannelMeta = "sonata.task.Channel";
constexpr const char* kMutexMeta = "sonata.task.Mutex";
constexpr const char* kSemaphoreMeta = "sonata.task.Semaphore";
constexpr const char* kEventMeta = "sonata.task.Event";
constexpr const char* kGroupMeta = "sonata.task.Group";
constexpr const char* kGatherMeta = "sonata.task.Gather";
constexpr const char* kCompanionsKey = "sonata.task.companions"; // object -> Lua table (weak keys)
constexpr const char* kErrorHandlerKey = "sonata.task.onError";

Scheduler& scheduler(lua_State* L) {
    return Scheduler::of(L);
}

// Reads a duration argument. nil -> fallback; negative values count as 0; NaN is rejected.
double checkSeconds(lua_State* L, int index, double fallback) {
    if (lua_isnoneornil(L, index)) {
        return fallback;
    }
    const double seconds = luaL_checknumber(L, index);
    luaL_argcheck(L, !std::isnan(seconds), index, "duration must not be NaN");
    return seconds < 0.0 ? 0.0 : seconds;
}

// Replaces the optional timeout at "index" by the absolute deadline it stands
// for, so continuations (which see the same stack) read the same deadline.
void timeoutToDeadline(lua_State* L, int index) {
    const double timeout = checkSeconds(L, index, kForever);
    const double deadline = timeout >= kForever ? kForever : scheduler(L).now() + timeout;
    lua_pushnumber(L, deadline);
    lua_replace(L, index);
}

lua_State* checkThread(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTHREAD);
    return lua_tothread(L, index);
}

// Pushes the companion table of the object at "index": where an object keeps the
// Lua values it holds (queued channel values, a group's threads, ...).
void pushCompanion(lua_State* L, int index) {
    index = lua_absindex(L, index);
    lua_getfield(L, LUA_REGISTRYINDEX, kCompanionsKey);
    lua_pushvalue(L, index);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 0);
        lua_pushvalue(L, index);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
}

// Userdata holding a T, destroyed with it.
template <typename T, typename... Args>
T* newObject(lua_State* L, const char* meta, Args&&... args) {
    void* memory = lua_newuserdatadtor(L, sizeof(T), [](lua_State*, void* p) { static_cast<T*>(p)->~T(); });
    T* object = new (memory) T(std::forward<Args>(args)...);
    luaL_getmetatable(L, meta);
    lua_setmetatable(L, -2);
    return object;
}

template <typename T>
T* checkObject(lua_State* L, int index, const char* meta) {
    return static_cast<T*>(luaL_checkudata(L, index, meta));
}

int pushFalse(lua_State* L, const char* why) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, why);
    return 2;
}

// Threads waiting on a coordination object, oldest first.
struct WaitQueue {
    struct Entry {
        lua_State* thread;
        std::uint64_t token;
    };
    std::deque<Entry> entries;

    // Parks L at the back of the queue (see Scheduler::park).
    int park(lua_State* L, double deadline, sched::Continuation k) {
        entries.push_back(Entry{L, 0});
        return scheduler(L).park(L, deadline, k, &entries.back().token);
    }

    // Wakes the oldest thread that is still waiting; false if there is none.
    bool wakeOne(Scheduler& s, const sched::Push& push) {
        while (!entries.empty()) {
            const Entry entry = entries.front();
            entries.pop_front();
            if (s.unpark(entry.thread, entry.token, push)) {
                return true;
            }
        }
        return false;
    }

    // Removes the oldest entry without waking it (its thread may have stopped
    // waiting already: unpark() tells).
    bool takeOne(Entry& out) {
        if (entries.empty()) {
            return false;
        }
        out = entries.front();
        entries.pop_front();
        return true;
    }

    void wakeAll(Scheduler& s, const sched::Push& push) {
        while (wakeOne(s, push)) {
        }
    }

    // Forgets L (its wait timed out).
    void remove(lua_State* L) {
        for (auto it = entries.begin(); it != entries.end();) {
            it = it->thread == L ? entries.erase(it) : std::next(it);
        }
    }
};

const sched::Push kPushTrue = [](lua_State* thread) {
    lua_pushboolean(thread, 1);
    return 1;
};

// basics ////////////////////////////////////////////////////////////////////

// task.spawn(functionOrThread, ...any): thread
int taskSpawn(lua_State* L) {
    scheduler(L).spawn(L, 1);
    return 1;
}

// task.defer(functionOrThread, ...any): thread
int taskDefer(lua_State* L) {
    scheduler(L).defer(L, 1);
    return 1;
}

// task.delay(seconds: number?, functionOrThread, ...any): thread
int taskDelay(lua_State* L) {
    const double seconds = checkSeconds(L, 1, 0.0);
    scheduler(L).delay(L, seconds, 2);
    return 1;
}

// task.wait(seconds: number?): number
int taskWait(lua_State* L) {
    const double seconds = checkSeconds(L, 1, 0.0);
    return scheduler(L).sleep(L, seconds);
}

// task.cancel(thread)
int taskCancel(lua_State* L) {
    scheduler(L).cancel(L, checkThread(L, 1));
    return 0;
}

// task.running(): thread?
int taskRunning(lua_State* L) {
    lua_State* current = scheduler(L).current();
    if (current == nullptr) {
        lua_pushnil(L);
    } else if (current == L) {
        lua_pushthread(L);
    } else {
        lua_checkstack(current, 1);
        lua_pushthread(current);
        lua_xmove(current, L, 1);
    }
    return 1;
}

// task.status(thread): string
int taskStatus(lua_State* L) {
    static const char* const kNames[] = {"running", "normal", "suspended", "waiting",
                                         "scheduled", "done", "error", "cancelled"};
    const Scheduler::Status status = scheduler(L).status(L, checkThread(L, 1));
    lua_pushstring(L, kNames[static_cast<int>(status)]);
    return 1;
}

// task.count(): number
int taskCount(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(scheduler(L).taskCount()));
    return 1;
}

// task.join ////////////////////////////////////////////////////////////////
// Stack: 1 thread, 2 deadline

int joinFinish(lua_State* L) {
    lua_settop(L, 2);
    Scheduler& s = scheduler(L);
    if (s.timedOut(L)) {
        lua_pushnil(L);
        lua_pushstring(L, "timed out");
        return 2;
    }
    return s.pushOutcome(L, lua_tothread(L, 1));
}

int taskJoinK(lua_State* L, int) {
    return joinFinish(L);
}

// task.join(thread, timeout: number?): true, ...any | false, any | nil, string
int taskJoin(lua_State* L) {
    lua_settop(L, 2);
    lua_State* thread = checkThread(L, 1);
    timeoutToDeadline(L, 2);

    const int r = scheduler(L).join(L, thread, lua_tonumber(L, 2), taskJoinK);
    return r == sched::kInline ? joinFinish(L) : r;
}

// task.timeout //////////////////////////////////////////////////////////////
// Stack after setup: 1 thread, 2 deadline

int timeoutFinish(lua_State* L) {
    lua_settop(L, 2);
    Scheduler& s = scheduler(L);
    lua_State* thread = lua_tothread(L, 1);
    if (s.timedOut(L)) {
        if (!s.finished(L, thread)) {
            s.cancel(L, thread);
        }
        return pushFalse(L, "timed out");
    }
    return s.pushOutcome(L, thread);
}

int taskTimeoutK(lua_State* L, int) {
    return timeoutFinish(L);
}

// task.timeout(seconds: number, fn, ...any): true, ...any | false, any
int taskTimeout(lua_State* L) {
    const double seconds = checkSeconds(L, 1, 0.0);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    Scheduler& s = scheduler(L);
    const double deadline = s.now() + seconds;
    lua_State* thread = s.spawn(L, 2); // the thread is on top
    lua_replace(L, 1);
    lua_pushnumber(L, deadline);
    lua_replace(L, 2);
    lua_settop(L, 2);

    const int r = s.join(L, thread, deadline, taskTimeoutK);
    return r == sched::kInline ? timeoutFinish(L) : r;
}

// task.all / task.race ////////////////////////////////////////////////////////
// Stack after setup: 1 list, 2 threads, 3 Gather

struct GatherState {
    std::size_t remaining = 0;
    std::size_t first = 0;   // 1-based index of the first task to fail (all) or finish (race); 0 = none yet
    bool firstFailed = false;
    lua_State* waiter = nullptr;
    std::uint64_t token = 0;
};

struct Gather {
    std::shared_ptr<GatherState> state = std::make_shared<GatherState>();
};

// Starts (or takes) every entry of the list at index 1 and leaves 2 = threads, 3 = Gather.
void startGather(lua_State* L, bool race) {
    lua_settop(L, 1);
    luaL_checktype(L, 1, LUA_TTABLE);
    const int n = static_cast<int>(lua_objlen(L, 1));
    luaL_argcheck(L, n > 0, 1, "list must not be empty");

    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        luaL_argcheck(L, lua_isfunction(L, -1) || lua_isthread(L, -1), 1, "entries must be functions or threads");
        lua_pop(L, 1);
    }

    Scheduler& s = scheduler(L);
    lua_createtable(L, n, 0); // 2
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        if (lua_isfunction(L, -1)) {
            s.spawn(L, lua_gettop(L)); // leaves fn, thread
            lua_remove(L, -2);
        }
        lua_rawseti(L, 2, i);
    }

    Gather* gather = newObject<Gather>(L, kGatherMeta); // 3
    std::shared_ptr<GatherState> state = gather->state;
    state->remaining = static_cast<std::size_t>(n);

    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        lua_State* thread = lua_tothread(L, -1);
        lua_pop(L, 1);

        const auto onFinish = [&s, state, i, race](bool failed) {
            --state->remaining;
            if (state->first == 0 && (race || failed)) {
                state->first = static_cast<std::size_t>(i);
                state->firstFailed = failed;
            }
            if ((state->first != 0 || state->remaining == 0) && state->waiter != nullptr) {
                s.unpark(state->waiter, state->token, nullptr);
            }
        };
        if (!s.watch(L, thread, onFinish)) {
            onFinish(s.status(L, thread) != Scheduler::Status::Done);
        }
    }
}

// Cancels every task in the threads table that has not finished.
void cancelRest(lua_State* L) {
    Scheduler& s = scheduler(L);
    const int n = static_cast<int>(lua_objlen(L, 2));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        lua_State* thread = lua_tothread(L, -1);
        lua_pop(L, 1);
        const int status = lua_costatus(L, thread);
        if (!s.finished(L, thread) && status != LUA_CORUN && status != LUA_CONOR) {
            s.cancel(L, thread);
        }
    }
}

// Raises the error of the task at threads[index].
int raiseOutcome(lua_State* L, std::size_t index) {
    lua_rawgeti(L, 2, static_cast<int>(index));
    lua_State* thread = lua_tothread(L, -1);
    lua_pop(L, 1);
    scheduler(L).pushOutcome(L, thread); // false, error
    lua_error(L);
}

int allFinish(lua_State* L) {
    lua_settop(L, 3);
    Scheduler& s = scheduler(L);
    const std::shared_ptr<GatherState> state = checkObject<Gather>(L, 3, kGatherMeta)->state;
    state->waiter = nullptr;

    if (s.timedOut(L) && state->first == 0 && state->remaining > 0) {
        cancelRest(L);
        luaL_error(L, "task.all: the tasks can never finish (nothing else is left to run)");
    }
    if (state->first != 0) {
        cancelRest(L);
        raiseOutcome(L, state->first);
    }

    const int n = static_cast<int>(lua_objlen(L, 2));
    lua_createtable(L, n, 0); // 4
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        lua_State* thread = lua_tothread(L, -1);
        lua_pop(L, 1);
        const int count = s.pushOutcome(L, thread); // true, results...
        if (count >= 2) {
            lua_pushvalue(L, -count + 1);
            lua_rawseti(L, 4, i);
        }
        lua_pop(L, count);
    }
    return 1;
}

int taskAllK(lua_State* L, int) {
    return allFinish(L);
}

// task.all(list: { function | thread }): { any }
int taskAll(lua_State* L) {
    startGather(L, false);
    const std::shared_ptr<GatherState> state = checkObject<Gather>(L, 3, kGatherMeta)->state;
    if (state->first != 0 || state->remaining == 0) {
        return allFinish(L);
    }
    state->waiter = L;
    const int r = scheduler(L).park(L, kForever, taskAllK, &state->token);
    return r == sched::kInline ? allFinish(L) : r;
}

int raceFinish(lua_State* L) {
    lua_settop(L, 3);
    Scheduler& s = scheduler(L);
    const std::shared_ptr<GatherState> state = checkObject<Gather>(L, 3, kGatherMeta)->state;
    state->waiter = nullptr;

    if (state->first == 0) {
        s.timedOut(L);
        cancelRest(L);
        luaL_error(L, "task.race: the tasks can never finish (nothing else is left to run)");
    }
    cancelRest(L);
    if (state->firstFailed) {
        raiseOutcome(L, state->first);
    }

    lua_pushnumber(L, static_cast<double>(state->first)); // 4
    lua_rawgeti(L, 2, static_cast<int>(state->first));
    lua_State* thread = lua_tothread(L, -1);
    lua_pop(L, 1);
    const int count = s.pushOutcome(L, thread); // true, results...
    lua_remove(L, -count);                       // drop the "true"
    return count;                                // index + results
}

int taskRaceK(lua_State* L, int) {
    return raceFinish(L);
}

// task.race(list: { function | thread }): (number, ...any)
int taskRace(lua_State* L) {
    startGather(L, true);
    const std::shared_ptr<GatherState> state = checkObject<Gather>(L, 3, kGatherMeta)->state;
    if (state->first != 0) {
        return raceFinish(L);
    }
    state->waiter = L;
    const int r = scheduler(L).park(L, kForever, taskRaceK, &state->token);
    return r == sched::kInline ? raceFinish(L) : r;
}

// task.onError ///////////////////////////////////////////////////////////////

// Handler threads that are still running: their own errors go to stderr, so a
// failing handler cannot keep triggering itself.
std::unordered_set<lua_State*>& handlerThreads() {
    static std::unordered_set<lua_State*> threads;
    return threads;
}

// task.onError(handler: ((message: string, traceback: string, thread: thread) -> ())?)
int taskOnError(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
    }
    lua_settop(L, 1);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, kErrorHandlerKey);

    Scheduler& s = scheduler(L);
    if (lua_isnil(L, 1)) {
        s.setErrorHandler(nullptr);
        return 0;
    }

    std::shared_ptr<sched::Remote> remote = s.remote();
    Scheduler* owner = &s;
    s.setErrorHandler([remote, owner](const sched::ThreadError& error) {
        if (handlerThreads().erase(error.thread) != 0) {
            std::fprintf(stderr, "error in task.onError handler: %s\n%s", error.message.c_str(), error.traceback.c_str());
            return;
        }

        lua_checkstack(error.thread, 1);
        lua_pushthread(error.thread);
        const int ref = lua_ref(error.thread, -1);
        lua_pop(error.thread, 1);

        remote->post([owner, ref, message = error.message, traceback = error.traceback](lua_State* aux) {
            lua_getfield(aux, LUA_REGISTRYINDEX, kErrorHandlerKey);
            lua_pushlstring(aux, message.data(), message.size());
            lua_pushlstring(aux, traceback.data(), traceback.size());
            lua_getref(aux, ref);
            lua_unref(aux, ref);
            if (!lua_isfunction(aux, -4)) {
                return; // removed in the meantime
            }
            lua_State* thread = owner->spawn(aux, lua_gettop(aux) - 3);
            if (!owner->finished(aux, thread)) {
                handlerThreads().insert(thread);
                owner->watch(aux, thread, [thread](bool) { handlerThreads().erase(thread); }, false);
            }
        });
    });
    return 0;
}

// Channel ///////////////////////////////////////////////////////////////////
//
// Values live in the channel's companion table: queued ones at [head, tail), and
// the value of each sender that is waiting for room under that sender's thread.

struct Channel {
    double capacity = kForever;
    bool closed = false;
    long long head = 1;
    long long tail = 1;
    WaitQueue receivers;
    WaitQueue senders;

    long long size() const { return tail - head; }
};

Channel* checkChannel(lua_State* L) {
    return checkObject<Channel>(L, 1, kChannelMeta);
}

// Pushes the value "sender" left in the companion table (at "companion") and clears it.
void takePending(lua_State* L, int companion, lua_State* sender) {
    lua_checkstack(sender, 1);
    lua_pushthread(sender);
    lua_xmove(sender, L, 1); // key
    lua_pushvalue(L, -1);
    lua_rawget(L, companion); // key, value
    lua_insert(L, -2);        // value, key
    lua_pushnil(L);
    lua_rawset(L, companion); // value
}

void setPending(lua_State* L, int valueIndex) {
    pushCompanion(L, 1);
    lua_pushthread(L);
    if (valueIndex == 0) {
        lua_pushnil(L);
    } else {
        lua_pushvalue(L, valueIndex);
    }
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

// Moves the oldest waiting sender's value to the back of the queue and wakes it.
void admitSender(lua_State* L, Channel& ch, int companion) {
    Scheduler& s = scheduler(L);
    WaitQueue::Entry entry{};
    while (ch.senders.takeOne(entry)) {
        takePending(L, companion, entry.thread);
        if (s.unpark(entry.thread, entry.token, kPushTrue)) {
            lua_rawseti(L, companion, static_cast<int>(ch.tail++));
            return;
        }
        lua_pop(L, 1); // that sender gave up waiting
    }
}

// Takes the next value (queued first, then straight from a waiting sender).
// Pushes it and returns true, or pushes nothing and returns false.
bool takeValue(lua_State* L, Channel& ch) {
    Scheduler& s = scheduler(L);
    pushCompanion(L, 1);
    const int companion = lua_gettop(L);

    if (ch.size() > 0) {
        lua_rawgeti(L, companion, static_cast<int>(ch.head));
        lua_pushnil(L);
        lua_rawseti(L, companion, static_cast<int>(ch.head));
        ++ch.head;
        admitSender(L, ch, companion); // there is room again
        lua_remove(L, companion);
        return true;
    }

    WaitQueue::Entry entry{};
    while (ch.senders.takeOne(entry)) {
        takePending(L, companion, entry.thread);
        if (s.unpark(entry.thread, entry.token, kPushTrue)) {
            lua_remove(L, companion);
            return true;
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
    return false;
}

// Hands the value at index 2 to a waiting receiver, or queues it if there is room.
bool putValue(lua_State* L, Channel& ch) {
    Scheduler& s = scheduler(L);
    const sched::Push deliver = [L](lua_State* receiver) {
        lua_checkstack(receiver, 2);
        lua_pushboolean(receiver, 1);
        lua_pushvalue(L, 2);
        lua_xmove(L, receiver, 1);
        return 2;
    };
    if (ch.size() == 0 && ch.receivers.wakeOne(s, deliver)) {
        return true;
    }
    if (static_cast<double>(ch.size()) < ch.capacity) {
        pushCompanion(L, 1);
        lua_pushvalue(L, 2);
        lua_rawseti(L, -2, static_cast<int>(ch.tail++));
        lua_pop(L, 1);
        return true;
    }
    return false;
}

// task.channel(capacity: number?): Channel
int taskChannel(lua_State* L) {
    const double capacity = lua_isnoneornil(L, 1) ? kForever : luaL_checknumber(L, 1);
    luaL_argcheck(L, capacity >= 0 && (capacity == kForever || capacity == std::floor(capacity)), 1,
                  "capacity must be a whole number >= 0");
    newObject<Channel>(L, kChannelMeta)->capacity = capacity;
    return 1;
}

// After a sender's wait. Stack: 1 channel, 2 value, 3 deadline, then what woke it.
int sendAfterWait(lua_State* L) {
    Channel* ch = checkChannel(L);
    if (scheduler(L).timedOut(L)) {
        ch->senders.remove(L);
        lua_settop(L, 3);
        setPending(L, 0);
        return pushFalse(L, "timed out");
    }
    const bool delivered = lua_toboolean(L, -1) != 0;
    lua_settop(L, 3);
    if (delivered) {
        lua_pushboolean(L, 1);
        return 1;
    }
    setPending(L, 0);
    return pushFalse(L, "closed");
}

int channelSendK(lua_State* L, int) {
    return sendAfterWait(L);
}

// ch:send(value: any, timeout: number?): true | (false, string)
int channelSend(lua_State* L) {
    lua_settop(L, 3);
    Channel* ch = checkChannel(L);
    timeoutToDeadline(L, 3);

    if (ch->closed) {
        return pushFalse(L, "closed");
    }
    if (putValue(L, *ch)) {
        lua_pushboolean(L, 1);
        return 1;
    }

    // No room: leave the value with the channel; a receiver takes it and wakes us.
    setPending(L, 2);
    const int r = ch->senders.park(L, lua_tonumber(L, 3), channelSendK);
    return r == sched::kInline ? sendAfterWait(L) : r;
}

// After a receiver's wait. Stack: 1 channel, 2 deadline, then (true, value) or (false, "closed").
int recvAfterWait(lua_State* L) {
    Channel* ch = checkChannel(L);
    if (scheduler(L).timedOut(L)) {
        ch->receivers.remove(L);
        lua_settop(L, 2);
        return pushFalse(L, "timed out");
    }
    return 2;
}

int channelRecvK(lua_State* L, int) {
    return recvAfterWait(L);
}

// ch:recv(timeout: number?): (true, any) | (false, string)
int channelRecv(lua_State* L) {
    lua_settop(L, 2);
    Channel* ch = checkChannel(L);
    timeoutToDeadline(L, 2);

    if (takeValue(L, *ch)) {
        lua_pushboolean(L, 1);
        lua_insert(L, -2);
        return 2;
    }
    if (ch->closed) {
        return pushFalse(L, "closed");
    }

    const int r = ch->receivers.park(L, lua_tonumber(L, 2), channelRecvK);
    return r == sched::kInline ? recvAfterWait(L) : r;
}

// ch:trySend(value: any): boolean
int channelTrySend(lua_State* L) {
    lua_settop(L, 2);
    Channel* ch = checkChannel(L);
    lua_pushboolean(L, !ch->closed && putValue(L, *ch));
    return 1;
}

// ch:tryRecv(): (true, any) | false
int channelTryRecv(lua_State* L) {
    lua_settop(L, 1);
    Channel* ch = checkChannel(L);
    if (takeValue(L, *ch)) {
        lua_pushboolean(L, 1);
        lua_insert(L, -2);
        return 2;
    }
    lua_pushboolean(L, 0);
    return 1;
}

// ch:close()
int channelClose(lua_State* L) {
    Channel* ch = checkChannel(L);
    if (ch->closed) {
        return 0;
    }
    ch->closed = true;

    Scheduler& s = scheduler(L);
    ch->receivers.wakeAll(s, [](lua_State* thread) {
        lua_checkstack(thread, 2);
        lua_pushboolean(thread, 0);
        lua_pushstring(thread, "closed");
        return 2;
    });
    ch->senders.wakeAll(s, [](lua_State* thread) {
        lua_checkstack(thread, 1);
        lua_pushboolean(thread, 0);
        return 1;
    });
    return 0;
}

// ch:isClosed(): boolean
int channelIsClosed(lua_State* L) {
    lua_pushboolean(L, checkChannel(L)->closed);
    return 1;
}

// ch:len(): number (also #ch)
int channelLen(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(checkChannel(L)->size()));
    return 1;
}

// ch:capacity(): number
int channelCapacity(lua_State* L) {
    lua_pushnumber(L, checkChannel(L)->capacity);
    return 1;
}

// Mutex ////////////////////////////////////////////////////////////////////

struct Mutex {
    lua_State* owner = nullptr;
    WaitQueue waiters;
};

Mutex* checkMutex(lua_State* L) {
    return checkObject<Mutex>(L, 1, kMutexMeta);
}

// task.mutex(): Mutex
int taskMutex(lua_State* L) {
    newObject<Mutex>(L, kMutexMeta);
    return 1;
}

// After a wait that ends with a boolean pushed (or a timeout). The waiting
// thread must already have been removed from "queue" if it was woken.
int boolAfterWait(lua_State* L, WaitQueue& queue, int args) {
    if (scheduler(L).timedOut(L)) {
        queue.remove(L);
        lua_settop(L, args);
        lua_pushboolean(L, 0);
        return 1;
    }
    const bool value = lua_toboolean(L, -1) != 0;
    lua_settop(L, args);
    lua_pushboolean(L, value);
    return 1;
}

int mutexLockK(lua_State* L, int) {
    return boolAfterWait(L, checkMutex(L)->waiters, 2);
}

// m:lock(timeout: number?): boolean
int mutexLock(lua_State* L) {
    lua_settop(L, 2);
    Mutex* m = checkMutex(L);
    timeoutToDeadline(L, 2);

    if (m->owner == nullptr) {
        m->owner = L;
        lua_pushboolean(L, 1);
        return 1;
    }
    if (m->owner == L) {
        luaL_error(L, "the mutex is already locked by this thread (it is not reentrant)");
    }

    const int r = m->waiters.park(L, lua_tonumber(L, 2), mutexLockK);
    return r == sched::kInline ? mutexLockK(L, 0) : r;
}

// m:tryLock(): boolean
int mutexTryLock(lua_State* L) {
    Mutex* m = checkMutex(L);
    const bool free = m->owner == nullptr;
    if (free) {
        m->owner = L;
    }
    lua_pushboolean(L, free);
    return 1;
}

// m:unlock()
int mutexUnlock(lua_State* L) {
    Mutex* m = checkMutex(L);
    luaL_argcheck(L, m->owner == L, 1, "the mutex is not locked by this thread");
    m->owner = nullptr;

    // Hand it straight to the next waiter, so nobody can barge in before it runs.
    Scheduler& s = scheduler(L);
    WaitQueue::Entry entry{};
    while (m->waiters.takeOne(entry)) {
        if (s.unpark(entry.thread, entry.token, kPushTrue)) {
            m->owner = entry.thread;
            break;
        }
    }
    return 0;
}

// m:isLocked(): boolean
int mutexIsLocked(lua_State* L) {
    lua_pushboolean(L, checkMutex(L)->owner != nullptr);
    return 1;
}

// Semaphore /////////////////////////////////////////////////////////////////

struct Semaphore {
    double permits = 0;
    WaitQueue waiters;
};

Semaphore* checkSemaphore(lua_State* L) {
    return checkObject<Semaphore>(L, 1, kSemaphoreMeta);
}

// task.semaphore(permits: number): Semaphore
int taskSemaphore(lua_State* L) {
    const double permits = luaL_checknumber(L, 1);
    luaL_argcheck(L, permits >= 0 && permits == std::floor(permits), 1, "permits must be a whole number >= 0");
    newObject<Semaphore>(L, kSemaphoreMeta)->permits = permits;
    return 1;
}

int semaphoreAcquireK(lua_State* L, int) {
    return boolAfterWait(L, checkSemaphore(L)->waiters, 2);
}

// s:acquire(timeout: number?): boolean
int semaphoreAcquire(lua_State* L) {
    lua_settop(L, 2);
    Semaphore* sem = checkSemaphore(L);
    timeoutToDeadline(L, 2);

    if (sem->permits > 0) {
        --sem->permits;
        lua_pushboolean(L, 1);
        return 1;
    }
    const int r = sem->waiters.park(L, lua_tonumber(L, 2), semaphoreAcquireK);
    return r == sched::kInline ? semaphoreAcquireK(L, 0) : r;
}

// s:tryAcquire(): boolean
int semaphoreTryAcquire(lua_State* L) {
    Semaphore* sem = checkSemaphore(L);
    const bool got = sem->permits > 0;
    if (got) {
        --sem->permits;
    }
    lua_pushboolean(L, got);
    return 1;
}

// s:release()
int semaphoreRelease(lua_State* L) {
    Semaphore* sem = checkSemaphore(L);
    if (!sem->waiters.wakeOne(scheduler(L), kPushTrue)) {
        ++sem->permits; // nobody waiting: the permit goes back
    }
    return 0;
}

// s:available(): number
int semaphoreAvailable(lua_State* L) {
    lua_pushnumber(L, checkSemaphore(L)->permits);
    return 1;
}

// Event /////////////////////////////////////////////////////////////////////

struct Event {
    bool set = false;
    WaitQueue waiters;
};

Event* checkEvent(lua_State* L) {
    return checkObject<Event>(L, 1, kEventMeta);
}

// task.event(): Event
int taskEvent(lua_State* L) {
    newObject<Event>(L, kEventMeta);
    return 1;
}

int eventWaitK(lua_State* L, int) {
    return boolAfterWait(L, checkEvent(L)->waiters, 2);
}

// e:wait(timeout: number?): boolean
int eventWait(lua_State* L) {
    lua_settop(L, 2);
    Event* e = checkEvent(L);
    timeoutToDeadline(L, 2);

    if (e->set) {
        lua_pushboolean(L, 1);
        return 1;
    }
    const int r = e->waiters.park(L, lua_tonumber(L, 2), eventWaitK);
    return r == sched::kInline ? eventWaitK(L, 0) : r;
}

// e:set()
int eventSet(lua_State* L) {
    Event* e = checkEvent(L);
    e->set = true;
    e->waiters.wakeAll(scheduler(L), kPushTrue);
    return 0;
}

// e:reset()
int eventReset(lua_State* L) {
    checkEvent(L)->set = false;
    return 0;
}

// e:isSet(): boolean
int eventIsSet(lua_State* L) {
    lua_pushboolean(L, checkEvent(L)->set);
    return 1;
}

// Group /////////////////////////////////////////////////////////////////////
//
// The companion table lists the group's threads (the finished, successful ones
// are pruned now and then).

struct GroupState {
    std::size_t running = 0;
    std::size_t failures = 0;
    WaitQueue waiters;
};

struct Group {
    std::shared_ptr<GroupState> state = std::make_shared<GroupState>();
};

Group* checkGroup(lua_State* L) {
    return checkObject<Group>(L, 1, kGroupMeta);
}

// task.group(): Group
int taskGroup(lua_State* L) {
    newObject<Group>(L, kGroupMeta);
    return 1;
}

// Drops finished, successful threads from the list at "list" once it has grown.
void pruneGroup(lua_State* L, int list, const GroupState& state) {
    const int n = static_cast<int>(lua_objlen(L, list));
    if (static_cast<std::size_t>(n) < 2 * (state.running + state.failures) + 32) {
        return;
    }

    Scheduler& s = scheduler(L);
    int kept = 0;
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, list, i);
        lua_State* thread = lua_tothread(L, -1);
        if (s.status(L, thread) == Scheduler::Status::Done) {
            lua_pop(L, 1);
        } else {
            lua_rawseti(L, list, ++kept);
        }
    }
    for (int i = kept + 1; i <= n; ++i) {
        lua_pushnil(L);
        lua_rawseti(L, list, i);
    }
}

// g:spawn(fn, ...any): thread
int groupSpawn(lua_State* L) {
    Group* group = checkGroup(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    std::shared_ptr<GroupState> state = group->state;

    Scheduler& s = scheduler(L);
    lua_State* thread = s.spawn(L, 2); // on top

    pushCompanion(L, 1);
    const int list = lua_gettop(L);
    pruneGroup(L, list, *state);
    lua_pushvalue(L, -2);
    lua_rawseti(L, list, static_cast<int>(lua_objlen(L, list)) + 1);
    lua_pop(L, 1);

    ++state->running;
    const auto onFinish = [&s, state](bool failed) {
        --state->running;
        if (failed) {
            ++state->failures;
        }
        if (state->running == 0) {
            state->waiters.wakeAll(s, nullptr);
        }
    };
    if (!s.watch(L, thread, onFinish)) {
        onFinish(s.status(L, thread) != Scheduler::Status::Done);
    }
    return 1;
}

// Stack: 1 group, 2 deadline
int groupWaitFinish(lua_State* L) {
    Group* group = checkGroup(L);
    Scheduler& s = scheduler(L);
    if (s.timedOut(L)) {
        group->state->waiters.remove(L);
        lua_settop(L, 2);
        lua_pushnil(L);
        lua_pushstring(L, "timed out");
        return 2;
    }
    lua_settop(L, 2);

    pushCompanion(L, 1); // 3
    lua_pushboolean(L, group->state->failures == 0); // 4
    lua_createtable(L, 0, 0); // 5
    const int n = static_cast<int>(lua_objlen(L, 3));
    int errors = 0;
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        lua_State* thread = lua_tothread(L, -1);
        lua_pop(L, 1);
        const Scheduler::Status status = s.status(L, thread);
        if (status == Scheduler::Status::Error || status == Scheduler::Status::Cancelled) {
            const int count = s.pushOutcome(L, thread); // false, error
            lua_rawseti(L, 5, ++errors);
            lua_pop(L, count - 1);
        }
    }
    return 2;
}

int groupWaitK(lua_State* L, int) {
    return groupWaitFinish(L);
}

// g:wait(timeout: number?): (boolean, { any }) | (nil, string)
int groupWait(lua_State* L) {
    lua_settop(L, 2);
    Group* group = checkGroup(L);
    timeoutToDeadline(L, 2);

    if (group->state->running == 0) {
        return groupWaitFinish(L);
    }
    const int r = group->state->waiters.park(L, lua_tonumber(L, 2), groupWaitK);
    return r == sched::kInline ? groupWaitFinish(L) : r;
}

// g:cancel()
int groupCancel(lua_State* L) {
    checkGroup(L);
    Scheduler& s = scheduler(L);
    pushCompanion(L, 1);
    const int n = static_cast<int>(lua_objlen(L, -1));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        lua_State* thread = lua_tothread(L, -1);
        lua_pop(L, 1);
        const int status = lua_costatus(L, thread);
        if (!s.finished(L, thread) && status != LUA_CORUN && status != LUA_CONOR) {
            s.cancel(L, thread);
        }
    }
    return 0;
}

// g:count(): number
int groupCount(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(checkGroup(L)->state->running));
    return 1;
}

// registration //////////////////////////////////////////////////////////////

constexpr NativeFunction kFunctions[] = {
    {"spawn", taskSpawn},
    {"defer", taskDefer},
    {"delay", taskDelay},
    {"wait", taskWait},
    {"cancel", taskCancel},
    {"running", taskRunning},
    {"status", taskStatus},
    {"count", taskCount},
    {"join", taskJoin, taskJoinK},
    {"all", taskAll, taskAllK},
    {"race", taskRace, taskRaceK},
    {"timeout", taskTimeout, taskTimeoutK},
    {"onError", taskOnError},
    {"channel", taskChannel},
    {"mutex", taskMutex},
    {"semaphore", taskSemaphore},
    {"event", taskEvent},
    {"group", taskGroup},
};

constexpr NativeFunction kChannelMethods[] = {
    {"send", channelSend, channelSendK},
    {"recv", channelRecv, channelRecvK},
    {"trySend", channelTrySend},
    {"tryRecv", channelTryRecv},
    {"close", channelClose},
    {"isClosed", channelIsClosed},
    {"len", channelLen},
    {"capacity", channelCapacity},
};

constexpr NativeFunction kMutexMethods[] = {
    {"lock", mutexLock, mutexLockK},
    {"tryLock", mutexTryLock},
    {"unlock", mutexUnlock},
    {"isLocked", mutexIsLocked},
};

constexpr NativeFunction kSemaphoreMethods[] = {
    {"acquire", semaphoreAcquire, semaphoreAcquireK},
    {"tryAcquire", semaphoreTryAcquire},
    {"release", semaphoreRelease},
    {"available", semaphoreAvailable},
};

constexpr NativeFunction kEventMethods[] = {
    {"wait", eventWait, eventWaitK},
    {"set", eventSet},
    {"reset", eventReset},
    {"isSet", eventIsSet},
};

constexpr NativeFunction kGroupMethods[] = {
    {"spawn", groupSpawn},
    {"wait", groupWait, groupWaitK},
    {"cancel", groupCancel},
    {"count", groupCount},
};

// Creates the metatable "name" (once per VM) with "methods" as its __index.
// Leaves it on the stack.
template <std::size_t N>
void registerType(lua_State* L, const char* name, const NativeFunction (&methods)[N]) {
    if (luaL_newmetatable(L, name)) {
        lua_createtable(L, 0, static_cast<int>(N));
        setFunctions(L, methods);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, name + 7); // "sonata.task.Channel" -> "task.Channel"
        lua_setfield(L, -2, "__type");
        lua_pushstring(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
}

} // namespace

void openTask(lua_State* L) {
    Scheduler::of(L);

    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kCompanionsKey);

    registerType(L, kChannelMeta, kChannelMethods);
    lua_pushcfunction(L, channelLen, "Channel.__len");
    lua_setfield(L, -2, "__len");
    lua_pop(L, 1);
    registerType(L, kMutexMeta, kMutexMethods);
    lua_pop(L, 1);
    registerType(L, kSemaphoreMeta, kSemaphoreMethods);
    lua_pop(L, 1);
    registerType(L, kEventMeta, kEventMethods);
    lua_pop(L, 1);
    registerType(L, kGroupMeta, kGroupMethods);
    lua_pop(L, 1);
    luaL_newmetatable(L, kGatherMeta);
    lua_pop(L, 1);

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)));
    setFunctions(L, kFunctions);
}

} // namespace sonata::lib::libs
