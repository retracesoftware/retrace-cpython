#include "Python.h"

#include "internal/pycore_frame.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_retrace.h"
#if PY_VERSION_HEX < 0x030C0000
#  include "internal/pycore_runtime.h"
#endif
#include "structmember.h"

#include <stdint.h>

#if PY_VERSION_HEX < 0x030C0000
#  define RETRACE_HEAD_LOCK(runtime) \
    PyThread_acquire_lock((runtime)->interpreters.mutex, WAIT_LOCK)
#  define RETRACE_HEAD_UNLOCK(runtime) \
    PyThread_release_lock((runtime)->interpreters.mutex)
#else
#  define RETRACE_HEAD_LOCK(runtime) HEAD_LOCK(runtime)
#  define RETRACE_HEAD_UNLOCK(runtime) HEAD_UNLOCK(runtime)
#endif

static int
retrace_u64_from_object(PyObject *value, uint64_t *out)
{
    unsigned long long converted = PyLong_AsUnsignedLongLong(value);
    if (converted == (unsigned long long)-1 && PyErr_Occurred()) {
        return -1;
    }
    *out = (uint64_t)converted;
    return 0;
}

static PyObject *
retrace_tuple_from_u64s(const uint64_t *values, Py_ssize_t size)
{
    PyObject *tuple = PyTuple_New(size);
    if (tuple == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < size; index++) {
        PyObject *item = PyLong_FromUnsignedLongLong(
            (unsigned long long)values[index]);
        if (item == NULL) {
            Py_DECREF(tuple);
            return NULL;
        }
        PyTuple_SET_ITEM(tuple, index, item);
    }
    return tuple;
}

static PyObject *
retrace_tuple_from_sequence(PyObject *sequence, const char *message)
{
    PyObject *fast = PySequence_Fast(sequence, message);
    if (fast == NULL) {
        return NULL;
    }

    Py_ssize_t size = PySequence_Fast_GET_SIZE(fast);
    PyObject *tuple = PyTuple_New(size);
    if (tuple == NULL) {
        Py_DECREF(fast);
        return NULL;
    }

    PyObject **items = PySequence_Fast_ITEMS(fast);
    for (Py_ssize_t index = 0; index < size; index++) {
        uint64_t value;
        if (retrace_u64_from_object(items[index], &value) < 0) {
            Py_DECREF(tuple);
            Py_DECREF(fast);
            return NULL;
        }
        PyObject *item = PyLong_FromUnsignedLongLong(
            (unsigned long long)value);
        if (item == NULL) {
            Py_DECREF(tuple);
            Py_DECREF(fast);
            return NULL;
        }
        PyTuple_SET_ITEM(tuple, index, item);
    }

    Py_DECREF(fast);
    return tuple;
}

static int
retrace_frame_is_visible(_PyInterpreterFrame *frame)
{
    return _PyRetrace_FrameIsVisible(frame);
}

static uint64_t
retrace_frame_call_ordinal(_PyInterpreterFrame *frame)
{
    return _PyRetrace_FrameCallOrdinal(frame);
}

static uint64_t
retrace_frame_coordinate(_PyInterpreterFrame *frame)
{
    return _PyRetrace_FrameCoordinate(frame);
}

static Py_ssize_t
retrace_frame_depth(_PyInterpreterFrame *frame)
{
    if (frame->retrace.coordinate_depth !=
            _PyFrame_RETRACE_COORDINATE_DEPTH_UNSET)
    {
        return (Py_ssize_t)frame->retrace.coordinate_depth;
    }

    Py_ssize_t depth = 0;
    for (_PyInterpreterFrame *parent = frame->previous;
         parent != NULL;
         parent = parent->previous)
    {
        if (retrace_frame_is_visible(parent)) {
            depth = retrace_frame_depth(parent) + 1;
            break;
        }
    }
    frame->retrace.coordinate_depth = (uint32_t)depth;
    return depth;
}

static _PyInterpreterFrame *
retrace_thread_state_frame(PyThreadState *target_tstate)
{
    if (target_tstate->retrace.thread_callback_active) {
        return target_tstate->retrace.thread_callback_frame;
    }
    if (target_tstate->cframe == NULL) {
        return NULL;
    }
    return target_tstate->cframe->current_frame;
}

static int
retrace_thread_id_from_object(PyObject *value, uint64_t *thread_id)
{
    return retrace_u64_from_object(value, thread_id);
}

static PyThreadState *
retrace_find_thread_state(PyInterpreterState *interp, uint64_t thread_id)
{
    if (thread_id == 0) {
        return NULL;
    }
    for (PyThreadState *scan = interp->threads.head; scan != NULL; scan = scan->next) {
        if (scan->retrace.thread_id == thread_id) {
            return scan;
        }
    }
    return NULL;
}

static Py_ssize_t
retrace_frame_word_position(_PyInterpreterFrame *frame)
{
    return retrace_frame_depth(frame) * 2;
}

static Py_ssize_t
retrace_coordinate_count(_PyInterpreterFrame *frame)
{
    while (frame != NULL && !retrace_frame_is_visible(frame)) {
        frame = frame->previous;
    }
    if (frame == NULL) {
        return 0;
    }
    return (retrace_frame_depth(frame) + 1) * 2;
}

static void
retrace_fill_coordinates(uint64_t *coordinates,
                         _PyInterpreterFrame *frame,
                         Py_ssize_t drop)
{
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (retrace_frame_is_visible(scan)) {
            Py_ssize_t position = retrace_frame_word_position(scan);
            if (position >= drop) {
                coordinates[position - drop] =
                    retrace_frame_call_ordinal(scan);
            }
            position++;
            if (position >= drop) {
                coordinates[position - drop] =
                    retrace_frame_coordinate(scan);
            }
        }
    }
}

static PyObject *
retrace_thread_delta(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    _PyInterpreterFrame *frame = retrace_thread_state_frame(tstate);
    while (frame != NULL && !retrace_frame_is_visible(frame)) {
        frame = frame->previous;
    }

    Py_ssize_t current_size = retrace_coordinate_count(frame);
    Py_ssize_t common_count =
        tstate->retrace.last_root_coordinate == (uint64_t)-1 ?
        0 : current_size;

    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (!retrace_frame_is_visible(scan)) {
            continue;
        }

        uint64_t call_ordinal = retrace_frame_call_ordinal(scan);
        Py_ssize_t position = retrace_frame_word_position(scan);
        if (scan->retrace.last_call_ordinal != call_ordinal &&
            position < common_count)
        {
            common_count = position;
        }
        uint64_t coordinate = retrace_frame_coordinate(scan);
        position++;
        if (scan->retrace.last_coordinate != coordinate &&
            position < common_count)
        {
            common_count = position;
        }
    }

    Py_ssize_t size = 1 + current_size - common_count;
    uint64_t *delta = PyMem_New(uint64_t, size);
    if (delta == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    delta[0] = (uint64_t)common_count;

    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous)
    {
        if (!retrace_frame_is_visible(scan)) {
            continue;
        }

        uint64_t call_ordinal = retrace_frame_call_ordinal(scan);
        Py_ssize_t position = retrace_frame_word_position(scan);
        if (position >= common_count) {
            scan->retrace.last_call_ordinal = call_ordinal;
            delta[1 + position - common_count] = call_ordinal;
        }
        uint64_t coordinate = retrace_frame_coordinate(scan);
        position++;
        if (position >= common_count) {
            scan->retrace.last_coordinate = coordinate;
            delta[1 + position - common_count] = coordinate;
        }
    }
    tstate->retrace.last_root_coordinate = tstate->retrace.root_coordinate;

    PyObject *result = retrace_tuple_from_u64s(delta, size);
    PyMem_Free(delta);
    return result;
}

static void
retrace_clear_call_at(PyInterpreterState *interp)
{
    interp->retrace.call_at_armed = 0;
    interp->retrace.call_at_thread_id = 0;
    Py_CLEAR(interp->retrace.call_at_coordinates);
    Py_CLEAR(interp->retrace.call_at_callback);
    Py_CLEAR(interp->retrace.call_at_overshoot_callback);
}

static int
retrace_compare_target_word(PyObject *target,
                            Py_ssize_t target_size,
                            Py_ssize_t position,
                            uint64_t current,
                            int *comparison)
{
    if (*comparison != 0 || position >= target_size) {
        return 0;
    }

    uint64_t target_value;
    if (retrace_u64_from_object(
            PyTuple_GET_ITEM(target, position), &target_value) < 0)
    {
        return -1;
    }
    if (target_value < current) {
        *comparison = -1;
    }
    else if (target_value > current) {
        *comparison = 1;
    }
    return 0;
}

static int
retrace_compare_target_to_frame_walk(PyObject *target,
                                     Py_ssize_t target_size,
                                     _PyInterpreterFrame *frame,
                                     Py_ssize_t *position,
                                     int *comparison)
{
    frame = _PyRetrace_NearestVisibleFrame(frame);
    if (frame == NULL) {
        return 0;
    }
    if (retrace_compare_target_to_frame_walk(
            target, target_size, frame->previous, position, comparison) < 0)
    {
        return -1;
    }
    if (*comparison != 0) {
        return 0;
    }

    if (retrace_compare_target_word(
            target, target_size, *position,
            retrace_frame_call_ordinal(frame), comparison) < 0)
    {
        return -1;
    }
    (*position)++;
    if (*comparison != 0) {
        return 0;
    }

    if (retrace_compare_target_word(
            target, target_size, *position,
            retrace_frame_coordinate(frame), comparison) < 0)
    {
        return -1;
    }
    (*position)++;
    return 0;
}

static int
retrace_compare_target_to_frame(PyObject *target,
                                _PyInterpreterFrame *frame,
                                int *comparison)
{
    Py_ssize_t target_size = PyTuple_GET_SIZE(target);
    Py_ssize_t position = 0;
    *comparison = 0;
    if (retrace_compare_target_to_frame_walk(
            target, target_size, frame, &position, comparison) < 0)
    {
        return -1;
    }
    if (*comparison == 0) {
        if (target_size < position) {
            *comparison = -1;
        }
        else if (target_size > position) {
            *comparison = 1;
        }
    }
    return 0;
}

PyDoc_STRVAR(retrace_coordinates_doc,
"coordinates($module, thread_id=None, drop=0, /)\n"
"--\n"
"\n"
"Return a thread's visible Python frame coordinates.\n"
"\n"
"Each visible frame contributes (call_ordinal, instruction_coordinate),\n"
"ordered from oldest visible Python frame to current frame.\n"
"Frames entered by Retrace callbacks are transparent and omitted.\n"
"drop omits that many leading coordinates from the returned sequence.");

PyDoc_STRVAR(retrace_thread_delta_doc,
"thread_delta($module, /)\n"
"--\n"
"\n"
"Return [common_prefix_count, *new_suffix] for the current thread's visible\n"
"coordinates. Frames entered by Retrace callbacks are transparent and omitted.");

PyDoc_STRVAR(retrace_hash_doc,
"hash($module, /)\n"
"--\n"
"\n"
"Return the current thread's 64-bit visible coordinate-location hash.");

static PyObject *
retrace_hash(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    uint64_t hash = _PyRetrace_CurrentCoordinateHash(tstate);
    return PyLong_FromUnsignedLongLong((unsigned long long)hash);
}

static PyObject *
retrace_coordinates(PyObject *module, PyObject *args)
{
    PyThreadState *current_tstate = _PyThreadState_GET();
    PyObject *thread_id_arg = Py_None;
    uint64_t thread_id = 0;
    Py_ssize_t drop = 0;
    int explicit_thread_id = 0;

    if (!PyArg_ParseTuple(args, "|On:coordinates",
                          &thread_id_arg, &drop))
    {
        return NULL;
    }
    if (drop < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "coordinates drop must be non-negative");
        return NULL;
    }
    if (thread_id_arg != Py_None) {
        if (retrace_thread_id_from_object(thread_id_arg, &thread_id) < 0) {
            return NULL;
        }
        explicit_thread_id = 1;
    }

    _PyRuntimeState *runtime = &_PyRuntime;
    PyInterpreterState *interp = current_tstate->interp;
    uint64_t *coordinates = NULL;

    RETRACE_HEAD_LOCK(runtime);
    PyThreadState *target_tstate = explicit_thread_id ?
        retrace_find_thread_state(interp, thread_id) : current_tstate;
    if (target_tstate == NULL) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyErr_SetString(PyExc_LookupError, "unknown thread id");
        return NULL;
    }
    _PyInterpreterFrame *frame =
        retrace_thread_state_frame(target_tstate);
    Py_ssize_t coordinate_count = retrace_coordinate_count(frame);
    uint64_t root_coordinate = target_tstate->retrace.root_coordinate;
    Py_ssize_t size = coordinate_count > drop ? coordinate_count - drop : 0;
    RETRACE_HEAD_UNLOCK(runtime);

    if (size != 0) {
        coordinates = PyMem_New(uint64_t, size);
        if (coordinates == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
    }

    RETRACE_HEAD_LOCK(runtime);
    target_tstate = explicit_thread_id ?
        retrace_find_thread_state(interp, thread_id) : current_tstate;
    if (target_tstate == NULL) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyMem_Free(coordinates);
        PyErr_SetString(PyExc_LookupError, "unknown thread id");
        return NULL;
    }
    frame = retrace_thread_state_frame(target_tstate);
    if (retrace_coordinate_count(frame) != coordinate_count) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyMem_Free(coordinates);
        PyErr_SetString(PyExc_RuntimeError, "thread frame stack changed");
        return NULL;
    }
    if (target_tstate->retrace.root_coordinate != root_coordinate) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyMem_Free(coordinates);
        PyErr_SetString(PyExc_RuntimeError, "thread root coordinate changed");
        return NULL;
    }
    if (size != 0) {
        retrace_fill_coordinates(coordinates, frame, drop);
    }
    RETRACE_HEAD_UNLOCK(runtime);

    PyObject *result = retrace_tuple_from_u64s(coordinates, size);
    PyMem_Free(coordinates);
    return result;
}

typedef enum {
    RETRACE_CALL_EXCLUDE,
    RETRACE_CALL_INCLUDE,
} RetraceCallWrapperMode;

static PyObject *
retrace_call_wrapper_new(PyObject *callable, RetraceCallWrapperMode mode);

static PyObject *
retrace_callback_ref(PyObject *callback, const char *name, int allow_none)
{
    if (allow_none && callback == Py_None) {
        return NULL;
    }
    if (!PyCallable_Check(callback)) {
        PyErr_Format(PyExc_TypeError,
                     "%s callback must be callable%s",
                     name,
                     allow_none ? " or None" : "");
        return NULL;
    }
    return Py_NewRef(callback);
}

PyDoc_STRVAR(retrace_set_thread_yield_callback_doc,
"set_thread_yield_callback($module, callback, /)\n"
"--\n"
"\n"
"Set a Python callback invoked as callback() before the current thread yields\n"
"from the eval loop. Callback frames are coordinate-transparent.");

static PyObject *
retrace_set_thread_yield_callback(PyObject *module, PyObject *callback)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    PyObject *callback_ref =
        retrace_callback_ref(callback, "thread yield", 1);
    if (callback_ref == NULL && PyErr_Occurred()) {
        return NULL;
    }

    PyObject *old_callback = interp->retrace.thread_yield_callback;
    interp->retrace.thread_yield_callback = callback_ref;
    Py_XDECREF(old_callback);

    Py_RETURN_NONE;
}

PyDoc_STRVAR(retrace_get_thread_yield_callback_doc,
"get_thread_yield_callback($module, /)\n"
"--\n"
"\n"
"Return the active thread yield callback, or None.");

static PyObject *
retrace_get_thread_yield_callback(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *callback = tstate->interp->retrace.thread_yield_callback;
    if (callback == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(callback);
}

PyDoc_STRVAR(retrace_set_thread_start_callback_doc,
"set_thread_start_callback($module, callback, /)\n"
"--\n"
"\n"
"Set a Python callback invoked as callback() on a newly started thread before\n"
"its first Python frame runs. Callback frames are coordinate-transparent.");

static PyObject *
retrace_set_thread_start_callback(PyObject *module, PyObject *callback)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    PyObject *callback_ref =
        retrace_callback_ref(callback, "thread start", 1);
    if (callback_ref == NULL && PyErr_Occurred()) {
        return NULL;
    }

    PyObject *old_callback = interp->retrace.thread_start_callback;
    interp->retrace.thread_start_callback = callback_ref;
    Py_XDECREF(old_callback);

    Py_RETURN_NONE;
}

PyDoc_STRVAR(retrace_get_thread_start_callback_doc,
"get_thread_start_callback($module, /)\n"
"--\n"
"\n"
"Return the active thread start callback, or None.");

static PyObject *
retrace_get_thread_start_callback(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *callback = tstate->interp->retrace.thread_start_callback;
    if (callback == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(callback);
}

PyDoc_STRVAR(retrace_set_thread_resume_callback_doc,
"set_thread_resume_callback($module, callback, /)\n"
"--\n"
"\n"
"Set a Python callback invoked as callback() before application bytecode runs\n"
"after the current thread resumes from an eval-loop yield. Callback frames are\n"
"coordinate-transparent.");

static PyObject *
retrace_set_thread_resume_callback(PyObject *module, PyObject *callback)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    PyObject *callback_ref =
        retrace_callback_ref(callback, "thread resume", 1);
    if (callback_ref == NULL && PyErr_Occurred()) {
        return NULL;
    }

    PyObject *old_callback = interp->retrace.thread_resume_callback;
    interp->retrace.thread_resume_callback = callback_ref;
    Py_XDECREF(old_callback);

    Py_RETURN_NONE;
}

PyDoc_STRVAR(retrace_get_thread_resume_callback_doc,
"get_thread_resume_callback($module, /)\n"
"--\n"
"\n"
"Return the active thread resume callback, or None.");

static PyObject *
retrace_get_thread_resume_callback(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *callback = tstate->interp->retrace.thread_resume_callback;
    if (callback == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(callback);
}

typedef struct {
    PyObject_HEAD
    PyObject *callable;
    vectorcallfunc vectorcall;
    RetraceCallWrapperMode mode;
} RetraceCallWrapper;

typedef struct {
    uint32_t previous_depth;
    _PyInterpreterFrame *previous_parent;
} RetraceExcludeScope;

typedef struct {
    int active;
    uint32_t previous_depth;
    _PyInterpreterFrame *previous_exclude_parent;
    _PyInterpreterFrame *previous_visible_parent;
    int previous_callback_active;
    _PyInterpreterFrame *previous_callback_frame;
} RetraceIncludeScope;

static PyTypeObject retrace_call_wrapper_type;

static int
retrace_enter_excluded(PyThreadState *tstate,
                       int hide_current_frame,
                       RetraceExcludeScope *scope)
{
    scope->previous_depth = 0;
    scope->previous_parent = NULL;
    if (tstate == NULL) {
        return 0;
    }
    if (tstate->retrace.coordinate_exclude_depth == UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "Retrace coordinate exclusion depth overflow");
        return -1;
    }

    scope->previous_depth = tstate->retrace.coordinate_exclude_depth;
    scope->previous_parent = tstate->retrace.coordinate_exclude_parent_frame;
    if (scope->previous_depth == 0) {
        _PyInterpreterFrame *frame;
        if (tstate->retrace.thread_callback_active &&
            tstate->retrace.thread_callback_frame != NULL)
        {
            frame = tstate->retrace.thread_callback_frame;
        }
        else {
            frame = tstate->cframe == NULL ? NULL : tstate->cframe->current_frame;
        }
        if (hide_current_frame && frame != NULL) {
            _PyRetrace_MarkFrameCallbackTransparent(frame);
            frame = frame->previous;
        }
        tstate->retrace.coordinate_exclude_parent_frame =
            _PyRetrace_NearestVisibleFrame(frame);
    }
    tstate->retrace.coordinate_exclude_depth = scope->previous_depth + 1;
    return 0;
}

static void
retrace_leave_excluded(PyThreadState *tstate, RetraceExcludeScope *scope)
{
    if (tstate == NULL) {
        return;
    }
    tstate->retrace.coordinate_exclude_depth = scope->previous_depth;
    if (scope->previous_depth == 0) {
        tstate->retrace.coordinate_exclude_parent_frame =
            scope->previous_parent;
    }
}

static void
retrace_enter_included(PyThreadState *tstate, RetraceIncludeScope *scope)
{
    scope->active = 0;
    scope->previous_depth = 0;
    scope->previous_exclude_parent = NULL;
    scope->previous_visible_parent = NULL;
    scope->previous_callback_active = 0;
    scope->previous_callback_frame = NULL;
    if (tstate == NULL) {
        return;
    }
    if (tstate->retrace.coordinate_exclude_depth == 0 &&
        !tstate->retrace.thread_callback_active)
    {
        return;
    }

    _PyInterpreterFrame *visible_parent =
        tstate->retrace.coordinate_exclude_depth > 0 ?
        tstate->retrace.coordinate_exclude_parent_frame :
        tstate->retrace.thread_callback_frame;

    scope->active = 1;
    scope->previous_depth = tstate->retrace.coordinate_exclude_depth;
    scope->previous_exclude_parent =
        tstate->retrace.coordinate_exclude_parent_frame;
    scope->previous_visible_parent =
        tstate->retrace.thread_visible_callback_parent_frame;
    scope->previous_callback_active = tstate->retrace.thread_callback_active;
    scope->previous_callback_frame = tstate->retrace.thread_callback_frame;
    tstate->retrace.coordinate_exclude_depth = 0;
    tstate->retrace.thread_callback_active = 0;
    tstate->retrace.thread_callback_frame = NULL;
    tstate->retrace.thread_visible_callback_parent_frame = visible_parent;
}

static void
retrace_leave_included(PyThreadState *tstate, RetraceIncludeScope *scope)
{
    if (tstate == NULL || !scope->active) {
        return;
    }
    tstate->retrace.coordinate_exclude_depth = scope->previous_depth;
    tstate->retrace.coordinate_exclude_parent_frame =
        scope->previous_exclude_parent;
    tstate->retrace.thread_visible_callback_parent_frame =
        scope->previous_visible_parent;
    tstate->retrace.thread_callback_active =
        scope->previous_callback_active;
    tstate->retrace.thread_callback_frame =
        scope->previous_callback_frame;
}

static PyObject *
retrace_call_with_mode(RetraceCallWrapperMode mode,
                       PyObject *callable,
                       PyObject *const *args,
                       size_t nargsf,
                       PyObject *kwnames,
                       int hide_current_frame)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *result;

    if (mode == RETRACE_CALL_EXCLUDE) {
        RetraceExcludeScope scope;
        if (retrace_enter_excluded(tstate, hide_current_frame, &scope) < 0) {
            return NULL;
        }
        result = PyObject_Vectorcall(callable, args, nargsf, kwnames);
        retrace_leave_excluded(tstate, &scope);
        return result;
    }

    RetraceIncludeScope scope;
    retrace_enter_included(tstate, &scope);
    result = PyObject_Vectorcall(callable, args, nargsf, kwnames);
    retrace_leave_included(tstate, &scope);
    return result;
}

static PyObject *
retrace_call_wrapper_vectorcall(PyObject *callable,
                                PyObject *const *args,
                                size_t nargsf,
                                PyObject *kwnames)
{
    RetraceCallWrapper *self = (RetraceCallWrapper *)callable;
    return retrace_call_with_mode(
        self->mode, self->callable, args, nargsf, kwnames, 0);
}

static PyObject *
retrace_call_wrapper_new(PyObject *callable, RetraceCallWrapperMode mode)
{
    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError,
                        "Retrace coordinate wrapper argument must be callable");
        return NULL;
    }

    RetraceCallWrapper *self = PyObject_GC_New(
        RetraceCallWrapper, &retrace_call_wrapper_type);
    if (self == NULL) {
        return NULL;
    }
    self->callable = Py_NewRef(callable);
    self->vectorcall = retrace_call_wrapper_vectorcall;
    self->mode = mode;
    PyObject_GC_Track(self);
    return (PyObject *)self;
}

static int
retrace_call_wrapper_traverse(RetraceCallWrapper *self,
                              visitproc visit,
                              void *arg)
{
    Py_VISIT(self->callable);
    return 0;
}

static int
retrace_call_wrapper_clear(RetraceCallWrapper *self)
{
    Py_CLEAR(self->callable);
    return 0;
}

static void
retrace_call_wrapper_dealloc(RetraceCallWrapper *self)
{
    PyObject_GC_UnTrack(self);
    retrace_call_wrapper_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
retrace_call_wrapper_descr_get(PyObject *self_obj,
                               PyObject *obj,
                               PyObject *type)
{
    RetraceCallWrapper *self = (RetraceCallWrapper *)self_obj;
    descrgetfunc get = Py_TYPE(self->callable)->tp_descr_get;
    if (get == NULL || obj == NULL) {
        return Py_NewRef(self_obj);
    }

    PyObject *bound = get(self->callable, obj, type);
    if (bound == NULL) {
        return NULL;
    }
    PyObject *wrapped = retrace_call_wrapper_new(bound, self->mode);
    Py_DECREF(bound);
    return wrapped;
}

static PyObject *
retrace_call_wrapper_repr(PyObject *self_obj)
{
    RetraceCallWrapper *self = (RetraceCallWrapper *)self_obj;
    const char *name =
        self->mode == RETRACE_CALL_EXCLUDE ? "exclude" : "include";
    return PyUnicode_FromFormat("<_retrace.%s wrapper for %R>",
                                name, self->callable);
}

static PyMemberDef retrace_call_wrapper_members[] = {
    {"__wrapped__", T_OBJECT, offsetof(RetraceCallWrapper, callable),
     READONLY, PyDoc_STR("Wrapped callable.")},
    {NULL},
};

static PyTypeObject retrace_call_wrapper_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_retrace.CallWrapper",
    .tp_basicsize = sizeof(RetraceCallWrapper),
    .tp_dealloc = (destructor)retrace_call_wrapper_dealloc,
    .tp_vectorcall_offset = offsetof(RetraceCallWrapper, vectorcall),
    .tp_call = PyVectorcall_Call,
    .tp_repr = retrace_call_wrapper_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)retrace_call_wrapper_traverse,
    .tp_clear = (inquiry)retrace_call_wrapper_clear,
    .tp_descr_get = retrace_call_wrapper_descr_get,
    .tp_members = retrace_call_wrapper_members,
    .tp_free = PyObject_GC_Del,
};

PyDoc_STRVAR(retrace_exclude_doc,
"exclude($module, callable, /)\n"
"--\n"
"\n"
"Return a callable wrapper whose Python frames are omitted from Retrace\n"
"coordinates. Intended for use as a decorator.");

static PyObject *
retrace_exclude(PyObject *module, PyObject *callable)
{
    return retrace_call_wrapper_new(callable, RETRACE_CALL_EXCLUDE);
}

PyDoc_STRVAR(retrace_include_doc,
"include($module, callable, /)\n"
"--\n"
"\n"
"Return a callable wrapper that re-enters visible Retrace coordinate space\n"
"when called from an excluded region. Intended for use as a decorator.");

static PyObject *
retrace_include(PyObject *module, PyObject *callable)
{
    return retrace_call_wrapper_new(callable, RETRACE_CALL_INCLUDE);
}

PyDoc_STRVAR(retrace_run_transparent_doc,
"run_transparent($module, callable, /)\n"
"--\n"
"\n"
"Call callable while keeping its Python frames out of Retrace coordinates.");

static PyObject *
retrace_run_transparent(PyObject *module, PyObject *callable)
{
    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError,
                        "run_transparent argument must be callable");
        return NULL;
    }

    return retrace_call_with_mode(
        RETRACE_CALL_EXCLUDE, callable, NULL, 0, NULL, 1);
}

PyDoc_STRVAR(retrace_call_at_doc,
"call_at($module, thread_id, coordinates, callback,\n"
"        overshoot_callback=None, /)\n"
"--\n"
"\n"
"Invoke callback() when thread_id reaches the given coordinates.\n"
"\n"
"If overshoot_callback is supplied, invoke it when the thread passes the\n"
"coordinates without hitting them exactly. Coordinates already in the past\n"
"raise ValueError unless overshoot_callback is supplied.\n"
"\n"
"Callbacks run in a coordinate-transparent scope. Wrap application work with\n"
"include(callable) to run it visibly as a child of the target frame.\n"
"\n"
"Pass None as the only argument to clear the active call_at.");

static PyObject *
retrace_call_at(PyObject *module, PyObject *args)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    Py_ssize_t argc = PyTuple_GET_SIZE(args);
    if (argc == 1 && PyTuple_GET_ITEM(args, 0) == Py_None) {
        retrace_clear_call_at(interp);
        Py_RETURN_NONE;
    }
    if (argc != 3 && argc != 4) {
        PyErr_SetString(PyExc_TypeError,
                        "call_at expects None or "
                        "(thread_id, coordinates, callback[, "
                        "overshoot_callback])");
        return NULL;
    }

    uint64_t thread_id = 0;
    if (retrace_thread_id_from_object(PyTuple_GET_ITEM(args, 0), &thread_id) < 0) {
        return NULL;
    }
    if (thread_id == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "call_at thread id must be non-zero");
        return NULL;
    }

    PyObject *callback = PyTuple_GET_ITEM(args, 2);
    PyObject *callback_ref =
        retrace_callback_ref(callback, "call_at", 0);
    if (callback_ref == NULL) {
        return NULL;
    }

    PyObject *overshoot_callback = NULL;
    PyObject *overshoot_callback_ref = NULL;
    if (argc == 4) {
        overshoot_callback = PyTuple_GET_ITEM(args, 3);
        overshoot_callback_ref = retrace_callback_ref(
            overshoot_callback, "call_at overshoot", 1);
        if (overshoot_callback_ref == NULL && PyErr_Occurred()) {
            Py_DECREF(callback_ref);
            return NULL;
        }
    }

    PyObject *coordinates =
        retrace_tuple_from_sequence(PyTuple_GET_ITEM(args, 1),
                                    "call_at coordinates must be a sequence");
    if (coordinates == NULL) {
        Py_DECREF(callback_ref);
        Py_XDECREF(overshoot_callback_ref);
        return NULL;
    }
    Py_ssize_t coordinate_size = PyTuple_GET_SIZE(coordinates);
    if (coordinate_size <= 0 || (coordinate_size % 2) != 0) {
        Py_DECREF(coordinates);
        Py_DECREF(callback_ref);
        Py_XDECREF(overshoot_callback_ref);
        PyErr_SetString(PyExc_ValueError,
                        "call_at coordinates must include frame pairs");
        return NULL;
    }

    _PyRuntimeState *runtime = &_PyRuntime;
    RETRACE_HEAD_LOCK(runtime);
    PyThreadState *target_tstate = retrace_find_thread_state(interp, thread_id);
    if (target_tstate == NULL) {
        RETRACE_HEAD_UNLOCK(runtime);
        Py_DECREF(coordinates);
        Py_DECREF(callback_ref);
        Py_XDECREF(overshoot_callback_ref);
        PyErr_SetString(PyExc_LookupError, "unknown thread id");
        return NULL;
    }
    _PyInterpreterFrame *frame = retrace_thread_state_frame(target_tstate);
    int comparison = 0;
    int compare_status =
        retrace_compare_target_to_frame(coordinates, frame, &comparison);
    RETRACE_HEAD_UNLOCK(runtime);
    if (compare_status < 0) {
        Py_DECREF(coordinates);
        Py_DECREF(callback_ref);
        Py_XDECREF(overshoot_callback_ref);
        return NULL;
    }
    if (comparison < 0 && overshoot_callback_ref == NULL) {
        Py_DECREF(coordinates);
        Py_DECREF(callback_ref);
        PyErr_SetString(PyExc_ValueError,
                        "call_at coordinates are in the past");
        return NULL;
    }

    retrace_clear_call_at(interp);
    interp->retrace.call_at_thread_id = thread_id;
    interp->retrace.call_at_coordinates = (PyObject *)coordinates;
    interp->retrace.call_at_callback = callback_ref;
    interp->retrace.call_at_overshoot_callback =
        overshoot_callback_ref;
    interp->retrace.call_at_armed = 1;

    Py_RETURN_NONE;
}

typedef struct RetraceThreadHandoffEntry {
    uint64_t thread_id;
    int permit;
    int waiting;
    PyThread_type_lock gate;
    struct RetraceThreadHandoffEntry *next;
} RetraceThreadHandoffEntry;

typedef struct {
    PyObject_HEAD
    PyThread_type_lock lock;
    int closed;
    RetraceThreadHandoffEntry *entries;
} RetraceThreadHandoff;

static void
retrace_thread_handoff_free_entries(RetraceThreadHandoffEntry *entry)
{
    while (entry != NULL) {
        RetraceThreadHandoffEntry *next = entry->next;
        if (entry->gate != NULL) {
            PyThread_free_lock(entry->gate);
        }
        PyMem_Free(entry);
        entry = next;
    }
}

static RetraceThreadHandoffEntry *
retrace_thread_handoff_find_entry(RetraceThreadHandoff *self,
                                  uint64_t thread_id)
{
    for (RetraceThreadHandoffEntry *entry = self->entries;
         entry != NULL;
         entry = entry->next)
    {
        if (entry->thread_id == thread_id) {
            return entry;
        }
    }
    return NULL;
}

static RetraceThreadHandoffEntry *
retrace_thread_handoff_get_entry(RetraceThreadHandoff *self,
                                 uint64_t thread_id)
{
    RetraceThreadHandoffEntry *entry =
        retrace_thread_handoff_find_entry(self, thread_id);
    if (entry != NULL) {
        return entry;
    }

    entry = PyMem_Calloc(1, sizeof(*entry));
    if (entry == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    entry->gate = PyThread_allocate_lock();
    if (entry->gate == NULL) {
        PyMem_Free(entry);
        PyErr_NoMemory();
        return NULL;
    }
    if (!PyThread_acquire_lock(entry->gate, NOWAIT_LOCK)) {
        PyThread_free_lock(entry->gate);
        PyMem_Free(entry);
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to initialize thread handoff gate");
        return NULL;
    }
    entry->thread_id = thread_id;
    entry->next = self->entries;
    self->entries = entry;
    return entry;
}

static PyObject *
retrace_thread_handoff_new(PyTypeObject *type,
                           PyObject *args,
                           PyObject *kwargs)
{
    static char *keywords[] = {NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, ":ThreadHandoff", keywords))
    {
        return NULL;
    }

    RetraceThreadHandoff *self =
        (RetraceThreadHandoff *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->lock = PyThread_allocate_lock();
    if (self->lock == NULL) {
        Py_DECREF(self);
        PyErr_NoMemory();
        return NULL;
    }
    self->closed = 0;
    self->entries = NULL;
    return (PyObject *)self;
}

static void
retrace_thread_handoff_close_locked(RetraceThreadHandoff *self)
{
    if (self->closed) {
        return;
    }
    self->closed = 1;
    for (RetraceThreadHandoffEntry *entry = self->entries;
         entry != NULL;
         entry = entry->next)
    {
        if (entry->waiting && !entry->permit) {
            PyThread_release_lock(entry->gate);
        }
    }
}

static void
retrace_thread_handoff_wake_entry_locked(RetraceThreadHandoffEntry *entry)
{
    int wake_target = !entry->permit && entry->waiting;
    entry->permit = 1;
    if (wake_target) {
        PyThread_release_lock(entry->gate);
    }
}

static PyObject *
retrace_thread_handoff_close(PyObject *self_obj, PyObject *Py_UNUSED(ignored))
{
    RetraceThreadHandoff *self = (RetraceThreadHandoff *)self_obj;
    if (self->lock != NULL) {
        PyThread_acquire_lock(self->lock, WAIT_LOCK);
        retrace_thread_handoff_close_locked(self);
        PyThread_release_lock(self->lock);
    }
    Py_RETURN_NONE;
}

static PyObject *
retrace_thread_handoff_wake(PyObject *self_obj, PyObject *target_arg)
{
    uint64_t target_id = 0;
    if (retrace_thread_id_from_object(target_arg, &target_id) < 0) {
        return NULL;
    }
    if (target_id == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "thread handoff target id must be non-zero");
        return NULL;
    }

    RetraceThreadHandoff *self = (RetraceThreadHandoff *)self_obj;
    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    if (self->closed) {
        PyThread_release_lock(self->lock);
        PyErr_SetString(PyExc_RuntimeError, "thread handoff is closed");
        return NULL;
    }

    RetraceThreadHandoffEntry *target =
        retrace_thread_handoff_get_entry(self, target_id);
    if (target == NULL) {
        PyThread_release_lock(self->lock);
        return NULL;
    }

    retrace_thread_handoff_wake_entry_locked(target);
    PyThread_release_lock(self->lock);
    Py_RETURN_NONE;
}

static PyObject *
retrace_thread_handoff_call(PyObject *self_obj,
                            PyObject *args,
                            PyObject *kwargs)
{
    if (kwargs != NULL && PyDict_GET_SIZE(kwargs) != 0) {
        PyErr_SetString(PyExc_TypeError,
                        "thread handoff does not accept keyword arguments");
        return NULL;
    }

    PyObject *target_arg;
    if (!PyArg_UnpackTuple(args, "thread handoff", 1, 1, &target_arg)) {
        return NULL;
    }

    uint64_t target_id = 0;
    if (retrace_thread_id_from_object(target_arg, &target_id) < 0) {
        return NULL;
    }
    if (target_id == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "thread handoff target id must be non-zero");
        return NULL;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    uint64_t current_id = tstate == NULL ? 0 : tstate->retrace.thread_id;
    if (current_id == 0) {
        PyErr_SetString(PyExc_RuntimeError, "no Retrace thread id");
        return NULL;
    }

    RetraceThreadHandoff *self = (RetraceThreadHandoff *)self_obj;
    PyThread_acquire_lock(self->lock, WAIT_LOCK);
    if (self->closed) {
        PyThread_release_lock(self->lock);
        PyErr_SetString(PyExc_RuntimeError, "thread handoff is closed");
        return NULL;
    }
    if (target_id == current_id) {
        PyThread_release_lock(self->lock);
        Py_RETURN_NONE;
    }

    RetraceThreadHandoffEntry *target =
        retrace_thread_handoff_get_entry(self, target_id);
    RetraceThreadHandoffEntry *current =
        target == NULL ? NULL :
        retrace_thread_handoff_get_entry(self, current_id);
    if (target == NULL || current == NULL) {
        PyThread_release_lock(self->lock);
        return NULL;
    }

    retrace_thread_handoff_wake_entry_locked(target);

    if (current->permit) {
        current->permit = 0;
        current->waiting = 0;
        PyThread_release_lock(self->lock);
        Py_RETURN_NONE;
    }

    current->waiting = 1;
    while (!self->closed && !current->permit) {
        PyThread_type_lock gate = current->gate;
        PyThread_release_lock(self->lock);

        PyLockStatus status;
        Py_BEGIN_ALLOW_THREADS
        status = PyThread_acquire_lock_timed(gate, -1, 0);
        Py_END_ALLOW_THREADS

        if (status != PY_LOCK_ACQUIRED) {
            PyThread_acquire_lock(self->lock, WAIT_LOCK);
            current->waiting = 0;
            PyThread_release_lock(self->lock);
            PyErr_SetString(PyExc_RuntimeError,
                            "thread handoff wait failed");
            return NULL;
        }
        PyThread_acquire_lock(self->lock, WAIT_LOCK);
    }

    current->waiting = 0;
    current->permit = 0;
    PyThread_release_lock(self->lock);
    Py_RETURN_NONE;
}

static void
retrace_thread_handoff_dealloc(PyObject *self_obj)
{
    RetraceThreadHandoff *self = (RetraceThreadHandoff *)self_obj;
    if (self->lock != NULL) {
        PyThread_acquire_lock(self->lock, WAIT_LOCK);
        retrace_thread_handoff_close_locked(self);
        PyThread_release_lock(self->lock);
        PyThread_free_lock(self->lock);
    }
    retrace_thread_handoff_free_entries(self->entries);
    Py_TYPE(self)->tp_free(self_obj);
}

static PyMethodDef retrace_thread_handoff_methods[] = {
    {"close", retrace_thread_handoff_close, METH_NOARGS,
     PyDoc_STR("Wake sleeping threads and reject future handoffs.")},
    {"wake", retrace_thread_handoff_wake, METH_O,
     PyDoc_STR("Mark thread_id runnable without parking the current thread.")},
    {NULL, NULL, 0, NULL},
};

PyDoc_STRVAR(retrace_thread_handoff_doc,
"ThreadHandoff()\n"
"--\n"
"\n"
"Create a replay handoff gate.\n"
"\n"
"Calling handoff(thread_id) marks thread_id runnable, then parks the current\n"
"thread with the GIL released until another handoff marks it runnable.\n"
"Calling handoff.wake(thread_id) marks thread_id runnable without parking.");

static PyTypeObject retrace_thread_handoff_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_retrace.ThreadHandoff",
    .tp_basicsize = sizeof(RetraceThreadHandoff),
    .tp_dealloc = retrace_thread_handoff_dealloc,
    .tp_call = retrace_thread_handoff_call,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = retrace_thread_handoff_doc,
    .tp_methods = retrace_thread_handoff_methods,
    .tp_new = retrace_thread_handoff_new,
};

static int
retrace_call_transparent_call_at_callback(PyThreadState *tstate,
                                         _PyInterpreterFrame *frame,
                                         PyObject *callback)
{
    tstate->retrace.thread_callback_frame = frame;
    tstate->retrace.thread_callback_active = 1;
    PyObject *result = PyObject_CallNoArgs(callback);

    tstate->retrace.thread_callback_active = 0;
    tstate->retrace.thread_callback_frame = NULL;

    if (result == NULL) {
        return -1;
    }
    if (result != Py_None) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_TypeError,
                        "call_at callback must return None");
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static void
retrace_call_thread_callback(PyThreadState *tstate,
                             PyObject *callback,
                             _PyInterpreterFrame *frame,
                             int use_current_frame)
{
    if (callback == NULL || tstate->retrace.thread_callback_active) {
        return;
    }

    callback = Py_NewRef(callback);
#if PY_VERSION_HEX >= 0x030C0000
    PyObject *saved_exc = PyErr_GetRaisedException();
#else
    PyObject *saved_type = NULL;
    PyObject *saved_value = NULL;
    PyObject *saved_traceback = NULL;
    PyErr_Fetch(&saved_type, &saved_value, &saved_traceback);
#endif

    if (frame == NULL) {
        frame = tstate->retrace.thread_pending_callback_frame;
    }
    if (frame == NULL && use_current_frame) {
        frame = _PyRetrace_CurrentThreadFrame(tstate);
    }
    tstate->retrace.thread_callback_frame = frame;
    tstate->retrace.thread_callback_active = 1;

    PyObject *result = PyObject_CallNoArgs(callback);
    if (result == NULL) {
        PyErr_WriteUnraisable(callback);
    }
    else {
        Py_DECREF(result);
    }

    tstate->retrace.thread_callback_active = 0;
    tstate->retrace.thread_callback_frame = NULL;

    Py_DECREF(callback);
#if PY_VERSION_HEX >= 0x030C0000
    PyErr_SetRaisedException(saved_exc);
#else
    PyErr_Restore(saved_type, saved_value, saved_traceback);
#endif
}

void
_PyRetrace_NoteThreadResume(PyThreadState *tstate)
{
    if (tstate == NULL || tstate->retrace.thread_id == 0) {
        return;
    }

    PyInterpreterState *interp = tstate->interp;
    if (!tstate->retrace.thread_started) {
        tstate->retrace.thread_started = 1;
        if (interp->retrace.thread_start_callback != NULL) {
            tstate->retrace.thread_start_pending = 1;
        }
        return;
    }

    if (interp->retrace.thread_resume_callback == NULL) {
        return;
    }

    tstate->retrace.thread_resume_pending = 1;
}

void
_PyRetrace_DeliverThreadResumeCallback(PyThreadState *tstate,
                                       _PyInterpreterFrame *frame)
{
    if (tstate == NULL || tstate->retrace.thread_id == 0) {
        return;
    }
    if (tstate->retrace.thread_callback_active) {
        return;
    }

    PyInterpreterState *interp = tstate->interp;
    if (tstate->retrace.thread_start_pending) {
        PyObject *callback = interp->retrace.thread_start_callback;
        tstate->retrace.thread_start_pending = 0;
        if (callback == NULL) {
            tstate->retrace.thread_pending_callback_frame = NULL;
            return;
        }
        retrace_call_thread_callback(tstate, callback, NULL, 0);
        tstate->retrace.thread_pending_callback_frame = NULL;
        return;
    }

    if (!tstate->retrace.thread_resume_pending) {
        return;
    }

    PyObject *callback = interp->retrace.thread_resume_callback;
    if (callback == NULL) {
        tstate->retrace.thread_resume_pending = 0;
        tstate->retrace.thread_pending_callback_frame = NULL;
        return;
    }

    if (frame == NULL) {
        frame = tstate->retrace.thread_pending_callback_frame;
    }
    if (frame == NULL) {
        frame = _PyRetrace_CurrentThreadFrame(tstate);
    }

    tstate->retrace.thread_resume_pending = 0;
    retrace_call_thread_callback(tstate, callback, frame, 1);
    tstate->retrace.thread_pending_callback_frame = NULL;
}

void
_PyRetrace_DeliverThreadYieldCallback(PyThreadState *tstate,
                                      _PyInterpreterFrame *frame)
{
    if (tstate == NULL || tstate->retrace.thread_id == 0 ||
        tstate->retrace.thread_callback_active)
    {
        return;
    }

    PyObject *callback = tstate->interp->retrace.thread_yield_callback;
    if (callback == NULL) {
        return;
    }

    retrace_call_thread_callback(tstate, callback, frame, 1);
}

int
_PyRetrace_CheckCallAt(PyThreadState *tstate,
                                 _PyInterpreterFrame *frame)
{
    if (tstate == NULL || tstate->retrace.thread_id == 0 ||
        frame == NULL)
    {
        return 0;
    }
    if (tstate->retrace.thread_callback_active) {
        return 0;
    }
    if (tstate->retrace.coordinate_exclude_depth > 0) {
        return 0;
    }

    PyInterpreterState *interp = tstate->interp;
    if (!interp->retrace.call_at_armed ||
        interp->retrace.call_at_thread_id != tstate->retrace.thread_id)
    {
        return 0;
    }
    if (interp->retrace.call_at_coordinates == NULL) {
        retrace_clear_call_at(interp);
        return 0;
    }

    int comparison = 0;
    if (retrace_compare_target_to_frame(
            interp->retrace.call_at_coordinates,
            frame,
            &comparison) < 0)
    {
        PyErr_Clear();
        return 0;
    }
    if (comparison > 0) {
        return 0;
    }

    PyObject *callback = comparison == 0 ?
        interp->retrace.call_at_callback :
        interp->retrace.call_at_overshoot_callback;
    if (callback == NULL) {
        retrace_clear_call_at(interp);
        return 0;
    }
    callback = Py_NewRef(callback);
    retrace_clear_call_at(interp);

    int result =
        retrace_call_transparent_call_at_callback(tstate, frame, callback);
    Py_DECREF(callback);
    return result;
}


static PyMethodDef retrace_methods[] = {
    {"coordinates", retrace_coordinates, METH_VARARGS,
     retrace_coordinates_doc},
    {"thread_delta", retrace_thread_delta,
     METH_NOARGS, retrace_thread_delta_doc},
    {"hash", retrace_hash, METH_NOARGS, retrace_hash_doc},
    {"set_thread_start_callback", retrace_set_thread_start_callback, METH_O,
     retrace_set_thread_start_callback_doc},
    {"get_thread_start_callback", retrace_get_thread_start_callback,
     METH_NOARGS, retrace_get_thread_start_callback_doc},
    {"set_thread_yield_callback", retrace_set_thread_yield_callback, METH_O,
     retrace_set_thread_yield_callback_doc},
    {"get_thread_yield_callback", retrace_get_thread_yield_callback,
     METH_NOARGS, retrace_get_thread_yield_callback_doc},
    {"set_thread_resume_callback", retrace_set_thread_resume_callback, METH_O,
     retrace_set_thread_resume_callback_doc},
    {"get_thread_resume_callback", retrace_get_thread_resume_callback,
     METH_NOARGS, retrace_get_thread_resume_callback_doc},
    {"exclude", retrace_exclude, METH_O, retrace_exclude_doc},
    {"include", retrace_include, METH_O, retrace_include_doc},
    {"run_transparent", retrace_run_transparent, METH_O,
     retrace_run_transparent_doc},
    {"call_at", retrace_call_at, METH_VARARGS,
     retrace_call_at_doc},
    {NULL, NULL, 0, NULL},
};

static int
retrace_exec(PyObject *module)
{
    if (PyType_Ready(&retrace_call_wrapper_type) < 0) {
        return -1;
    }
    if (PyType_Ready(&retrace_thread_handoff_type) < 0) {
        return -1;
    }
    Py_INCREF(&retrace_thread_handoff_type);
    if (PyModule_AddObject(
            module,
            "ThreadHandoff",
            (PyObject *)&retrace_thread_handoff_type) < 0)
    {
        Py_DECREF(&retrace_thread_handoff_type);
        return -1;
    }
    return 0;
}

static PyModuleDef_Slot retrace_slots[] = {
    {Py_mod_exec, retrace_exec},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
    {0, NULL},
};

static struct PyModuleDef retracemodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_retrace",
    .m_doc = "Retrace CPython probe support.",
    .m_size = 0,
    .m_methods = retrace_methods,
    .m_slots = retrace_slots,
};

PyMODINIT_FUNC
PyInit__retrace(void)
{
    return PyModuleDef_Init(&retracemodule);
}
