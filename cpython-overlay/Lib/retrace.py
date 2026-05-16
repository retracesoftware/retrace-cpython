"""Public Python helpers for the native Retrace probe module."""

from __future__ import annotations

from builtins import callable as _is_callable

import _retrace


_MISSING = object()


ThreadHandoff = _retrace.ThreadHandoff


class CoordinateSpace:
    """A per-thread Retrace coordinate space handle."""

    __slots__ = ("_id",)

    def __init__(self, space_id=_MISSING):
        self._id = (
            _retrace.allocate_space_id()
            if space_id is _MISSING
            else space_id
        )

    @property
    def id(self):
        return self._id

    def coordinates(self, thread_id=None, drop=0):
        return _retrace.coordinates(thread_id, drop, self._id)

    def thread_delta(self):
        return _retrace.thread_delta(self._id)

    def hash(self):
        return _retrace.hash(self._id)

    def run(self, callable, /, *args, **kwargs):
        if not _is_callable(callable):
            raise TypeError("space.run argument must be callable")
        return _retrace.run_in_space(self._id, callable, *args, **kwargs)

    def wrap(self, callable, /):
        if not _is_callable(callable):
            raise TypeError("space wrapper argument must be callable")
        return _retrace.wrap_for_space(self._id, callable)

    def call_at(self, thread_id, coordinates=_MISSING, callback=_MISSING,
                overshoot_callback=None, /):
        if thread_id is None and coordinates is _MISSING and callback is _MISSING:
            if overshoot_callback is not None:
                raise TypeError(
                    "call_at(None) does not accept "
                    "overshoot_callback")
            return _retrace.call_at(None, self._id)
        if coordinates is _MISSING or callback is _MISSING:
            raise TypeError(
                "call_at expects None or "
                "(thread_id, coordinates, callback[, overshoot_callback])")

        hit_callback = _excluded_callback(callback, "call_at", False)
        miss_callback = _excluded_callback(
            overshoot_callback, "call_at overshoot", True)
        return _retrace.call_at(
            thread_id, coordinates, hit_callback, miss_callback, self._id)


root_space = CoordinateSpace(None)
disabled_space = CoordinateSpace(1)

coordinates = root_space.coordinates
thread_delta = root_space.thread_delta
hash = root_space.hash
exclude = disabled_space.wrap
include = root_space.wrap
disable = disabled_space.wrap
enable = root_space.wrap
run_disabled = disabled_space.run


def with_new_coordinates(callable, /, *args, **kwargs):
    return CoordinateSpace().run(callable, *args, **kwargs)


def _excluded_callback(callback, name: str, allow_none: bool):
    if allow_none and callback is None:
        return None
    if not callable(callback):
        suffix = " or None" if allow_none else ""
        raise TypeError(f"{name} callback must be callable{suffix}")
    return exclude(callback)


class _Callbacks:
    __slots__ = ()

    def set_thread_switch(self, callback, space=None, /):
        """Set the bytecode thread-switch callback."""
        _retrace.set_thread_switch_callback(
            _excluded_callback(callback, "thread switch", True),
            _space_id(space),
        )

    @property
    def thread_switch(self):
        """Callback invoked before bytecode runs on a different thread."""
        return _retrace.get_thread_switch_callback()

    @thread_switch.setter
    def thread_switch(self, callback):
        self.set_thread_switch(callback)


callbacks = _Callbacks()


def _space_id(space):
    if isinstance(space, CoordinateSpace):
        return space.id
    return space


def call_at(thread_id, coordinates=_MISSING, callback=_MISSING,
            overshoot_callback=None, /):
    """Arm or clear a coordinate callback in root_space."""
    return root_space.call_at(
        thread_id, coordinates, callback, overshoot_callback)


__all__ = [
    "ThreadHandoff",
    "callbacks",
    "coordinates",
    "CoordinateSpace",
    "disable",
    "disabled_space",
    "enable",
    "exclude",
    "hash",
    "include",
    "root_space",
    "run_disabled",
    "with_new_coordinates",
    "call_at",
    "thread_delta",
]
