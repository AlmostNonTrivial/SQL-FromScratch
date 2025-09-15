/*
* 2024 SQL-FromScratch
*
* OS Layer
*
* Comment out the following to use an in-memory fs
*/

#define USE_PLATFORM_FS

#include "os_layer.hpp"

#if defined(USE_PLATFORM_FS) && defined(_WIN32)

#include <windows.h>
#include <io.h>

#define OS_INVALID_HANDLE INVALID_HANDLE_VALUE

os_file_handle_t
os_file_open(const char *filename, bool read_write, bool create)
{
	DWORD access = read_write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
	DWORD creation = create ? OPEN_ALWAYS : OPEN_EXISTING;

	HANDLE handle =
		CreateFileA(filename, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);

	return (os_file_handle_t)handle;
}

void
os_file_close(os_file_handle_t handle)
{
	if (handle != OS_INVALID_HANDLE)
	{
		CloseHandle((HANDLE)handle);
	}
}

bool
os_file_exists(const char *filename)
{
	DWORD attrs = GetFileAttributesA(filename);
	return (attrs != INVALID_FILE_ATTRIBUTES);
}

void
os_file_delete(const char *filename)
{
	DeleteFileA(filename);
}

os_file_size_t
os_file_read(os_file_handle_t handle, void *buffer, os_file_size_t size)
{
	DWORD bytes_read = 0;
	ReadFile((HANDLE)handle, buffer, (DWORD)size, &bytes_read, NULL);
	return (os_file_size_t)bytes_read;
}

os_file_size_t
os_file_write(os_file_handle_t handle, const void *buffer, os_file_size_t size)
{
	DWORD bytes_written = 0;
	WriteFile((HANDLE)handle, buffer, (DWORD)size, &bytes_written, NULL);
	return (os_file_size_t)bytes_written;
}

void
os_file_sync(os_file_handle_t handle)
{
	FlushFileBuffers((HANDLE)handle);
}

void
os_file_seek(os_file_handle_t handle, os_file_offset_t offset)
{
	LARGE_INTEGER li;
	li.QuadPart = offset;
	SetFilePointerEx((HANDLE)handle, li, NULL, FILE_BEGIN);
}

os_file_offset_t
os_file_size(os_file_handle_t handle)
{
	LARGE_INTEGER size;
	GetFileSizeEx((HANDLE)handle, &size);
	return (os_file_offset_t)size.QuadPart;
}

void
os_file_truncate(os_file_handle_t handle, os_file_offset_t size)
{
	LARGE_INTEGER li;
	li.QuadPart = size;
	SetFilePointerEx((HANDLE)handle, li, NULL, FILE_BEGIN);
	SetEndOfFile((HANDLE)handle);
}

#elif defined(USE_PLATFORM_FS)

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

os_file_handle_t
os_file_open(const char *filename, bool read_write, bool create)
{
	int flags = read_write ? O_RDWR : O_RDONLY;
	if (create)
		flags |= O_CREAT;

	return open(filename, flags, 0644);
}

void
os_file_close(os_file_handle_t handle)
{
	if (handle != OS_INVALID_HANDLE)
	{
		close(handle);
	}
}

bool
os_file_exists(const char *filename)
{
	struct stat st;
	return stat(filename, &st) == 0;
}

void
os_file_delete(const char *filename)
{
	unlink(filename);
}

os_file_size_t
os_file_read(os_file_handle_t handle, void *buffer, os_file_size_t size)
{
	ssize_t result = read(handle, buffer, size);
	return (result < 0) ? 0 : (os_file_size_t)result;
}

os_file_size_t
os_file_write(os_file_handle_t handle, const void *buffer, os_file_size_t size)
{
	ssize_t result = write(handle, buffer, size);
	return (result < 0) ? 0 : (os_file_size_t)result;
}

void
os_file_sync(os_file_handle_t handle)
{
	fsync(handle);
}

void
os_file_seek(os_file_handle_t handle, os_file_offset_t offset)
{
	lseek(handle, offset, SEEK_SET);
}

os_file_offset_t
os_file_size(os_file_handle_t handle)
{
	struct stat st;
	if (fstat(handle, &st) == 0)
	{
		return (os_file_offset_t)st.st_size;
	}
	return 0;
}

void
os_file_truncate(os_file_handle_t handle, os_file_offset_t size)
{
	ftruncate(handle, size);
}

#else

/*
 * Using STL so this layer doesn't have to depend
 * on the arena
 */

#include "os_layer.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <cstring>

#ifndef OS_INVALID_HANDLE
#define OS_INVALID_HANDLE ((os_file_handle_t)0)
#endif

struct file_handle
{
    std::string filepath;
    size_t position;
    bool read_write;
};

struct file_data
{
    std::vector<uint8_t> contents;
};

static struct
{
    std::unordered_map<std::string, file_data> files;
    std::unordered_map<os_file_handle_t, file_handle> handles;
    os_file_handle_t next_handle = 1;

    void init()
    {
        next_handle = 1;
        files.clear();
        handles.clear();
    }
} FILESYSTEM = {};

os_file_handle_t
os_file_open(const char *filename, bool read_write, bool create)
{
    if (FILESYSTEM.files.empty())
    {
        FILESYSTEM.init();
    }

    std::string filepath(filename);

    auto file_it = FILESYSTEM.files.find(filepath);

    if (file_it == FILESYSTEM.files.end())
    {
        if (!create)
        {
            return OS_INVALID_HANDLE;
        }

        FILESYSTEM.files[filepath] = file_data();
    }

    os_file_handle_t handle = FILESYSTEM.next_handle++;

    file_handle fh;
    fh.filepath = filepath;
    fh.position = 0;
    fh.read_write = read_write;

    FILESYSTEM.handles[handle] = fh;

    return handle;
}

void
os_file_close(os_file_handle_t handle)
{
    if (handle != OS_INVALID_HANDLE)
    {
        FILESYSTEM.handles.erase(handle);
    }
}

bool
os_file_exists(const char *filename)
{
    if (FILESYSTEM.files.empty())
    {
        return false;
    }

    return FILESYSTEM.files.find(filename) != FILESYSTEM.files.end();
}

void
os_file_delete(const char *filename)
{
    if (FILESYSTEM.files.empty())
    {
        return;
    }

    std::string filepath(filename);
    FILESYSTEM.files.erase(filepath);

    // Close any open handles to this file
    std::vector<os_file_handle_t> handles_to_close;

    for (const auto& [handle_id, handle_data] : FILESYSTEM.handles)
    {
        if (handle_data.filepath == filepath)
        {
            handles_to_close.push_back(handle_id);
        }
    }

    for (auto handle : handles_to_close)
    {
        os_file_close(handle);
    }
}

os_file_size_t
os_file_read(os_file_handle_t handle, void *buffer, os_file_size_t size)
{
    auto handle_it = FILESYSTEM.handles.find(handle);
    if (handle_it == FILESYSTEM.handles.end())
    {
        return 0;
    }

    file_handle& handle_data = handle_it->second;

    auto file_it = FILESYSTEM.files.find(handle_data.filepath);
    if (file_it == FILESYSTEM.files.end())
    {
        return 0;
    }

    file_data& file = file_it->second;
    size_t& position = handle_data.position;

    os_file_size_t bytes_to_read = 0;

    if (position < file.contents.size())
    {
        bytes_to_read = static_cast<os_file_size_t>(file.contents.size() - position);
        if (size < bytes_to_read)
        {
            bytes_to_read = size;
        }
    }

    if (bytes_to_read > 0)
    {
        memcpy(buffer, file.contents.data() + position, bytes_to_read);
        position += bytes_to_read;
    }

    return bytes_to_read;
}

os_file_size_t
os_file_write(os_file_handle_t handle, const void *buffer, os_file_size_t size)
{
    auto handle_it = FILESYSTEM.handles.find(handle);
    if (handle_it == FILESYSTEM.handles.end())
    {
        return 0;
    }

    file_handle& handle_data = handle_it->second;
    if (!handle_data.read_write)
    {
        return 0;
    }

    auto file_it = FILESYSTEM.files.find(handle_data.filepath);
    if (file_it == FILESYSTEM.files.end())
    {
        return 0;
    }

    file_data& file = file_it->second;
    size_t& position = handle_data.position;
    size_t required_size = position + size;

    // Resize vector if needed (this properly updates size)
    if (required_size > file.contents.size())
    {
        file.contents.resize(required_size, 0);
    }

    memcpy(file.contents.data() + position, buffer, size);
    position += size;

    return size;
}

void
os_file_sync(os_file_handle_t handle)
{
    /* No-op for memory-based implementation */
    (void)handle;
}

void
os_file_seek(os_file_handle_t handle, os_file_offset_t offset)
{
    auto handle_it = FILESYSTEM.handles.find(handle);
    if (handle_it == FILESYSTEM.handles.end())
    {
        return;
    }

    handle_it->second.position = static_cast<size_t>(offset);
}

os_file_offset_t
os_file_size(os_file_handle_t handle)
{
    auto handle_it = FILESYSTEM.handles.find(handle);
    if (handle_it == FILESYSTEM.handles.end())
    {
        return 0;
    }

    auto file_it = FILESYSTEM.files.find(handle_it->second.filepath);
    if (file_it == FILESYSTEM.files.end())
    {
        return 0;
    }

    return static_cast<os_file_offset_t>(file_it->second.contents.size());
}

void
os_file_truncate(os_file_handle_t handle, os_file_offset_t size)
{
    auto handle_it = FILESYSTEM.handles.find(handle);
    if (handle_it == FILESYSTEM.handles.end())
    {
        return;
    }

    file_handle& handle_data = handle_it->second;
    if (!handle_data.read_write)
    {
        return;
    }

    auto file_it = FILESYSTEM.files.find(handle_data.filepath);
    if (file_it == FILESYSTEM.files.end())
    {
        return;
    }

    file_data& file = file_it->second;
    size_t new_size = static_cast<size_t>(size);

    // Resize properly handles both growing and shrinking
    file.contents.resize(new_size, 0);

    // Adjust position if it's beyond the new size
    if (handle_data.position > new_size)
    {
        handle_data.position = new_size;
    }
}
#endif
