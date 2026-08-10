#include "ggml-backend-dl.h"

#ifdef _WIN32

dl_handle * dl_load_library(const fs::path & path) {
    // suppress error dialogs for missing DLLs
    DWORD old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    SetErrorMode(old_mode | SEM_FAILCRITICALERRORS);

    HMODULE handle = LoadLibraryW(path.wstring().c_str());

    SetErrorMode(old_mode);

    return handle;
}

void * dl_get_sym(dl_handle * handle, const char * name) {
    DWORD old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    SetErrorMode(old_mode | SEM_FAILCRITICALERRORS);

    void * p = (void *) GetProcAddress(handle, name);

    SetErrorMode(old_mode);

    return p;
}

const char * dl_error() {
    return "";
}

#else

/* HYPONOIA PATCH 1 of 2 — see vendored/llama.cpp/HYPONOIA-PATCHES.md.
   Hyponoia registers every ggml backend statically, so runtime .so loading is
   dead code by construction. Left in, it puts dlopen/dlsym in the binary's
   undefined dynamic symbols — a property the shipped binary does not have
   today (`nm -D --undefined-only build/c/hyponoia | grep -c dlopen` = 0) —
   breaks `-static`, and trips scripts/security-vendored.sh. */
dl_handle * dl_load_library(const fs::path & path) {
    (void) path;
    return nullptr;
}

void * dl_get_sym(dl_handle * handle, const char * name) {
    (void) handle; (void) name;
    return nullptr;
}

const char * dl_error() {
    return "dynamic backend loading is compiled out";
}

#endif
