#pragma once

#include <atomic>
#include <string>

// Plain-HTTP POST to 127.0.0.1:port/path. Returns the response body, or "" on any failure,
// on timeout, or within 200 ms of *cancel turning true.
std::string loopbackPost(int port, const std::string& path, const std::string& body, int timeoutMs,
                         const std::atomic<bool>* cancel = nullptr);
