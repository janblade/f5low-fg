#pragma once

// f5low.dll's version: MAJOR.MINOR.PATCH.BUILD+COMMIT, e.g. 1.0.0.173+ebad7c9.
// MAJOR.MINOR.PATCH are raised here by hand for a release. build.ps1 adds the build number (this repo's commit count)
// and the short commit (with -dirty when tracked files have uncommitted changes), writes them to _version_build.h for
// the compiler and puts the whole version in the DLL's version resource, which OptiScaler shows. A build without
// build.ps1 reports build 0, commit "local".
#define XEFG_CUDA_VERSION_MAJOR 1
#define XEFG_CUDA_VERSION_MINOR 1
#define XEFG_CUDA_VERSION_PATCH 0

#if __has_include("_version_build.h")
#include "_version_build.h"
#endif

#ifndef XEFG_CUDA_VERSION_TEXT
#define XEFG_CUDA_VERSION_STR2(x) #x
#define XEFG_CUDA_VERSION_STR(x) XEFG_CUDA_VERSION_STR2(x)
#define XEFG_CUDA_VERSION_TEXT                                                                                         \
    XEFG_CUDA_VERSION_STR(XEFG_CUDA_VERSION_MAJOR)                                                                     \
    "." XEFG_CUDA_VERSION_STR(XEFG_CUDA_VERSION_MINOR) "." XEFG_CUDA_VERSION_STR(XEFG_CUDA_VERSION_PATCH) ".0+local"
#endif
