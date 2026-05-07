#ifndef Py_INTERNAL_RETRACE_IDENTITY_HASH_H
#define Py_INTERNAL_RETRACE_IDENTITY_HASH_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#ifndef _PY_RETRACE_IDENTITY_HASH_STANDALONE_TEST
#  include "pycore_interp.h"      // PyInterpreterState
#  include "pycore_pymem.h"       // PyMem_RawMalloc()
#endif
#include "pycore_retrace_mixers.h" // _PyRetrace_Mix64()

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define _PY_RETRACE_IDENTITY_HASH_MIN_CAPACITY ((size_t)16)
#define _PY_RETRACE_IDENTITY_HASH_PSL_MASK ((uintptr_t)7)
#define _PY_RETRACE_IDENTITY_HASH_POINTER_MASK \
    (~_PY_RETRACE_IDENTITY_HASH_PSL_MASK)
#define _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED ((size_t)7)

struct _PyRetraceIdentityHashTable {
    size_t mask;
    size_t used;
    size_t resize_at;
    uintptr_t *keys;
    Py_hash_t *values;
};

static inline size_t
_PyRetrace_IdentityHashCapacity(_PyRetraceIdentityHashTable *table)
{
    return table->mask + 1;
}

static inline size_t
_PyRetrace_IdentityHashResizeAt(size_t capacity)
{
    return (capacity * 5) / 8;
}

static inline uintptr_t
_PyRetrace_IdentityHashObjectKey(PyObject *obj)
{
    uintptr_t key = (uintptr_t)obj;
    assert(key != 0);
    assert((key & _PY_RETRACE_IDENTITY_HASH_PSL_MASK) == 0);
    return key;
}

static inline uintptr_t
_PyRetrace_IdentityHashEntryKey(uintptr_t entry)
{
    return entry & _PY_RETRACE_IDENTITY_HASH_POINTER_MASK;
}

static inline size_t
_PyRetrace_IdentityHashEntryPsl(uintptr_t entry)
{
    return (size_t)(entry & _PY_RETRACE_IDENTITY_HASH_PSL_MASK);
}

static inline uintptr_t
_PyRetrace_IdentityHashEntry(uintptr_t key, size_t psl)
{
    if (psl > _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED) {
        psl = _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED;
    }
    return key | (uintptr_t)psl;
}

static inline size_t
_PyRetrace_IdentityHashIndex(uintptr_t key, size_t mask)
{
    uint64_t hash = _PyRetrace_Mix64(
        (uint64_t)key, UINT64_C(0x9e3779b97f4a7c15));
    return (size_t)hash & mask;
}

static inline size_t
_PyRetrace_IdentityHashExactPsl(
    uintptr_t key,
    size_t index,
    size_t mask)
{
    size_t home = _PyRetrace_IdentityHashIndex(key, mask);
    return (index - home) & mask;
}

static inline int
_PyRetrace_IdentityHashEntryMatches(uintptr_t entry, uintptr_t key)
{
    return (entry ^ key) <= _PY_RETRACE_IDENTITY_HASH_PSL_MASK;
}

static inline _PyRetraceIdentityHashTable *
_PyRetrace_NewIdentityHashTable(size_t min_capacity)
{
    size_t capacity = _PY_RETRACE_IDENTITY_HASH_MIN_CAPACITY;
    while (capacity < min_capacity) {
        if (capacity > ((size_t)-1) / 2) {
            return NULL;
        }
        capacity *= 2;
    }
    if (capacity > ((size_t)-1) / sizeof(uintptr_t) ||
        capacity > ((size_t)-1) / sizeof(Py_hash_t))
    {
        return NULL;
    }

    _PyRetraceIdentityHashTable *table =
        (_PyRetraceIdentityHashTable *)PyMem_RawMalloc(sizeof(*table));
    if (table == NULL) {
        return NULL;
    }

    table->keys = (uintptr_t *)PyMem_RawMalloc(capacity * sizeof(uintptr_t));
    table->values = (Py_hash_t *)PyMem_RawMalloc(
        capacity * sizeof(Py_hash_t));
    if (table->keys == NULL || table->values == NULL) {
        PyMem_RawFree(table->keys);
        PyMem_RawFree(table->values);
        PyMem_RawFree(table);
        return NULL;
    }

    memset(table->keys, 0, capacity * sizeof(uintptr_t));
    table->mask = capacity - 1;
    table->used = 0;
    table->resize_at = _PyRetrace_IdentityHashResizeAt(capacity);
    return table;
}

static inline void
_PyRetrace_FreeIdentityHashTable(_PyRetraceIdentityHashTable *table)
{
    if (table == NULL) {
        return;
    }
    PyMem_RawFree(table->keys);
    PyMem_RawFree(table->values);
    PyMem_RawFree(table);
}

static inline void
_PyRetrace_IdentityHashTableInsertKnownRoom(
    _PyRetraceIdentityHashTable *table,
    uintptr_t key,
    Py_hash_t value)
{
    size_t index = _PyRetrace_IdentityHashIndex(key, table->mask);
    size_t psl = 0;

    for (;;) {
        uintptr_t entry = table->keys[index];
        if (entry == 0) {
            table->keys[index] = _PyRetrace_IdentityHashEntry(key, psl);
            table->values[index] = value;
            table->used++;
            return;
        }
        if (_PyRetrace_IdentityHashEntryMatches(entry, key)) {
            table->values[index] = value;
            return;
        }

        size_t entry_psl = _PyRetrace_IdentityHashEntryPsl(entry);
        if (entry_psl == _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED) {
            entry_psl = _PyRetrace_IdentityHashExactPsl(
                _PyRetrace_IdentityHashEntryKey(entry), index, table->mask);
        }
        if (entry_psl < psl) {
            uintptr_t entry_key = _PyRetrace_IdentityHashEntryKey(entry);
            Py_hash_t entry_value = table->values[index];

            table->keys[index] = _PyRetrace_IdentityHashEntry(key, psl);
            table->values[index] = value;

            key = entry_key;
            value = entry_value;
            psl = entry_psl;
        }

        index = (index + 1) & table->mask;
        psl++;
    }
}

static inline int
_PyRetrace_GrowIdentityHashTable(PyInterpreterState *interp)
{
    _PyRetraceIdentityHashTable *old_table = interp->retrace.identity_hashes;
    size_t new_capacity = old_table == NULL
        ? _PY_RETRACE_IDENTITY_HASH_MIN_CAPACITY
        : _PyRetrace_IdentityHashCapacity(old_table) * 2;
    if (old_table != NULL &&
        new_capacity <= _PyRetrace_IdentityHashCapacity(old_table))
    {
        return -1;
    }

    _PyRetraceIdentityHashTable *new_table =
        _PyRetrace_NewIdentityHashTable(new_capacity);
    if (new_table == NULL) {
        return -1;
    }

    if (old_table != NULL) {
        size_t old_capacity = _PyRetrace_IdentityHashCapacity(old_table);
        for (size_t i = 0; i < old_capacity; i++) {
            uintptr_t entry = old_table->keys[i];
            if (entry != 0) {
                _PyRetrace_IdentityHashTableInsertKnownRoom(
                    new_table,
                    _PyRetrace_IdentityHashEntryKey(entry),
                    old_table->values[i]);
            }
        }
        _PyRetrace_FreeIdentityHashTable(old_table);
    }

    interp->retrace.identity_hashes = new_table;
    return 0;
}

static inline int
_PyRetrace_LookupIdentityHash(
    PyInterpreterState *interp,
    PyObject *obj,
    Py_hash_t *value)
{
    _PyRetraceIdentityHashTable *table =
        interp == NULL ? NULL : interp->retrace.identity_hashes;
    if (table == NULL || table->used == 0) {
        return 0;
    }

    uintptr_t key = _PyRetrace_IdentityHashObjectKey(obj);
    size_t index = _PyRetrace_IdentityHashIndex(key, table->mask);
    size_t psl = 0;

    for (;;) {
        uintptr_t entry = table->keys[index];
        if (entry == 0) {
            return 0;
        }
        if (_PyRetrace_IdentityHashEntryMatches(entry, key)) {
            *value = table->values[index];
            return 1;
        }

        size_t entry_psl = _PyRetrace_IdentityHashEntryPsl(entry);
        if (entry_psl < psl) {
            if (entry_psl != _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED) {
                return 0;
            }
            entry_psl = _PyRetrace_IdentityHashExactPsl(
                _PyRetrace_IdentityHashEntryKey(entry), index, table->mask);
            if (entry_psl < psl) {
                return 0;
            }
        }

        index = (index + 1) & table->mask;
        psl++;
    }
}

static inline int
_PyRetrace_SetIdentityHash(
    PyInterpreterState *interp,
    PyObject *obj,
    Py_hash_t value)
{
    if (interp == NULL) {
        return -1;
    }

    _PyRetraceIdentityHashTable *table = interp->retrace.identity_hashes;
    if (table == NULL || table->used + 1 > table->resize_at) {
        if (_PyRetrace_GrowIdentityHashTable(interp) < 0) {
            return -1;
        }
        table = interp->retrace.identity_hashes;
    }

    uintptr_t key = _PyRetrace_IdentityHashObjectKey(obj);
    _PyRetrace_IdentityHashTableInsertKnownRoom(table, key, value);
    return 0;
}

static inline int
_PyRetrace_DeleteIdentityHash(PyInterpreterState *interp, PyObject *obj)
{
    _PyRetraceIdentityHashTable *table =
        interp == NULL ? NULL : interp->retrace.identity_hashes;
    if (table == NULL || table->used == 0) {
        return 0;
    }

    uintptr_t key = _PyRetrace_IdentityHashObjectKey(obj);
    size_t index = _PyRetrace_IdentityHashIndex(key, table->mask);
    size_t psl = 0;

    for (;;) {
        uintptr_t entry = table->keys[index];
        if (entry == 0) {
            return 0;
        }
        if (_PyRetrace_IdentityHashEntryMatches(entry, key)) {
            break;
        }

        size_t entry_psl = _PyRetrace_IdentityHashEntryPsl(entry);
        if (entry_psl < psl) {
            if (entry_psl != _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED) {
                return 0;
            }
            entry_psl = _PyRetrace_IdentityHashExactPsl(
                _PyRetrace_IdentityHashEntryKey(entry), index, table->mask);
            if (entry_psl < psl) {
                return 0;
            }
        }

        index = (index + 1) & table->mask;
        psl++;
    }

    table->used--;
    size_t hole = index;
    size_t scan = (hole + 1) & table->mask;
    for (;;) {
        uintptr_t entry = table->keys[scan];
        if (entry == 0) {
            table->keys[hole] = 0;
            return 1;
        }

        uintptr_t entry_key = _PyRetrace_IdentityHashEntryKey(entry);
        size_t entry_psl = _PyRetrace_IdentityHashEntryPsl(entry);
        if (entry_psl == _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED) {
            entry_psl = _PyRetrace_IdentityHashExactPsl(
                entry_key, scan, table->mask);
        }
        if (entry_psl == 0) {
            table->keys[hole] = 0;
            return 1;
        }

        table->keys[hole] =
            _PyRetrace_IdentityHashEntry(entry_key, entry_psl - 1);
        table->values[hole] = table->values[scan];
        hole = scan;
        scan = (scan + 1) & table->mask;
    }
}

static inline void
_PyRetrace_ClearIdentityHashTable(PyInterpreterState *interp)
{
    if (interp == NULL) {
        return;
    }
    _PyRetrace_FreeIdentityHashTable(interp->retrace.identity_hashes);
    interp->retrace.identity_hashes = NULL;
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_RETRACE_IDENTITY_HASH_H */
