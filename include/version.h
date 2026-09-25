// version.h — firmware version metadata, compiled in at build time.

#pragma once

constexpr const char *FIRMWARE_VERSION = "0.1.0";

// __DATE__ / __TIME__ are compiler-substituted at the point of translation
// (main.cpp / rest_routes.cpp), so the printable build timestamp is built
// from them where needed rather than stored as a single constant here.
