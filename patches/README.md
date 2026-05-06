# Patch Stacks

Store CPython patch series here.

Patch lookup order for version `3.12.8`:

1. `patches/3.12.8/*.patch`
2. `patches/3.12/*.patch`

When a patch directory contains `series.toml`, that manifest owns the supported
Python version range and the patch application order. Without a manifest,
patches are applied by filename order.

`retrace_module = "builtin"` records that the patch stack registers `retrace`
in CPython's built-in module table rather than shipping a separate extension
module.

Patch files should be generated with `git format-patch` from a clean CPython
release branch. They should contain changes to upstream CPython files only.
Retrace-owned files that are copied into the checkout live under
`cpython-overlay/` at the repository root.
