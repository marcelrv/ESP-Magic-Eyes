// version.h — firmware version metadata, compiled in at build time.

#pragma once

constexpr const char *FIRMWARE_VERSION = "0.3.0-dev";

// Short git commit this image was built from, injected by
// scripts/git_sha.py as -DGIT_SHA. "unknown" when built outside a git
// checkout. Lets the update check recognise the "latest" channel build.
#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif
constexpr const char *FIRMWARE_GIT_SHA = GIT_SHA;

// __DATE__ / __TIME__ are compiler-substituted at the point of translation
// (main.cpp / rest_routes.cpp), so the printable build timestamp is built
// from them where needed rather than stored as a single constant here.
