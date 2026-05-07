#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cc_bin="${CC:-cc}"
tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/retrace-identity-hash.XXXXXX")"
trap 'rm -rf "${tmpdir}"' EXIT

"${cc_bin}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I"${repo_root}/cpython-overlay/Include" \
  -I"${repo_root}/cpython-overlay/Include/internal" \
  "${repo_root}/tests/unit/test_retrace_identity_hash.c" \
  -o "${tmpdir}/test_retrace_identity_hash"

"${tmpdir}/test_retrace_identity_hash"
