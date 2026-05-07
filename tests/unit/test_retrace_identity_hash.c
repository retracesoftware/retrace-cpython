#define Py_BUILD_CORE 1
#define _PY_RETRACE_IDENTITY_HASH_STANDALONE_TEST 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef intptr_t Py_hash_t;

#include "cpython/retrace_state.h"

typedef struct _is {
    _PyRetraceInterpreterState retrace;
} PyInterpreterState;

static size_t test_alloc_count;
static size_t test_free_count;
static long test_fail_after = -1;

static void *
test_raw_malloc(size_t size)
{
    if (test_fail_after >= 0) {
        if (test_fail_after == 0) {
            return NULL;
        }
        test_fail_after--;
    }
    void *ptr = malloc(size);
    if (ptr != NULL) {
        test_alloc_count++;
    }
    return ptr;
}

static void
test_raw_free(void *ptr)
{
    if (ptr != NULL) {
        test_free_count++;
        free(ptr);
    }
}

#define PyMem_RawMalloc test_raw_malloc
#define PyMem_RawFree test_raw_free

#include "pycore_retrace_identity_hash.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

typedef struct {
    uintptr_t word;
} FakeObject;

static FakeObject object_pool[70000];

static void
fail_at(const char *file, int line, const char *expr)
{
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    abort();
}

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fail_at(__FILE__, __LINE__, #expr); \
        } \
    } while (0)

static PyObject *
object_at(size_t index)
{
    CHECK(index < ARRAY_SIZE(object_pool));
    PyObject *obj = (PyObject *)&object_pool[index];
    CHECK(((uintptr_t)obj & _PY_RETRACE_IDENTITY_HASH_PSL_MASK) == 0);
    return obj;
}

static void
clear_interp(PyInterpreterState *interp)
{
    _PyRetrace_ClearIdentityHashTable(interp);
    memset(interp, 0, sizeof(*interp));
}

static void
check_lookup(
    PyInterpreterState *interp,
    PyObject *obj,
    int expected_found,
    Py_hash_t expected_value)
{
    Py_hash_t value = 0;
    int found = _PyRetrace_LookupIdentityHash(interp, obj, &value);
    CHECK(found == expected_found);
    if (expected_found) {
        CHECK(value == expected_value);
    }
}

static size_t
table_used(PyInterpreterState *interp)
{
    _PyRetraceIdentityHashTable *table = interp->retrace.identity_hashes;
    return table == NULL ? 0 : table->used;
}

static size_t
table_capacity(PyInterpreterState *interp)
{
    _PyRetraceIdentityHashTable *table = interp->retrace.identity_hashes;
    return table == NULL ? 0 : _PyRetrace_IdentityHashCapacity(table);
}

static void
collect_colliders(
    uintptr_t *keys,
    size_t count,
    size_t start_index,
    size_t mask,
    size_t home)
{
    size_t found = 0;
    for (size_t i = start_index; i < ARRAY_SIZE(object_pool) && found < count; i++) {
        uintptr_t key = (uintptr_t)object_at(i);
        if (_PyRetrace_IdentityHashIndex(key, mask) == home) {
            keys[found++] = key;
        }
    }
    CHECK(found == count);
}

static size_t
count_saturated_entries(PyInterpreterState *interp)
{
    _PyRetraceIdentityHashTable *table = interp->retrace.identity_hashes;
    CHECK(table != NULL);
    size_t saturated = 0;
    size_t capacity = _PyRetrace_IdentityHashCapacity(table);
    for (size_t i = 0; i < capacity; i++) {
        uintptr_t entry = table->keys[i];
        if (entry != 0 &&
            _PyRetrace_IdentityHashEntryPsl(entry) ==
                _PY_RETRACE_IDENTITY_HASH_PSL_SATURATED)
        {
            saturated++;
        }
    }
    return saturated;
}

static void
test_empty_lookup_and_delete(void)
{
    PyInterpreterState interp = {0};
    Py_hash_t value = 123;

    CHECK(_PyRetrace_LookupIdentityHash(&interp, object_at(1), &value) == 0);
    CHECK(value == 123);
    CHECK(_PyRetrace_DeleteIdentityHash(&interp, object_at(1)) == 0);
    CHECK(interp.retrace.identity_hashes == NULL);
}

static void
test_insert_lookup_update_delete(void)
{
    PyInterpreterState interp = {0};
    PyObject *a = object_at(10);
    PyObject *b = object_at(11);

    CHECK(_PyRetrace_SetIdentityHash(&interp, a, 42) == 0);
    CHECK(table_capacity(&interp) == _PY_RETRACE_IDENTITY_HASH_MIN_CAPACITY);
    CHECK(table_used(&interp) == 1);
    check_lookup(&interp, a, 1, 42);
    check_lookup(&interp, b, 0, 0);

    CHECK(_PyRetrace_SetIdentityHash(&interp, a, -1) == 0);
    CHECK(table_used(&interp) == 1);
    check_lookup(&interp, a, 1, -1);

    CHECK(_PyRetrace_DeleteIdentityHash(&interp, b) == 0);
    CHECK(table_used(&interp) == 1);
    CHECK(_PyRetrace_DeleteIdentityHash(&interp, a) == 1);
    CHECK(table_used(&interp) == 0);
    check_lookup(&interp, a, 0, 0);

    clear_interp(&interp);
}

static void
test_growth_and_full_verification(void)
{
    PyInterpreterState interp = {0};
    const size_t count = 5000;

    for (size_t i = 0; i < count; i++) {
        CHECK(_PyRetrace_SetIdentityHash(
            &interp, object_at(100 + i), (Py_hash_t)(i * 17 + 5)) == 0);
    }
    CHECK(table_used(&interp) == count);
    CHECK(table_capacity(&interp) > _PY_RETRACE_IDENTITY_HASH_MIN_CAPACITY);

    for (size_t i = 0; i < count; i++) {
        check_lookup(&interp, object_at(100 + i), 1, (Py_hash_t)(i * 17 + 5));
    }
    for (size_t i = 0; i < 200; i++) {
        check_lookup(&interp, object_at(20000 + i), 0, 0);
    }

    clear_interp(&interp);
}

static void
test_saturated_psl_and_backshift_delete(void)
{
    PyInterpreterState interp = {0};
    uintptr_t keys[10];
    collect_colliders(keys, ARRAY_SIZE(keys), 10000, 15, 3);

    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        CHECK(_PyRetrace_SetIdentityHash(
            &interp, (PyObject *)keys[i], (Py_hash_t)(1000 + i)) == 0);
    }
    CHECK(table_capacity(&interp) == 16);
    CHECK(table_used(&interp) == ARRAY_SIZE(keys));
    CHECK(count_saturated_entries(&interp) >= 3);

    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        check_lookup(&interp, (PyObject *)keys[i], 1, (Py_hash_t)(1000 + i));
    }

    CHECK(_PyRetrace_DeleteIdentityHash(&interp, (PyObject *)keys[0]) == 1);
    CHECK(_PyRetrace_DeleteIdentityHash(&interp, (PyObject *)keys[5]) == 1);
    CHECK(_PyRetrace_DeleteIdentityHash(&interp, (PyObject *)keys[9]) == 1);
    CHECK(table_used(&interp) == ARRAY_SIZE(keys) - 3);

    check_lookup(&interp, (PyObject *)keys[0], 0, 0);
    check_lookup(&interp, (PyObject *)keys[5], 0, 0);
    check_lookup(&interp, (PyObject *)keys[9], 0, 0);

    for (size_t i = 1; i < ARRAY_SIZE(keys) - 1; i++) {
        if (i != 5) {
            check_lookup(&interp, (PyObject *)keys[i], 1, (Py_hash_t)(1000 + i));
        }
    }

    uintptr_t missing[1];
    collect_colliders(missing, 1, 60000, 15, 3);
    CHECK(_PyRetrace_DeleteIdentityHash(&interp, (PyObject *)missing[0]) == 0);
    CHECK(table_used(&interp) == ARRAY_SIZE(keys) - 3);

    clear_interp(&interp);
}

static void
test_delete_all_and_reuse(void)
{
    PyInterpreterState interp = {0};
    const size_t count = 257;

    for (size_t i = 0; i < count; i++) {
        CHECK(_PyRetrace_SetIdentityHash(
            &interp, object_at(30000 + i), (Py_hash_t)(9000 + i)) == 0);
    }
    for (size_t i = 0; i < count; i += 2) {
        CHECK(_PyRetrace_DeleteIdentityHash(&interp, object_at(30000 + i)) == 1);
    }
    for (size_t i = 1; i < count; i += 2) {
        CHECK(_PyRetrace_DeleteIdentityHash(&interp, object_at(30000 + i)) == 1);
    }
    CHECK(table_used(&interp) == 0);

    for (size_t i = 0; i < count; i++) {
        check_lookup(&interp, object_at(30000 + i), 0, 0);
    }

    CHECK(_PyRetrace_SetIdentityHash(&interp, object_at(30500), 12345) == 0);
    CHECK(table_used(&interp) == 1);
    check_lookup(&interp, object_at(30500), 1, 12345);

    clear_interp(&interp);
}

static uint64_t
next_random(uint64_t *state)
{
    *state = *state * UINT64_C(6364136223846793005) +
             UINT64_C(1442695040888963407);
    return *state;
}

static void
test_deterministic_churn_against_reference(void)
{
    enum { KEY_COUNT = 4096, OP_COUNT = 25000 };
    PyInterpreterState interp = {0};
    unsigned char present[KEY_COUNT] = {0};
    Py_hash_t values[KEY_COUNT] = {0};
    size_t expected_used = 0;
    uint64_t rng = UINT64_C(0x123456789abcdef0);

    for (size_t op_index = 0; op_index < OP_COUNT; op_index++) {
        uint64_t random = next_random(&rng);
        size_t index = (size_t)(random % KEY_COUNT);
        PyObject *obj = object_at(40000 + index);
        int op = (int)((random >> 32) & 3);

        if (op == 0 || op == 1) {
            Py_hash_t value = (Py_hash_t)(random ^ (random >> 33));
            CHECK(_PyRetrace_SetIdentityHash(&interp, obj, value) == 0);
            if (!present[index]) {
                expected_used++;
            }
            present[index] = 1;
            values[index] = value;
        }
        else if (op == 2) {
            int removed = _PyRetrace_DeleteIdentityHash(&interp, obj);
            CHECK(removed == (int)present[index]);
            if (present[index]) {
                expected_used--;
            }
            present[index] = 0;
        }
        else {
            check_lookup(&interp, obj, present[index], values[index]);
        }

        if ((op_index % 997) == 0) {
            CHECK(table_used(&interp) == expected_used);
            for (size_t i = 0; i < KEY_COUNT; i += 113) {
                check_lookup(
                    &interp, object_at(40000 + i), present[i], values[i]);
            }
        }
    }

    CHECK(table_used(&interp) == expected_used);
    for (size_t i = 0; i < KEY_COUNT; i++) {
        check_lookup(&interp, object_at(40000 + i), present[i], values[i]);
    }

    clear_interp(&interp);
}

static void
test_allocation_failures_preserve_table(void)
{
    PyInterpreterState interp = {0};
    PyObject *first = object_at(50000);

    test_fail_after = 0;
    CHECK(_PyRetrace_SetIdentityHash(&interp, first, 11) == -1);
    CHECK(interp.retrace.identity_hashes == NULL);

    test_fail_after = 2;
    CHECK(_PyRetrace_SetIdentityHash(&interp, first, 11) == -1);
    CHECK(interp.retrace.identity_hashes == NULL);

    test_fail_after = -1;
    for (size_t i = 0; i < 10; i++) {
        CHECK(_PyRetrace_SetIdentityHash(
            &interp, object_at(50000 + i), (Py_hash_t)(700 + i)) == 0);
    }
    CHECK(table_capacity(&interp) == 16);
    CHECK(table_used(&interp) == 10);

    test_fail_after = 0;
    CHECK(_PyRetrace_SetIdentityHash(&interp, object_at(50020), 900) == -1);
    test_fail_after = -1;
    CHECK(table_capacity(&interp) == 16);
    CHECK(table_used(&interp) == 10);
    for (size_t i = 0; i < 10; i++) {
        check_lookup(&interp, object_at(50000 + i), 1, (Py_hash_t)(700 + i));
    }
    check_lookup(&interp, object_at(50020), 0, 0);

    clear_interp(&interp);
}

int
main(void)
{
    CHECK(sizeof(void *) == 8);
    CHECK(sizeof(Py_hash_t) == sizeof(void *));

    test_empty_lookup_and_delete();
    test_insert_lookup_update_delete();
    test_growth_and_full_verification();
    test_saturated_psl_and_backshift_delete();
    test_delete_all_and_reuse();
    test_deterministic_churn_against_reference();
    test_allocation_failures_preserve_table();

    CHECK(test_alloc_count == test_free_count);
    printf("retrace identity hash table tests passed\n");
    return 0;
}
