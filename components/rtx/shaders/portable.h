// glslc does not implement #pragma once, so shared shader headers need include guards.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_PORTABLE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_PORTABLE_H

// GLSL defines one translation unit per shader; C++ needs inline for shared function definitions.
// precise prevents fused arithmetic where shaders must agree on rounding, such as camera rays.
#ifdef __cplusplus
#define RTX_SHADER inline
#define RTX_PRECISE
#else
#define RTX_SHADER
#define RTX_PRECISE precise
#endif

#endif
