// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN. Replaces what `--backend c-common` emits — see the header
// comment in whiteout_c_common.h for why the symbols are prefixed and why
// the shared-math accessors are absent.
//
// The generated module TU allocates these buffers through its own
// anonymous-namespace wrapBytes / wrapCString helpers; this file only owns
// the release half, which is the part callers link against.

#include "whiteout_c_common.h"

#include <string>
#include <vector>

extern "C" {

void whiteout_flakes_Bytes_free(whiteout_Bytes buf) {
    // `_owner` is the std::vector the wrapper heap-allocated; `data` points
    // into it. A null owner means a borrowed or absent buffer — nothing to
    // release.
    delete static_cast<std::vector<unsigned char>*>(buf._owner);
}

void whiteout_flakes_CString_free(whiteout_CString str) {
    delete static_cast<std::string*>(str._owner);
}

} // extern "C"
