#include "library.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

void* openLibrary(const char* path, std::string& error) {
    HMODULE library = LoadLibraryA(path);
    if (library == nullptr) {
        error = "error " + std::to_string(GetLastError());
    }
    return library;
}

void* findLibrarySymbol(void* library, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}

#else

#include <dlfcn.h>

void* openLibrary(const char* path, std::string& error) {
    void* library = dlopen(path, RTLD_NOW);
    if (library == nullptr) {
        error = dlerror();
    }
    return library;
}

void* findLibrarySymbol(void* library, const char* name) { return dlsym(library, name); }

#endif
