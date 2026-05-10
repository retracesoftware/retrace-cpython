#ifndef Py_CPYTHON_RETRACE_STATE_H
#define Py_CPYTHON_RETRACE_STATE_H

#include <stdint.h>

#define _PyFrame_RETRACE_LAST_COORDINATE_UNSET UINT64_MAX
#define _PyFrame_RETRACE_COORDINATE_DEPTH_UNSET UINT32_MAX
#define _PyFrame_RETRACE_COORDINATE_HASH_UNSET UINT64_MAX

typedef struct _object PyObject;
typedef struct _ts PyThreadState;
typedef struct _PyRetraceIdentityHashTable _PyRetraceIdentityHashTable;
struct _PyInterpreterFrame;

typedef struct {
    uint32_t coordinate_depth;
    int64_t coordinate_bias;
    uint64_t last_coordinate;
    uint64_t coordinate_hash;
} _PyRetraceFrameState;

typedef struct {
    int thread_resume_pending;
    uint64_t thread_id;
    unsigned long cpython_thread_ident;
    uint64_t root_coordinate;
    uint64_t last_root_coordinate;
    uint64_t root_coordinate_hash;
    int thread_callback_active;
} _PyRetraceThreadState;

typedef struct {
    _PyRetraceIdentityHashTable *identity_hashes;
    PyObject *thread_yield_callback;
    PyObject *thread_resume_callback;
    int replay_checkpoint_armed;
    uint64_t replay_checkpoint_thread_id;
    uint64_t replay_checkpoint_top;
    PyObject *replay_checkpoint_coordinates;
    PyObject *replay_checkpoint_callback;
    int replay_checkpoint_callback_active;
} _PyRetraceInterpreterState;

#endif /* !Py_CPYTHON_RETRACE_STATE_H */
