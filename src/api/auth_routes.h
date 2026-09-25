// auth_routes.h — /api/auth/* (password protection, see net/auth.h).
//
//   GET  /api/auth/status   — public (no credentials needed), so every page
//        can warn when the device is unprotected:
//        {controlPassword, adminPassword, adminProtected, otaRestartRequired,
//         storageError}. storageError: the stored passwords couldn't be
//        read, so every protected request is refused (net/auth.cpp).
//        adminProtected is true when *either* password is set, since admin
//        routes fall back to the control password.
//   POST /api/auth/password — admin. {level: "control"|"admin", password}.
//        An empty password clears that level; otherwise 4–64 characters.
//        Sent in plain text (there's no TLS); only its hashes are stored.

#pragma once

class AsyncWebServer;

namespace AuthRoutes {

void registerRoutes(AsyncWebServer &server);

} // namespace AuthRoutes
