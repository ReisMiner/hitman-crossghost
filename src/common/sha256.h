#pragma once

#include <string>

// Hashes a local file with CNG SHA-256. Returns false on any I/O or CNG error.
bool Sha256File(const std::wstring& path, std::string& hex);
