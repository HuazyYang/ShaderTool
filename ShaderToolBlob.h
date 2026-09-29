#ifndef SHADER_TOOL_BLOB_H
#define SHADER_TOOL_BLOB_H

/*
 * ShaderToolBlob -- pure C11, libc-only reader/writer for the NVSP permutation
 * blob format (byte-compatible with ShaderMake):
 *
 *     char[4]  "NVSP"                              (no NUL)
 *     repeat per permutation:
 *         uint32_t permutationSize;                (little-endian, packed)
 *         uint32_t dataSize;                       (little-endian, packed)
 *         char     key[permutationSize];           (no NUL)
 *         uint8_t  data[dataSize];
 *
 * There is no count field and no terminator; readers stop when 8 or fewer
 * bytes remain or when an entry's dataSize is 0. Both header fields are
 * written and read as explicit little-endian byte sequences regardless of
 * host endianness.
 *
 * Key policy (the one canonical rule, used by writer and reader alike):
 * defines are stable-sorted by NAME only (ordinal byte compare of the part
 * before '='), formatted "NAME=VALUE" and joined by single spaces; a define
 * with a NULL or empty value is normalized to "NAME=1". Keys never contain
 * empty values or stray spaces. Consequently a define name must be non-empty
 * and must not contain ' ' or '=', and a value must not contain ' ';
 * violations are rejected with -1.
 *
 * All functions return 0 on success and -1 on failure unless noted.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShaderToolBlobConstant {
    const char *Name;
    const char *Value;   /* NULL or "" is treated as "1" */
} ShaderToolBlobConstant;

/*
 * Builds the canonical permutation key for a set of defines.
 * Two-call sizing: buffer==NULL => *length receives the required size
 * (excluding the terminating NUL) and the call returns 0. With a buffer,
 * capacity must cover the key plus a terminating NUL; on success the key is
 * written NUL-terminated and *length (optional in this case) receives its
 * length. count==0 yields the empty key ("").
 */
int ShaderToolBlobBuildKey(const ShaderToolBlobConstant *constants, size_t count,
                           char *buffer, size_t capacity, size_t *length);

/*
 * Finds the permutation matching the given defines in an NVSP blob.
 * Raw fallback: data without the NVSP magic + count==0 => returns the whole
 * buffer with 0 (the data is a raw shader binary, no permutation requested).
 * Without the magic + count>0 => -1. Not found or truncated => -1.
 */
int ShaderToolBlobFindPermutation(const void *blob, size_t size,
                                  const ShaderToolBlobConstant *constants, size_t count,
                                  const void **binary, size_t *binary_size);

/*
 * Called once per permutation; a nonzero return stops the enumeration.
 * `key` points into the blob and is NOT NUL-terminated; an empty key is
 * reported with key_length==0 (presentation such as "<default>" is the
 * caller's business).
 */
typedef int (*ShaderToolBlobEnumerateCallback)(void *context, const char *key,
    size_t key_length, const void *binary, size_t binary_size);

/*
 * Walks every well-formed entry of an NVSP blob. A blob without the magic
 * enumerates nothing (returns 0); parsing stops silently at a truncated
 * entry or a dataSize==0 terminator, mirroring the reader in Find.
 * Returns 0 whether the walk completed or the callback stopped it early;
 * -1 only for invalid arguments.
 */
int ShaderToolBlobEnumeratePermutations(const void *blob, size_t size,
    ShaderToolBlobEnumerateCallback callback, void *context);

/*
 * Formats a human-readable "permutation not found" message: the requested
 * key as "NAME=VALUE;" pairs (or "<default>" when count==0), then the list
 * of permutations available in the blob, one key per line.
 * Two-call sizing like ShaderToolBlobBuildKey (size excludes the NUL).
 * On a capacity failure the buffer contents are unspecified (it may have
 * been partially overwritten, without a terminating NUL).
 */
int ShaderToolBlobFormatNotFoundMessage(const void *blob, size_t size,
    const ShaderToolBlobConstant *constants, size_t count,
    char *buffer, size_t capacity, size_t *length);

/* Returns 0 on success, nonzero on failure. */
typedef int (*ShaderToolBlobWriteCallback)(void *context, const void *data, size_t size);

/* Writes the 4-byte NVSP signature. */
int ShaderToolBlobWriteFileHeader(ShaderToolBlobWriteCallback write, void *context);

/*
 * Writes one permutation entry (little-endian header, key, data).
 * key_length and binary_size must fit in uint32_t; binary_size must be
 * nonzero (readers treat dataSize==0 as a blob terminator).
 */
int ShaderToolBlobWritePermutation(ShaderToolBlobWriteCallback write, void *context,
    const char *key, size_t key_length, const void *binary, size_t binary_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SHADER_TOOL_BLOB_H */
