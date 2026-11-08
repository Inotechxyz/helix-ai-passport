// main/langstudy_builtin.h
// The offline fallback: a complete 10-lesson / 100-word LSPACK body compiled
// into the firmware. It is stored without an END line; callers finalize it so
// the CRC is always computed by the same code that validates generated packs.
#pragma once

#include "langstudy_model.h"

#include <stdbool.h>
#include <stddef.h>

// Raw LSPACK body (header + LESSON + WORD lines, no END). Points at static
// storage and is not NUL-terminated by contract; use out_len.
const char *ls_builtin_pack_body(size_t *out_len);

// Finalize and parse the built-in body into *out. Returns false on any error,
// in which case the firmware has no usable content and should stay offline.
bool ls_builtin_load(ls_pack_t *out);
