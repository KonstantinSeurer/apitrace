
#ifndef RETRACE_LIBRARY_HPP
#define RETRACE_LIBRARY_HPP

#include <cstdint>

#include "trace_model.hpp"

typedef void (*run_api_calls_cb)(uintptr_t data);

struct replay_sequence {
    run_api_calls_cb run_api;
    trace::Call *call;
};

typedef void *(*get_proc_addr_cb)(const char *procName);

typedef void (*resize_window_cb)(int width, int height);

struct replay_args {
    /* For GL */
    get_proc_addr_cb get_public_proc_addr;
    get_proc_addr_cb get_private_proc_addr;

    /* For WSI */
    resize_window_cb resize_window;
};

extern "C" {

void get_replay_sequences(const replay_sequence **sequences,
                          uint32_t *sequence_count,
                          const replay_args *_args);

}

typedef void(*get_replay_sequences_cb)(const replay_sequence **sequences,
                                       uint32_t *sequence_count,
                                       const replay_args *_args);

/* Set by get_replay_sequences */
extern replay_args args;

#endif
