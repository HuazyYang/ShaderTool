#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "ShaderToolUtils.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

void LogV(int level, const char *format, ...) {
    FILE *fd;
    switch (level) {
        case 0:
            fd = stderr;
            break;
        case 1:
            fd = stdout;
            break;
        case 2:
        default:
            fd = stdout;
            break;
    }
    va_list ap;
    va_start(ap, format);
    vfprintf(fd, format, ap);
    va_end(ap);
}

static void *MyHeapReAlloc(void *pv, size_t byte_count) {
    return realloc(pv, byte_count);
}

#pragma region[ DynamicArray ]

int DynamicArrayReserve(DynamicArray *array, size_t element_size, size_t size,
                               size_t extra_capacity, const char *description) {
    size_t required_capacity, new_capacity, allocation_size;
    void *new_buffer;

    required_capacity = size + extra_capacity;
    if (required_capacity <= array->Capacity)
        return 0;

    new_capacity = array->Capacity ? array->Capacity : 16;
    while (new_capacity < required_capacity)
        new_capacity *= 2;
    allocation_size = new_capacity * element_size;

    new_buffer = MyHeapReAlloc(array->Buffer, allocation_size);
    if (!new_buffer) {
        LogE("Failed to allocate enough memory for %s\n", description);
        return -1;
    }
    array->Buffer = new_buffer;
    array->Capacity = new_capacity;
    return 0;
}

int DynamicArrayResize(DynamicArray *array, size_t element_size, size_t size,
                              size_t extra_capacity, const char *description) {
    if (DynamicArrayReserve(array, element_size, size, extra_capacity, description))
        return -1;
    array->Size = size;
    return 0;
}

int DynamicArrayAppend(DynamicArray *array, size_t element_size, const void *elements,
                              size_t count, const char *description) {
    size_t old_size = array->Size;
    size_t new_size = old_size + count;
    if (DynamicArrayResize(array, element_size, new_size, 0, description))
        return -1;
    if (count)
        memcpy((uint8_t *)array->Buffer + old_size * element_size, elements,
               count * element_size);
    return 0;
}

int DynamicArrayErase(DynamicArray *array, size_t element_size, size_t start,
                             size_t end) {
    size_t right_count;

    if (start > end || end > array->Size)
        return -1;
    right_count = array->Size - end;
    if (start < end)
        memmove((uint8_t *)array->Buffer + start * element_size,
                (uint8_t *)array->Buffer + end * element_size, right_count * element_size);
    array->Size = start + right_count;
    return 0;
}

void DynamicArrayDeinit(DynamicArray *array) {
    free(array->Buffer);
    memset(array, 0, sizeof(*array));
}

#pragma endregion[DynamicArray]

#pragma region[ AStringView ]

AStringView AStringViewCreate(const char *sz) {
    AStringView s;
    if (sz) {
        s.Buffer = sz;
        s.Length = strlen(sz);
    } else {
        s.Buffer = "";
        s.Length = 0;
    }
    return s;
}

void AStringViewInit(AStringView *s, const char *psz) {
    if (psz) {
        s->Buffer = psz;
        s->Length = strlen(psz);
    } else {
        s->Buffer = "";
        s->Length = 0;
    }
}

void AStringViewInit2(AStringView *s, const char *psz, size_t length) {
    s->Buffer = psz ? psz : "";
    s->Length = length;
}

bool AStringViewIsEmpty(const AStringView *s) {
    return !s->Buffer || s->Length == 0;
}

bool AStringViewEqual(const AStringView *s, const char *sz) {
    size_t len;
    if (sz == NULL)
        sz = "";

    len = strlen(sz);
    return len == s->Length && strncmp(s->Buffer, sz, s->Length) == 0;
}

bool AStringViewEqual2(const AStringView *s, const AStringView *s2) {
    return s->Length == s2->Length && strncmp(s->Buffer, s2->Buffer, s->Length) == 0;
}

bool AStringViewEqualAtLeast(const AStringView *s, const AStringView *s2) {
    return s->Length >= s2->Length && strncmp(s->Buffer, s2->Buffer, s2->Length) == 0;
}

#pragma endregion[AStringView]

#pragma region[ AString ]

void AStringDeinit(AString *s) {
    DynamicArrayDeinitTyped(s);
}

int AStringResize(AString *s, size_t length) {
    if (DynamicArrayResizeTyped(s, char, length, 1, "AString"))
        return -1;
    s->Buffer[length] = 0;
    return 0;
}

int AStringCatN(AString *s, size_t source_count, ...) {
    size_t len = s->Length, previous_length;
    va_list ap;
    const AStringView *current_source;
    size_t i;

    previous_length = len;

    va_start(ap, source_count);
    for (i = 0; i < source_count; ++i) {
        current_source = va_arg(ap, const AStringView *);
        len += current_source->Length;
    }
    va_end(ap);

    if (AStringResize(s, len))
        return -1;

    va_start(ap, source_count);
    for (i = 0; i < source_count; ++i) {
        current_source = va_arg(ap, const AStringView *);
        memcpy(s->Buffer + previous_length, current_source->Buffer, current_source->Length);
        previous_length += current_source->Length;
    }
    va_end(ap);

    s->Buffer[s->Length] = 0;

    return 0;
}

int AStringCopy(AString *s, const AStringView *src) {
    s->Length = 0;
    return AStringCatN(s, 1, src);
}

int NormalizeAPath(const AStringView *s, AString *result) {
    const AStringView sep = AStringViewInitializer("/", 1);
    const AStringView dots2 = AStringViewInitializer("..", 2);
    const AStringView dots1 = AStringViewInitializer(".", 1);
    const char *start = s->Buffer, *next, *end = s->Buffer + s->Length;
    AStringView path_part;
    size_t component_start, new_length;

    // TODO: resolve relative path to absolute path for .. to work properly

    if (AStringResize(result, 0))
        return -1;

    for (; start != end;) {
        if (*start == '\\' || *start == '/') {
            if (!result->Length || result->Buffer[result->Length - 1] != '/') {
                if (AStringCatN(result, 1, &sep))
                    return -1;
            }
            ++start;
            if (start == end)
                break;
        }

        for (; start != end && (*start == '\\' || *start == '/'); ++start)
            ;
        if (start == end)
            break;

        for (next = start + 1; next != end && (*next != '\\' && *next != '/'); ++next)
            ;

        AStringViewInit2(&path_part, start, next - start);

        if (AStringViewEqual2(&path_part, &dots2)) {
            if (!result->Length || result->Buffer[result->Length - 1] != '/') {
                LogE(
                    "Path normalization with .. but no preceding relative "
                    "directory\n");
                return -1;
            }
            component_start = result->Length - 1;
            while (component_start && result->Buffer[component_start - 1] != '/')
                --component_start;
            new_length = component_start;
            if (new_length > 1 && result->Buffer[new_length - 1] == '/')
                --new_length;
            if (AStringResize(result, new_length))
                return -1;
            for (; next != end && (*next == '\\' || *next == '/'); ++next)
                ;
            if (next != end && result->Length &&
                result->Buffer[result->Length - 1] != '/') {
                if (AStringCatN(result, 1, &sep))
                    return -1;
            }
        } else if (AStringViewEqual2(&path_part, &dots1)) {
            // Forward to next name
            for (; next != end && (*next == '\\' || *next == '/'); ++next)
                ;
        } else {
            if (AStringCatN(result, 1, &path_part))
                return -1;
        }

        start = next;
    }

    return 0;
}

int AStringAppendFormat(AString *string, const char *format, ...) {
    size_t old_length;
    size_t new_length;
    va_list ap;
    int grown;

    old_length = string->Length;
    va_start(ap, format);
    grown = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if(grown < 0) {
        LogE("AStringAppendFormat failed\n");
        return  -1;
    }

    if ((size_t)grown > SIZE_MAX - old_length) {
        LogE("AStringAppendFormat result is too large\n");
        return -1;
    }
    new_length = old_length + (size_t)grown;
    if (AStringResize(string, new_length))
        return -1;

    va_start(ap, format);
    vsnprintf(string->Buffer + old_length, (size_t)grown + 1, format, ap);
    va_end(ap);
    return 0;
}

#pragma endregion[AString]

#pragma region[ ByteBuffer ]

int ByteBufferResize(ByteBuffer *buffer, size_t size) {
    return DynamicArrayResizeTyped(buffer, uint8_t, size, 0, "ByteBuffer");
}

int ByteBufferWrite(ByteBuffer *buffer, const void *data, size_t size) {
    return DynamicArrayAppendTyped(buffer, uint8_t, data, size, "ByteBuffer");
}

void ByteBufferGetAStringView(const ByteBuffer *buffer, AStringView *s) {
    s->Buffer = buffer->Buffer;
    s->Length = buffer->Size;
}

int ByteBufferCatAStringViews(ByteBuffer *buffer, size_t n, ...) {
    size_t total_size;
    size_t previous_size;
    va_list ap;
    size_t i;
    const AStringView *curr;

    total_size = buffer->Size;
    previous_size = total_size;

    va_start(ap, n);
    for (i = 0; i < n; ++i) {
        curr = va_arg(ap, const AStringView *);
        total_size += curr->Length;
    }
    va_end(ap);

    if (ByteBufferResize(buffer, total_size))
        return -1;

    va_start(ap, n);
    for (i = 0; i < n; ++i) {
        curr = va_arg(ap, const AStringView *);
        memcpy((uint8_t *)buffer->Buffer + previous_size, curr->Buffer, curr->Length);
        previous_size += curr->Length;
    }
    va_end(ap);

    return 0;
}

void ByteBufferDeinit(ByteBuffer *buffer) {
    DynamicArrayDeinitTyped(buffer);
}

int ByteBufferEraseRange(ByteBuffer *buffer, size_t start, size_t end) {
    if (start > end || end > buffer->Size) {
        LogW("Erase byte buffer range out of range\n");
        return -1;
    }

    return DynamicArrayEraseTyped(buffer, uint8_t, start, end);
}

#pragma endregion[ByteBuffer]


#ifdef _WIN32
static void ReportError(const char *operation, const char *path, DWORD error) {
    WCHAR wide_message[512];
    char message[1024] = "unknown error";
    DWORD count = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0,
        wide_message, (DWORD)(sizeof(wide_message) / sizeof(wide_message[0])), NULL);
    if (count) {
        while (count &&
               (wide_message[count - 1] == L'\r' || wide_message[count - 1] == L'\n'))
            wide_message[--count] = 0;
        if (!WideCharToMultiByte(CP_UTF8, 0, wide_message, -1, message,
                                 (int)sizeof(message), NULL, NULL))
            memcpy(message, "error message conversion failed",
                   sizeof("error message conversion failed"));
    }
    fprintf(stderr, "ShaderTool: %s%s%s%s failed, Win32 error %lu: %s\n", operation,
            path ? " '" : "", path ? path : "", path ? "'" : "", (unsigned long)error,
            message);
}

/* Based on Microsoft's CreatePipeEx sample by Dave Hart (1997). */
static BOOL CreatePipeEx(LPHANDLE read_pipe, LPHANDLE write_pipe,
                         LPSECURITY_ATTRIBUTES attributes, DWORD size, DWORD read_mode,
                         DWORD write_mode) {
    static LONG serial_number;
    HANDLE read_handle, write_handle;
    DWORD error;
    WCHAR pipe_name[MAX_PATH];
    if ((read_mode | write_mode) & ~FILE_FLAG_OVERLAPPED) {
        SetLastError(ERROR_INVALID_PARAMETER);
        ReportError("validate compiler pipe flags", NULL, ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!size)
        size = 4096;
    swprintf(pipe_name, _countof(pipe_name), L"\\\\.\\Pipe\\ShaderTool.%08x.%08x",
             GetCurrentProcessId(), InterlockedIncrement(&serial_number));
    read_handle =
        CreateNamedPipeW(pipe_name, PIPE_ACCESS_INBOUND | read_mode,
                         PIPE_TYPE_BYTE | PIPE_WAIT, 1, size, size, 120 * 1000, attributes);
    if (read_handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        ReportError("create named compiler output pipe", NULL, error);
        SetLastError(error);
        return FALSE;
    }
    write_handle = CreateFileW(pipe_name, GENERIC_WRITE, 0, attributes, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | write_mode, NULL);
    if (write_handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        ReportError("open compiler output pipe writer", NULL, error);
        if (!CloseHandle(read_handle))
            ReportError("close compiler output pipe", NULL, GetLastError());
        SetLastError(error);
        return FALSE;
    }
    *read_pipe = read_handle;
    *write_pipe = write_handle;
    return TRUE;
}

static int Utf8ToWide(const char *text, WideBuffer *wide, const char *context) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    DWORD error;
    if (!count) {
        error = GetLastError();
        ReportError("convert UTF-8", context, error);
        return -1;
    }
    if (DynamicArrayResizeTyped(wide, WCHAR, (size_t)count, 0, "UTF-16 buffer"))
        return -1;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide->Buffer,
                             count)) {
        error = GetLastError();
        ReportError("convert UTF-8", context, error);
        return -1;
    }
    return 0;
}

static int WideBufferAppend(WideBuffer *text, const WCHAR *value, size_t count) {
    size_t new_size = text->Size + count;
    if (DynamicArrayResizeTyped(text, WCHAR, new_size, 1, "compiler command line"))
        return -1;
    memcpy(text->Buffer + new_size - count, value, count * sizeof(WCHAR));
    text->Buffer[new_size] = 0;
    return 0;
}

static int WideBufferAppendSlashes(WideBuffer *text, size_t count) {
    WCHAR slash = L'\\';
    while (count--)
        if (WideBufferAppend(text, &slash, 1))
            return -1;
    return 0;
}

static int WideBufferAppendQuoted(WideBuffer *line, const char *argument) {
    WideBuffer wide = {0};
    WCHAR quote = L'"', slash = L'\\', space = L' ';
    size_t i, slashes = 0;
    if (Utf8ToWide(argument, &wide, "compiler argument"))
        goto allocation_failure;
    if ((line->Size && WideBufferAppend(line, &space, 1)) ||
        WideBufferAppend(line, &quote, 1))
        goto allocation_failure;
    for (i = 0; wide.Buffer[i]; ++i) {
        if (wide.Buffer[i] == L'\\') {
            if (slashes == SIZE_MAX)
                goto allocation_failure;
            ++slashes;
            continue;
        }
        if (wide.Buffer[i] == L'"') {
            if (WideBufferAppendSlashes(line, slashes * 2))
                goto allocation_failure;
            if (WideBufferAppend(line, &slash, 1))
                goto allocation_failure;
        } else {
            if (WideBufferAppendSlashes(line, slashes))
                goto allocation_failure;
        }
        slashes = 0;
        if (WideBufferAppend(line, &wide.Buffer[i], 1))
            goto allocation_failure;
    }
    if (WideBufferAppendSlashes(line, slashes * 2))
        goto allocation_failure;
    if (WideBufferAppend(line, &quote, 1))
        goto allocation_failure;
    DynamicArrayDeinitTyped(&wide);
    return 0;
allocation_failure:
    LogE("compiler command line size overflow or allocation failed\n");
    DynamicArrayDeinitTyped(&wide);
    return -1;
}

static double TimeoutSeconds(void) {
    return 6.0 * 60.0;
}

static DWORD RemainingMilliseconds(LARGE_INTEGER deadline, LARGE_INTEGER frequency) {
    LARGE_INTEGER now;
    double milliseconds;
    if (!QueryPerformanceCounter(&now))
        return 0;
    if (now.QuadPart >= deadline.QuadPart)
        return 0;
    milliseconds =
        (double)(deadline.QuadPart - now.QuadPart) * 1000.0 / (double)frequency.QuadPart;
    if (milliseconds >= (double)(INFINITE - 1))
        return INFINITE - 1;
    if (milliseconds < 1.0)
        return 1;
    return (DWORD)milliseconds;
}

int ProgramRun(char *const arguments[], bool merge_stderr,
               ProgramOutputCallback callback, void *context, int *exit_code) {
    const char *program = arguments[0];
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    HANDLE read_pipe = NULL, write_pipe = NULL, event = NULL, waits[2];
    PROCESS_INFORMATION process = {0};
    STARTUPINFOW startup = {0};
    OVERLAPPED overlap = {0};
    LARGE_INTEGER frequency, now, deadline;
    WideBuffer command = {0};
    size_t i;
    char buffer[4096];
    DWORD count = 0, code, wait_result, error;
    bool pending = false, process_done = false, eof = false, timed_out = false;
    bool callback_failed = false, terminated = false;
    int result = -1;

    if (!CreatePipeEx(&read_pipe, &write_pipe, &attributes, 0, FILE_FLAG_OVERLAPPED, 0))
        goto cleanup;
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        error = GetLastError();
        ReportError("disable compiler pipe inheritance", NULL, error);
        goto cleanup;
    }
    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) {
        error = GetLastError();
        ReportError("create compiler output event", NULL, error);
        goto cleanup;
    }
    for (i = 0; arguments[i]; ++i)
        if (WideBufferAppendQuoted(&command, arguments[i]))
            goto cleanup;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = merge_stderr ? write_pipe : GetStdHandle(STD_ERROR_HANDLE);
    if (startup.hStdInput == INVALID_HANDLE_VALUE ||
        startup.hStdError == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        ReportError("get inherited compiler standard handles", NULL, error);
        goto cleanup;
    }
    if (!CreateProcessW(NULL, command.Buffer, NULL, NULL, TRUE, 0, NULL, NULL, &startup,
                        &process)) {
        error = GetLastError();
        ReportError("create compiler process", program, error);
        goto cleanup;
    }
    if (!CloseHandle(process.hThread)) {
        error = GetLastError();
        ReportError("close compiler primary thread", NULL, error);
        goto cleanup;
    }
    process.hThread = NULL;
    if (!CloseHandle(write_pipe)) {
        error = GetLastError();
        ReportError("close parent compiler pipe writer", NULL, error);
        goto cleanup;
    }
    write_pipe = NULL;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&now)) {
        error = GetLastError();
        ReportError("initialize monotonic compiler deadline", NULL, error);
        goto cleanup;
    }
    {
        double timeout_ticks = TimeoutSeconds() * (double)frequency.QuadPart;
        if (timeout_ticks > (double)(LLONG_MAX - now.QuadPart)) {
            fprintf(stderr, "ShaderTool: compiler timeout is too large\n");
            goto cleanup;
        }
        deadline.QuadPart = now.QuadPart + (LONGLONG)timeout_ticks;
    }
    overlap.hEvent = event;
    waits[0] = event;
    waits[1] = process.hProcess;

    while (!eof || !process_done) {
        if (!pending && !eof) {
            if (!ResetEvent(event)) {
                error = GetLastError();
                ReportError("reset compiler output event", NULL, error);
                goto terminate;
            }
            count = 0;
            if (ReadFile(read_pipe, buffer, sizeof(buffer), &count, &overlap)) {
                if (count && callback(context, buffer, count, false)) {
                    callback_failed = true;
                    goto terminate;
                }
                continue;
            }
            error = GetLastError();
            if (error == ERROR_IO_PENDING)
                pending = true;
            else if (error == ERROR_BROKEN_PIPE)
                eof = true;
            else {
                ReportError("read compiler output", NULL, error);
                goto terminate;
            }
        }
        if (eof && process_done)
            break;
        if (eof)
            wait_result = WaitForSingleObject(process.hProcess,
                                              RemainingMilliseconds(deadline, frequency));
        else
            wait_result =
                WaitForMultipleObjects(process_done ? 1 : 2, waits, FALSE,
                                       RemainingMilliseconds(deadline, frequency));
        if (wait_result == WAIT_TIMEOUT) {
            fprintf(stderr, "ShaderTool: compiler timed out after %.3f seconds\n",
                    TimeoutSeconds());
            timed_out = true;
            goto terminate;
        }
        if (wait_result == WAIT_FAILED) {
            error = GetLastError();
            ReportError("wait for compiler and output", NULL, error);
            goto terminate;
        }
        if ((eof && wait_result == WAIT_OBJECT_0) ||
            (!process_done && wait_result == WAIT_OBJECT_0 + 1)) {
            process_done = true;
            continue;
        }
        if (wait_result == WAIT_OBJECT_0) {
            pending = false;
            if (!GetOverlappedResult(read_pipe, &overlap, &count, FALSE)) {
                error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    eof = true;
                    continue;
                }
                ReportError("collect compiler output read", NULL, error);
                goto terminate;
            }
            if (count && callback(context, buffer, count, false)) {
                callback_failed = true;
                goto terminate;
            }
        }
        continue;
    terminate:
        if (!terminated && process.hProcess) {
            if (!TerminateProcess(process.hProcess, 1)) {
                error = GetLastError();
                ReportError("terminate compiler process", NULL, error);
            }
            terminated = true;
        }
        process_done = true;
        if (pending) {
            if (!CancelIoEx(read_pipe, &overlap) && GetLastError() != ERROR_NOT_FOUND) {
                error = GetLastError();
                ReportError("cancel compiler output read", NULL, error);
            }
            pending = false;
        }
        eof = true;
    }
    wait_result =
        WaitForSingleObject(process.hProcess, RemainingMilliseconds(deadline, frequency));
    if (wait_result == WAIT_FAILED) {
        error = GetLastError();
        ReportError("collect compiler process", NULL, error);
        goto cleanup;
    }
    if (wait_result == WAIT_TIMEOUT && !terminated) {
        fprintf(stderr, "ShaderTool: compiler timed out while collecting process status\n");
        if (!TerminateProcess(process.hProcess, 1)) {
            error = GetLastError();
            ReportError("terminate compiler process", NULL, error);
        }
        timed_out = true;
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    if (callback(context, "", 0, true))
        callback_failed = true;
    if (timed_out || callback_failed || terminated)
        goto cleanup;
    if (!GetExitCodeProcess(process.hProcess, &code)) {
        error = GetLastError();
        ReportError("get compiler exit code", NULL, error);
        goto cleanup;
    }

    *exit_code = (int)code;
    result = 0;
cleanup:
    if (result && process.hProcess && !terminated) {
        if (!TerminateProcess(process.hProcess, 1) &&
            GetLastError() != ERROR_ACCESS_DENIED) {
            error = GetLastError();
            ReportError("terminate compiler during cleanup", NULL, error);
        }
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    if (process.hProcess && !CloseHandle(process.hProcess)) {
        error = GetLastError();
        ReportError("close compiler process", NULL, error);
    }
    if (process.hThread && !CloseHandle(process.hThread)) {
        error = GetLastError();
        ReportError("close compiler thread", NULL, error);
    }
    if (event && !CloseHandle(event)) {
        error = GetLastError();
        ReportError("close compiler output event", NULL, error);
    }
    if (read_pipe && !CloseHandle(read_pipe)) {
        error = GetLastError();
        ReportError("close compiler output pipe", NULL, error);
    }
    if (write_pipe && !CloseHandle(write_pipe)) {
        error = GetLastError();
        ReportError("close compiler pipe writer", NULL, error);
    }
    DynamicArrayDeinitTyped(&command);
    return result;
}

static int ReadAll(HANDLE file, void *bytes, size_t size, const char *path) {
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = size - offset > MAXDWORD ? MAXDWORD : (DWORD)(size - offset);
        DWORD transferred = 0, error;
        if (!ReadFile(file, (unsigned char *)bytes + offset, chunk, &transferred, NULL)) {
            error = GetLastError();
            ReportError("read depfile", path, error);
            return -1;
        }
        if (!transferred) {
            ReportError("read depfile", path, ERROR_HANDLE_EOF);
            return -1;
        }
        offset += transferred;
    }
    return 0;
}

static int WriteAll(HANDLE file, const void *bytes, size_t size, const char *path) {
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = size - offset > MAXDWORD ? MAXDWORD : (DWORD)(size - offset);
        DWORD transferred = 0, error;
        if (!WriteFile(file, (const unsigned char *)bytes + offset, chunk, &transferred,
                       NULL)) {
            error = GetLastError();
            ReportError("write depfile", path, error);
            return -1;
        }
        if (!transferred) {
            ReportError("write depfile", path, ERROR_WRITE_FAULT);
            return -1;
        }
        offset += transferred;
    }
    return 0;
}

int FileReadAll(const char *path, ByteBuffer *content) {
    WideBuffer wide = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER length;
    DWORD error;
    int result = -1;
    if (Utf8ToWide(path, &wide, path))
        goto cleanup;
    file = CreateFileW(wide.Buffer, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        ReportError("open header", path, GetLastError());
        goto cleanup;
    }
    if (!GetFileSizeEx(file, &length)) {
        ReportError("query header size", path, GetLastError());
        goto cleanup;
    }
    if (length.QuadPart < 0 || (ULONGLONG)length.QuadPart > SIZE_MAX) {
        LogE("ShaderTool: header '%s' is too large for this platform\n", path);
        goto cleanup;
    }
    if (ByteBufferResize(content, (size_t)length.QuadPart + 1))
        goto cleanup;
    content->Size--;
    if (ReadAll(file, content->Buffer, content->Size, path))
        goto cleanup;
    content->Buffer[content->Size] = 0;
    result = 0;
cleanup:
    DynamicArrayDeinitTyped(&wide);
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) {
        error = GetLastError();
        ReportError("close header", path, error);
        result = -1;
    }
    return result;
}

int FileWriteAll(const char *path, const void *bytes, size_t size,
                                       bool append) {
    WideBuffer wide = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD error;
    int result = -1;
    if (Utf8ToWide(path, &wide, path))
        goto cleanup;
    file = CreateFileW(wide.Buffer, append ? FILE_APPEND_DATA : GENERIC_WRITE,
                       FILE_SHARE_READ, NULL, append ? OPEN_EXISTING : CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        ReportError(append ? "open header for append" : "open header for rewrite", path,
                    GetLastError());
        goto cleanup;
    }
    if (WriteAll(file, bytes, size, path))
        goto cleanup;
    result = 0;
cleanup:
    DynamicArrayDeinitTyped(&wide);
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) {
        error = GetLastError();
        ReportError("close header", path, error);
        result = -1;
    }
    return result;
}

int FileWriteIfChanged(const char *path, const void *bytes,
                                            size_t size, bool *changed) {
    WideBuffer wide = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER length, zero;
    ByteBuffer old = {0};
    DWORD error;
    int result = -1;
    *changed = true;
    if (Utf8ToWide(path, &wide, path))
        goto cleanup;
    file = CreateFileW(wide.Buffer, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        ReportError("open depfile", path, error);
        goto cleanup;
    }
    if (!GetFileSizeEx(file, &length)) {
        error = GetLastError();
        ReportError("query depfile size", path, error);
        goto cleanup;
    }
    if (length.QuadPart >= 0 && (ULONGLONG)length.QuadPart <= SIZE_MAX &&
        (size_t)length.QuadPart == size) {
        if (ByteBufferResize(&old, size ? size : 1))
            goto cleanup;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) {
            error = GetLastError();
            ReportError("seek depfile", path, error);
            goto cleanup;
        }
        if (ReadAll(file, old.Buffer, size, path))
            goto cleanup;
        if (!memcmp(old.Buffer, bytes, size)) {
            *changed = false;
            result = 0;
            goto cleanup;
        }
    }
    zero.QuadPart = 0;
    if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) {
        error = GetLastError();
        ReportError("seek depfile", path, error);
        goto cleanup;
    }
    if (WriteAll(file, bytes, size, path))
        goto cleanup;
    if (!SetEndOfFile(file)) {
        error = GetLastError();
        ReportError("truncate depfile", path, error);
        goto cleanup;
    }
    result = 0;
cleanup:
    ByteBufferDeinit(&old);
    DynamicArrayDeinitTyped(&wide);
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) {
        error = GetLastError();
        ReportError("close depfile", path, error);
        result = -1;
    }
    return result;
}
#else
static void ReportError(const char *operation, const char *path, int error) {
    fprintf(stderr, "ShaderTool: %s%s%s%s failed, POSIX error %d: %s\n", operation,
            path ? " '" : "", path ? path : "", path ? "'" : "", error, strerror(error));
}

static double MonotonicSeconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) {
        ReportError("clock_gettime", NULL, errno);
        return -1.0;
    }
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static double TimeoutSeconds(void) {
    return 6.0 * 60.0;
}

static void KillOnce(pid_t child, bool *killed) {
    if (*killed)
        return;
    if (kill(child, SIGKILL) && errno != ESRCH)
        ReportError("kill compiler", NULL, errno);
    *killed = true;
}

int ProgramRun(char *const arguments[], bool merge_stderr,
               ProgramOutputCallback callback, void *context, int *exit_code) {
    const char *program = arguments[0];
    int descriptors[2] = {-1, -1};
    int status = 0, result = -1;
    pid_t child = -1;
    bool child_done = false, eof = false, killed = false, timed_out = false;
    bool callback_failed = false;
    double now, deadline;
    char buffer[4096];

    if (pipe(descriptors)) {
        ReportError("pipe", NULL, errno);
        return -1;
    }
    child = fork();
    if (child < 0) {
        ReportError("fork", NULL, errno);
        goto cleanup;
    }
    if (child == 0) {
        int error;
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0 ||
            (merge_stderr && dup2(descriptors[1], STDERR_FILENO) < 0)) {
            error = errno;
            dprintf(STDERR_FILENO, "ShaderTool: dup2 failed, POSIX error %d: %s\n", error,
                    strerror(error));
            _exit(126);
        }
        close(descriptors[1]);
        execv(program, arguments);
        error = errno;
        dprintf(STDERR_FILENO, "ShaderTool: execv '%s' failed, POSIX error %d: %s\n",
                program, error, strerror(error));
        _exit(127);
    }
    close(descriptors[1]);
    descriptors[1] = -1;
    {
        int flags = fcntl(descriptors[0], F_GETFL);
        if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) < 0) {
            ReportError("set compiler pipe nonblocking", NULL, errno);
            KillOnce(child, &killed);
        }
    }
    now = MonotonicSeconds();
    if (now < 0.0) {
        KillOnce(child, &killed);
        goto drain;
    }
    deadline = now + TimeoutSeconds();

drain:
    while (!eof || !child_done) {
        struct pollfd descriptor = {descriptors[0], POLLIN | POLLHUP, 0};
        int timeout_ms = 100;
        if (!child_done && !killed) {
            now = MonotonicSeconds();
            if (now < 0.0)
                KillOnce(child, &killed);
            else if (now >= deadline) {
                fprintf(stderr, "ShaderTool: compiler timed out after %.3f seconds\n",
                        TimeoutSeconds());
                timed_out = true;
                KillOnce(child, &killed);
            } else if ((deadline - now) * 1000.0 < timeout_ms) {
                timeout_ms = (int)((deadline - now) * 1000.0);
                if (timeout_ms < 0)
                    timeout_ms = 0;
            }
        }
        if (!eof) {
            int poll_result = poll(&descriptor, 1, timeout_ms);
            if (poll_result < 0 && errno != EINTR) {
                ReportError("poll compiler output", NULL, errno);
                KillOnce(child, &killed);
            }
            if (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) {
                for (;;) {
                    ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
                    if (count > 0) {
                        if (callback(context, buffer, (size_t)count, false)) {
                            KillOnce(child, &killed);
                            callback_failed = true;
                        }
                        continue;
                    }
                    if (count == 0)
                        eof = true;
                    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        ReportError("read compiler output", NULL, errno);
                        eof = true;
                    }
                    break;
                }
            }
        }
        if (!child_done) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child)
                child_done = true;
            else if (waited < 0 && errno != EINTR) {
                ReportError("waitpid compiler", NULL, errno);
                child_done = true;
            }
        }
    }
    if (callback(context, "", 0, true))
        goto cleanup;
    if (timed_out || callback_failed)
        goto cleanup;
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = 128 + WTERMSIG(status);
    else {
        fprintf(stderr, "ShaderTool: compiler ended with an unknown status\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (child > 0 && !child_done) {
        KillOnce(child, &killed);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (descriptors[0] >= 0 && close(descriptors[0]))
        ReportError("close compiler pipe", NULL, errno);
    if (descriptors[1] >= 0 && close(descriptors[1]))
        ReportError("close compiler pipe", NULL, errno);
    return result;
}

static int ReadAll(int descriptor, void *bytes, size_t size, const char *path) {
    size_t offset = 0;
    while (offset < size) {
        size_t chunk =
            size - offset > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : size - offset;
        ssize_t count = read(descriptor, (unsigned char *)bytes + offset, chunk);
        if (count > 0)
            offset += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else {
            ReportError("read depfile", path, count < 0 ? errno : EIO);
            return -1;
        }
    }
    return 0;
}

static int WriteAll(int descriptor, const void *bytes, size_t size, const char *path) {
    size_t offset = 0;
    while (offset < size) {
        size_t chunk =
            size - offset > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : size - offset;
        ssize_t count = write(descriptor, (const unsigned char *)bytes + offset, chunk);
        if (count > 0)
            offset += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else {
            ReportError("write depfile", path, count < 0 ? errno : EIO);
            return -1;
        }
    }
    return 0;
}

int FileReadAll(const char *path, ByteBuffer *content) {
    int descriptor = -1, result = -1;
    struct stat record;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        ReportError("open header", path, errno);
        return -1;
    }
    if (fstat(descriptor, &record)) {
        ReportError("stat header", path, errno);
        goto cleanup;
    }
    if (record.st_size < 0 || (uintmax_t)record.st_size > SIZE_MAX) {
        fprintf(stderr, "ShaderTool: header '%s' is too large for this platform\n", path);
        goto cleanup;
    }
    if (ByteBufferResize(content, (size_t)record.st_size + 1))
        goto cleanup;
    content->Size--;
    if (ReadAll(descriptor, content->Buffer, content->Size, path))
        goto cleanup;
    content->Buffer[content->Size] = 0;
    result = 0;
cleanup:
    if (descriptor >= 0 && close(descriptor)) {
        ReportError("close header", path, errno);
        result = -1;
    }
    return result;
}

int FileWriteAll(const char *path, const void *bytes, size_t size,
                                       bool append) {
    int descriptor, flags, result = -1;
    flags = O_WRONLY | (append ? O_APPEND : O_TRUNC);
    descriptor = open(path, flags);
    if (descriptor < 0) {
        ReportError(append ? "open header for append" : "open header for rewrite", path,
                    errno);
        return -1;
    }
    if (WriteAll(descriptor, bytes, size, path))
        goto cleanup;
    result = 0;
cleanup:
    if (close(descriptor)) {
        ReportError("close header", path, errno);
        result = -1;
    }
    return result;
}

int FileWriteIfChanged(const char *path, const void *bytes,
                                            size_t size, bool *changed) {
    int descriptor = -1, result = -1;
    struct stat record;
    ByteBuffer old = {0};
    *changed = true;
    descriptor = open(path, O_RDWR | O_CREAT, 0666);
    if (descriptor < 0) {
        ReportError("open depfile", path, errno);
        return -1;
    }
    if (fstat(descriptor, &record)) {
        ReportError("stat depfile", path, errno);
        goto cleanup;
    }
    if (record.st_size >= 0 && (uintmax_t)record.st_size <= SIZE_MAX &&
        (size_t)record.st_size == size) {
        if (ByteBufferResize(&old, size ? size : 1))
            goto cleanup;
        if (lseek(descriptor, 0, SEEK_SET) < 0) {
            ReportError("seek depfile", path, errno);
            goto cleanup;
        }
        if (ReadAll(descriptor, old.Buffer, size, path))
            goto cleanup;
        if (!memcmp(old.Buffer, bytes, size)) {
            *changed = false;
            result = 0;
            goto cleanup;
        }
    }
    if (lseek(descriptor, 0, SEEK_SET) < 0) {
        ReportError("seek depfile", path, errno);
        goto cleanup;
    }
    if (WriteAll(descriptor, bytes, size, path))
        goto cleanup;
    if ((off_t)size < 0 || (size_t)(off_t)size != size) {
        fprintf(stderr, "ShaderTool: depfile '%s' is too large for this platform\n", path);
        goto cleanup;
    }
    if (ftruncate(descriptor, (off_t)size)) {
        ReportError("truncate depfile", path, errno);
        goto cleanup;
    }
    result = 0;
cleanup:
    ByteBufferDeinit(&old);
    if (descriptor >= 0 && close(descriptor)) {
        ReportError("close depfile", path, errno);
        result = -1;
    }
    return result;
}


#endif
