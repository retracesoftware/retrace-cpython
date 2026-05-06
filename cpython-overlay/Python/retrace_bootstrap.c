#include "Python.h"
#include "pycore_retrace.h"

void
_PyRetrace_NoteThreadSwitch(PyThreadState *from_tstate,
                            PyThreadState *to_tstate)
{
}

void
_PyRetrace_DeliverThreadSwitchCallback(PyThreadState *tstate)
{
}

int
_PyRetrace_CheckReplayCheckpoint(PyThreadState *tstate,
                                 _PyInterpreterFrame *frame)
{
    return 0;
}
