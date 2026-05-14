#ifndef Py_INTERNAL_RETRACE_H
#define Py_INTERNAL_RETRACE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_frame.h"         // _PyFrame_RetraceCoordinate()
#if PY_VERSION_HEX >= 0x030E0000
#include "pycore_interpframe.h"   // _PyInterpreterFrame helpers
#include "pycore_ceval.h"         // _Py_HandlePending()
#endif
#include "pycore_interp.h"        // PyInterpreterState
#if PY_VERSION_HEX >= 0x030D0000
#include "pycore_pyhash.h"        // _Py_HashPointerRaw()
#endif
#include "pycore_pystate.h"       // PyThreadState
#include "pycore_retrace_identity_hash.h" // identity hash table
#include "pycore_retrace_mixers.h" // _PyRetrace_Mix64()

#include <stdlib.h>
#include <string.h>

#define _PY_RETRACE_ROOT_SEED_ENV "RETRACE_ROOT_SEED"
#define _PY_RETRACE_DEFAULT_ROOT_SEED "retrace"
#define _PY_RETRACE_CALL_ORDINAL_HASH_TAG (UINT64_C(1) << 63)
#define _PY_RETRACE_COORDINATE_HASH_MASK \
    (_PY_RETRACE_CALL_ORDINAL_HASH_TAG - UINT64_C(1))

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
    (void)frame;
}

static inline void
_PyFrame_RetraceInitializeLastCoordinate(_PyInterpreterFrame *frame)
{
    frame->retrace.last_call_ordinal =
        _PyFrame_RETRACE_LAST_CALL_ORDINAL_UNSET;
    frame->retrace.last_instruction_counter =
        _PyFrame_RETRACE_LAST_INSTRUCTION_COUNTER_UNSET;
    frame->retrace.last_delta_space = NULL;
}

static inline void
_PyFrame_RetraceResetLastCoordinate(_PyInterpreterFrame *frame)
{
    _PyFrame_RetraceInitializeLastCoordinate(frame);
}

static inline uint64_t
_PyFrame_RetraceCoordinate(_PyInterpreterFrame *frame)
{
    return frame->retrace.instruction_counter;
}

static inline void
_PyFrame_RetraceCoordinateJump(
    _PyInterpreterFrame *frame,
    _Py_CODEUNIT *from,
    _Py_CODEUNIT *to)
{
    (void)frame;
    (void)from;
    (void)to;
}

#if PY_VERSION_HEX >= 0x030D0000
static inline _PyInterpreterFrame *
_PyRetrace_PyThreadStateCurrentFrame(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return NULL;
    }
    return tstate->current_frame;
}
#endif

static inline int
_PyRetrace_FrameIsVisible(_PyInterpreterFrame *frame)
{
    return frame != NULL &&
#if PY_VERSION_HEX >= 0x030C0000
           frame->owner != FRAME_OWNED_BY_CSTACK &&
#endif
           frame->retrace.space != NULL &&
           !_PyFrame_IsIncomplete(frame);
}

static inline uint64_t
_PyRetrace_FrameCoordinate(_PyInterpreterFrame *frame)
{
    return _PyFrame_RetraceCoordinate(frame);
}

static inline uint64_t
_PyRetrace_FrameCallOrdinal(_PyInterpreterFrame *frame)
{
    return frame->retrace.current_call_ordinal;
}

static inline uint32_t
_PyRetrace_FrameSpaceId(_PyInterpreterFrame *frame)
{
    return frame->retrace.space == NULL ?
        _PyFrame_RETRACE_SPACE_ID_ROOT : frame->retrace.space->space_id;
}

static inline void
_PyRetrace_InitializeSpaceState(
    _PyRetraceThreadSpaceState *space,
    uint32_t space_id,
    uint64_t root_hash)
{
    space->space_id = space_id;
    space->seen = 0;
    space->depth = 0;
    space->root_call_ordinal = 0;
    space->last_delta_root_call_ordinal =
        _PyFrame_RETRACE_LAST_CALL_ORDINAL_UNSET;
    space->root_coordinate_hash = root_hash;
    space->next = space;
}

static inline uint64_t
_PyRetrace_SpaceRootCoordinateHash(
    PyThreadState *tstate,
    uint32_t space_id)
{
    (void)space_id;
    uint64_t hash = tstate == NULL || tstate->retrace.thread_id == 0 ?
        _PyRetrace_InitialRootCoordinateHash() : tstate->retrace.thread_id;
    return _PyRetrace_NormalizeCoordinateHash(hash);
}

static inline _PyRetraceThreadSpaceState *
_PyRetrace_FindThreadSpace(PyThreadState *tstate, uint32_t space_id)
{
    if (tstate == NULL || tstate->retrace.last_space == NULL) {
        return NULL;
    }
    _PyRetraceThreadSpaceState *first =
        tstate->retrace.last_space->next;
    for (_PyRetraceThreadSpaceState *space = first; ;
         space = space->next)
    {
        if (space->space_id == space_id) {
            return space;
        }
        if (space == tstate->retrace.last_space) {
            break;
        }
    }
    return NULL;
}

static inline int
_PyRetrace_ThreadOwnsSpace(
    PyThreadState *tstate,
    _PyRetraceThreadSpaceState *candidate)
{
    if (candidate == NULL) {
        return 1;
    }
    if (tstate == NULL || tstate->retrace.last_space == NULL) {
        return 0;
    }
    _PyRetraceThreadSpaceState *first =
        tstate->retrace.last_space->next;
    for (_PyRetraceThreadSpaceState *space = first; ;
         space = space->next)
    {
        if (space == candidate) {
            return 1;
        }
        if (space == tstate->retrace.last_space) {
            break;
        }
    }
    return 0;
}

static inline _PyRetraceThreadSpaceState *
_PyRetrace_GetThreadSpace(PyThreadState *tstate, uint32_t space_id)
{
    _PyRetraceThreadSpaceState *space =
        _PyRetrace_FindThreadSpace(tstate, space_id);
    if (space != NULL) {
        return space;
    }
    if (tstate == NULL) {
        return NULL;
    }
    space = PyMem_RawCalloc(1, sizeof(*space));
    if (space == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    _PyRetrace_InitializeSpaceState(
        space, space_id, _PyRetrace_SpaceRootCoordinateHash(tstate, space_id));
    space->next = tstate->retrace.last_space->next;
    tstate->retrace.last_space->next = space;
    tstate->retrace.last_space = space;
    return space;
}

static inline int
_PyRetrace_SetCurrentSpaceById(PyThreadState *tstate, uint32_t space_id)
{
    if (tstate == NULL) {
        return 0;
    }
    _PyRetraceThreadSpaceState *space =
        _PyRetrace_GetThreadSpace(tstate, space_id);
    if (space == NULL) {
        return -1;
    }
    tstate->retrace.current_space = space;
    tstate->retrace.inherited_space_id = space_id;
    if (space->depth == 0) {
        tstate->retrace.call_ordinal_ptr = &space->root_call_ordinal;
    }
    return 0;
}

static inline uint64_t
_PyRetrace_MixFrameCoordinateHash(
    uint64_t hash,
    uint64_t instruction_counter,
    uint64_t call_ordinal)
{
    hash = _PyRetrace_NormalizeCoordinateHash(
        _PyRetrace_Mix64(
            hash, instruction_counter & _PY_RETRACE_COORDINATE_HASH_MASK));
    if (call_ordinal != 0) {
        hash = _PyRetrace_NormalizeCoordinateHash(
            _PyRetrace_Mix64(hash,
                             _PY_RETRACE_CALL_ORDINAL_HASH_TAG |
                             call_ordinal));
    }
    return hash;
}

static inline _PyRetraceThreadSpaceState *
_PyRetrace_CurrentSpace(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return NULL;
    }
    if (tstate->retrace.current_space == NULL ||
        tstate->retrace.current_space->space_id !=
            tstate->retrace.inherited_space_id)
    {
        if (_PyRetrace_SetCurrentSpaceById(
                tstate, tstate->retrace.inherited_space_id) < 0)
        {
            return NULL;
        }
    }
    return tstate->retrace.current_space;
}

static inline _PyInterpreterFrame *
_PyRetrace_NearestVisibleFrame(_PyInterpreterFrame *frame)
{
    while (frame != NULL) {
        if (_PyRetrace_FrameIsVisible(frame)) {
            return frame;
        }
        frame = frame->previous;
    }
    return NULL;
}

static inline _PyInterpreterFrame *
_PyRetrace_NearestVisibleFrameInSpace(
    _PyInterpreterFrame *frame,
    _PyRetraceThreadSpaceState *space)
{
    while (frame != NULL) {
        if (_PyRetrace_FrameIsVisible(frame) &&
            frame->retrace.space == space)
        {
            return frame;
        }
        frame = frame->previous;
    }
    return NULL;
}

static inline int
_PyRetrace_FrameIsCallbackTransparent(_PyInterpreterFrame *frame)
{
    (void)frame;
    return 0;
}

static inline int
_PyRetrace_FrameIsFreshCoordinateParent(_PyInterpreterFrame *frame)
{
    (void)frame;
    return 0;
}

static inline _PyInterpreterFrame *
_PyRetrace_CurrentThreadFrame(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return NULL;
    }
    if (tstate->retrace.thread_callback_active &&
        (tstate->retrace.current_space == NULL ||
         tstate->retrace.current_space->space_id !=
            _PyFrame_RETRACE_SPACE_ID_ROOT))
    {
        return tstate->retrace.thread_callback_frame;
    }
#if PY_VERSION_HEX >= 0x030D0000
    return _PyRetrace_PyThreadStateCurrentFrame(tstate);
#else
    if (tstate->cframe == NULL) {
        return NULL;
    }
    return tstate->cframe->current_frame;
#endif
}

static inline _PyInterpreterFrame *
_PyRetrace_PinThreadCallbackFrame(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame)
{
    if (tstate == NULL) {
        return NULL;
    }
    _PyInterpreterFrame *previous =
        tstate->retrace.thread_pending_callback_frame;
    if (frame != NULL && !tstate->retrace.thread_callback_active) {
        tstate->retrace.thread_pending_callback_frame = frame;
    }
    return previous;
}

static inline void
_PyRetrace_RestoreThreadCallbackFrame(
    PyThreadState *tstate,
    _PyInterpreterFrame *previous)
{
    if (tstate == NULL || tstate->retrace.thread_callback_active) {
        return;
    }
    tstate->retrace.thread_pending_callback_frame = previous;
}

#if PY_VERSION_HEX >= 0x030E0000
static inline int
_PyRetrace_HandlePending(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    _PyInterpreterFrame *previous =
        _PyRetrace_PinThreadCallbackFrame(tstate, frame);
    int err = _Py_HandlePending(tstate);
    _PyRetrace_RestoreThreadCallbackFrame(tstate, previous);
    return err;
}
#endif

static inline uint64_t
_PyRetrace_RootCoordinateHash(
    PyThreadState *tstate,
    _PyRetraceThreadSpaceState *space)
{
    if (space == NULL) {
        return _PyRetrace_SpaceRootCoordinateHash(
            tstate, _PyFrame_RETRACE_SPACE_ID_ROOT);
    }
    uint64_t hash =
        _PyRetrace_NormalizeCoordinateHash(space->root_coordinate_hash);
    if (space->root_call_ordinal != 0) {
        hash = _PyRetrace_MixFrameCoordinateHash(
            hash, 0, space->root_call_ordinal);
    }
    return hash;
}

static inline uint64_t
_PyRetrace_FrameCoordinateHashInSpace(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame,
    _PyRetraceThreadSpaceState *space)
{
    frame = _PyRetrace_NearestVisibleFrameInSpace(frame, space);
    if (frame == NULL) {
        return _PyRetrace_RootCoordinateHash(tstate, space);
    }
    uint64_t hash = _PyRetrace_FrameCoordinateHashInSpace(
        tstate, frame->previous, space);
    return _PyRetrace_MixFrameCoordinateHash(
        hash,
        _PyRetrace_FrameCoordinate(frame),
        _PyRetrace_FrameCallOrdinal(frame));
}

static inline uint64_t
_PyRetrace_FrameCoordinateHash(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame)
{
    frame = _PyRetrace_NearestVisibleFrame(frame);
    if (frame == NULL) {
        _PyRetraceThreadSpaceState *space = _PyRetrace_CurrentSpace(tstate);
        return _PyRetrace_RootCoordinateHash(tstate, space);
    }
    return _PyRetrace_FrameCoordinateHashInSpace(
        tstate, frame, frame->retrace.space);
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
    _PyInterpreterFrame *frame,
    _PyRetraceThreadSpaceState *space)
{
    frame = _PyRetrace_NearestVisibleFrameInSpace(frame, space);
    if (frame == NULL) {
        return;
    }
    _PyRetrace_MixVisibleFrameCoordinates(hash, frame->previous, space);
    *hash = _PyRetrace_MixFrameCoordinateHash(
        *hash,
        _PyRetrace_FrameCoordinate(frame),
        _PyRetrace_FrameCallOrdinal(frame));
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
        _PyRetraceThreadSpaceState *space =
            _PyRetrace_CurrentSpace(parent);
        _PyRetrace_MixVisibleFrameCoordinates(
            &hash, _PyRetrace_CurrentThreadFrame(parent), space);
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
    tstate->retrace.thread_started = 0;
    tstate->retrace.thread_start_pending = 0;
    tstate->retrace.thread_resume_pending = 0;
    _PyRetrace_InitializeSpaceState(
        &tstate->retrace.root_space,
        _PyFrame_RETRACE_SPACE_ID_ROOT,
        _PyRetrace_InitialRootCoordinateHash());
    tstate->retrace.current_space = &tstate->retrace.root_space;
    tstate->retrace.last_space = &tstate->retrace.root_space;
    tstate->retrace.call_ordinal_ptr =
        &tstate->retrace.root_space.root_call_ordinal;
    tstate->retrace.inherited_space_id = _PyFrame_RETRACE_SPACE_ID_ROOT;
    tstate->retrace.thread_callback_active = 0;
    tstate->retrace.thread_callback_frame = NULL;
    tstate->retrace.thread_pending_callback_frame = NULL;
}

static inline void
_PyRetrace_ClearThreadState(PyThreadState *tstate)
{
    if (tstate == NULL) {
        return;
    }
    _PyRetraceThreadSpaceState *root = &tstate->retrace.root_space;
    _PyRetraceThreadSpaceState *space = root->next;
    while (space != NULL && space != root) {
        _PyRetraceThreadSpaceState *next = space->next;
        PyMem_RawFree(space);
        space = next;
    }
    root->next = root;
    tstate->retrace.last_space = root;
    tstate->retrace.current_space = root;
    tstate->retrace.call_ordinal_ptr = &root->root_call_ordinal;
}

static inline void
_PyRetrace_ClearSpaceCallbacks(PyInterpreterState *interp)
{
    if (interp == NULL) {
        return;
    }
    _PyRetraceSpaceCallbackState *entry = interp->retrace.space_callbacks;
    while (entry != NULL) {
        _PyRetraceSpaceCallbackState *next = entry->next;
        Py_XDECREF(entry->thread_start_callback);
        Py_XDECREF(entry->thread_yield_callback);
        Py_XDECREF(entry->thread_resume_callback);
        Py_XDECREF(entry->call_at_coordinates);
        Py_XDECREF(entry->call_at_callback);
        Py_XDECREF(entry->call_at_overshoot_callback);
        PyMem_RawFree(entry);
        entry = next;
    }
    interp->retrace.space_callbacks = NULL;
    interp->retrace.call_at_extra_armed = 0;
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

    child->retrace.thread_started = parent == NULL;
    child->retrace.thread_start_pending = 0;
    child->retrace.thread_resume_pending = 0;
    child->retrace.thread_id =
        _PyRetrace_UniqueThreadId(
            child->interp, child, _PyRetrace_ThreadIdSeed(parent));
    child->retrace.root_space.root_coordinate_hash = child->retrace.thread_id;
    uint32_t inherited_space_id =
        parent == NULL || parent->retrace.current_space == NULL ?
        _PyFrame_RETRACE_SPACE_ID_ROOT :
        parent->retrace.current_space->space_id;
    child->retrace.inherited_space_id = inherited_space_id;
    (void)_PyRetrace_SetCurrentSpaceById(child, inherited_space_id);
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
    return _PyRetrace_NearestVisibleFrame(
               _PyRetrace_CurrentThreadFrame(tstate)) != NULL;
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

static inline int
_PyRetrace_ActivateFrame(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    if (frame == NULL) {
        return 0;
    }
    if (frame->retrace.space != NULL) {
        if (_PyRetrace_ThreadOwnsSpace(tstate, frame->retrace.space)) {
            return 0;
        }
        _PyRetraceFrameState_Init(&frame->retrace);
    }
    _PyRetraceThreadSpaceState *space = _PyRetrace_CurrentSpace(tstate);
    if (space == NULL) {
        return -1;
    }
    uint64_t *ordinal_ptr = tstate == NULL ?
        NULL : tstate->retrace.call_ordinal_ptr;
    frame->retrace.space = space;
    frame->retrace.current_call_ordinal =
        ordinal_ptr == NULL ? 0 : *ordinal_ptr;
    frame->retrace.previous_call_ordinal_ptr = ordinal_ptr;
    frame->retrace.instruction_active = 0;
    frame->retrace.instruction_may_activate_python = 0;
    frame->retrace.coordinate_hash =
        _PyFrame_RETRACE_COORDINATE_HASH_UNSET;
    if (tstate != NULL) {
        tstate->retrace.call_ordinal_ptr =
            &frame->retrace.current_call_ordinal;
    }
    space->seen = 1;
    if (space->depth != UINT32_MAX) {
        space->depth++;
    }
    return 0;
}

static inline int
_PyRetrace_OpcodeMayActivatePython(int opcode)
{
    (void)opcode;
    return 1;
}

static inline void
_PyRetrace_BeginInstruction(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame,
    int opcode)
{
    (void)tstate;
    if (!_PyRetrace_FrameIsVisible(frame)) {
        return;
    }
    frame->retrace.instruction_active = 1;
    frame->retrace.instruction_may_activate_python =
        _PyRetrace_OpcodeMayActivatePython(opcode) ? 1 : 0;
}

static inline void
_PyRetrace_EndInstruction(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    (void)tstate;
    if (!_PyRetrace_FrameIsVisible(frame) ||
        !frame->retrace.instruction_active)
    {
        return;
    }
    frame->retrace.instruction_active = 0;
    frame->retrace.instruction_counter++;
    if (frame->retrace.instruction_may_activate_python) {
        frame->retrace.current_call_ordinal = 0;
    }
    frame->retrace.instruction_may_activate_python = 0;
    frame->retrace.coordinate_hash =
        _PyFrame_RETRACE_COORDINATE_HASH_UNSET;
}

static inline void
_PyRetrace_DeactivateFrame(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    if (frame == NULL || frame->retrace.space == NULL) {
        return;
    }
    if (!_PyRetrace_ThreadOwnsSpace(tstate, frame->retrace.space)) {
        _PyRetraceFrameState_Init(&frame->retrace);
        return;
    }
    uint64_t *previous = frame->retrace.previous_call_ordinal_ptr;
    if (tstate != NULL &&
        tstate->retrace.call_ordinal_ptr ==
            &frame->retrace.current_call_ordinal)
    {
        tstate->retrace.call_ordinal_ptr = previous;
    }
    if (previous != NULL) {
        ++*previous;
    }
    if (frame->retrace.space->depth > 0) {
        frame->retrace.space->depth--;
    }
    frame->retrace.space = NULL;
    frame->retrace.previous_call_ordinal_ptr = NULL;
    frame->retrace.current_call_ordinal = 0;
    frame->retrace.instruction_active = 0;
    frame->retrace.instruction_may_activate_python = 0;
    frame->retrace.coordinate_hash =
        _PyFrame_RETRACE_COORDINATE_HASH_UNSET;
    if (tstate != NULL && tstate->retrace.call_ordinal_ptr == NULL &&
        tstate->retrace.current_space != NULL)
    {
        tstate->retrace.call_ordinal_ptr =
            &tstate->retrace.current_space->root_call_ordinal;
    }
}

static inline void
_PyRetrace_LeaveFrame(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    _PyRetrace_EndInstruction(tstate, frame);
    _PyRetrace_DeactivateFrame(tstate, frame);
}

static inline void
_PyRetrace_SuspendFrame(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    _PyRetrace_EndInstruction(tstate, frame);
    _PyRetrace_DeactivateFrame(tstate, frame);
}

static inline void
_PyRetrace_CopyFrameStateForNewFrame(
    _PyRetraceFrameState *dest,
    const _PyRetraceFrameState *src)
{
    (void)src;
    _PyRetraceFrameState_Init(dest);
}

PyAPI_FUNC(void) _PyRetrace_NoteThreadResume(PyThreadState *tstate);
PyAPI_FUNC(void) _PyRetrace_DeliverThreadResumeCallback(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame);
PyAPI_FUNC(void) _PyRetrace_DeliverThreadYieldCallback(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame);
PyAPI_FUNC(int) _PyRetrace_CheckCallAt(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_RETRACE_H */
