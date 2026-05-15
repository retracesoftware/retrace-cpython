# Coordinate Structure

Retrace coordinates are ordered streams of unsigned integer words. They name a
dynamic Python execution point inside one Retrace coordinate space. They are not
source locations, line numbers, or stable coordinates for unpatched CPython.

The public shape is:

```text
(
    frame_0_instruction_coordinate, frame_0_call_ordinal,
    frame_1_instruction_coordinate, frame_1_call_ordinal,
    ...
    frame_n_instruction_coordinate, frame_n_call_ordinal,
)
```

Frames are ordered from oldest visible frame to current visible frame. Every
visible frame contributes exactly two words, so the tuple length is always even.
The first word in a pair is the frame's instruction coordinate. The second word
is that frame's call ordinal.

## Instruction Coordinate

The instruction coordinate is the logical bytecode coordinate for the frame in
this exact patched interpreter build:

```c
frame->retrace.coordinate_bias + _PyInterpreterFrame_LASTI(frame)
```

Retrace adjusts `coordinate_bias` when CPython redirects the instruction pointer,
including branch and exception-handler jumps. Ordinary fallthrough bytecode
execution does not increment a separate global counter.

There is no separate "after this bytecode" coordinate. A callback boundary is
the coordinate of the bytecode CPython is about to execute. If a coordinate is
sampled from a C function called by a Python bytecode, the leaf Python frame may
still be in the bytecode that entered C.

## Call Ordinal

The call ordinal is the child-activation counter for a frame's current bytecode
instruction.

For each active frame:

```c
frame->retrace.current_call_ordinal = 0;
```

is set when the frame is activated and again before every bytecode callback
boundary. If one bytecode instruction activates multiple direct Python child
frames before the parent advances, those children are distinguished by the
parent frame's call ordinal while each child is active.

For example, if a parent is parked at instruction coordinate `12` and that
instruction invokes two Python callbacks:

```text
first callback:  (..., 12, 0, 0, 0)
second callback: (..., 12, 1, 0, 0)
parent next boundary: (..., 13, 0)
```

The child frame starts with its own call ordinal set to zero. The parent frame's
call ordinal carries the child-activation number.

## Parent And Root Slots

Frame activation saves a pointer to the current parent ordinal slot:

```c
frame->retrace.current_call_ordinal = 0;
frame->retrace.previous_call_ordinal_ptr = tstate->retrace.call_ordinal_ptr;
tstate->retrace.call_ordinal_ptr = &frame->retrace.current_call_ordinal;
```

When the frame returns, suspends, or unwinds, Retrace restores that parent slot
and increments the value it points to exactly once:

```c
if (frame->retrace.previous_call_ordinal_ptr != NULL) {
    ++*frame->retrace.previous_call_ordinal_ptr;
}
```

If a frame has no visible parent in the selected coordinate space, the parent
slot is the thread-space root call ordinal. The root ordinal is internal state;
it is not emitted as an extra coordinate pair.

## Spaces And Visibility

Coordinates are scoped to one `(thread, coordinate space)` pair.

If a thread has never used the requested space, `coordinates()` returns `None`.
If the thread has used the space but has no active visible frame in it,
`coordinates()` returns `()`.

Transparent Retrace callback frames are skipped. Coordinates, coordinate hashes,
and thread deltas observed inside such callbacks describe the pinned application
coordinate that caused the callback.

## Derived Streams

`thread_delta()` and `hash()` use the same root-to-leaf frame-pair stream as
`coordinates()`.

`thread_delta()` reports the unchanged prefix length followed by the changed
suffix. `hash()` hashes the same visible coordinate stream for the selected
thread and space; it does not use a different coordinate model.
