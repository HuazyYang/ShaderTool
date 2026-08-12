#ifndef SHADER_TOOL_UTILS_H
#define SHADER_TOOL_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef struct DynamicArray {
    void *Buffer;
    size_t Size;
    size_t Capacity;
} DynamicArray;

typedef struct AStringView {
    const char *Buffer;
    size_t Length;
} AStringView;

#define A_STRING_VIEW_LITERAL(text) \
    ((AStringView){(text), sizeof(text) - 1})

typedef struct AString {
    union {
        DynamicArray Array;
        struct {
            char *Buffer;
            size_t Length;
            size_t Capacity;
        };
    };
} AString;

typedef struct ByteBuffer {
    union {
        DynamicArray Array;
        struct {
            char *Buffer;
            size_t Size;
            size_t Capacity;
        };
    };
} ByteBuffer;

typedef struct WideBuffer {
    union {
        DynamicArray Array;
        struct {
            wchar_t *Buffer;
            size_t Size;
            size_t Capacity;
        };
    };
} WideBuffer;

typedef int (*ProgramOutputCallback)(void *context, const char *bytes, size_t size,
                                     bool final);

void LogV(int level, const char *format, ...);
#define LOG_E(...) LogV(0, __VA_ARGS__)
#define LOG_W(...) LogV(1, __VA_ARGS__)
#define LOG_I(...) LogV(2, __VA_ARGS__)

int DynamicArrayReserve(DynamicArray *array, size_t element_size, size_t size,
                        size_t extra_capacity, const char *description);
int DynamicArrayResize(DynamicArray *array, size_t element_size, size_t size,
                       size_t extra_capacity, const char *description);
int DynamicArrayAppend(DynamicArray *array, size_t element_size, const void *elements,
                       size_t count, const char *description);
int DynamicArrayErase(DynamicArray *array, size_t element_size, size_t start, size_t end);
void DynamicArrayDeinit(DynamicArray *array);

#define DYNAMIC_ARRAY_RESIZE_TYPED(array, type, size, extra_capacity, description)    \
    DynamicArrayResize(&(array)->Array, sizeof(type), (size), (extra_capacity), \
                       (description))
#define DYNAMIC_ARRAY_APPEND_TYPED(array, type, elements, count, description) \
    DynamicArrayAppend(&(array)->Array, sizeof(type), (elements), (count), (description))
#define DYNAMIC_ARRAY_ERASE_TYPED(array, type, start, end) \
    DynamicArrayErase(&(array)->Array, sizeof(type), (start), (end))
#define DYNAMIC_ARRAY_DEINIT_TYPED(array) DynamicArrayDeinit(&(array)->Array)

AStringView AStringViewFromCString(const char *text);
AStringView AStringViewFromBuffer(const char *data, size_t length);
bool AStringViewIsEmpty(const AStringView *string);
bool AStringViewEqual(const AStringView *string, const char *text);
bool AStringViewEqual2(const AStringView *left, const AStringView *right);
bool AStringViewEqualAtLeast(const AStringView *string, const AStringView *prefix);

void AStringDeinit(AString *string);
int AStringResize(AString *string, size_t length);
int AStringCatN(AString *string, size_t count, ...);
int AStringCopy(AString *string, const AStringView *source);
int NormalizeAPath(const AStringView *string, AString *result);
int AStringAppendFormat(AString *string, const char *format, ...);

int ByteBufferResize(ByteBuffer *buffer, size_t size);
int ByteBufferWrite(ByteBuffer *buffer, const void *data, size_t size);
void ByteBufferGetAStringView(const ByteBuffer *buffer, AStringView *view);
int ByteBufferCatAStringViews(ByteBuffer *buffer, size_t count, ...);
void ByteBufferDeinit(ByteBuffer *buffer);
int ByteBufferEraseRange(ByteBuffer *buffer, size_t start, size_t end);

int ProgramRun(char *const cmd_line[], bool merge_stderr,
               ProgramOutputCallback callback, void *context, int *exit_code);
int FileReadAll(const char *path, ByteBuffer *content);
int FileWriteAll(const char *path, const void *bytes, size_t size, bool append);
int FileWriteIfChanged(const char *path, const void *bytes, size_t size, bool *changed);

#endif
