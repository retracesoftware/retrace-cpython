#ifndef Py_INTERNAL_RETRACE_H
#define Py_INTERNAL_RETRACE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_frame.h"         // _PyFrame_RetraceInstructionCoordinate()

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
_PyRetrace_FrameInstructionCounter(_PyInterpreterFrame *frame)
{
    int64_t coordinate = _PyFrame_RetraceInstructionCoordinate(frame);
    if (coordinate < 0) {
        return 0;
    }
    return (uint64_t)coordinate;
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
