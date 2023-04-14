#include "depends.h"

// vlc_threads.h Ç≈éQè∆Ç∑ÇÈÇΩÇﬂÅAíËã`ÇµÇƒÇ®Ç≠
int poll(struct pollfd *fds, unsigned nfds, int timeout)
{
    RaiseException(STATUS_NONCONTINUABLE_EXCEPTION, EXCEPTION_SOFTWARE_ORIGINATE, 0, nullptr);
    return 0;
}

static void *(__cdecl *msvcrt_malloc_ptr)(size_t);
static void (__cdecl *msvcrt_free_ptr)(void *);
static char *(__cdecl *msvcrt_strdup_ptr)(const char *);

bool InitializeMsvcrtMalloc()
{
    HMODULE module = GetModuleHandleA("MSVCRT.DLL");

    if (!module)
        return false;

    msvcrt_malloc_ptr = (void *(__cdecl *)(size_t))GetProcAddress(module, "malloc");
    msvcrt_free_ptr = (void (__cdecl *)(void *))GetProcAddress(module, "free");
    msvcrt_strdup_ptr = (char *(__cdecl *)(const char *))GetProcAddress(module, "_strdup");

    if (msvcrt_malloc_ptr && msvcrt_free_ptr && msvcrt_strdup_ptr)
        return true;

    return false;
}

void *msvcrt_malloc(size_t size)
{
    return msvcrt_malloc_ptr(size);
}

void msvcrt_free(void *memblock)
{
    msvcrt_free_ptr(memblock);
}

char *msvcrt_strdup(const char *strSource)
{
    return msvcrt_strdup_ptr(strSource);
}
