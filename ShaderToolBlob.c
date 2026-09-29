#include "ShaderToolBlob.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char BLOB_SIGNATURE[4] = {'N', 'V', 'S', 'P'};

#define BLOB_SIGNATURE_SIZE   ((size_t)4)
#define BLOB_ENTRY_HEADER_SIZE ((size_t)8)   /* uint32 permutationSize + uint32 dataSize */

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t ReadU32Le(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void WriteU32Le(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
}

static int AddSizeChecked(size_t a, size_t b, size_t *result)
{
    if (b > SIZE_MAX - a)
        return -1;

    *result = a + b;
    return 0;
}

static const char *NormalizeValue(const char *value)
{
    return (value == NULL || value[0] == '\0') ? "1" : value;
}

/* Key policy: names must be non-empty and free of ' '/'=', values free of ' ',
   so that a key always parses as "NAME=VALUE" pairs joined by single spaces. */
static int ValidateConstant(const ShaderToolBlobConstant *constant)
{
    const char *value = NULL;

    if (constant->Name == NULL || constant->Name[0] == '\0')
        return -1;

    if (strchr(constant->Name, ' ') != NULL || strchr(constant->Name, '=') != NULL)
        return -1;

    value = NormalizeValue(constant->Value);
    if (strchr(value, ' ') != NULL)
        return -1;

    return 0;
}

/* Stable insertion sort of an index array, ordered by ordinal compare of the
   constant NAMES only. Counts are tiny; stability keeps equal names in input
   order, matching the canonical key policy. */
static void SortConstantIndices(const ShaderToolBlobConstant *constants,
                                size_t *indices, size_t count)
{
    size_t i = 0;

    for (i = 1; i < count; i++) {
        size_t index = indices[i];
        size_t j = i;

        while (j > 0 && strcmp(constants[indices[j - 1]].Name,
                               constants[index].Name) > 0) {
            indices[j] = indices[j - 1];
            j--;
        }

        indices[j] = index;
    }
}

/* ------------------------------------------------------------------------- */
/* Key building                                                               */
/* ------------------------------------------------------------------------- */

int ShaderToolBlobBuildKey(const ShaderToolBlobConstant *constants, size_t count,
                           char *buffer, size_t capacity, size_t *length)
{
    int result = -1;
    size_t required = 0;
    size_t stack_indices[16] = {0};
    size_t *indices = stack_indices;
    size_t *heap_indices = NULL;
    size_t i = 0;
    char *cursor = NULL;

    if (count > 0 && constants == NULL)
        goto cleanup;

    if (buffer == NULL && length == NULL)
        goto cleanup;

    for (i = 0; i < count; i++) {
        size_t pair_size = 0;

        if (ValidateConstant(&constants[i]) != 0)
            goto cleanup;

        /* "NAME=VALUE" plus a joining space for every pair after the first */
        if (AddSizeChecked(strlen(constants[i].Name),
                           strlen(NormalizeValue(constants[i].Value)), &pair_size) != 0)
            goto cleanup;
        if (AddSizeChecked(pair_size, (i > 0) ? 2 : 1, &pair_size) != 0)
            goto cleanup;
        if (AddSizeChecked(required, pair_size, &required) != 0)
            goto cleanup;
    }

    if (buffer == NULL) {
        *length = required;
        result = 0;
        goto cleanup;
    }

    if (capacity <= required) /* need room for the key plus its NUL */
        goto cleanup;

    if (count > sizeof(stack_indices) / sizeof(stack_indices[0])) {
        heap_indices = (size_t *)malloc(count * sizeof(size_t));
        if (heap_indices == NULL)
            goto cleanup;
        indices = heap_indices;
    }

    for (i = 0; i < count; i++)
        indices[i] = i;

    SortConstantIndices(constants, indices, count);

    cursor = buffer;
    for (i = 0; i < count; i++) {
        const ShaderToolBlobConstant *constant = &constants[indices[i]];
        const char *value = NormalizeValue(constant->Value);
        size_t name_length = strlen(constant->Name);
        size_t value_length = strlen(value);

        if (i > 0)
            *cursor++ = ' ';

        memcpy(cursor, constant->Name, name_length);
        cursor += name_length;
        *cursor++ = '=';
        memcpy(cursor, value, value_length);
        cursor += value_length;
    }

    *cursor = '\0';
    if (length != NULL)
        *length = required;

    result = 0;

cleanup:
    free(heap_indices);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Reading                                                                    */
/* ------------------------------------------------------------------------- */

int ShaderToolBlobFindPermutation(const void *blob, size_t size,
                                  const ShaderToolBlobConstant *constants, size_t count,
                                  const void **binary, size_t *binary_size)
{
    int result = -1;
    char stack_key[256] = {0};
    char *key = stack_key;
    char *heap_key = NULL;
    size_t key_length = 0;
    const unsigned char *cursor = NULL;
    size_t remaining = 0;

    if (blob == NULL || size < BLOB_SIGNATURE_SIZE)
        goto cleanup;

    if (binary == NULL || binary_size == NULL)
        goto cleanup;

    if (memcmp(blob, BLOB_SIGNATURE, BLOB_SIGNATURE_SIZE) != 0) {
        if (count == 0) {
            /* Not a permutation blob and no permutation requested: raw data. */
            *binary = blob;
            *binary_size = size;
            result = 0;
        }
        goto cleanup;
    }

    if (ShaderToolBlobBuildKey(constants, count, NULL, 0, &key_length) != 0)
        goto cleanup;

    if (key_length >= sizeof(stack_key)) {
        if (key_length == SIZE_MAX)
            goto cleanup;
        heap_key = (char *)malloc(key_length + 1);
        if (heap_key == NULL)
            goto cleanup;
        key = heap_key;
    }

    if (ShaderToolBlobBuildKey(constants, count, key, key_length + 1, NULL) != 0)
        goto cleanup;

    cursor = (const unsigned char *)blob + BLOB_SIGNATURE_SIZE;
    remaining = size - BLOB_SIGNATURE_SIZE;

    while (remaining > BLOB_ENTRY_HEADER_SIZE) {
        uint32_t permutation_size = ReadU32Le(cursor);
        uint32_t data_size = ReadU32Le(cursor + 4);
        uint64_t entry_size = (uint64_t)BLOB_ENTRY_HEADER_SIZE +
                              permutation_size + data_size;

        if (data_size == 0)
            goto cleanup; /* last header in the blob is empty */

        if ((uint64_t)remaining < entry_size)
            goto cleanup; /* insufficient bytes in the blob, cannot continue */

        if ((size_t)permutation_size == key_length &&
            (key_length == 0 ||
             memcmp(cursor + BLOB_ENTRY_HEADER_SIZE, key, key_length) == 0)) {
            *binary = cursor + BLOB_ENTRY_HEADER_SIZE + permutation_size;
            *binary_size = data_size;
            result = 0;
            goto cleanup;
        }

        cursor += (size_t)entry_size;
        remaining -= (size_t)entry_size;
    }

cleanup:
    free(heap_key);
    return result;
}

int ShaderToolBlobEnumeratePermutations(const void *blob, size_t size,
    ShaderToolBlobEnumerateCallback callback, void *context)
{
    const unsigned char *cursor = NULL;
    size_t remaining = 0;

    if (blob == NULL || callback == NULL)
        return -1;

    if (size < BLOB_SIGNATURE_SIZE ||
        memcmp(blob, BLOB_SIGNATURE, BLOB_SIGNATURE_SIZE) != 0)
        return 0; /* not a permutation blob: nothing to enumerate */

    cursor = (const unsigned char *)blob + BLOB_SIGNATURE_SIZE;
    remaining = size - BLOB_SIGNATURE_SIZE;

    while (remaining > BLOB_ENTRY_HEADER_SIZE) {
        uint32_t permutation_size = ReadU32Le(cursor);
        uint32_t data_size = ReadU32Le(cursor + 4);
        uint64_t entry_size = (uint64_t)BLOB_ENTRY_HEADER_SIZE +
                              permutation_size + data_size;

        if (data_size == 0)
            return 0;

        if ((uint64_t)remaining < entry_size)
            return 0;

        if (callback(context, (const char *)(cursor + BLOB_ENTRY_HEADER_SIZE),
                     permutation_size,
                     cursor + BLOB_ENTRY_HEADER_SIZE + permutation_size,
                     data_size) != 0)
            return 0;

        cursor += (size_t)entry_size;
        remaining -= (size_t)entry_size;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                                */
/* ------------------------------------------------------------------------- */

typedef struct MessageSink {
    char *buffer;      /* NULL when only measuring */
    size_t capacity;
    size_t length;     /* total required length, may exceed what fits */
} MessageSink;

static void SinkAppend(MessageSink *sink, const char *text, size_t text_length)
{
    if (sink->buffer != NULL && sink->length < sink->capacity) {
        size_t fit = sink->capacity - sink->length;

        if (fit > text_length)
            fit = text_length;

        memcpy(sink->buffer + sink->length, text, fit);
    }

    sink->length += text_length;
}

static void SinkAppendString(MessageSink *sink, const char *text)
{
    SinkAppend(sink, text, strlen(text));
}

typedef struct EnumerateMessageContext {
    MessageSink *sink;
    size_t permutation_count;
} EnumerateMessageContext;

static int CountPermutationCallback(void *context, const char *key,
    size_t key_length, const void *binary, size_t binary_size)
{
    EnumerateMessageContext *enum_context = (EnumerateMessageContext *)context;

    (void)key;
    (void)key_length;
    (void)binary;
    (void)binary_size;

    enum_context->permutation_count++;
    return 0;
}

static int AppendPermutationCallback(void *context, const char *key,
    size_t key_length, const void *binary, size_t binary_size)
{
    EnumerateMessageContext *enum_context = (EnumerateMessageContext *)context;

    (void)binary;
    (void)binary_size;

    if (key_length > 0)
        SinkAppend(enum_context->sink, key, key_length);
    else
        SinkAppendString(enum_context->sink, "<default>");

    SinkAppendString(enum_context->sink, "\n");
    return 0;
}

int ShaderToolBlobFormatNotFoundMessage(const void *blob, size_t size,
    const ShaderToolBlobConstant *constants, size_t count,
    char *buffer, size_t capacity, size_t *length)
{
    int result = -1;
    MessageSink sink = {0};
    EnumerateMessageContext enum_context = {0};
    size_t i = 0;

    if (count > 0 && constants == NULL)
        goto cleanup;

    if (buffer == NULL && length == NULL)
        goto cleanup;

    sink.buffer = buffer;
    sink.capacity = capacity;
    enum_context.sink = &sink;

    SinkAppendString(&sink,
        "Couldn't find the required shader permutation in the blob, "
        "or the blob is corrupted.\n");
    SinkAppendString(&sink, "Required permutation key: \n");

    if (count > 0) {
        for (i = 0; i < count; i++) {
            const char *name = (constants[i].Name != NULL) ? constants[i].Name : "";

            SinkAppendString(&sink, name);
            SinkAppendString(&sink, "=");
            SinkAppendString(&sink, NormalizeValue(constants[i].Value));
            SinkAppendString(&sink, ";");
        }
    } else {
        SinkAppendString(&sink, "<default>");
    }

    SinkAppendString(&sink, "\n");

    if (blob != NULL)
        ShaderToolBlobEnumeratePermutations(blob, size,
            CountPermutationCallback, &enum_context);

    if (enum_context.permutation_count > 0) {
        SinkAppendString(&sink, "Permutations available in the blob:\n");
        ShaderToolBlobEnumeratePermutations(blob, size,
            AppendPermutationCallback, &enum_context);
    } else {
        SinkAppendString(&sink, "No permutations found in the blob.");
    }

    if (buffer == NULL) {
        *length = sink.length;
        result = 0;
        goto cleanup;
    }

    if (capacity <= sink.length) /* need room for the text plus its NUL */
        goto cleanup;

    buffer[sink.length] = '\0';
    if (length != NULL)
        *length = sink.length;

    result = 0;

cleanup:
    return result;
}

/* ------------------------------------------------------------------------- */
/* Writing                                                                    */
/* ------------------------------------------------------------------------- */

int ShaderToolBlobWriteFileHeader(ShaderToolBlobWriteCallback write, void *context)
{
    if (write == NULL)
        return -1;

    return (write(context, BLOB_SIGNATURE, BLOB_SIGNATURE_SIZE) == 0) ? 0 : -1;
}

int ShaderToolBlobWritePermutation(ShaderToolBlobWriteCallback write, void *context,
    const char *key, size_t key_length, const void *binary, size_t binary_size)
{
    unsigned char header[BLOB_ENTRY_HEADER_SIZE] = {0};

    if (write == NULL)
        return -1;

    if (key == NULL && key_length > 0)
        return -1;

    /* dataSize==0 acts as a blob terminator for readers: refuse to write one. */
    if (binary == NULL || binary_size == 0)
        return -1;

    if (key_length > UINT32_MAX || binary_size > UINT32_MAX)
        return -1;

    WriteU32Le(header, (uint32_t)key_length);
    WriteU32Le(header + 4, (uint32_t)binary_size);

    if (write(context, header, sizeof(header)) != 0)
        return -1;

    if (key_length > 0 && write(context, key, key_length) != 0)
        return -1;

    if (write(context, binary, binary_size) != 0)
        return -1;

    return 0;
}
