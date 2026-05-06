#include "Python.h"

#include "internal/pycore_frame.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_retrace.h"
#if PY_VERSION_HEX < 0x030C0000
#  include "internal/pycore_runtime.h"
#endif

#include <stddef.h>
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

typedef struct {
    PyObject_VAR_HEAD
    uint64_t ob_item[1];
} RetraceU64BufferObject;

static PyTypeObject RetraceU64Buffer_Type;

static RetraceU64BufferObject *
retrace_u64buffer_new(Py_ssize_t size)
{
    return PyObject_NewVar(RetraceU64BufferObject, &RetraceU64Buffer_Type, size);
}

static void
retrace_u64buffer_dealloc(RetraceU64BufferObject *self)
{
    PyObject_Free(self);
}

static Py_ssize_t
retrace_u64buffer_len(RetraceU64BufferObject *self)
{
    return Py_SIZE(self);
}

static PyObject *
retrace_u64buffer_item(RetraceU64BufferObject *self, Py_ssize_t i)
{
    if (i < 0 || i >= Py_SIZE(self)) {
        PyErr_SetString(PyExc_IndexError, "U64Buffer index out of range");
        return NULL;
    }
    return PyLong_FromUnsignedLongLong((unsigned long long)self->ob_item[i]);
}

static int
retrace_u64buffer_value(PyObject *value, uint64_t *out)
{
    unsigned long long converted = PyLong_AsUnsignedLongLong(value);
    if (converted == (unsigned long long)-1 && PyErr_Occurred()) {
        PyErr_Clear();
        return 0;
    }
    *out = (uint64_t)converted;
    return 1;
}

static int
retrace_u64buffer_value_required(PyObject *value, uint64_t *out)
{
    unsigned long long converted = PyLong_AsUnsignedLongLong(value);
    if (converted == (unsigned long long)-1 && PyErr_Occurred()) {
        return -1;
    }
    *out = (uint64_t)converted;
    return 0;
}

static RetraceU64BufferObject *
retrace_u64buffer_from_sequence(PyObject *sequence, const char *message)
{
    PyObject *fast = PySequence_Fast(sequence, message);
    if (fast == NULL) {
        return NULL;
    }

    Py_ssize_t size = PySequence_Fast_GET_SIZE(fast);
    RetraceU64BufferObject *buffer = retrace_u64buffer_new(size);
    if (buffer == NULL) {
        Py_DECREF(fast);
        return NULL;
    }

    PyObject **items = PySequence_Fast_ITEMS(fast);
    for (Py_ssize_t index = 0; index < size; index++) {
        int converted = retrace_u64buffer_value_required(
            items[index], &buffer->ob_item[index]);
        if (converted < 0) {
            Py_DECREF(buffer);
            Py_DECREF(fast);
            return NULL;
        }
    }

    Py_DECREF(fast);
    return buffer;
}

static int
retrace_u64buffer_contains(RetraceU64BufferObject *self, PyObject *value)
{
    uint64_t needle;
    if (!retrace_u64buffer_value(value, &needle)) {
        return 0;
    }

    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        if (self->ob_item[i] == needle) {
            return 1;
        }
    }
    return 0;
}

static PyObject *
retrace_u64buffer_slice(RetraceU64BufferObject *self, Py_ssize_t start,
                        Py_ssize_t step, Py_ssize_t slicelen)
{
    RetraceU64BufferObject *result = retrace_u64buffer_new(slicelen);
    if (result == NULL) {
        return NULL;
    }

    for (Py_ssize_t i = 0, source = start; i < slicelen; i++, source += step) {
        result->ob_item[i] = self->ob_item[source];
    }
    return (PyObject *)result;
}

static PyObject *
retrace_u64buffer_subscript(RetraceU64BufferObject *self, PyObject *item)
{
    if (PyIndex_Check(item)) {
        Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) {
            return NULL;
        }
        if (i < 0) {
            i += Py_SIZE(self);
        }
        return retrace_u64buffer_item(self, i);
    }

    if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step;
        if (PySlice_Unpack(item, &start, &stop, &step) < 0) {
            return NULL;
        }
        Py_ssize_t slicelen = PySlice_AdjustIndices(Py_SIZE(self), &start,
                                                    &stop, step);
        return retrace_u64buffer_slice(self, start, step, slicelen);
    }

    PyErr_Format(PyExc_TypeError,
                 "U64Buffer indices must be integers or slices, not %.200s",
                 Py_TYPE(item)->tp_name);
    return NULL;
}

static Py_hash_t
retrace_u64buffer_hash(RetraceU64BufferObject *self)
{
    PyObject *tuple = PySequence_Tuple((PyObject *)self);
    if (tuple == NULL) {
        return -1;
    }
    Py_hash_t hash = PyObject_Hash(tuple);
    Py_DECREF(tuple);
    return hash;
}

static PyObject *
retrace_u64buffer_richcompare(PyObject *self, PyObject *other, int op)
{
    PyObject *self_tuple = PySequence_Tuple(self);
    if (self_tuple == NULL) {
        return NULL;
    }

    PyObject *other_tuple;
    if (PyObject_TypeCheck(other, &RetraceU64Buffer_Type)) {
        other_tuple = PySequence_Tuple(other);
        if (other_tuple == NULL) {
            Py_DECREF(self_tuple);
            return NULL;
        }
    }
    else if (PyTuple_Check(other)) {
        other_tuple = Py_NewRef(other);
    }
    else {
        Py_DECREF(self_tuple);
        if (op == Py_EQ) {
            Py_RETURN_FALSE;
        }
        if (op == Py_NE) {
            Py_RETURN_TRUE;
        }
        Py_RETURN_NOTIMPLEMENTED;
    }

    PyObject *result = PyObject_RichCompare(self_tuple, other_tuple, op);
    Py_DECREF(self_tuple);
    Py_DECREF(other_tuple);
    return result;
}

static PyObject *
retrace_u64buffer_count(RetraceU64BufferObject *self, PyObject *value)
{
    uint64_t needle;
    Py_ssize_t count = 0;
    if (retrace_u64buffer_value(value, &needle)) {
        for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
            if (self->ob_item[i] == needle) {
                count++;
            }
        }
    }
    return PyLong_FromSsize_t(count);
}

static PyObject *
retrace_u64buffer_index(RetraceU64BufferObject *self, PyObject *args)
{
    PyObject *value;
    Py_ssize_t start = 0;
    Py_ssize_t stop = Py_SIZE(self);
    if (!PyArg_ParseTuple(args, "O|nn:index", &value, &start, &stop)) {
        return NULL;
    }

    Py_ssize_t size = Py_SIZE(self);
    if (start < 0) {
        start += size;
        if (start < 0) {
            start = 0;
        }
    }
    if (stop < 0) {
        stop += size;
        if (stop < 0) {
            stop = 0;
        }
    }
    if (stop > size) {
        stop = size;
    }

    uint64_t needle;
    if (retrace_u64buffer_value(value, &needle)) {
        for (Py_ssize_t i = start; i < stop; i++) {
            if (self->ob_item[i] == needle) {
                return PyLong_FromSsize_t(i);
            }
        }
    }

    PyErr_SetString(PyExc_ValueError, "U64Buffer.index(x): x not in U64Buffer");
    return NULL;
}

static PyObject *
retrace_u64buffer_repr(RetraceU64BufferObject *self)
{
    PyObject *tuple = PySequence_Tuple((PyObject *)self);
    if (tuple == NULL) {
        return NULL;
    }
    PyObject *repr = PyUnicode_FromFormat("retrace.U64Buffer(%R)", tuple);
    Py_DECREF(tuple);
    return repr;
}

static int
retrace_u64buffer_getbuffer(RetraceU64BufferObject *self, Py_buffer *view,
                            int flags)
{
    if (view == NULL) {
        PyErr_SetString(PyExc_BufferError,
                        "U64Buffer getbuffer view argument is NULL");
        return -1;
    }

    view->buf = self->ob_item;
    view->obj = Py_NewRef(self);
    view->len = Py_SIZE(self) * (Py_ssize_t)sizeof(uint64_t);
    view->readonly = 1;
    view->ndim = 1;
    view->itemsize = (Py_ssize_t)sizeof(uint64_t);
    view->suboffsets = NULL;
    view->shape = NULL;
    if ((flags & PyBUF_ND) == PyBUF_ND) {
        view->shape = &((PyVarObject *)self)->ob_size;
    }
    view->strides = NULL;
    if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES) {
        view->strides = &view->itemsize;
    }
    view->format = NULL;
    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT) {
        view->format = "Q";
    }
    view->internal = NULL;
    return 0;
}

static void
retrace_u64buffer_releasebuffer(RetraceU64BufferObject *self, Py_buffer *view)
{
}

static PySequenceMethods retrace_u64buffer_as_sequence = {
    .sq_length = (lenfunc)retrace_u64buffer_len,
    .sq_item = (ssizeargfunc)retrace_u64buffer_item,
    .sq_contains = (objobjproc)retrace_u64buffer_contains,
};

static PyMappingMethods retrace_u64buffer_as_mapping = {
    .mp_length = (lenfunc)retrace_u64buffer_len,
    .mp_subscript = (binaryfunc)retrace_u64buffer_subscript,
};

static PyBufferProcs retrace_u64buffer_as_buffer = {
    .bf_getbuffer = (getbufferproc)retrace_u64buffer_getbuffer,
    .bf_releasebuffer = (releasebufferproc)retrace_u64buffer_releasebuffer,
};

static PyMethodDef retrace_u64buffer_methods[] = {
    {"count", (PyCFunction)retrace_u64buffer_count, METH_O,
     PyDoc_STR("Return number of occurrences of value.")},
    {"index", (PyCFunction)retrace_u64buffer_index, METH_VARARGS,
     PyDoc_STR("Return first index of value.")},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject RetraceU64Buffer_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "retrace.U64Buffer",
    .tp_basicsize = offsetof(RetraceU64BufferObject, ob_item),
    .tp_itemsize = sizeof(uint64_t),
    .tp_dealloc = (destructor)retrace_u64buffer_dealloc,
    .tp_repr = (reprfunc)retrace_u64buffer_repr,
    .tp_hash = (hashfunc)retrace_u64buffer_hash,
    .tp_richcompare = retrace_u64buffer_richcompare,
    .tp_as_sequence = &retrace_u64buffer_as_sequence,
    .tp_as_mapping = &retrace_u64buffer_as_mapping,
    .tp_as_buffer = &retrace_u64buffer_as_buffer,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_methods = retrace_u64buffer_methods,
    .tp_iter = PySeqIter_New,
};

static int
retrace_frame_is_visible(_PyInterpreterFrame *frame)
{
    return _PyRetrace_FrameIsVisible(frame);
}

static uint64_t
retrace_frame_instruction_counter(_PyInterpreterFrame *frame)
{
    return _PyRetrace_FrameInstructionCounter(frame);
}

static _PyInterpreterFrame *
retrace_thread_state_frame(PyThreadState *current_tstate,
                           PyThreadState *target_tstate)
{
    if (target_tstate->retrace_thread_callback_active &&
        target_tstate->retrace_thread_callback_frame != NULL)
    {
        return target_tstate->retrace_thread_callback_frame;
    }

    PyInterpreterState *interp = current_tstate->interp;
    if (interp->retrace_replay_checkpoint_callback_active &&
        target_tstate == interp->retrace_replay_checkpoint_callback_tstate &&
        interp->retrace_replay_checkpoint_callback_frame != NULL)
    {
        return interp->retrace_replay_checkpoint_callback_frame;
    }
    if (target_tstate->cframe == NULL) {
        return NULL;
    }
    return target_tstate->cframe->current_frame;
}

static PyThreadState *
retrace_find_thread_state(PyInterpreterState *interp, unsigned long thread_id)
{
    for (PyThreadState *scan = interp->threads.head; scan != NULL; scan = scan->next) {
        if (scan->thread_id == thread_id) {
            return scan;
        }
    }
    return NULL;
}

static Py_ssize_t
retrace_visible_frame_count(_PyInterpreterFrame *frame)
{
    Py_ssize_t size = 0;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (retrace_frame_is_visible(scan)) {
            size++;
        }
    }
    return size;
}

static void
retrace_fill_instruction_counters(RetraceU64BufferObject *buffer,
                                  _PyInterpreterFrame *frame,
                                  Py_ssize_t drop)
{
    Py_ssize_t index = 0;
    Py_ssize_t visible_index = 0;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (retrace_frame_is_visible(scan)) {
            if (visible_index++ < drop) {
                continue;
            }
            buffer->ob_item[index++] = retrace_frame_instruction_counter(scan);
        }
    }
}

static PyObject *
retrace_instruction_counters_delta(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    _PyInterpreterFrame *frame = retrace_thread_state_frame(tstate, tstate);
    while (frame != NULL && !retrace_frame_is_visible(frame)) {
        frame = frame->previous;
    }

    Py_ssize_t current_size = 0;
    Py_ssize_t new_count = 0;
    int found_common = 0;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (!retrace_frame_is_visible(scan)) {
            continue;
        }

        uint64_t counter = retrace_frame_instruction_counter(scan);
        if (!found_common && scan->retrace_last_instruction_counter == counter) {
            found_common = 1;
            new_count = current_size;
        }
        current_size++;
    }
    if (!found_common) {
        new_count = current_size;
    }

    Py_ssize_t common_count = current_size - new_count;
    RetraceU64BufferObject *delta = retrace_u64buffer_new(1 + new_count);
    if (delta == NULL) {
        return NULL;
    }
    delta->ob_item[0] = (uint64_t)common_count;

    Py_ssize_t copied = 0;
    for (_PyInterpreterFrame *scan = frame;
         scan != NULL && copied < new_count;
         scan = scan->previous)
    {
        if (!retrace_frame_is_visible(scan)) {
            continue;
        }

        uint64_t counter = retrace_frame_instruction_counter(scan);
        scan->retrace_last_instruction_counter = counter;
        delta->ob_item[1 + copied] = counter;
        copied++;
    }

    return (PyObject *)delta;
}

typedef struct {
    RetraceU64BufferObject *buffer;
    PyObject *owned;
} RetraceCounterSequence;

static int
retrace_counter_sequence_init(RetraceCounterSequence *sequence, PyObject *value)
{
    sequence->buffer = NULL;
    sequence->owned = NULL;

    if (PyObject_TypeCheck(value, &RetraceU64Buffer_Type)) {
        sequence->buffer = (RetraceU64BufferObject *)value;
        return 0;
    }

    sequence->owned = (PyObject *)retrace_u64buffer_from_sequence(
        value, "counters must be a sequence");
    if (sequence->owned == NULL) {
        return -1;
    }
    sequence->buffer = (RetraceU64BufferObject *)sequence->owned;
    return 0;
}

static void
retrace_counter_sequence_clear(RetraceCounterSequence *sequence)
{
    Py_XDECREF(sequence->owned);
}

static uint64_t
retrace_counter_sequence_get(RetraceCounterSequence *sequence,
                             Py_ssize_t index)
{
    return sequence->buffer->ob_item[index];
}

static Py_ssize_t
retrace_common_counters_prefix_length(_PyInterpreterFrame *frame,
                                      RetraceCounterSequence *counters)
{
    Py_ssize_t index = 0;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (!retrace_frame_is_visible(scan)) {
            continue;
        }
        if (index >= Py_SIZE(counters->buffer)) {
            return index;
        }

        uint64_t counter = retrace_counter_sequence_get(counters, index);
        if (counter != retrace_frame_instruction_counter(scan)) {
            return index;
        }
        index++;
    }
    return index;
}

static void
retrace_clear_replay_checkpoint(PyInterpreterState *interp)
{
    interp->retrace_replay_checkpoint_armed = 0;
    interp->retrace_replay_checkpoint_thread_id = 0;
    interp->retrace_replay_checkpoint_top = 0;
    Py_CLEAR(interp->retrace_replay_checkpoint_counters);
    Py_CLEAR(interp->retrace_replay_checkpoint_callback);
}

static int
retrace_replay_checkpoint_matches(PyInterpreterState *interp,
                                  _PyInterpreterFrame *frame)
{
    RetraceU64BufferObject *target =
        (RetraceU64BufferObject *)interp->retrace_replay_checkpoint_counters;
    if (target == NULL) {
        return 0;
    }

    Py_ssize_t target_size = Py_SIZE(target);
    Py_ssize_t index = 0;
    for (_PyInterpreterFrame *scan = frame; scan != NULL; scan = scan->previous) {
        if (!retrace_frame_is_visible(scan)) {
            continue;
        }
        if (index >= target_size) {
            return 0;
        }
        if (target->ob_item[index] != retrace_frame_instruction_counter(scan)) {
            return 0;
        }
        index++;
    }
    return index == target_size;
}

PyDoc_STRVAR(retrace_instruction_counters_doc,
"instruction_counters($module, thread_id=None, drop=0, /)\n"
"--\n"
"\n"
"Return a thread's visible Python frame instruction counters.\n"
"\n"
"drop omits that many leading counters from the returned sequence.");

PyDoc_STRVAR(retrace_instruction_counters_delta_doc,
"instruction_counters_delta($module, /)\n"
"--\n"
"\n"
"Return [common_count, *new_counters] for the current thread's visible\n"
"instruction counters.");

static PyObject *
retrace_instruction_counters(PyObject *module, PyObject *args)
{
    PyThreadState *current_tstate = _PyThreadState_GET();
    unsigned long thread_id = current_tstate->thread_id;
    PyObject *thread_id_arg = Py_None;
    Py_ssize_t drop = 0;

    if (!PyArg_ParseTuple(args, "|On:instruction_counters",
                          &thread_id_arg, &drop))
    {
        return NULL;
    }
    if (drop < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "instruction_counters drop must be non-negative");
        return NULL;
    }
    if (thread_id_arg != Py_None) {
        thread_id = PyLong_AsUnsignedLong(thread_id_arg);
        if (thread_id == (unsigned long)-1 && PyErr_Occurred()) {
            return NULL;
        }
    }

    _PyRuntimeState *runtime = &_PyRuntime;
    PyInterpreterState *interp = current_tstate->interp;
    RetraceU64BufferObject *buffer = NULL;

    RETRACE_HEAD_LOCK(runtime);
    PyThreadState *target_tstate = retrace_find_thread_state(interp, thread_id);
    if (target_tstate == NULL) {
        RETRACE_HEAD_UNLOCK(runtime);
        PyErr_Format(PyExc_LookupError, "unknown thread id: %lu", thread_id);
        return NULL;
    }
    _PyInterpreterFrame *frame =
        retrace_thread_state_frame(current_tstate, target_tstate);
    Py_ssize_t visible_count = retrace_visible_frame_count(frame);
    Py_ssize_t size = visible_count > drop ? visible_count - drop : 0;
    RETRACE_HEAD_UNLOCK(runtime);

    buffer = retrace_u64buffer_new(size);
    if (buffer == NULL) {
        return NULL;
    }

    RETRACE_HEAD_LOCK(runtime);
    target_tstate = retrace_find_thread_state(interp, thread_id);
    if (target_tstate == NULL) {
        RETRACE_HEAD_UNLOCK(runtime);
        Py_DECREF(buffer);
        PyErr_Format(PyExc_LookupError, "unknown thread id: %lu", thread_id);
        return NULL;
    }
    frame = retrace_thread_state_frame(current_tstate, target_tstate);
    if (retrace_visible_frame_count(frame) != visible_count) {
        RETRACE_HEAD_UNLOCK(runtime);
        Py_DECREF(buffer);
        PyErr_SetString(PyExc_RuntimeError, "thread frame stack changed");
        return NULL;
    }
    retrace_fill_instruction_counters(buffer, frame, drop);
    RETRACE_HEAD_UNLOCK(runtime);

    return (PyObject *)buffer;
}

PyDoc_STRVAR(retrace_common_counters_prefix_length_doc,
"common_counters_prefix_length($module, counters, thread_id=None, /)\n"
"--\n"
"\n"
"Return the common prefix length for counters and a thread's counters.");

static PyObject *
retrace_common_counters_prefix_length_method(PyObject *module, PyObject *args)
{
    PyThreadState *current_tstate = _PyThreadState_GET();
    PyObject *counters_arg;
    unsigned long thread_id = current_tstate->thread_id;

    if (!PyArg_ParseTuple(args, "O|k:common_counters_prefix_length",
                          &counters_arg, &thread_id))
    {
        return NULL;
    }

    RetraceCounterSequence counters;
    if (retrace_counter_sequence_init(&counters, counters_arg) < 0) {
        return NULL;
    }

    _PyRuntimeState *runtime = &_PyRuntime;
    PyInterpreterState *interp = current_tstate->interp;
    Py_ssize_t prefix_length;

    RETRACE_HEAD_LOCK(runtime);
    PyThreadState *target_tstate = retrace_find_thread_state(interp, thread_id);
    if (target_tstate == NULL) {
        RETRACE_HEAD_UNLOCK(runtime);
        retrace_counter_sequence_clear(&counters);
        PyErr_Format(PyExc_LookupError, "unknown thread id: %lu", thread_id);
        return NULL;
    }
    _PyInterpreterFrame *frame =
        retrace_thread_state_frame(current_tstate, target_tstate);
    prefix_length = retrace_common_counters_prefix_length(frame, &counters);
    RETRACE_HEAD_UNLOCK(runtime);

    retrace_counter_sequence_clear(&counters);
    return PyLong_FromSsize_t(prefix_length);
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

    PyObject *old_callback = interp->retrace_thread_yield_callback;
    interp->retrace_thread_yield_callback = Py_XNewRef(callback);
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
    PyObject *callback = tstate->interp->retrace_thread_yield_callback;
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

    PyObject *old_callback = interp->retrace_thread_resume_callback;
    interp->retrace_thread_resume_callback = Py_XNewRef(callback);
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
    PyObject *callback = tstate->interp->retrace_thread_resume_callback;
    if (callback == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(callback);
}

PyDoc_STRVAR(retrace_set_replay_checkpoint_doc,
"set_replay_checkpoint($module, thread_id, counters, callback, /)\n"
"--\n"
"\n"
"Invoke callback() when thread_id reaches the given instruction counters.\n"
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
                        "(thread_id, counters, callback)");
        return NULL;
    }

    unsigned long thread_id = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(args, 0));
    if (thread_id == (unsigned long)-1 && PyErr_Occurred()) {
        return NULL;
    }

    PyObject *callback = PyTuple_GET_ITEM(args, 2);
    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError,
                        "replay checkpoint callback must be callable");
        return NULL;
    }

    RetraceU64BufferObject *counters =
        retrace_u64buffer_from_sequence(
            PyTuple_GET_ITEM(args, 1),
            "checkpoint counters must be a sequence");
    if (counters == NULL) {
        return NULL;
    }
    if (Py_SIZE(counters) == 0) {
        Py_DECREF(counters);
        PyErr_SetString(PyExc_ValueError,
                        "checkpoint counters must not be empty");
        return NULL;
    }

    PyObject *callback_ref = Py_NewRef(callback);
    retrace_clear_replay_checkpoint(interp);
    interp->retrace_replay_checkpoint_thread_id = thread_id;
    interp->retrace_replay_checkpoint_top = counters->ob_item[0];
    interp->retrace_replay_checkpoint_counters = (PyObject *)counters;
    interp->retrace_replay_checkpoint_callback = callback_ref;
    interp->retrace_replay_checkpoint_armed = 1;

    Py_RETURN_NONE;
}

static _PyInterpreterFrame *
retrace_current_thread_frame(PyThreadState *tstate)
{
    if (tstate->retrace_thread_callback_active &&
        tstate->retrace_thread_callback_frame != NULL)
    {
        return tstate->retrace_thread_callback_frame;
    }
    if (tstate->cframe == NULL) {
        return NULL;
    }
    return tstate->cframe->current_frame;
}

static void
retrace_call_thread_callback(PyThreadState *tstate,
                             PyObject *callback,
                             _PyInterpreterFrame *frame)
{
    if (callback == NULL || tstate->retrace_thread_callback_active) {
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

    tstate->retrace_thread_callback_active = 1;
    tstate->retrace_thread_callback_frame = frame;

    PyObject *result = PyObject_CallNoArgs(callback);
    if (result == NULL) {
        PyErr_WriteUnraisable(callback);
    }
    else {
        Py_DECREF(result);
    }

    tstate->retrace_thread_callback_frame = NULL;
    tstate->retrace_thread_callback_active = 0;

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
    if (tstate == NULL) {
        return;
    }

    PyInterpreterState *interp = tstate->interp;
    if (interp->retrace_thread_resume_callback == NULL) {
        return;
    }

    tstate->retrace_thread_resume_pending = 1;
    tstate->retrace_thread_resume_frame = retrace_current_thread_frame(tstate);
}

void
_PyRetrace_DeliverThreadResumeCallback(PyThreadState *tstate)
{
    if (tstate == NULL || !tstate->retrace_thread_resume_pending) {
        return;
    }
    if (tstate->retrace_thread_callback_active) {
        return;
    }

    PyInterpreterState *interp = tstate->interp;
    _PyInterpreterFrame *frame = tstate->retrace_thread_resume_frame;

    PyObject *callback = interp->retrace_thread_resume_callback;
    if (callback == NULL) {
        tstate->retrace_thread_resume_pending = 0;
        tstate->retrace_thread_resume_frame = NULL;
        return;
    }

    tstate->retrace_thread_resume_pending = 0;
    tstate->retrace_thread_resume_frame = NULL;
    retrace_call_thread_callback(tstate, callback, frame);
}

void
_PyRetrace_DeliverThreadYieldCallback(PyThreadState *tstate)
{
    if (tstate == NULL || tstate->retrace_thread_callback_active) {
        return;
    }

    PyObject *callback = tstate->interp->retrace_thread_yield_callback;
    if (callback == NULL) {
        return;
    }

    retrace_call_thread_callback(
        tstate, callback, retrace_current_thread_frame(tstate));
}

int
_PyRetrace_CheckReplayCheckpoint(PyThreadState *tstate,
                                 _PyInterpreterFrame *frame)
{
    if (tstate == NULL || frame == NULL) {
        return 0;
    }

    PyInterpreterState *interp = tstate->interp;
    if (!interp->retrace_replay_checkpoint_armed ||
        interp->retrace_replay_checkpoint_callback_active ||
        interp->retrace_replay_checkpoint_thread_id != tstate->thread_id)
    {
        return 0;
    }
    if (interp->retrace_replay_checkpoint_top !=
            retrace_frame_instruction_counter(frame))
    {
        return 0;
    }
    if (!retrace_replay_checkpoint_matches(interp, frame)) {
        return 0;
    }

    PyObject *callback = interp->retrace_replay_checkpoint_callback;
    if (callback == NULL) {
        retrace_clear_replay_checkpoint(interp);
        return 0;
    }
    callback = Py_NewRef(callback);
    retrace_clear_replay_checkpoint(interp);

    interp->retrace_replay_checkpoint_callback_active = 1;
    interp->retrace_replay_checkpoint_callback_tstate = tstate;
    interp->retrace_replay_checkpoint_callback_frame = frame;

    PyObject *result = PyObject_CallNoArgs(callback);

    interp->retrace_replay_checkpoint_callback_frame = NULL;
    interp->retrace_replay_checkpoint_callback_tstate = NULL;
    interp->retrace_replay_checkpoint_callback_active = 0;

    Py_DECREF(callback);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}


static PyMethodDef retrace_methods[] = {
    {"instruction_counters", retrace_instruction_counters, METH_VARARGS,
     retrace_instruction_counters_doc},
    {"instruction_counters_delta", retrace_instruction_counters_delta,
     METH_NOARGS, retrace_instruction_counters_delta_doc},
    {"common_counters_prefix_length",
     retrace_common_counters_prefix_length_method, METH_VARARGS,
     retrace_common_counters_prefix_length_doc},
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
    if (PyModule_AddType(module, &RetraceU64Buffer_Type) < 0) {
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
    .m_name = "retrace",
    .m_doc = "Retrace CPython probe support.",
    .m_size = 0,
    .m_methods = retrace_methods,
    .m_slots = retrace_slots,
};

PyMODINIT_FUNC
PyInit_retrace(void)
{
    return PyModuleDef_Init(&retracemodule);
}
