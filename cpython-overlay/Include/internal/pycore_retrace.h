#ifndef Py_INTERNAL_RETRACE_H
#define Py_INTERNAL_RETRACE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_frame.h"         // _PyFrame_RetraceCoordinate()
#include "pycore_initconfig.h"    // _Py_get_xoption()
#include "pycore_interp.h"        // PyInterpreterState
#include "pycore_pystate.h"       // PyThreadState
#include "pycore_retrace_identity_hash.h" // identity hash table
#include "pycore_retrace_mixers.h" // _PyRetrace_Mix64()

#include <wchar.h>

static inline int
_PyRetrace_InitialCoordinateMode(PyInterpreterState *interp)
{
    const wchar_t *option =
        _Py_get_xoption(&interp->config.xoptions, L"retrace_coordinates");
    if (option == NULL) {
        return 0;
    }

    const wchar_t *separator = wcschr(option, L'=');
    const wchar_t *value = separator == NULL ? L"enabled" : separator + 1;
    if (wcscmp(value, L"disabled") == 0) {
        return -1;
    }
    return 0;
}

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
_PyRetrace_ThreadRootCoordinateHash(uint64_t thread_id)
{
    if (thread_id == 0) {
        return _PyRetrace_InitialRootCoordinateHash();
    }
    return _PyRetrace_NormalizeCoordinateHash(
        _PyRetrace_Mix64(_PyRetrace_InitialRootCoordinateHash(), thread_id));
}

static inline uint64_t
_PyRetrace_ReserveThreadId(PyInterpreterState *interp)
{
    if (interp == NULL) {
        return 0;
    }
    if (interp->retrace.next_thread_id == 0) {
        interp->retrace.next_thread_id = 1;
    }

    uint64_t thread_id = interp->retrace.next_thread_id++;
    if (interp->retrace.next_thread_id == 0) {
        interp->retrace.next_thread_id = 1;
    }
    return thread_id;
}

static inline int
_PyFrame_RetraceCoordinateDisabled(_PyInterpreterFrame *frame)
{
    return frame->retrace.last_coordinate == _PyFrame_RETRACE_COORDINATE_DISABLED;
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
    if (!_PyFrame_RetraceCoordinateDisabled(frame)) {
        _PyFrame_RetraceInitializeLastCoordinate(frame);
    }
}

static inline void
_PyFrame_RetraceDisableCoordinate(_PyInterpreterFrame *frame)
{
    frame->retrace.last_coordinate = _PyFrame_RETRACE_COORDINATE_DISABLED;
    _PyFrame_RetraceResetCoordinateDepth(frame);
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
_PyFrame_RetraceBumpParentCoordinate(
    _PyInterpreterFrame *frame,
    int inherit_disabled)
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
        if (_PyFrame_RetraceCoordinateDisabled(parent)) {
            if (inherit_disabled) {
                _PyFrame_RetraceDisableCoordinate(frame);
                return -1;
            }
            continue;
        }
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
           !_PyFrame_RetraceCoordinateDisabled(frame) &&
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
    if (tstate->retrace.thread_callback_active &&
        tstate->retrace.thread_callback_frame != NULL)
    {
        return tstate->retrace.thread_callback_frame;
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

static inline int
_PyRetrace_ThreadStateStartsDisabled(PyThreadState *parent)
{
    if (parent == NULL) {
        return 0;
    }
    if (parent->retrace.thread_id == 0 ||
        parent->retrace.coordinate_mode < 0)
    {
        return 1;
    }

    _PyInterpreterFrame *frame = _PyRetrace_CurrentThreadFrame(parent);
    return frame != NULL && !_PyRetrace_FrameIsVisible(frame);
}

static inline void
_PyRetrace_InitializeThreadState(
    PyInterpreterState *interp,
    PyThreadState *tstate)
{
    if (tstate == NULL) {
        return;
    }
    tstate->retrace.coordinate_mode = _PyRetrace_InitialCoordinateMode(interp);
    tstate->retrace.thread_id = 0;
    tstate->retrace.root_coordinate = 0;
    tstate->retrace.last_root_coordinate = (uint64_t)-1;
    tstate->retrace.root_coordinate_hash = _PyRetrace_ThreadRootCoordinateHash(0);
}

static inline void
_PyRetrace_SeedThreadState(PyThreadState *parent, PyThreadState *child)
{
    if (child == NULL || child->interp == NULL) {
        return;
    }
    if (_PyRetrace_ThreadStateStartsDisabled(parent)) {
        child->retrace.coordinate_mode = -1;
        child->retrace.thread_id = 0;
        child->retrace.root_coordinate_hash = _PyRetrace_ThreadRootCoordinateHash(0);
        return;
    }

    if (parent != NULL) {
        child->retrace.coordinate_mode = 0;
    }
    child->retrace.thread_id = _PyRetrace_ReserveThreadId(child->interp);
    child->retrace.root_coordinate_hash =
        _PyRetrace_ThreadRootCoordinateHash(child->retrace.thread_id);
}

static inline int
_PyRetrace_SuppressCoordinateBump(PyThreadState *tstate)
{
    if (tstate == NULL || tstate->retrace.thread_callback_active) {
        return 1;
    }
    PyInterpreterState *interp = tstate->interp;
    return interp != NULL && interp->retrace.replay_checkpoint_callback_active;
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
    if (tstate == NULL || _PyRetrace_SuppressCoordinateBump(tstate)) {
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
    int inherit_disabled = 1;
    if (tstate != NULL) {
        if (tstate->retrace.coordinate_mode < 0) {
            _PyFrame_RetraceDisableCoordinate(frame);
            return;
        }
        inherit_disabled = tstate->retrace.coordinate_mode <= 0;
    }
    if (_PyRetrace_SuppressCoordinateBump(tstate)) {
        _PyFrame_RetraceDisableCoordinate(frame);
        return;
    }

    int parent_state =
        _PyFrame_RetraceBumpParentCoordinate(frame, inherit_disabled);
    if (parent_state < 0) {
        _PyFrame_RetraceDisableCoordinate(frame);
        return;
    }
    if (parent_state == 0 && tstate != NULL) {
        tstate->retrace.root_coordinate++;
        tstate->retrace.root_coordinate_hash =
            _PyRetrace_NormalizeCoordinateHash(
                _PyRetrace_Mix64(tstate->retrace.root_coordinate_hash,
                                 tstate->retrace.root_coordinate));
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
