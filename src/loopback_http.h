#pragma once

#include <string>

// Plain-HTTP POST to 127.0.0.1:port/path. Returns the response body, or "" on any failure.
std::string loopbackPost(int port, const std::string& path, const std::string& body, int timeoutMs);
