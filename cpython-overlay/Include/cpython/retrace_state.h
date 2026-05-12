#ifndef Py_CPYTHON_RETRACE_STATE_H
#define Py_CPYTHON_RETRACE_STATE_H

#include <stdint.h>

#define _PyFrame_RETRACE_LAST_COORDINATE_UNSET UINT64_MAX
#define _PyFrame_RETRACE_CALL_ORDINAL_TRANSPARENT UINT64_MAX
#define _PyFrame_RETRACE_LAST_CALL_ORDINAL_UNSET (UINT64_MAX - UINT64_C(1))
#define _PyFrame_RETRACE_CALL_ORDINAL_FRESH_PARENT (UINT64_MAX - UINT64_C(2))
#define _PyFrame_RETRACE_COORDINATE_DEPTH_UNSET UINT32_MAX
#define _PyFrame_RETRACE_COORDINATE_HASH_UNSET UINT64_MAX
#define _PyFrame_RETRACE_CHILD_ORDINAL_COORDINATE_UNSET INT64_MIN

typedef struct _object PyObject;
typedef struct _ts PyThreadState;
typedef struct _PyRetraceIdentityHashTable _PyRetraceIdentityHashTable;
struct _PyInterpreterFrame;

typedef struct {
    uint32_t coordinate_depth;
    int64_t coordinate_bias;
    uint64_t current_call_ordinal;
    uint64_t next_child_call_ordinal;
    int64_t child_ordinal_coordinate;
    uint64_t last_call_ordinal;
    uint64_t last_coordinate;
    uint64_t coordinate_hash;
} _PyRetraceFrameState;

static inline void
_PyRetraceFrameState_Init(_PyRetraceFrameState *state)
{
    state->coordinate_depth = _PyFrame_RETRACE_COORDINATE_DEPTH_UNSET;
    state->coordinate_bias = 0;
    state->current_call_ordinal = 0;
    state->next_child_call_ordinal = 0;
    state->child_ordinal_coordinate =
        _PyFrame_RETRACE_CHILD_ORDINAL_COORDINATE_UNSET;
    state->last_call_ordinal = _PyFrame_RETRACE_LAST_CALL_ORDINAL_UNSET;
    state->last_coordinate = _PyFrame_RETRACE_LAST_COORDINATE_UNSET;
    state->coordinate_hash = _PyFrame_RETRACE_COORDINATE_HASH_UNSET;
}

typedef struct {
    int thread_started;
    int thread_start_pending;
    int thread_resume_pending;
    uint64_t thread_id;
    unsigned long cpython_thread_ident;
    uint64_t root_coordinate;
    uint64_t last_root_coordinate;
    uint64_t root_coordinate_hash;
    int thread_callback_active;
    struct _PyInterpreterFrame *thread_callback_frame;
    struct _PyInterpreterFrame *thread_pending_callback_frame;
    struct _PyInterpreterFrame *thread_visible_callback_parent_frame;
    uint32_t coordinate_exclude_depth;
    struct _PyInterpreterFrame *coordinate_exclude_parent_frame;
} _PyRetraceThreadState;

typedef struct {
    _PyRetraceIdentityHashTable *identity_hashes;
    PyObject *thread_start_callback;
    PyObject *thread_yield_callback;
    PyObject *thread_resume_callback;
    int call_at_armed;
    uint64_t call_at_thread_id;
    PyObject *call_at_coordinates;
    PyObject *call_at_callback;
    PyObject *call_at_overshoot_callback;
} _PyRetraceInterpreterState;

#endif /* !Py_CPYTHON_RETRACE_STATE_H */
