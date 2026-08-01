/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 Fernando Sahmkow */
/*
 * HAND-WRITTEN. Replaces what `--backend c-common-header` emits.
 *
 * The filename is fixed: the generated `whiteout_flakes.h` hardcodes
 * `#include "whiteout_c_common.h"`. So are the two struct names, which the
 * generated module TU uses in its signatures. Everything else here differs
 * from the generated version, deliberately:
 *
 *   1. The free functions carry a `whiteout_flakes_` prefix. Upstream names
 *      them `whiteout_Bytes_free` / `whiteout_CString_free` — the same
 *      symbols WhiteoutLib's own C ABI exports from its copy of this file.
 *      Two shared libraries can each export them and the loader picks one;
 *      two *static* libraries in one binary is a duplicate-symbol link
 *      error, and a published sys crate links statically by default. A
 *      dependency graph pulling in both `whiteout` and `whiteoutflakes`
 *      is entirely plausible, so the names cannot collide.
 *
 *   2. The shared-math accessors (`whiteout_Vector3f_*`, `whiteout_Matrix44f_*`,
 *      ~460 lines) are gone. Nothing in the flakes surface references them —
 *      math crosses this ABI as flat floats via the shim TU — and they
 *      would collide with WhiteoutLib's copies for the same reason.
 *
 * If the flakes surface ever grows a call that needs the math handles,
 * regenerate the upstream common TU into a separate file rather than
 * reinstating it here.
 *
 * Struct layouts are byte-identical to upstream's, so `bindings/rust`'s
 * `RawBytes` / `RawCString` mirrors stay valid.
 */

#ifndef WHITEOUT_C_COMMON_H
#define WHITEOUT_C_COMMON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Heap-allocated byte buffer. `data` is owned by the wrapper; free via
 * `whiteout_flakes_Bytes_free`. An absent buffer has `_owner == NULL`. */
typedef struct {
    const uint8_t* data;
    size_t         size;
    void*          _owner;   /* opaque; passed back to free() */
} whiteout_Bytes;

void whiteout_flakes_Bytes_free(whiteout_Bytes buf);

/* Heap-allocated null-terminated string. Free via
 * `whiteout_flakes_CString_free`. */
typedef struct {
    const char* chars;
    size_t      length;
    void*       _owner;
} whiteout_CString;

void whiteout_flakes_CString_free(whiteout_CString str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WHITEOUT_C_COMMON_H */
