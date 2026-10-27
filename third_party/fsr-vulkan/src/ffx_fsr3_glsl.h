/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef FFX_FSR3_GLSL_H
#define FFX_FSR3_GLSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The FSR 3.1 upscaler's GLSL, embedded in the executable (ffx_fsr3_glsl.c): the text of a pass shader
 * ("ffx_fsr3upscaler_<pass>_pass.glsl") or of a header it includes ("fsr3upscaler/ffx_core.h" or just the file name),
 * NULL when unknown. */
const char *ffxFsr3GlslFile(const char *name, size_t *length);
/* A hash of all of it: part of the key of the host's compiled-shader cache. */
uint64_t ffxFsr3GlslHash(void);

/* Implemented by the host (runtime/nvn/vulkan/shaders.c): GLSL compute shader to SPIR-V words (malloc'd, NULL on error)
 * with -D macros ("NAME=value"), resolving #include through the given function. optimize: spirv-opt at the size level (the
 * OpNames are gone), else none. salt identifies the includes' contents for the host's disk cache. */
uint32_t *vk_glsl_to_spirv_ex(const char *src, size_t srclen, const char *name, const char *const *defs, int ndefs,
                              const char *(*include)(const char *name, size_t *length), uint64_t salt, int optimize, size_t *nwords);

#ifdef __cplusplus
}
#endif

#endif
