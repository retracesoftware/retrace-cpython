#include "Python.h"

#include "internal/pycore_frame.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_retrace.h"
#if PY_VERSION_HEX < 0x030C0000
#  include "internal/pycore_runtime.h"
#endif

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
retrace_coordinate_count(_PyInterpreterFrame *frame)
{
    while (frame != NULL && !retrace_frame_is_visible(frame)) {
        frame = frame->previous;
    }
    if (frame == NULL) {
        return 0;
    }
    return retrace_frame_depth(frame) + 1;
}

static void
retrace_fill_coordinates(uint64_t *coordinates,
                         _PyInterpreterFrame *frame,
                         Py_ssize_t drop)
{
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (retrace_frame_is_visible(scan)) {
            Py_ssize_t position = retrace_frame_depth(scan);
            if (position >= drop) {
                coordinates[position - drop] = retrace_frame_coordinate(scan);
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

        uint64_t coordinate = retrace_frame_coordinate(scan);
        Py_ssize_t position = retrace_frame_depth(scan);
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

        uint64_t coordinate = retrace_frame_coordinate(scan);
        Py_ssize_t position = retrace_frame_depth(scan);
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
retrace_clear_replay_checkpoint(PyInterpreterState *interp)
{
    interp->retrace.replay_checkpoint_armed = 0;
    interp->retrace.replay_checkpoint_thread_id = 0;
    interp->retrace.replay_checkpoint_top = 0;
    Py_CLEAR(interp->retrace.replay_checkpoint_coordinates);
    Py_CLEAR(interp->retrace.replay_checkpoint_callback);
}

static int
retrace_replay_checkpoint_matches(PyThreadState *tstate,
                                  _PyInterpreterFrame *frame)
{
    PyObject *target = tstate->interp->retrace.replay_checkpoint_coordinates;
    if (target == NULL) {
        return 0;
    }

    Py_ssize_t target_size = PyTuple_GET_SIZE(target);
    Py_ssize_t current_size = retrace_coordinate_count(frame);
    if (target_size != current_size) {
        return 0;
    }

    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (!retrace_frame_is_visible(scan)) {
            continue;
        }
        Py_ssize_t position = retrace_frame_depth(scan);
        uint64_t target_coordinate;
        if (retrace_u64_from_object(
                PyTuple_GET_ITEM(target, position), &target_coordinate) < 0)
        {
            PyErr_Clear();
            return 0;
        }
        if (target_coordinate != retrace_frame_coordinate(scan)) {
            return 0;
        }
    }
    return 1;
}

PyDoc_STRVAR(retrace_coordinates_doc,
"coordinates($module, thread_id=None, drop=0, /)\n"
"--\n"
"\n"
"Return a thread's visible Python frame coordinates.\n"
"\n"
"Coordinates are ordered from oldest visible Python frame to current frame.\n"
"drop omits that many leading coordinates from the returned sequence.");

PyDoc_STRVAR(retrace_thread_delta_doc,
"thread_delta($module, /)\n"
"--\n"
"\n"
"Return [common_prefix_count, *new_suffix] for the current thread's visible\n"
"coordinates.");

PyDoc_STRVAR(retrace_hash_doc,
"hash($module, /)\n"
"--\n"
"\n"
"Return the current thread's 64-bit coordinate-location hash.");

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

static int
retrace_validate_thread_callback(PyObject **callback, const char *name)
{
    if (*callback == Py_None) {
        *callback = NULL;
        return 0;
    }
    if (!PyCallable_Check(*callback)) {
        PyErr_Format(PyExc_TypeError,
                     "%s callback must be callable or None",
                     name);
        return -1;
    }
    return 0;
}

PyDoc_STRVAR(retrace_set_thread_yield_callback_doc,
"set_thread_yield_callback($module, callback, /)\n"
"--\n"
"\n"
"Set a Python callback invoked as callback() before the current thread yields\n"
"from the eval loop.");

static PyObject *
retrace_set_thread_yield_callback(PyObject *module, PyObject *callback)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    if (retrace_validate_thread_callback(&callback, "thread yield") < 0) {
        return NULL;
    }

    PyObject *old_callback = interp->retrace.thread_yield_callback;
    interp->retrace.thread_yield_callback = Py_XNewRef(callback);
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

PyDoc_STRVAR(retrace_set_thread_resume_callback_doc,
"set_thread_resume_callback($module, callback, /)\n"
"--\n"
"\n"
"Set a Python callback invoked as callback() before application bytecode runs\n"
"after the current thread resumes from an eval-loop yield.");

static PyObject *
retrace_set_thread_resume_callback(PyObject *module, PyObject *callback)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    if (retrace_validate_thread_callback(&callback, "thread resume") < 0) {
        return NULL;
    }

    PyObject *old_callback = interp->retrace.thread_resume_callback;
    interp->retrace.thread_resume_callback = Py_XNewRef(callback);
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

PyDoc_STRVAR(retrace_set_replay_checkpoint_doc,
"set_replay_checkpoint($module, thread_id, coordinates, callback, /)\n"
"--\n"
"\n"
"Invoke callback() when thread_id reaches the given coordinates.\n"
"\n"
"Pass None as the only argument to clear the active checkpoint.");

static PyObject *
retrace_set_replay_checkpoint(PyObject *module, PyObject *args)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;

    Py_ssize_t argc = PyTuple_GET_SIZE(args);
    if (argc == 1 && PyTuple_GET_ITEM(args, 0) == Py_None) {
        retrace_clear_replay_checkpoint(interp);
        Py_RETURN_NONE;
    }
    if (argc != 3) {
        PyErr_SetString(PyExc_TypeError,
                        "set_replay_checkpoint expects None or "
                        "(thread_id, coordinates, callback)");
        return NULL;
    }

    uint64_t thread_id = 0;
    if (retrace_thread_id_from_object(PyTuple_GET_ITEM(args, 0), &thread_id) < 0) {
        return NULL;
    }
    if (thread_id == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "replay checkpoint thread id must be non-zero");
        return NULL;
    }

    PyObject *callback = PyTuple_GET_ITEM(args, 2);
    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError,
                        "replay checkpoint callback must be callable");
        return NULL;
    }

    PyObject *coordinates =
        retrace_tuple_from_sequence(PyTuple_GET_ITEM(args, 1),
                                    "checkpoint coordinates must be a sequence");
    if (coordinates == NULL) {
        return NULL;
    }
    if (PyTuple_GET_SIZE(coordinates) <= 0) {
        Py_DECREF(coordinates);
        PyErr_SetString(PyExc_ValueError,
                        "checkpoint coordinates must include a frame");
        return NULL;
    }

    PyObject *callback_ref = Py_NewRef(callback);
    retrace_clear_replay_checkpoint(interp);
    interp->retrace.replay_checkpoint_thread_id = thread_id;
    if (retrace_u64_from_object(
            PyTuple_GET_ITEM(coordinates, PyTuple_GET_SIZE(coordinates) - 1),
            &interp->retrace.replay_checkpoint_top) < 0)
    {
        Py_DECREF(coordinates);
        Py_DECREF(callback_ref);
        return NULL;
    }
    interp->retrace.replay_checkpoint_coordinates = (PyObject *)coordinates;
    interp->retrace.replay_checkpoint_callback = callback_ref;
    interp->retrace.replay_checkpoint_armed = 1;

    Py_RETURN_NONE;
}

static void
retrace_call_thread_callback(PyThreadState *tstate,
                             PyObject *callback)
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

    tstate->retrace.thread_callback_active = 1;

    PyObject *result = PyObject_CallNoArgs(callback);
    if (result == NULL) {
        PyErr_WriteUnraisable(callback);
    }
    else {
        Py_DECREF(result);
    }

    tstate->retrace.thread_callback_active = 0;

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
    if (interp->retrace.thread_resume_callback == NULL) {
        return;
    }

    tstate->retrace.thread_resume_pending = 1;
}

void
_PyRetrace_DeliverThreadResumeCallback(PyThreadState *tstate)
{
    if (tstate == NULL || tstate->retrace.thread_id == 0 ||
        !tstate->retrace.thread_resume_pending)
    {
        return;
    }
    if (tstate->retrace.thread_callback_active) {
        return;
    }

    PyInterpreterState *interp = tstate->interp;
    PyObject *callback = interp->retrace.thread_resume_callback;
    if (callback == NULL) {
        tstate->retrace.thread_resume_pending = 0;
        return;
    }

    tstate->retrace.thread_resume_pending = 0;
    retrace_call_thread_callback(tstate, callback);
}

void
_PyRetrace_DeliverThreadYieldCallback(PyThreadState *tstate)
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

    retrace_call_thread_callback(tstate, callback);
}

int
_PyRetrace_CheckReplayCheckpoint(PyThreadState *tstate,
                                 _PyInterpreterFrame *frame)
{
    if (tstate == NULL || tstate->retrace.thread_id == 0 ||
        frame == NULL)
    {
        return 0;
    }

    PyInterpreterState *interp = tstate->interp;
    if (!interp->retrace.replay_checkpoint_armed ||
        interp->retrace.replay_checkpoint_callback_active ||
        interp->retrace.replay_checkpoint_thread_id != tstate->retrace.thread_id)
    {
        return 0;
    }
    if (interp->retrace.replay_checkpoint_top !=
            retrace_frame_coordinate(frame))
    {
        return 0;
    }
    if (!retrace_replay_checkpoint_matches(tstate, frame)) {
        return 0;
    }

    PyObject *callback = interp->retrace.replay_checkpoint_callback;
    if (callback == NULL) {
        retrace_clear_replay_checkpoint(interp);
        return 0;
    }
    callback = Py_NewRef(callback);
    retrace_clear_replay_checkpoint(interp);

    interp->retrace.replay_checkpoint_callback_active = 1;
    PyObject *result = PyObject_CallNoArgs(callback);

    interp->retrace.replay_checkpoint_callback_active = 0;

    Py_DECREF(callback);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}


static PyMethodDef retrace_methods[] = {
    {"coordinates", retrace_coordinates, METH_VARARGS,
     retrace_coordinates_doc},
    {"thread_delta", retrace_thread_delta,
     METH_NOARGS, retrace_thread_delta_doc},
    {"hash", retrace_hash, METH_NOARGS, retrace_hash_doc},
    {"set_thread_yield_callback", retrace_set_thread_yield_callback, METH_O,
     retrace_set_thread_yield_callback_doc},
    {"get_thread_yield_callback", retrace_get_thread_yield_callback,
     METH_NOARGS, retrace_get_thread_yield_callback_doc},
    {"set_thread_resume_callback", retrace_set_thread_resume_callback, METH_O,
     retrace_set_thread_resume_callback_doc},
    {"get_thread_resume_callback", retrace_get_thread_resume_callback,
     METH_NOARGS, retrace_get_thread_resume_callback_doc},
    {"set_replay_checkpoint", retrace_set_replay_checkpoint, METH_VARARGS,
     retrace_set_replay_checkpoint_doc},
    {NULL, NULL, 0, NULL},
};

static int
retrace_exec(PyObject *module)
{
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
