#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <basetsd.h>

// VLC�̊e��w�b�_���Ŏg�p���邽�߁A��`���Ă���
typedef SSIZE_T ssize_t;

// vlc_threads.h �ŎQ�Ƃ��邽�߁A�錾���Ă���
int poll(struct pollfd *fds, unsigned nfds, int timeout);

//
bool InitializeMsvcrtMalloc();
void *msvcrt_malloc(size_t size);
void msvcrt_free(void *memblock);
char *msvcrt_strdup(const char *strSource);
