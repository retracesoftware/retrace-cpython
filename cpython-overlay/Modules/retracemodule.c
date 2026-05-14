#include "Python.h"

#include "internal/pycore_frame.h"
#if PY_VERSION_HEX >= 0x030D0000
#  include "internal/pycore_pyerrors.h"
#endif
#include "internal/pycore_pystate.h"
#include "internal/pycore_retrace.h"
#if PY_VERSION_HEX >= 0x030D0000
#  include "internal/pycore_time.h"
#endif
#if PY_VERSION_HEX < 0x030C0000
#  include "internal/pycore_runtime.h"
#endif
#include "structmember.h"

#include <stdint.h>

#if PY_VERSION_HEX >= 0x030D0000
typedef PyTime_t RetracePyTime;
#else
typedef _PyTime_t RetracePyTime;
#endif

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

static _PyInterpreterFrame *
retrace_thread_state_frame(PyThreadState *target_tstate,
                           _PyRetraceThreadSpaceState *space)
{
    if (target_tstate->retrace.thread_callback_active &&
        space != NULL &&
        space->space_id == _PyFrame_RETRACE_SPACE_ID_ROOT &&
        (target_tstate->retrace.current_space == NULL ||
         target_tstate->retrace.current_space->space_id !=
            _PyFrame_RETRACE_SPACE_ID_ROOT))
    {
        return target_tstate->retrace.thread_callback_frame;
    }
#if PY_VERSION_HEX >= 0x030D0000
    return _PyRetrace_PyThreadStateCurrentFrame(target_tstate);
#else
    if (target_tstate->cframe == NULL) {
        return NULL;
    }
    return target_tstate->cframe->current_frame;
#endif
}

static int
retrace_space_id_from_object(PyObject *value, uint32_t *space_id)
{
    if (value == Py_None) {
        *space_id = _PyFrame_RETRACE_SPACE_ID_ROOT;
        return 0;
    }
    unsigned long long converted = PyLong_AsUnsignedLongLong(value);
    if (converted == (unsigned long long)-1 && PyErr_Occurred()) {
        return -1;
    }
    if (converted > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "Retrace space id is too large");
        return -1;
    }
    *space_id = (uint32_t)converted;
    return 0;
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

static _PyRetraceThreadSpaceState *
retrace_find_space(PyThreadState *tstate, uint32_t space_id)
{
    return _PyRetrace_FindThreadSpace(tstate, space_id);
}

static Py_ssize_t
retrace_coordinate_count(_PyInterpreterFrame *frame,
                         _PyRetraceThreadSpaceState *space)
{
    Py_ssize_t count = 0;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (_PyRetrace_FrameIsVisible(scan) && scan->retrace.space == space) {
            count += 2;
        }
    }
    return count;
}

static void
retrace_fill_coordinates(uint64_t *coordinates,
                         Py_ssize_t coordinate_count,
                         _PyInterpreterFrame *frame,
                         _PyRetraceThreadSpaceState *space)
{
    Py_ssize_t position = coordinate_count;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (_PyRetrace_FrameIsVisible(scan) && scan->retrace.space == space) {
            position -= 2;
            coordinates[position] = _PyRetrace_FrameCoordinate(scan);
            coordinates[position + 1] = _PyRetrace_FrameCallOrdinal(scan);
        }
    }
}

static PyObject *
retrace_thread_delta(PyObject *module, PyObject *args)
{
    PyObject *space_id_arg = Py_None;
    if (!PyArg_ParseTuple(args, "|O:thread_delta", &space_id_arg)) {
        return NULL;
    }
    uint32_t space_id;
    if (retrace_space_id_from_object(space_id_arg, &space_id) < 0) {
        return NULL;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    _PyRetraceThreadSpaceState *space = retrace_find_space(tstate, space_id);
    if (space == NULL || !space->seen) {
        Py_RETURN_NONE;
    }
    _PyInterpreterFrame *frame = retrace_thread_state_frame(tstate, space);

    Py_ssize_t current_size = retrace_coordinate_count(frame, space);
    Py_ssize_t common_count =
        space->last_delta_root_call_ordinal ==
            _PyFrame_RETRACE_LAST_CALL_ORDINAL_UNSET ||
        space->last_delta_root_call_ordinal != space->root_call_ordinal ?
        0 : current_size;

    Py_ssize_t position = current_size;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (!_PyRetrace_FrameIsVisible(scan) || scan->retrace.space != space) {
            continue;
        }

        position -= 2;
        uint64_t instruction_counter = _PyRetrace_FrameCoordinate(scan);
        if ((scan->retrace.last_delta_space != space ||
             scan->retrace.last_instruction_counter != instruction_counter) &&
            position < common_count)
        {
            common_count = position;
        }
        uint64_t call_ordinal = _PyRetrace_FrameCallOrdinal(scan);
        if ((scan->retrace.last_delta_space != space ||
             scan->retrace.last_call_ordinal != call_ordinal) &&
            position + 1 < common_count)
        {
            common_count = position + 1;
        }
    }

    Py_ssize_t size = 1 + current_size - common_count;
    uint64_t *delta = PyMem_New(uint64_t, size);
    if (delta == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    delta[0] = (uint64_t)common_count;

    position = current_size;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous)
    {
        if (!_PyRetrace_FrameIsVisible(scan) || scan->retrace.space != space) {
            continue;
        }

        position -= 2;
        uint64_t instruction_counter = _PyRetrace_FrameCoordinate(scan);
        if (position >= common_count) {
            delta[1 + position - common_count] = instruction_counter;
        }
        uint64_t call_ordinal = _PyRetrace_FrameCallOrdinal(scan);
        if (position + 1 >= common_count) {
            delta[1 + position + 1 - common_count] = call_ordinal;
        }
        scan->retrace.last_delta_space = space;
        scan->retrace.last_instruction_counter = instruction_counter;
        scan->retrace.last_call_ordinal = call_ordinal;
    }
    space->last_delta_root_call_ordinal = space->root_call_ordinal;

    PyObject *result = retrace_tuple_from_u64s(delta, size);
    PyMem_Free(delta);
    return result;
}

static void
retrace_clear_call_at(PyInterpreterState *interp)
{
    interp->retrace.call_at_armed = 0;
    interp->retrace.call_at_thread_id = 0;
    interp->retrace.call_at_space_id = _PyFrame_RETRACE_SPACE_ID_ROOT;
    Py_CLEAR(interp->retrace.call_at_coordinates);
    Py_CLEAR(interp->retrace.call_at_callback);
    Py_CLEAR(interp->retrace.call_at_overshoot_callback);
}

static int
retrace_compare_target_to_frame(PyObject *target,
                                _PyInterpreterFrame *frame,
                                _PyRetraceThreadSpaceState *space,
                                int *comparison)
{
    Py_ssize_t target_size = PyTuple_GET_SIZE(target);
    Py_ssize_t current_size = retrace_coordinate_count(frame, space);
    uint64_t *current = PyMem_New(uint64_t, current_size);
    if (current == NULL && current_size != 0) {
        PyErr_NoMemory();
        return -1;
    }
    if (current_size != 0) {
        retrace_fill_coordinates(current, current_size, frame, space);
    }

    *comparison = 0;
    Py_ssize_t common = target_size < current_size ?
        target_size : current_size;
    for (Py_ssize_t position = 0; position < common; position++) {
        uint64_t target_value;
        if (retrace_u64_from_object(
                PyTuple_GET_ITEM(target, position), &target_value) < 0)
        {
            PyMem_Free(current);
            return -1;
        }
        if (target_value < current[position]) {
            *comparison = -1;
            break;
        }
        else if (target_value > current[position]) {
            *comparison = 1;
            break;
        }
    }
    if (*comparison == 0) {
        if (target_size < current_size) {
            *comparison = -1;
        }
        else if (target_size > current_size) {
            *comparison = 1;
        }
    }
    PyMem_Free(current);
    return 0;
}

PyDoc_STRVAR(retrace_coordinates_doc,
"coordinates($module, thread_id=None, drop=0, space_id=None, /)\n"
"--\n"
"\n"
"Return a thread's visible Python frame coordinates.\n"
"\n"
"Each frame in the selected coordinate space contributes\n"
"(instruction_counter, call_ordinal), ordered from oldest frame to current.\n"
"drop omits that many leading coordinates from the returned sequence.");

PyDoc_STRVAR(retrace_thread_delta_doc,
"thread_delta($module, space_id=None, /)\n"
"--\n"
"\n"
"Return [common_prefix_count, *new_suffix] for the current thread's visible\n"
"coordinates in the selected coordinate space.");

PyDoc_STRVAR(retrace_hash_doc,
"hash($module, space_id=None, /)\n"
"--\n"
"\n"
"Return the current thread's 64-bit visible coordinate-location hash.");

static PyObject *
retrace_hash(PyObject *module, PyObject *args)
{
    PyObject *space_id_arg = Py_None;
    if (!PyArg_ParseTuple(args, "|O:hash", &space_id_arg)) {
        return NULL;
    }
    uint32_t space_id;
    if (retrace_space_id_from_object(space_id_arg, &space_id) < 0) {
        return NULL;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    _PyRetraceThreadSpaceState *space = retrace_find_space(tstate, space_id);
    if (space == NULL || !space->seen) {
        Py_RETURN_NONE;
    }
    uint64_t hash = _PyRetrace_FrameCoordinateHashInSpace(
        tstate, retrace_thread_state_frame(tstate, space), space);
    return PyLong_FromUnsignedLongLong((unsigned long long)hash);
}

static PyObject *
retrace_coordinates(PyObject *module, PyObject *args)
{
    PyThreadState *current_tstate = _PyThreadState_GET();
    PyObject *thread_id_arg = Py_None;
    PyObject *space_id_arg = Py_None;
    uint64_t thread_id = 0;
    Py_ssize_t drop = 0;
    int explicit_thread_id = 0;
    uint32_t space_id = _PyFrame_RETRACE_SPACE_ID_ROOT;

    if (!PyArg_ParseTuple(args, "|OnO:coordinates",
                          &thread_id_arg, &drop, &space_id_arg))
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
    if (retrace_space_id_from_object(space_id_arg, &space_id) < 0) {
        return NULL;
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
    _PyRetraceThreadSpaceState *space =
        retrace_find_space(target_tstate, space_id);
    if (space == NULL || !space->seen) {
        RETRACE_HEAD_UNLOCK(runtime);
        Py_RETURN_NONE;
    }
    _PyInterpreterFrame *frame =
        retrace_thread_state_frame(target_tstate, space);
    Py_ssize_t coordinate_count = retrace_coordinate_count(frame, space);
    uint64_t root_call_ordinal = space->root_call_ordinal;
    Py_ssize_t size = coordinate_count > drop ? coordinate_count - drop : 0;
    RETRACE_HEAD_UNLOCK(runtime);

    if (coordinate_count != 0) {
        coordinates = PyMem_New(uint64_t, coordinate_count);
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
    space = retrace_find_space(target_tstate, space_id);
    if (space == NULL || !space->seen) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyMem_Free(coordinates);
        Py_RETURN_NONE;
    }
    frame = retrace_thread_state_frame(target_tstate, space);
    if (retrace_coordinate_count(frame, space) != coordinate_count) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyMem_Free(coordinates);
        PyErr_SetString(PyExc_RuntimeError, "thread frame stack changed");
        return NULL;
    }
    if (space->root_call_ordinal != root_call_ordinal) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyMem_Free(coordinates);
        PyErr_SetString(PyExc_RuntimeError,
                        "thread root call ordinal changed");
        return NULL;
    }
    if (coordinate_count != 0) {
        retrace_fill_coordinates(coordinates, coordinate_count, frame, space);
    }
    RETRACE_HEAD_UNLOCK(runtime);

    PyObject *result = retrace_tuple_from_u64s(
        coordinates == NULL ? NULL : coordinates + drop, size);
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
    _PyRetraceThreadSpaceState *previous_space;
    uint32_t previous_space_id;
    uint64_t *previous_call_ordinal_ptr;
    _PyRetraceThreadSpaceState *space;
    uint64_t local_root_call_ordinal;
    int use_local_root;
    int active;
} RetraceSpaceScope;

static uint32_t retrace_next_dynamic_space_id =
    _PyFrame_RETRACE_SPACE_ID_DISABLED + UINT32_C(1);

static PyTypeObject retrace_call_wrapper_type;

static uint32_t
retrace_allocate_dynamic_space_id(void)
{
    uint32_t space_id = retrace_next_dynamic_space_id++;
    if (retrace_next_dynamic_space_id == _PyFrame_RETRACE_SPACE_ID_ROOT ||
        retrace_next_dynamic_space_id == _PyFrame_RETRACE_SPACE_ID_DISABLED)
    {
        retrace_next_dynamic_space_id =
            _PyFrame_RETRACE_SPACE_ID_DISABLED + UINT32_C(1);
    }
    return space_id;
}

static int
retrace_enter_space(PyThreadState *tstate,
                    uint32_t space_id,
                    RetraceSpaceScope *scope)
{
    scope->previous_space = NULL;
    scope->previous_space_id = _PyFrame_RETRACE_SPACE_ID_ROOT;
    scope->previous_call_ordinal_ptr = NULL;
    scope->space = NULL;
    scope->local_root_call_ordinal = 0;
    scope->use_local_root = 0;
    scope->active = 0;
    if (tstate == NULL) {
        return 0;
    }

    _PyRetraceThreadSpaceState *space =
        _PyRetrace_GetThreadSpace(tstate, space_id);
    if (space == NULL) {
        return -1;
    }

    scope->previous_space = tstate->retrace.current_space;
    scope->previous_space_id = tstate->retrace.inherited_space_id;
    scope->previous_call_ordinal_ptr = tstate->retrace.call_ordinal_ptr;
    scope->space = space;
    scope->active = 1;

    _PyInterpreterFrame *parent =
        _PyRetrace_NearestVisibleFrameInSpace(
            _PyRetrace_CurrentThreadFrame(tstate), space);
    if (parent == NULL) {
        scope->use_local_root = 1;
        scope->local_root_call_ordinal = space->root_call_ordinal;
        tstate->retrace.call_ordinal_ptr =
            &scope->local_root_call_ordinal;
    }
    else {
        tstate->retrace.call_ordinal_ptr =
            &parent->retrace.current_call_ordinal;
    }
    tstate->retrace.current_space = space;
    tstate->retrace.inherited_space_id = space_id;
    return 0;
}

static void
retrace_leave_space(PyThreadState *tstate, RetraceSpaceScope *scope)
{
    if (tstate == NULL || !scope->active) {
        return;
    }
    if (scope->use_local_root && scope->space != NULL) {
        scope->space->root_call_ordinal = scope->local_root_call_ordinal;
    }
    tstate->retrace.current_space = scope->previous_space;
    tstate->retrace.inherited_space_id = scope->previous_space_id;
    tstate->retrace.call_ordinal_ptr = scope->previous_call_ordinal_ptr;
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
    (void)hide_current_frame;

    uint32_t space_id = mode == RETRACE_CALL_EXCLUDE ?
        _PyFrame_RETRACE_SPACE_ID_DISABLED :
        _PyFrame_RETRACE_SPACE_ID_ROOT;
    RetraceSpaceScope scope;
    if (retrace_enter_space(tstate, space_id, &scope) < 0) {
        return NULL;
    }
    result = PyObject_Vectorcall(callable, args, nargsf, kwnames);
    retrace_leave_space(tstate, &scope);
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

PyDoc_STRVAR(retrace_allocate_space_id_doc,
"allocate_space_id($module, /)\n"
"--\n"
"\n"
"Return a new native Retrace coordinate space id.");

static PyObject *
retrace_allocate_space_id(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    uint32_t space_id = retrace_allocate_dynamic_space_id();
    return PyLong_FromUnsignedLong((unsigned long)space_id);
}

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

PyDoc_STRVAR(retrace_with_new_coordinates_doc,
"with_new_coordinates($module, callable, /, *args, **kwargs)\n"
"--\n"
"\n"
"Call callable as the root of a fresh Retrace coordinate space.");

PyDoc_STRVAR(retrace_run_in_space_doc,
"run_in_space($module, space_id, callable, /, *args, **kwargs)\n"
"--\n"
"\n"
"Call callable in the selected Retrace coordinate space.");

static PyObject *
retrace_run_in_space(PyObject *module,
                     PyObject *const *args,
                     Py_ssize_t nargs,
                     PyObject *kwnames)
{
    if (nargs < 2) {
        PyErr_SetString(PyExc_TypeError,
                        "run_in_space expected at least 2 arguments");
        return NULL;
    }
    uint32_t space_id;
    if (retrace_space_id_from_object(args[0], &space_id) < 0) {
        return NULL;
    }
    PyObject *callable = args[1];
    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError,
                        "run_in_space callable argument must be callable");
        return NULL;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    RetraceSpaceScope scope;
    if (retrace_enter_space(tstate, space_id, &scope) < 0) {
        return NULL;
    }
    PyObject *result = PyObject_Vectorcall(
        callable, args + 2, (size_t)(nargs - 2), kwnames);
    retrace_leave_space(tstate, &scope);
    return result;
}

static PyObject *
retrace_with_new_coordinates(PyObject *module,
                             PyObject *const *args,
                             Py_ssize_t nargs,
                             PyObject *kwnames)
{
    if (nargs < 1) {
        PyErr_SetString(PyExc_TypeError,
                        "with_new_coordinates expected at least 1 argument");
        return NULL;
    }
    PyObject *callable = args[0];
    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError,
                        "with_new_coordinates argument must be callable");
        return NULL;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    RetraceSpaceScope scope;
    if (retrace_enter_space(
            tstate, retrace_allocate_dynamic_space_id(), &scope) < 0)
    {
        return NULL;
    }
    PyObject *result = PyObject_Vectorcall(
        callable, args + 1, (size_t)(nargs - 1), kwnames);
    retrace_leave_space(tstate, &scope);
    return result;
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
    if (argc != 3 && argc != 4 && argc != 5) {
        PyErr_SetString(PyExc_TypeError,
                        "call_at expects None or "
                        "(thread_id, coordinates, callback[, "
                        "overshoot_callback[, space_id]])");
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
    if (argc >= 4) {
        overshoot_callback = PyTuple_GET_ITEM(args, 3);
        overshoot_callback_ref = retrace_callback_ref(
            overshoot_callback, "call_at overshoot", 1);
        if (overshoot_callback_ref == NULL && PyErr_Occurred()) {
            Py_DECREF(callback_ref);
            return NULL;
        }
    }
    uint32_t space_id = _PyFrame_RETRACE_SPACE_ID_ROOT;
    if (argc == 5 &&
        retrace_space_id_from_object(PyTuple_GET_ITEM(args, 4), &space_id) < 0)
    {
        Py_DECREF(callback_ref);
        Py_XDECREF(overshoot_callback_ref);
        return NULL;
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
    _PyRetraceThreadSpaceState *space =
        retrace_find_space(target_tstate, space_id);
    if (space == NULL || !space->seen) {
        RETRACE_HEAD_UNLOCK(runtime);
        Py_DECREF(coordinates);
        Py_DECREF(callback_ref);
        Py_XDECREF(overshoot_callback_ref);
        PyErr_SetString(PyExc_LookupError, "unknown coordinate space");
        return NULL;
    }
    _PyInterpreterFrame *frame = retrace_thread_state_frame(target_tstate, space);
    int comparison = 0;
    int compare_status =
        retrace_compare_target_to_frame(coordinates, frame, space, &comparison);
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
    interp->retrace.call_at_space_id = space_id;
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
    int has_timeout;
    PY_TIMEOUT_T timeout_us;
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
    static char *keywords[] = {"timeout", NULL};
    PyObject *timeout_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "|O:ThreadHandoff", keywords, &timeout_obj))
    {
        return NULL;
    }

    int has_timeout = 0;
    PY_TIMEOUT_T timeout_us = -1;
    if (timeout_obj != Py_None) {
        RetracePyTime timeout;
        if (_PyTime_FromSecondsObject(
                &timeout, timeout_obj, _PyTime_ROUND_TIMEOUT) < 0)
        {
            return NULL;
        }
        if (timeout < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "thread handoff timeout must be non-negative");
            return NULL;
        }

        RetracePyTime microseconds =
            _PyTime_AsMicroseconds(timeout, _PyTime_ROUND_TIMEOUT);
        if (microseconds > PY_TIMEOUT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "thread handoff timeout is too large");
            return NULL;
        }
        has_timeout = 1;
        timeout_us = (PY_TIMEOUT_T)microseconds;
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
    self->has_timeout = has_timeout;
    self->timeout_us = timeout_us;
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

static int
retrace_thread_handoff_wait_entry_locked(RetraceThreadHandoff *self,
                                         RetraceThreadHandoffEntry *entry)
{
    if (entry->permit) {
        entry->permit = 0;
        entry->waiting = 0;
        return 0;
    }

    entry->waiting = 1;
    while (!self->closed && !entry->permit) {
        PyThread_type_lock gate = entry->gate;
        PyThread_release_lock(self->lock);

        PyLockStatus status;
        Py_BEGIN_ALLOW_THREADS
        status = PyThread_acquire_lock_timed(
            gate, self->has_timeout ? self->timeout_us : -1, 0);
        Py_END_ALLOW_THREADS

        PyThread_acquire_lock(self->lock, WAIT_LOCK);
        if (self->has_timeout && status == PY_LOCK_FAILURE) {
            entry->waiting = 0;
            PyErr_SetString(PyExc_TimeoutError,
                            "thread handoff timed out");
            return -1;
        }
        else if (status != PY_LOCK_ACQUIRED) {
            entry->waiting = 0;
            PyErr_SetString(PyExc_RuntimeError,
                            "thread handoff wait failed");
            return -1;
        }
    }

    entry->waiting = 0;
    entry->permit = 0;
    return 0;
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
retrace_thread_handoff_start(PyObject *self_obj, PyObject *Py_UNUSED(ignored))
{
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

    RetraceThreadHandoffEntry *current =
        retrace_thread_handoff_get_entry(self, current_id);
    if (current == NULL) {
        PyThread_release_lock(self->lock);
        return NULL;
    }

    int status = retrace_thread_handoff_wait_entry_locked(self, current);
    PyThread_release_lock(self->lock);
    if (status < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
retrace_thread_handoff_to(PyObject *self_obj, PyObject *target_arg)
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

    int status = retrace_thread_handoff_wait_entry_locked(self, current);
    PyThread_release_lock(self->lock);
    if (status < 0) {
        return NULL;
    }
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
     PyDoc_STR("Wake sleeping threads and reject future starts or transfers.")},
    {"start", retrace_thread_handoff_start, METH_NOARGS,
     PyDoc_STR("Register and park the current replay thread.")},
    {"to", retrace_thread_handoff_to, METH_O,
     PyDoc_STR("Transfer execution to thread_id and park the current thread.")},
    {NULL, NULL, 0, NULL},
};

PyDoc_STRVAR(retrace_thread_handoff_doc,
"ThreadHandoff(timeout=None)\n"
"--\n"
"\n"
"Create a replay handoff gate.\n"
"\n"
"start() registers the current stable thread id, then parks the thread with\n"
"the GIL released until another thread transfers execution to it.\n"
"to(thread_id) marks thread_id runnable, then parks the current thread until\n"
"another transfer marks it runnable.\n"
"If timeout is not None, parked waits raise TimeoutError after timeout\n"
"seconds without a transfer.");

static PyTypeObject retrace_thread_handoff_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_retrace.ThreadHandoff",
    .tp_basicsize = sizeof(RetraceThreadHandoff),
    .tp_dealloc = retrace_thread_handoff_dealloc,
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
    RetraceSpaceScope scope;
    if (retrace_enter_space(
            tstate, _PyFrame_RETRACE_SPACE_ID_DISABLED, &scope) < 0)
    {
        tstate->retrace.thread_callback_active = 0;
        tstate->retrace.thread_callback_frame = NULL;
        return -1;
    }
    PyObject *result = PyObject_CallNoArgs(callback);
    retrace_leave_space(tstate, &scope);

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
#if PY_VERSION_HEX >= 0x030D0000
    PyObject *saved_exc = _PyErr_GetRaisedException(tstate);
#elif PY_VERSION_HEX >= 0x030C0000
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

    RetraceSpaceScope scope;
    if (retrace_enter_space(
            tstate, _PyFrame_RETRACE_SPACE_ID_DISABLED, &scope) < 0)
    {
        PyErr_WriteUnraisable(callback);
        tstate->retrace.thread_callback_active = 0;
        tstate->retrace.thread_callback_frame = NULL;
        Py_DECREF(callback);
#if PY_VERSION_HEX >= 0x030D0000
        _PyErr_SetRaisedException(tstate, saved_exc);
#elif PY_VERSION_HEX >= 0x030C0000
        PyErr_SetRaisedException(saved_exc);
#else
        PyErr_Restore(saved_type, saved_value, saved_traceback);
#endif
        return;
    }
    PyObject *result = PyObject_CallNoArgs(callback);
    retrace_leave_space(tstate, &scope);
    if (result == NULL) {
        PyErr_WriteUnraisable(callback);
    }
    else {
        Py_DECREF(result);
    }

    tstate->retrace.thread_callback_active = 0;
    tstate->retrace.thread_callback_frame = NULL;

    Py_DECREF(callback);
#if PY_VERSION_HEX >= 0x030D0000
    _PyErr_SetRaisedException(tstate, saved_exc);
#elif PY_VERSION_HEX >= 0x030C0000
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
    _PyRetraceThreadSpaceState *space =
        retrace_find_space(tstate, interp->retrace.call_at_space_id);
    if (space == NULL || !space->seen) {
        retrace_clear_call_at(interp);
        return 0;
    }

    int comparison = 0;
    if (retrace_compare_target_to_frame(
            interp->retrace.call_at_coordinates,
            frame,
            space,
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
     METH_VARARGS, retrace_thread_delta_doc},
    {"hash", retrace_hash, METH_VARARGS, retrace_hash_doc},
    {"allocate_space_id", retrace_allocate_space_id, METH_NOARGS,
     retrace_allocate_space_id_doc},
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
    {"with_new_coordinates", _PyCFunction_CAST(retrace_with_new_coordinates),
     METH_FASTCALL | METH_KEYWORDS, retrace_with_new_coordinates_doc},
    {"run_in_space", _PyCFunction_CAST(retrace_run_in_space),
     METH_FASTCALL | METH_KEYWORDS, retrace_run_in_space_doc},
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
