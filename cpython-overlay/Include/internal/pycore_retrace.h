#ifndef Py_INTERNAL_RETRACE_H
#define Py_INTERNAL_RETRACE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_frame.h"         // _PyFrame_RetraceCoordinate()
#include "pycore_interp.h"        // PyInterpreterState
#include "pycore_pystate.h"       // PyThreadState
#include "pycore_retrace_identity_hash.h" // identity hash table
#include "pycore_retrace_mixers.h" // _PyRetrace_Mix64()

#include <stdlib.h>
#include <string.h>

#define _PY_RETRACE_ROOT_SEED_ENV "RETRACE_ROOT_SEED"
#define _PY_RETRACE_DEFAULT_ROOT_SEED "retrace"

static inline uint64_t
_PyRetrace_NormalizeCoordinateHash(uint64_t hash)
{
    if (hash == _PyFrame_RETRACE_COORDINATE_HASH_UNSET) {
        return _PyFrame_RETRACE_COORDINATE_HASH_UNSET - 1;
    }
    return hash;
}

static inline uint64_t
_PyRetrace_InitialRootCoordinateHash(void)
{
    return _PyRetrace_NormalizeCoordinateHash(
        _PyRetrace_Mix64(UINT64_C(0x243f6a8885a308d3),
                         UINT64_C(0x13198a2e03707344)));
}

static inline uint64_t
_PyRetrace_NormalizeThreadId(uint64_t thread_id)
{
    thread_id = _PyRetrace_NormalizeCoordinateHash(thread_id);
    return thread_id == 0 ? UINT64_C(1) : thread_id;
}

static inline uint64_t
_PyRetrace_MixBytes(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *cursor = (const unsigned char *)data;
    hash = _PyRetrace_NormalizeCoordinateHash(
        _PyRetrace_Mix64(hash, (uint64_t)size));
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, cursor, sizeof(word));
        hash = _PyRetrace_NormalizeCoordinateHash(
            _PyRetrace_Mix64(hash, word));
        cursor += sizeof(uint64_t);
        size -= sizeof(uint64_t);
    }
    if (size != 0) {
        uint64_t tail = 0;
        memcpy(&tail, cursor, size);
        hash = _PyRetrace_NormalizeCoordinateHash(
            _PyRetrace_Mix64(hash, tail));
    }
    return hash;
}

static inline void
_PyFrame_RetraceResetCoordinateDepth(_PyInterpreterFrame *frame)
{
    frame->retrace.coordinate_depth = _PyFrame_RETRACE_COORDINATE_DEPTH_UNSET;
}

static inline void
_PyFrame_RetraceInitializeLastCoordinate(_PyInterpreterFrame *frame)
{
    frame->retrace.last_coordinate = _PyFrame_RETRACE_LAST_COORDINATE_UNSET;
}

static inline void
_PyFrame_RetraceResetLastCoordinate(_PyInterpreterFrame *frame)
{
    _PyFrame_RetraceInitializeLastCoordinate(frame);
}

static inline int64_t
_PyFrame_RetraceCoordinate(_PyInterpreterFrame *frame)
{
    return frame->retrace.coordinate_bias + (int64_t)_PyInterpreterFrame_LASTI(frame);
}

static inline void
_PyFrame_RetraceCoordinateJump(
    _PyInterpreterFrame *frame,
    _Py_CODEUNIT *from,
    _Py_CODEUNIT *to)
{
    PyCodeObject *code = frame->f_code;
    int from_offset = (int)(from - _PyCode_CODE(code));
    int to_offset = (int)(to - _PyCode_CODE(code));
    frame->retrace.coordinate_bias += (int64_t)from_offset + 1 - to_offset;
}

static inline int
_PyFrame_RetraceBumpParentCoordinate(_PyInterpreterFrame *frame)
{
    for (_PyInterpreterFrame *parent = frame->previous;
         parent != NULL;
         parent = parent->previous)
    {
#if PY_VERSION_HEX >= 0x030C0000
        if (parent->owner == FRAME_OWNED_BY_CSTACK) {
            continue;
        }
#endif
        if (!_PyFrame_IsIncomplete(parent)) {
            parent->retrace.coordinate_bias += 1;
            return 1;
        }
    }
    return 0;
}

static inline int
_PyRetrace_FrameIsVisible(_PyInterpreterFrame *frame)
{
    return frame != NULL &&
#if PY_VERSION_HEX >= 0x030C0000
           frame->owner != FRAME_OWNED_BY_CSTACK &&
#endif
           !_PyFrame_IsIncomplete(frame);
}

static inline uint64_t
_PyRetrace_FrameCoordinate(_PyInterpreterFrame *frame)
{
    int64_t coordinate = _PyFrame_RetraceCoordinate(frame);
    if (coordinate < 0) {
        return 0;
    }
    return (uint64_t)coordinate;
}

static inline uint64_t
_PyRetrace_RootCoordinateHash(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return _PyRetrace_InitialRootCoordinateHash();
    }
    return _PyRetrace_NormalizeCoordinateHash(
        tstate->retrace.root_coordinate_hash);
}

static inline _PyInterpreterFrame *
_PyRetrace_NearestVisibleFrame(_PyInterpreterFrame *frame)
{
    while (frame != NULL && !_PyRetrace_FrameIsVisible(frame)) {
        frame = frame->previous;
    }
    return frame;
}

static inline _PyInterpreterFrame *
_PyRetrace_CurrentThreadFrame(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return NULL;
    }
    if (tstate->cframe == NULL) {
        return NULL;
    }
    return tstate->cframe->current_frame;
}

static inline uint64_t
_PyRetrace_FrameBaseCoordinateHash(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame);

static inline uint64_t
_PyRetrace_ComputeFrameBaseCoordinateHash(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame)
{
    _PyInterpreterFrame *parent =
        _PyRetrace_NearestVisibleFrame(frame->previous);
    if (parent == NULL) {
        return _PyRetrace_RootCoordinateHash(tstate);
    }

    uint64_t parent_base =
        _PyRetrace_FrameBaseCoordinateHash(tstate, parent);
    return _PyRetrace_NormalizeCoordinateHash(
        _PyRetrace_Mix64(parent_base, _PyRetrace_FrameCoordinate(parent)));
}

static inline uint64_t
_PyRetrace_FrameBaseCoordinateHash(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame)
{
    frame = _PyRetrace_NearestVisibleFrame(frame);
    if (frame == NULL) {
        return _PyRetrace_RootCoordinateHash(tstate);
    }
    if (frame->retrace.coordinate_hash !=
        _PyFrame_RETRACE_COORDINATE_HASH_UNSET)
    {
        return frame->retrace.coordinate_hash;
    }

    frame->retrace.coordinate_hash =
        _PyRetrace_ComputeFrameBaseCoordinateHash(tstate, frame);
    return frame->retrace.coordinate_hash;
}

static inline void
_PyRetrace_RefreshFrameBaseCoordinateHash(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame)
{
    frame->retrace.coordinate_hash =
        _PyRetrace_ComputeFrameBaseCoordinateHash(tstate, frame);
}

static inline uint64_t
_PyRetrace_FrameCoordinateHash(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame)
{
    frame = _PyRetrace_NearestVisibleFrame(frame);
    if (frame == NULL) {
        return _PyRetrace_RootCoordinateHash(tstate);
    }
    return _PyRetrace_NormalizeCoordinateHash(
        _PyRetrace_Mix64(_PyRetrace_FrameBaseCoordinateHash(tstate, frame),
                         _PyRetrace_FrameCoordinate(frame)));
}

static inline uint64_t
_PyRetrace_CurrentCoordinateHash(PyThreadState *tstate)
{
    return _PyRetrace_FrameCoordinateHash(
        tstate, _PyRetrace_CurrentThreadFrame(tstate));
}

static inline void
_PyRetrace_MixVisibleFrameCoordinates(
    uint64_t *hash,
    _PyInterpreterFrame *frame)
{
    frame = _PyRetrace_NearestVisibleFrame(frame);
    if (frame == NULL) {
        return;
    }
    _PyRetrace_MixVisibleFrameCoordinates(hash, frame->previous);
    *hash = _PyRetrace_NormalizeCoordinateHash(
        _PyRetrace_Mix64(*hash, _PyRetrace_FrameCoordinate(frame)));
}

static inline uint64_t
_PyRetrace_RootThreadIdSeed(void)
{
    static const char domain[] = "retrace.root-seed.v2";
    const char *seed = getenv(_PY_RETRACE_ROOT_SEED_ENV);
    if (seed == NULL) {
        seed = _PY_RETRACE_DEFAULT_ROOT_SEED;
    }

    uint64_t hash = _PyRetrace_InitialRootCoordinateHash();
    hash = _PyRetrace_MixBytes(hash, domain, sizeof(domain) - 1);
    hash = _PyRetrace_MixBytes(hash, seed, strlen(seed));
    return _PyRetrace_NormalizeThreadId(hash);
}

static inline uint64_t
_PyRetrace_ThreadIdSeed(PyThreadState *parent)
{
    static const char domain[] = "retrace.thread-id.v2";
    uint64_t hash = _PyRetrace_InitialRootCoordinateHash();
    hash = _PyRetrace_MixBytes(hash, domain, sizeof(domain) - 1);
    if (parent == NULL || parent->retrace.thread_id == 0) {
        hash = _PyRetrace_Mix64(hash, _PyRetrace_RootThreadIdSeed());
    }
    else {
        hash = _PyRetrace_Mix64(hash, parent->retrace.thread_id);
        _PyRetrace_MixVisibleFrameCoordinates(
            &hash, _PyRetrace_CurrentThreadFrame(parent));
    }
    return _PyRetrace_NormalizeThreadId(hash);
}

static inline int
_PyRetrace_ThreadIdIsActive(
    PyInterpreterState *interp,
    PyThreadState *skip,
    uint64_t thread_id)
{
    if (thread_id == 0) {
        return 1;
    }
    for (PyThreadState *scan = interp->threads.head;
         scan != NULL;
         scan = scan->next)
    {
        if (scan != skip && scan->retrace.thread_id == thread_id) {
            return 1;
        }
    }
    return 0;
}

static inline uint64_t
_PyRetrace_UniqueThreadId(
    PyInterpreterState *interp,
    PyThreadState *skip,
    uint64_t thread_id)
{
    thread_id = _PyRetrace_NormalizeThreadId(thread_id);
    uint64_t retry = UINT64_C(0x9e3779b97f4a7c15);
    while (_PyRetrace_ThreadIdIsActive(interp, skip, thread_id)) {
        thread_id = _PyRetrace_NormalizeThreadId(
            _PyRetrace_Mix64(thread_id, retry));
        retry += UINT64_C(0x9e3779b97f4a7c15);
    }
    return thread_id;
}

static inline void
_PyRetrace_InitializeThreadState(
    PyInterpreterState *interp,
    PyThreadState *tstate)
{
    if (tstate == NULL) {
        return;
    }
    tstate->retrace.thread_id = 0;
    tstate->retrace.cpython_thread_ident = 0;
    tstate->retrace.root_coordinate = 0;
    tstate->retrace.last_root_coordinate = (uint64_t)-1;
    tstate->retrace.root_coordinate_hash = _PyRetrace_InitialRootCoordinateHash();
}

static inline void
_PyRetrace_ClearThreadState(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return;
    }
}

static inline void
_PyRetrace_SeedThreadState(PyThreadState *parent, PyThreadState *child)
{
    if (child == NULL || child->interp == NULL) {
        return;
    }
    if (parent == child) {
        parent = NULL;
    }

    child->retrace.thread_id =
        _PyRetrace_UniqueThreadId(
            child->interp, child, _PyRetrace_ThreadIdSeed(parent));
    child->retrace.root_coordinate_hash = child->retrace.thread_id;
}

static inline void
_PyRetrace_RegisterThreadIdent(PyThreadState *tstate, unsigned long ident)
{
    if (tstate == NULL) {
        return;
    }
    tstate->retrace.cpython_thread_ident = ident;
}

static inline PyObject *
_PyRetrace_ThreadIdentObject(PyThreadState *tstate)
{
    if (tstate == NULL || tstate->retrace.thread_id == 0) {
        PyErr_SetString(PyExc_RuntimeError, "no Retrace thread id");
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(
        (unsigned long long)tstate->retrace.thread_id);
}

static inline PyThreadState *
_PyRetrace_ThreadStateFromPublicIdent(
    PyInterpreterState *interp,
    unsigned long id)
{
    PyThreadState *native_match = NULL;
    PyThreadState *retrace_match = NULL;
    int native_count = 0;
    int retrace_count = 0;

    for (PyThreadState *scan = interp->threads.head;
         scan != NULL;
         scan = scan->next)
    {
        unsigned long native_id = scan->thread_id != 0 ?
            scan->thread_id : scan->retrace.cpython_thread_ident;
        if (native_id != 0 && native_id == id) {
            native_match = scan;
            native_count++;
        }
        if (scan->retrace.thread_id != 0 &&
            (unsigned long)scan->retrace.thread_id == id)
        {
            retrace_match = scan;
            retrace_count++;
        }
    }

    if (native_count == 1 &&
        (retrace_count == 0 || retrace_match == native_match))
    {
        return native_match;
    }
    if (native_count == 0 && retrace_count == 1) {
        return retrace_match;
    }
    return NULL;
}

static inline int
_PyRetrace_ThreadMatchesAsyncExcId(
    PyInterpreterState *interp,
    PyThreadState *target,
    unsigned long id)
{
    return target == _PyRetrace_ThreadStateFromPublicIdent(interp, id);
}

static inline unsigned long
_PyRetrace_CPythonThreadIdentFromPublicIdent(
    PyInterpreterState *interp,
    unsigned long id)
{
    PyThreadState *tstate = _PyRetrace_ThreadStateFromPublicIdent(interp, id);
    if (tstate == NULL) {
        return id;
    }
    if (tstate->thread_id != 0) {
        return tstate->thread_id;
    }
    if (tstate->retrace.cpython_thread_ident != 0) {
        return tstate->retrace.cpython_thread_ident;
    }
    return id;
}

static inline Py_hash_t
_PyRetrace_NormalizePyHash(Py_hash_t hash)
{
    if (hash == -1) {
        return -2;
    }
    return hash;
}

static inline Py_hash_t
_PyRetrace_RawPointerHash(const void *ptr)
{
    return _PyRetrace_NormalizePyHash(_Py_HashPointerRaw(ptr));
}

static inline int
_PyRetrace_CanCachePointerHash(const void *ptr)
{
    uintptr_t key = (uintptr_t)ptr;
    return key != 0 &&
           (key & _PY_RETRACE_IDENTITY_HASH_PSL_MASK) == 0;
}

static inline int
_PyRetrace_UseCoordinatePointerHash(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return 0;
    }
    if (tstate->retrace.thread_id == 0) {
        return 0;
    }
    return _PyRetrace_FrameIsVisible(_PyRetrace_CurrentThreadFrame(tstate));
}

static inline Py_hash_t
_PyRetrace_HashPointer(const void *ptr)
{
    if (!_PyRetrace_CanCachePointerHash(ptr)) {
        return _PyRetrace_RawPointerHash(ptr);
    }

    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate == NULL ? NULL : tstate->interp;
    Py_hash_t hash;
    if (interp != NULL &&
        _PyRetrace_LookupIdentityHash(interp, (PyObject *)ptr, &hash))
    {
        return hash;
    }

    if (_PyRetrace_UseCoordinatePointerHash(tstate)) {
        hash = _PyRetrace_NormalizePyHash(
            (Py_hash_t)_PyRetrace_CurrentCoordinateHash(tstate));
    }
    else {
        hash = _PyRetrace_RawPointerHash(ptr);
    }

    if (interp != NULL) {
        (void)_PyRetrace_SetIdentityHash(interp, (PyObject *)ptr, hash);
    }
    return hash;
}

static inline void
_PyRetrace_DeleteObjectIdentityHash(PyObject *obj)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate == NULL || tstate->interp == NULL ||
        !_PyRetrace_CanCachePointerHash(obj))
    {
        return;
    }
    (void)_PyRetrace_DeleteIdentityHash(tstate->interp, obj);
}

static inline void
_PyRetrace_ActivateFrame(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    int parent_state = _PyFrame_RetraceBumpParentCoordinate(frame);
    if (parent_state == 0 && tstate != NULL) {
        tstate->retrace.root_coordinate++;
        frame->retrace.coordinate_bias +=
            (int64_t)tstate->retrace.root_coordinate;
    }
    _PyRetrace_RefreshFrameBaseCoordinateHash(tstate, frame);
    _PyFrame_RetraceResetCoordinateDepth(frame);
}

PyAPI_FUNC(void) _PyRetrace_NoteThreadResume(PyThreadState *tstate);
PyAPI_FUNC(void) _PyRetrace_DeliverThreadResumeCallback(PyThreadState *tstate);
PyAPI_FUNC(void) _PyRetrace_DeliverThreadYieldCallback(PyThreadState *tstate);
PyAPI_FUNC(int) _PyRetrace_CheckReplayCheckpoint(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_RETRACE_H */
