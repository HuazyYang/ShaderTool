/* Standalone tests for ShaderToolBlob (pure C11, libc only).
   Build example: cl /std:c11 /W4 ..\ShaderToolBlob.c BlobTests.c */

#include "../ShaderToolBlob.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failed_checks = 0;
static int g_total_checks = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        g_total_checks++;                                                      \
        if (!(condition)) {                                                    \
            g_failed_checks++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------- */
/* In-memory write sink                                                       */
/* ------------------------------------------------------------------------- */

typedef struct BlobSink {
    unsigned char data[4096];
    size_t size;
    int fail_writes; /* nonzero: report failure to the writer */
} BlobSink;

static int BlobSinkWrite(void *context, const void *data, size_t size)
{
    BlobSink *sink = (BlobSink *)context;

    if (sink->fail_writes)
        return -1;

    if (sink->size + size > sizeof(sink->data))
        return -1;

    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* BuildKey                                                                   */
/* ------------------------------------------------------------------------- */

static void TestBuildKey(void)
{
    char buffer[256] = {0};
    size_t length = 0;

    /* Unsorted input sorts by NAME; NULL and "" values normalize to "1". */
    {
        const ShaderToolBlobConstant constants[] = {
            {"B", "0"}, {"A", NULL}, {"C", ""},
        };

        /* Two-call sizing first. */
        CHECK(ShaderToolBlobBuildKey(constants, 3, NULL, 0, &length) == 0);
        CHECK(length == strlen("A=1 B=0 C=1"));

        CHECK(ShaderToolBlobBuildKey(constants, 3, buffer, sizeof(buffer), &length) == 0);
        CHECK(strcmp(buffer, "A=1 B=0 C=1") == 0);
        CHECK(length == strlen(buffer));

        /* Exact capacity (length + NUL) succeeds; one byte less fails. */
        CHECK(ShaderToolBlobBuildKey(constants, 3, buffer, length + 1, NULL) == 0);
        CHECK(ShaderToolBlobBuildKey(constants, 3, buffer, length, NULL) == -1);
    }

    /* Stable sort: equal names keep their input order. */
    {
        const ShaderToolBlobConstant constants[] = {
            {"Z", "9"}, {"X", "2"}, {"X", "3"},
        };

        CHECK(ShaderToolBlobBuildKey(constants, 3, buffer, sizeof(buffer), NULL) == 0);
        CHECK(strcmp(buffer, "X=2 X=3 Z=9") == 0);
    }

    /* Empty constants build the empty key. */
    length = 123;
    CHECK(ShaderToolBlobBuildKey(NULL, 0, NULL, 0, &length) == 0);
    CHECK(length == 0);
    CHECK(ShaderToolBlobBuildKey(NULL, 0, buffer, sizeof(buffer), &length) == 0);
    CHECK(buffer[0] == '\0' && length == 0);

    /* Invalid constants are rejected. */
    {
        const ShaderToolBlobConstant bad_name_space[] = {{"A B", "1"}};
        const ShaderToolBlobConstant bad_name_equals[] = {{"A=B", "1"}};
        const ShaderToolBlobConstant bad_name_empty[] = {{"", "1"}};
        const ShaderToolBlobConstant bad_name_null[] = {{NULL, "1"}};
        const ShaderToolBlobConstant bad_value_space[] = {{"A", "1 2"}};

        CHECK(ShaderToolBlobBuildKey(bad_name_space, 1, buffer, sizeof(buffer), NULL) == -1);
        CHECK(ShaderToolBlobBuildKey(bad_name_equals, 1, buffer, sizeof(buffer), NULL) == -1);
        CHECK(ShaderToolBlobBuildKey(bad_name_empty, 1, buffer, sizeof(buffer), NULL) == -1);
        CHECK(ShaderToolBlobBuildKey(bad_name_null, 1, buffer, sizeof(buffer), NULL) == -1);
        CHECK(ShaderToolBlobBuildKey(bad_value_space, 1, buffer, sizeof(buffer), NULL) == -1);
    }

    /* Sizing call with neither buffer nor length is invalid. */
    CHECK(ShaderToolBlobBuildKey(NULL, 0, NULL, 0, NULL) == -1);
}

/* ------------------------------------------------------------------------- */
/* Write -> find round trip                                                   */
/* ------------------------------------------------------------------------- */

static const ShaderToolBlobConstant g_variant_a[] = {{"MODE", "0"}, {"ALPHA", NULL}};
static const ShaderToolBlobConstant g_variant_b[] = {{"MODE", "1"}, {"ALPHA", NULL}};
static const ShaderToolBlobConstant g_variant_c[] = {{"MODE", "2"}, {"ALPHA", ""}};

static const unsigned char g_payload_a[] = {1, 2, 3, 4};
static const unsigned char g_payload_b[] = {5, 6, 7, 8, 9};
static const unsigned char g_payload_c[] = {200, 201};

static int BuildTestBlob(BlobSink *sink)
{
    const struct {
        const ShaderToolBlobConstant *constants;
        size_t count;
        const unsigned char *payload;
        size_t payload_size;
    } entries[] = {
        {g_variant_a, 2, g_payload_a, sizeof(g_payload_a)},
        {g_variant_b, 2, g_payload_b, sizeof(g_payload_b)},
        {g_variant_c, 2, g_payload_c, sizeof(g_payload_c)},
    };
    char key[256] = {0};
    size_t key_length = 0;
    size_t i = 0;

    if (ShaderToolBlobWriteFileHeader(BlobSinkWrite, sink) != 0)
        return -1;

    for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (ShaderToolBlobBuildKey(entries[i].constants, entries[i].count,
                                   key, sizeof(key), &key_length) != 0)
            return -1;
        if (ShaderToolBlobWritePermutation(BlobSinkWrite, sink, key, key_length,
                                           entries[i].payload, entries[i].payload_size) != 0)
            return -1;
    }

    return 0;
}

static void TestWriteFindRoundTrip(void)
{
    BlobSink sink = {{0}, 0, 0};
    const void *binary = NULL;
    size_t binary_size = 0;

    CHECK(BuildTestBlob(&sink) == 0);
    CHECK(sink.size > 4 && memcmp(sink.data, "NVSP", 4) == 0);

    /* Every written permutation is found, in any query order of defines. */
    {
        const ShaderToolBlobConstant query_b[] = {{"ALPHA", "1"}, {"MODE", "1"}};

        CHECK(ShaderToolBlobFindPermutation(sink.data, sink.size, g_variant_a, 2,
                                            &binary, &binary_size) == 0);
        CHECK(binary_size == sizeof(g_payload_a));
        CHECK(binary != NULL && memcmp(binary, g_payload_a, binary_size) == 0);

        CHECK(ShaderToolBlobFindPermutation(sink.data, sink.size, query_b, 2,
                                            &binary, &binary_size) == 0);
        CHECK(binary_size == sizeof(g_payload_b));
        CHECK(binary != NULL && memcmp(binary, g_payload_b, binary_size) == 0);

        CHECK(ShaderToolBlobFindPermutation(sink.data, sink.size, g_variant_c, 2,
                                            &binary, &binary_size) == 0);
        CHECK(binary_size == sizeof(g_payload_c));
        CHECK(binary != NULL && memcmp(binary, g_payload_c, binary_size) == 0);
    }

    /* Missing permutation and empty query on a keyed blob both fail. */
    {
        const ShaderToolBlobConstant missing[] = {{"MODE", "3"}, {"ALPHA", "1"}};

        CHECK(ShaderToolBlobFindPermutation(sink.data, sink.size, missing, 2,
                                            &binary, &binary_size) == -1);
        CHECK(ShaderToolBlobFindPermutation(sink.data, sink.size, NULL, 0,
                                            &binary, &binary_size) == -1);
    }

    /* An empty-key entry round-trips too (found with count==0). */
    {
        BlobSink keyless = {{0}, 0, 0};
        static const unsigned char payload[] = {42};

        CHECK(ShaderToolBlobWriteFileHeader(BlobSinkWrite, &keyless) == 0);
        CHECK(ShaderToolBlobWritePermutation(BlobSinkWrite, &keyless, NULL, 0,
                                             payload, sizeof(payload)) == 0);
        CHECK(ShaderToolBlobFindPermutation(keyless.data, keyless.size, NULL, 0,
                                            &binary, &binary_size) == 0);
        CHECK(binary_size == 1 && ((const unsigned char *)binary)[0] == 42);
    }

    /* Writer rejects a dataSize==0 entry and oversized inputs are impossible
       to build here, but a failing sink propagates as -1. */
    {
        BlobSink failing = {{0}, 0, 1};
        static const unsigned char payload[] = {1};

        CHECK(ShaderToolBlobWritePermutation(BlobSinkWrite, &sink, "K=1", 3, payload, 0) == -1);
        CHECK(ShaderToolBlobWritePermutation(BlobSinkWrite, &sink, "K=1", 3, NULL, 4) == -1);
        CHECK(ShaderToolBlobWriteFileHeader(BlobSinkWrite, &failing) == -1);
        CHECK(ShaderToolBlobWritePermutation(BlobSinkWrite, &failing, "K=1", 3,
                                             payload, sizeof(payload)) == -1);
    }
}

/* ------------------------------------------------------------------------- */
/* Raw fallback / malformed blobs                                             */
/* ------------------------------------------------------------------------- */

static void TestRawFallbackAndMalformed(void)
{
    static const unsigned char raw[] = {0x44, 0x58, 0x42, 0x43, 0xAA, 0xBB}; /* "DXBC".. */
    const ShaderToolBlobConstant query[] = {{"MODE", "1"}};
    const void *binary = NULL;
    size_t binary_size = 0;

    /* No magic + count==0: the whole buffer is the binary. */
    CHECK(ShaderToolBlobFindPermutation(raw, sizeof(raw), NULL, 0,
                                        &binary, &binary_size) == 0);
    CHECK(binary == raw && binary_size == sizeof(raw));

    /* No magic + count>0: failure. */
    CHECK(ShaderToolBlobFindPermutation(raw, sizeof(raw), query, 1,
                                        &binary, &binary_size) == -1);

    /* NULL blob / undersized blob / NULL outputs. */
    CHECK(ShaderToolBlobFindPermutation(NULL, 16, NULL, 0, &binary, &binary_size) == -1);
    CHECK(ShaderToolBlobFindPermutation(raw, 3, NULL, 0, &binary, &binary_size) == -1);
    CHECK(ShaderToolBlobFindPermutation(raw, sizeof(raw), NULL, 0, NULL, &binary_size) == -1);
    CHECK(ShaderToolBlobFindPermutation(raw, sizeof(raw), NULL, 0, &binary, NULL) == -1);
}

static void TestTruncatedBlob(void)
{
    BlobSink sink = {{0}, 0, 0};
    const void *binary = NULL;
    size_t binary_size = 0;
    size_t truncated_size = 0;

    CHECK(BuildTestBlob(&sink) == 0);

    /* Cut the last entry short: earlier entries stay reachable, the
       truncated one never matches, and no out-of-bounds read occurs. */
    truncated_size = sink.size - 1;
    CHECK(ShaderToolBlobFindPermutation(sink.data, truncated_size, g_variant_a, 2,
                                        &binary, &binary_size) == 0);
    CHECK(ShaderToolBlobFindPermutation(sink.data, truncated_size, g_variant_c, 2,
                                        &binary, &binary_size) == -1);

    /* Truncate into the middle of an entry header as well. */
    CHECK(ShaderToolBlobFindPermutation(sink.data, 4 + 5, g_variant_a, 2,
                                        &binary, &binary_size) == -1);

    /* Exactly one header's worth of trailing bytes is not parsed
       (the loop requires remaining > 8). */
    {
        unsigned char short_blob[12] = {'N', 'V', 'S', 'P', 0, 0, 0, 0, 1, 0, 0, 0};

        CHECK(ShaderToolBlobFindPermutation(short_blob, sizeof(short_blob), NULL, 0,
                                            &binary, &binary_size) == -1);
    }
}

static void TestDataSizeZeroTerminator(void)
{
    /* "NVSP" + entry with permutationSize=0, dataSize=0, then garbage that
       would match if parsing continued past the terminator. */
    unsigned char blob[64] = {'N', 'V', 'S', 'P',
                              0, 0, 0, 0,   /* permutationSize = 0 */
                              0, 0, 0, 0};  /* dataSize = 0 -> terminator */
    const void *binary = NULL;
    size_t binary_size = 0;

    memset(blob + 12, 0xCD, sizeof(blob) - 12);

    CHECK(ShaderToolBlobFindPermutation(blob, sizeof(blob), NULL, 0,
                                        &binary, &binary_size) == -1);
}

/* ------------------------------------------------------------------------- */
/* Enumeration                                                                */
/* ------------------------------------------------------------------------- */

typedef struct EnumerateRecorder {
    int calls;
    int stop_after;      /* 0 = never stop */
    char first_key[256];
    size_t first_key_length;
    size_t total_binary_size;
} EnumerateRecorder;

static int RecordPermutation(void *context, const char *key, size_t key_length,
                             const void *binary, size_t binary_size)
{
    EnumerateRecorder *recorder = (EnumerateRecorder *)context;

    (void)binary;

    recorder->calls++;
    if (recorder->calls == 1 && key_length < sizeof(recorder->first_key)) {
        memcpy(recorder->first_key, key, key_length);
        recorder->first_key[key_length] = '\0';
        recorder->first_key_length = key_length;
    }
    recorder->total_binary_size += binary_size;

    return (recorder->stop_after != 0 && recorder->calls >= recorder->stop_after) ? 1 : 0;
}

static void TestEnumerate(void)
{
    BlobSink sink = {{0}, 0, 0};
    EnumerateRecorder recorder = {0, 0, {0}, 0, 0};

    CHECK(BuildTestBlob(&sink) == 0);

    /* Full walk sees all three entries in file order. */
    CHECK(ShaderToolBlobEnumeratePermutations(sink.data, sink.size,
                                              RecordPermutation, &recorder) == 0);
    CHECK(recorder.calls == 3);
    CHECK(strcmp(recorder.first_key, "ALPHA=1 MODE=0") == 0);
    CHECK(recorder.total_binary_size ==
          sizeof(g_payload_a) + sizeof(g_payload_b) + sizeof(g_payload_c));

    /* Early stop after the first entry. */
    {
        EnumerateRecorder stopper = {0, 1, {0}, 0, 0};

        CHECK(ShaderToolBlobEnumeratePermutations(sink.data, sink.size,
                                                  RecordPermutation, &stopper) == 0);
        CHECK(stopper.calls == 1);
    }

    /* Truncated blob stops without touching the cut entry. */
    {
        EnumerateRecorder truncated = {0, 0, {0}, 0, 0};

        CHECK(ShaderToolBlobEnumeratePermutations(sink.data, sink.size - 1,
                                                  RecordPermutation, &truncated) == 0);
        CHECK(truncated.calls == 2);
    }

    /* Non-NVSP data enumerates nothing; NULL arguments are rejected. */
    {
        EnumerateRecorder none = {0, 0, {0}, 0, 0};
        static const unsigned char raw[] = {1, 2, 3, 4, 5};

        CHECK(ShaderToolBlobEnumeratePermutations(raw, sizeof(raw),
                                                  RecordPermutation, &none) == 0);
        CHECK(none.calls == 0);
        CHECK(ShaderToolBlobEnumeratePermutations(NULL, 0, RecordPermutation, &none) == -1);
        CHECK(ShaderToolBlobEnumeratePermutations(raw, sizeof(raw), NULL, &none) == -1);
    }

    /* Empty key is reported with key_length==0. */
    {
        BlobSink keyless = {{0}, 0, 0};
        EnumerateRecorder empty_key = {0, 0, {0xAB}, 99, 0};
        static const unsigned char payload[] = {7};

        CHECK(ShaderToolBlobWriteFileHeader(BlobSinkWrite, &keyless) == 0);
        CHECK(ShaderToolBlobWritePermutation(BlobSinkWrite, &keyless, "", 0,
                                             payload, sizeof(payload)) == 0);
        CHECK(ShaderToolBlobEnumeratePermutations(keyless.data, keyless.size,
                                                  RecordPermutation, &empty_key) == 0);
        CHECK(empty_key.calls == 1 && empty_key.first_key_length == 0);
    }
}

/* ------------------------------------------------------------------------- */
/* FormatNotFoundMessage                                                      */
/* ------------------------------------------------------------------------- */

static void TestFormatNotFoundMessage(void)
{
    BlobSink sink = {{0}, 0, 0};
    const ShaderToolBlobConstant query[] = {{"MODE", "3"}, {"ALPHA", NULL}};
    char message[2048] = {0};
    size_t required = 0;
    size_t written = 0;

    CHECK(BuildTestBlob(&sink) == 0);

    /* Two-call sizing: measured length matches the written length. */
    CHECK(ShaderToolBlobFormatNotFoundMessage(sink.data, sink.size, query, 2,
                                              NULL, 0, &required) == 0);
    CHECK(required > 0);
    CHECK(ShaderToolBlobFormatNotFoundMessage(sink.data, sink.size, query, 2,
                                              message, sizeof(message), &written) == 0);
    CHECK(written == required);
    CHECK(strlen(message) == written);

    /* Exact capacity succeeds, one byte less fails. */
    CHECK(ShaderToolBlobFormatNotFoundMessage(sink.data, sink.size, query, 2,
                                              message, required + 1, NULL) == 0);
    CHECK(ShaderToolBlobFormatNotFoundMessage(sink.data, sink.size, query, 2,
                                              message, required, NULL) == -1);

    /* Content shape: requested pairs (input order, values normalized) and
       every available key on its own line. */
    CHECK(ShaderToolBlobFormatNotFoundMessage(sink.data, sink.size, query, 2,
                                              message, sizeof(message), NULL) == 0);
    CHECK(strstr(message, "MODE=3;ALPHA=1;") != NULL);
    CHECK(strstr(message, "Permutations available in the blob:") != NULL);
    CHECK(strstr(message, "ALPHA=1 MODE=0\n") != NULL);
    CHECK(strstr(message, "ALPHA=1 MODE=1\n") != NULL);
    CHECK(strstr(message, "ALPHA=1 MODE=2\n") != NULL);

    /* Empty request renders as <default>; a raw blob lists no permutations. */
    {
        static const unsigned char raw[] = {9, 9, 9, 9};

        CHECK(ShaderToolBlobFormatNotFoundMessage(raw, sizeof(raw), NULL, 0,
                                                  message, sizeof(message), NULL) == 0);
        CHECK(strstr(message, "<default>") != NULL);
        CHECK(strstr(message, "No permutations found in the blob.") != NULL);
    }
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    TestBuildKey();
    TestWriteFindRoundTrip();
    TestRawFallbackAndMalformed();
    TestTruncatedBlob();
    TestDataSizeZeroTerminator();
    TestEnumerate();
    TestFormatNotFoundMessage();

    if (g_failed_checks == 0) {
        printf("OK: %d checks passed\n", g_total_checks);
        return 0;
    }

    printf("FAILED: %d of %d checks failed\n", g_failed_checks, g_total_checks);
    return 1;
}
