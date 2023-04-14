#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <basetsd.h>

// VLCの各種ヘッダ内で使用するため、定義しておく
typedef SSIZE_T ssize_t;

// vlc_threads.h で参照するため、宣言しておく
int poll(struct pollfd *fds, unsigned nfds, int timeout);

//
bool InitializeMsvcrtMalloc();
void *msvcrt_malloc(size_t size);
void msvcrt_free(void *memblock);
char *msvcrt_strdup(const char *strSource);
