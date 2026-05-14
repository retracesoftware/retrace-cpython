#ifndef Py_CPYTHON_RETRACE_STATE_H
#define Py_CPYTHON_RETRACE_STATE_H

#include <stdint.h>

#define _PyFrame_RETRACE_LAST_INSTRUCTION_COUNTER_UNSET UINT64_MAX
#define _PyFrame_RETRACE_LAST_CALL_ORDINAL_UNSET (UINT64_MAX - UINT64_C(1))
#define _PyFrame_RETRACE_COORDINATE_HASH_UNSET UINT64_MAX

#define _PyFrame_RETRACE_SPACE_ID_ROOT UINT32_C(0)
#define _PyFrame_RETRACE_SPACE_ID_DISABLED UINT32_C(1)

typedef struct _object PyObject;
typedef struct _ts PyThreadState;
typedef struct _PyRetraceIdentityHashTable _PyRetraceIdentityHashTable;
typedef struct _PyRetraceThreadSpaceState _PyRetraceThreadSpaceState;
struct _PyInterpreterFrame;

typedef struct {
    uint64_t instruction_counter;
    uint64_t current_call_ordinal;
    uint64_t *previous_call_ordinal_ptr;
    _PyRetraceThreadSpaceState *space;
    uint64_t last_call_ordinal;
    uint64_t last_instruction_counter;
    _PyRetraceThreadSpaceState *last_delta_space;
    uint64_t coordinate_hash;
    unsigned char instruction_active;
    unsigned char instruction_may_activate_python;
} _PyRetraceFrameState;

static inline void
_PyRetraceFrameState_Init(_PyRetraceFrameState *state)
{
    state->instruction_counter = 0;
    state->current_call_ordinal = 0;
    state->previous_call_ordinal_ptr = NULL;
    state->space = NULL;
    state->last_call_ordinal = _PyFrame_RETRACE_LAST_CALL_ORDINAL_UNSET;
    state->last_instruction_counter =
        _PyFrame_RETRACE_LAST_INSTRUCTION_COUNTER_UNSET;
    state->last_delta_space = NULL;
    state->coordinate_hash = _PyFrame_RETRACE_COORDINATE_HASH_UNSET;
    state->instruction_active = 0;
    state->instruction_may_activate_python = 0;
}

struct _PyRetraceThreadSpaceState {
    uint32_t space_id;
    int seen;
    uint32_t depth;
    uint64_t root_call_ordinal;
    uint64_t last_delta_root_call_ordinal;
    uint64_t root_coordinate_hash;
    _PyRetraceThreadSpaceState *next;
};

typedef struct {
    int thread_started;
    int thread_start_pending;
    int thread_resume_pending;
    uint64_t thread_id;
    unsigned long cpython_thread_ident;
    _PyRetraceThreadSpaceState root_space;
    _PyRetraceThreadSpaceState *current_space;
    _PyRetraceThreadSpaceState *last_space;
    uint64_t *call_ordinal_ptr;
    uint32_t inherited_space_id;
    int thread_callback_active;
    struct _PyInterpreterFrame *thread_callback_frame;
    struct _PyInterpreterFrame *thread_pending_callback_frame;
} _PyRetraceThreadState;

typedef struct {
    _PyRetraceIdentityHashTable *identity_hashes;
    PyObject *thread_start_callback;
    PyObject *thread_yield_callback;
    PyObject *thread_resume_callback;
    int call_at_armed;
    uint64_t call_at_thread_id;
    uint32_t call_at_space_id;
    PyObject *call_at_coordinates;
    PyObject *call_at_callback;
    PyObject *call_at_overshoot_callback;
} _PyRetraceInterpreterState;

#endif /* !Py_CPYTHON_RETRACE_STATE_H */
