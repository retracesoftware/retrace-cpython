#include "Python.h"
#include "pycore_retrace.h"

void
_PyRetrace_NoteThreadResume(PyThreadState *tstate)
{
}

void
_PyRetrace_DeliverThreadResumeCallback(PyThreadState *tstate)
{
}

void
_PyRetrace_DeliverThreadYieldCallback(PyThreadState *tstate)
{
}

int
_PyRetrace_CheckReplayCheckpoint(PyThreadState *tstate,
                                 _PyInterpreterFrame *frame)
{
    return 0;
}
