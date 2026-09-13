#pragma once

#include <string>

// Null when the library cannot be loaded, with the loader's reason in error.
void* openLibrary(const char* path, std::string& error);
void* findLibrarySymbol(void* library, const char* name);
