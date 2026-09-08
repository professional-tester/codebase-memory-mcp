// Compile vendored tree-sitter runtime as a single compilation unit.
// Source: tree-sitter v0.26.0 (DeusData fork)
//
// lib.c internally #includes all other runtime .c files, so we only
// need this one entry point. The runtime headers are at vendored/ts_runtime/src/.
//
// NOTE (v1.0.2 #9): the build cache keys on THIS file's content, not on the
// nested #includes below. Bump CBM_TS_RUNTIME_REV when changing any nested
// vendored/ts_runtime/src/*.c file so test/production builds pick it up.
#define CBM_TS_RUNTIME_REV 2
#include "vendored/ts_runtime/src/lib.c"
