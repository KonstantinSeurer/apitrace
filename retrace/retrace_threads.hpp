#pragma once

#include <cassert>
#include <functional>
#include <iostream>
#include <vector>

#include "os_thread.hpp"

namespace retrace {

class RelayRunner;

struct Baton {
    void *data;
    uint32_t thread_id;
};


/**
 * Implement multi-threading by mimicking a relay race.
 */
class RelayRace
{
private:
    /**
     * Runners indexed by the leg they run (i.e, the thread_ids from the
     * trace).
     */
    std::vector<RelayRunner*> runners;

public:
    RelayRace();

    ~RelayRace();

    RelayRunner *
    getRunner(unsigned leg);

    inline RelayRunner *
    getForeRunner() {
        return getRunner(0);
    }

    void
    run(void);

    void
    passBaton(Baton next);

    void
    finishLine();

    void
    stopRunners();

    std::function<void(Baton &)> get_next_baton;

    std::function<void(Baton)> run_baton;

    std::function<void()> flush;
};

/**
 * Each runner is a thread.
 *
 * The fore runner doesn't have its own thread, but instead uses the thread
 * where the race started.
 */
class RelayRunner
{
private:
    friend class RelayRace;

    RelayRace *race;

    unsigned leg;

    std::mutex mutex;
    std::condition_variable wake_cond;

    /**
     * There are protected by the mutex.
     */
    bool finished;
    Baton baton;

    std::thread thread;

    static void
    runnerThread(RelayRunner *_this);

public:
    RelayRunner(RelayRace *race, unsigned _leg);

    ~RelayRunner();

    /**
     * Thread main loop.
     */
    void runRace(void);

    /**
     * Interpret successive calls.
     */
    void runLeg(Baton baton);

    /**
     * Called by other threads when relinquishing the baton.
     */
    void receiveBaton(Baton baton);

    /**
     * Called by the fore runner when the race is over.
     */
    void finishRace();
};

}
