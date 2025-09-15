/*
* 2024 SQL-FromScratch
*
* OS Layer
*
* The OS layer provides a filesystem abstraction for cross-platform I/O
* and in-memory fs implementation for testing.
*/


#pragma once

#include <cstddef>
#include <stdint.h>

typedef int64_t os_file_offset_t;
typedef size_t  os_file_size_t;

#ifdef _WIN32
typedef void *os_file_handle_t;
#define OS_INVALID_HANDLE ((os_file_handle_t)INVALID_HANDLE_VALUE)
#else
typedef int os_file_handle_t;
#define OS_INVALID_HANDLE ((os_file_handle_t)-1)
#endif


os_file_handle_t os_file_open(const char *filename, bool read_write, bool create);

void os_file_close(os_file_handle_t handle);

bool os_file_exists(const char *filename);

void os_file_delete(const char *filename);

os_file_size_t os_file_read(os_file_handle_t handle, void *buffer, os_file_size_t size);

os_file_size_t os_file_write(os_file_handle_t handle, const void *buffer, os_file_size_t size);

void os_file_sync(os_file_handle_t handle);

void os_file_seek(os_file_handle_t handle, os_file_offset_t offset);

os_file_offset_t os_file_size(os_file_handle_t handle);

void os_file_truncate(os_file_handle_t handle, os_file_offset_t size);
