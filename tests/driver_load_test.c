#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <dlfcn.h>

int main() {
    const char* path = "/Library/Audio/Plug-Ins/HAL/Zinkos.driver/Contents/MacOS/Zinkos";

    printf("Loading %s...\n", path);
    void* handle = dlopen(path, RTLD_NOW);
    if (!handle) {
        printf("dlopen FAILED: %s\n", dlerror());
        return 1;
    }

    printf("dlopen OK\n");

    // Look for the factory function
    void* sym = dlsym(handle, "ZinkosPlugIn_Create");
    if (!sym) {
        printf("dlsym ZinkosPlugIn_Create FAILED: %s\n", dlerror());
    } else {
        printf("ZinkosPlugIn_Create found at %p\n", sym);
    }

    dlclose(handle);
    return 0;
}
