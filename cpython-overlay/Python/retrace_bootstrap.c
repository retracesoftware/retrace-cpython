#include "Python.h"
#include "pycore_retrace.h"

void
_PyRetrace_NoteThreadResume(PyThreadState *tstate)
{
}

void
_PyRetrace_DeliverThreadResumeCallback(PyThreadState *tstate,
                                       _PyInterpreterFrame *frame)
{
}

void
_PyRetrace_DeliverThreadYieldCallback(PyThreadState *tstate,
                                      _PyInterpreterFrame *frame)
{
}

int
_PyRetrace_CheckCallAt(PyThreadState *tstate,
                       _PyInterpreterFrame *frame)
{
    return 0;
}
