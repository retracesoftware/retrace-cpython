"""Public Python helpers for the native Retrace probe module."""

from __future__ import annotations

import _retrace


_MISSING = object()


coordinates = _retrace.coordinates
thread_delta = _retrace.thread_delta
hash = _retrace.hash
exclude = _retrace.exclude
include = _retrace.include
run_transparent = _retrace.run_transparent
ThreadHandoff = _retrace.ThreadHandoff


def _excluded_callback(callback, name: str, allow_none: bool):
    if allow_none and callback is None:
        return None
    if not callable(callback):
        suffix = " or None" if allow_none else ""
        raise TypeError(f"{name} callback must be callable{suffix}")
    return exclude(callback)


class _Callbacks:
    __slots__ = ()

    @property
    def thread_start(self):
        """Callback invoked once on a newly started Python thread."""
        return _retrace.get_thread_start_callback()

    @thread_start.setter
    def thread_start(self, callback):
        _retrace.set_thread_start_callback(
            _excluded_callback(callback, "thread start", True))

    @property
    def thread_yield(self):
        """Callback invoked before a Python thread yields the GIL."""
        return _retrace.get_thread_yield_callback()

    @thread_yield.setter
    def thread_yield(self, callback):
        _retrace.set_thread_yield_callback(
            _excluded_callback(callback, "thread yield", True))

    @property
    def thread_resume(self):
        """Callback invoked after a Python thread resumes from a yield."""
        return _retrace.get_thread_resume_callback()

    @thread_resume.setter
    def thread_resume(self, callback):
        _retrace.set_thread_resume_callback(
            _excluded_callback(callback, "thread resume", True))


callbacks = _Callbacks()


def call_at(thread_id, coordinates=_MISSING, callback=_MISSING,
            overshoot_callback=None, /):
    """Arm or clear a coordinate callback.

    Registered callbacks are wrapped with exclude(). Use include(callable)
    inside a callback to run application-visible work under the target frame.
    """
    if thread_id is None and coordinates is _MISSING and callback is _MISSING:
        if overshoot_callback is not None:
            raise TypeError(
                "call_at(None) does not accept "
                "overshoot_callback")
        return _retrace.call_at(None)
    if coordinates is _MISSING or callback is _MISSING:
        raise TypeError(
            "call_at expects None or "
            "(thread_id, coordinates, callback[, overshoot_callback])")

    hit_callback = _excluded_callback(callback, "call_at", False)
    miss_callback = _excluded_callback(
        overshoot_callback, "call_at overshoot", True)
    if miss_callback is None:
        return _retrace.call_at(
            thread_id, coordinates, hit_callback)
    return _retrace.call_at(
        thread_id, coordinates, hit_callback, miss_callback)


__all__ = [
    "ThreadHandoff",
    "callbacks",
    "coordinates",
    "exclude",
    "hash",
    "include",
    "run_transparent",
    "call_at",
    "thread_delta",
]
