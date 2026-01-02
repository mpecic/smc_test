#include <android/native_activity.h>
#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define LOG_TAG "SMC_DIRECT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The target function we want to patch in place
__attribute__((noinline)) int target_function() {
    // 0x2a1f03e0 -> mov w0, wzr (return 0)
    return 0;
}

void print_page_details(void *addr) {
    FILE *f = fopen("/proc/self/smaps", "r"); // smaps gives detailed flags
    if (!f) return;

    char line[512];
    uintptr_t target = (uintptr_t)addr;
    int found = 0;

    LOGI("--- MEMORY PAGE DETAILS ---");

    while (fgets(line, sizeof(line), f)) {
        // Parse range: "7bc1768000-7bc1769000 ..."
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            if (target >= start && target < end) {
                found = 1;
                LOGI("MAP: %s", line); // Print the main mapping line
            } else {
                found = 0;
            }
        }

        // If we are inside the correct block, print the "Shared/Private" details
        if (found) {
            if (strstr(line, "Shared_Clean") || strstr(line, "Shared_Dirty") || 
                strstr(line, "Private_Clean") || strstr(line, "Private_Dirty") ||
                strstr(line, "Rss:")) {
                LOGI("   %s", line);
            }
        }
    }
    fclose(f);
}

void run_security_test() {
    LOGI("--- STARTING DIRECT SMC TEST (NON-JIT) ---");
    LOGI("1. Original function returns: %d", target_function());

    // 1. Calculate Page Alignment
    void *func_ptr = (void *)target_function;
    size_t page_size = sysconf(_SC_PAGESIZE);
    void *page_start = (void *)((uintptr_t)func_ptr & ~(page_size - 1));

    LOGI("2. Target function at: %p", func_ptr);
    LOGI("   Page start at:      %p", page_start);

    // 2. Attempt to Change Permissions to RWX (Read | Write | Execute)
    // THIS IS THE DANGER ZONE.
    // We expect this to fail on modern Android due to W^X and SELinux.
    LOGI("3. Attempting mprotect(RWX)...");

print_page_details(page_start);

    if (mprotect(page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == -1) {
        LOGE("CRITICAL FAILURE: mprotect failed with error: %s", strerror(errno));
        LOGE("Reason: Android likely blocked 'PROT_WRITE' on executable code.");
        return;
    }

    LOGI("   mprotect SUCCESS! (This is unexpected on secure devices)");

print_page_details(page_start);

    // 3. Patch the code
    uint32_t *code_ptr = (uint32_t *)func_ptr;
    int patched = 0;

    for (int i = 0; i < 64; i++) {
        // Look for 'mov w0, wzr' (0x2a1f03e0)
        if (code_ptr[i] == 0x2a1f03e0) {
            LOGI("   Found opcode at offset %d. Patching...", i);
            code_ptr[i] = 0x52800020; // mov w0, #1
            patched = 1;
            break;
        }
    }

    if (!patched) {
        LOGE("   Could not find instruction pattern.");
        return;
    }

    // 4. Flush I-Cache
    __builtin___clear_cache((char *)func_ptr, (char *)func_ptr + 64);

    // 5. Restore Permissions
    if (mprotect(page_start, page_size, PROT_READ | PROT_EXEC) == -1) {
        LOGE("mprotect restore failed: %s", strerror(errno));
        return;
    }
    LOGI("4. Permissions restored to RX.");

print_page_details(page_start);

    // 6. Execute again
    int result = target_function();
    LOGI("5. New value: %d", result);

    if (result == 1) LOGI("SUCCESS: Direct memory modification worked!");
    else LOGI("FAILURE: Value did not change.");
}

void ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize) {
    run_security_test();
}
