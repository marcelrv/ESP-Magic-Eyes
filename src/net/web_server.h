// web_server.h — single global AsyncWebServer instance bootstrap.
//
// Owns raw server setup (static file serving from LittleFS, onNotFound
// captive-portal redirect). API route registration itself lives in
// src/api/rest_routes.* and is wired in during begin().

#pragma once

class AsyncWebServer;

namespace WebServer {

// Registers static file serving + API routes and starts the server on
// port 80. Call once from setup(), after LittleFS.begin() and
// NvsStore::begin()/WifiManager::begin().
void begin();

// Accessor for other modules (rest_routes.cpp) that need to register
// routes against the single global server instance.
AsyncWebServer &getServer();

} // namespace WebServer
