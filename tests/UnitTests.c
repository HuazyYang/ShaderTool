#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the implementation so parser-internal types and functions can be
 * exercised without widening ShaderTool's production API. */
#define main ShaderToolProgramMain
#include "../ShaderTool.c"
#undef main

static int Failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
                    #condition);                                                \
            ++Failures;                                                         \
        }                                                                       \
    } while (0)

static void TestDynamicArray(void) {
    DynamicArray array = {0};
    const int first[] = {1, 2, 3};
    const int second[] = {4, 5};

    CHECK(DynamicArrayAppend(&array, sizeof(int), first, 3, "test") == 0);
    CHECK(DynamicArrayAppend(&array, sizeof(int), second, 2, "test") == 0);
    CHECK(array.Size == 5);
    CHECK(array.Capacity >= array.Size);
    CHECK(!memcmp(array.Buffer, (int[]){1, 2, 3, 4, 5}, 5 * sizeof(int)));
    CHECK(DynamicArrayErase(&array, sizeof(int), 1, 4) == 0);
    CHECK(array.Size == 2);
    CHECK(!memcmp(array.Buffer, (int[]){1, 5}, 2 * sizeof(int)));
    CHECK(DynamicArrayErase(&array, sizeof(int), 2, 1) != 0);
    CHECK(DynamicArrayErase(&array, sizeof(int), 0, 3) != 0);
    DynamicArrayDeinit(&array);
    CHECK(!array.Buffer && !array.Size && !array.Capacity);
}

static void TestStringViews(void) {
    AStringView empty = AStringViewFromCString(NULL);
    AStringView text = AStringViewFromCString("shader");
    AStringView same;
    AStringView prefix = AStringViewFromCString("sha");

    same = AStringViewFromBuffer("shader suffix", 6);
    CHECK(AStringViewIsEmpty(&empty));
    CHECK(AStringViewEqual(&empty, NULL));
    CHECK(AStringViewEqual(&text, "shader"));
    CHECK(!AStringViewEqual(&text, "shade"));
    CHECK(AStringViewEqual2(&text, &same));
    CHECK(AStringViewEqualAtLeast(&text, &prefix));
    CHECK(!AStringViewEqualAtLeast(&prefix, &text));
}

static void TestStrings(void) {
    AString string = {0};
    AStringView shader = AStringViewFromCString("shader");
    AStringView tool = AStringViewFromCString("tool");

    CHECK(AStringCopy(&string, &shader) == 0);
    CHECK(AStringCatN(&string, 1, &tool) == 0);
    CHECK(string.Length == 10 && !strcmp(string.Buffer, "shadertool"));
    CHECK(AStringAppendFormat(&string, "-%d-%s", 17, "ok") == 0);
    CHECK(string.Length == 16 && !strcmp(string.Buffer, "shadertool-17-ok"));
    AStringDeinit(&string);
}

static void CheckNormalized(const char *input, const char *expected) {
    AString result = {0};
    AStringView view = AStringViewFromCString(input);
    CHECK(NormalizeAPath(&view, &result) == 0);
    if (!result.Buffer || strcmp(result.Buffer, expected)) {
        fprintf(stderr, "NormalizeAPath(\"%s\") produced \"%s\", expected \"%s\"\n",
                input, result.Buffer ? result.Buffer : "(null)", expected);
        ++Failures;
    }
    AStringDeinit(&result);
}

static void TestPaths(void) {
    CheckNormalized("", "");
    CheckNormalized("shader.hlsl", "shader.hlsl");
    CheckNormalized("dir\\nested//./shader.hlsl", "dir/nested/shader.hlsl");
    CheckNormalized("dir/nested/../shader.hlsl", "dir/shader.hlsl");
    CheckNormalized("dir/../shader.hlsl", "shader.hlsl");
    CheckNormalized("/root//shader.hlsl", "/root/shader.hlsl");
    CheckNormalized("/root/../shader.hlsl", "/shader.hlsl");
}

static void TestByteBuffer(void) {
    ByteBuffer buffer = {0};
    AStringView left = AStringViewFromCString("abc");
    AStringView right = AStringViewFromCString("def");
    AStringView result;

    CHECK(ByteBufferCatAStringViews(&buffer, 2, &left, &right) == 0);
    CHECK(buffer.Size == 6 && !memcmp(buffer.Buffer, "abcdef", 6));
    CHECK(ByteBufferEraseRange(&buffer, 2, 4) == 0);
    ByteBufferGetAStringView(&buffer, &result);
    CHECK(result.Length == 4 && !memcmp(result.Buffer, "abef", 4));
    CHECK(ByteBufferEraseRange(&buffer, 5, 5) != 0);
    ByteBufferDeinit(&buffer);
}

static void CheckProfile(const char *profile, int valid, uint32_t expected_major,
                         uint32_t expected_minor) {
    AStringView view = AStringViewFromCString(profile);
    uint32_t major = UINT32_MAX, minor = UINT32_MAX;
    int result = ParseProfile(&view, &major, &minor);
    CHECK((result == 0) == valid);
    if (valid) {
        CHECK(major == expected_major);
        CHECK(minor == expected_minor);
    }
}

static void TestProfiles(void) {
    CheckProfile("ps_5_0", 1, 5, 0);
    CheckProfile("vs_6_8", 1, 6, 8);
    CheckProfile("lib_6_10", 1, 6, 10);
    CheckProfile("ps_", 0, 0, 0);
    CheckProfile("ps_6", 0, 0, 0);
    CheckProfile("ps_x_0", 0, 0, 0);
    CheckProfile("ps_6_x", 0, 0, 0);
    CheckProfile("_6_0", 0, 0, 0);
    CheckProfile("ps_6_0_extra", 0, 0, 0);
    CheckProfile("ps_4294967296_0", 0, 0, 0);
}

static void TestOptionPolicy(void) {
    const BackendSpec *fxc = FindBackendSpec("fxc");
    const BackendSpec *dxc = FindBackendSpec("dxc");
    const BackendSpec *spirv = FindBackendSpec("spirv");

    CHECK(fxc && dxc && spirv);
    CHECK(!FindBackendSpec("DXC"));
    CHECK(FindOptionSpec(fxc, "-D"));
    CHECK(FindOptionSpec(dxc, "-HV"));
    CHECK(!FindOptionSpec(fxc, "-HV"));
    CHECK(FindOptionSpec(spirv, "-fspv-reflect"));
    CHECK(!FindOptionSpec(dxc, "-fspv-reflect"));
    CHECK(FindOptionSpec(spirv, "-fspv-target-env=vulkan1.3"));
    CHECK(!FindOptionSpec(spirv, "-fspv-target-env"));
    CHECK(!FindOptionSpec(dxc, "-Odextra"));
}

static void TestFiles(const char *root) {
    char path[1024];
    ByteBuffer content = {0};
    bool changed = false;
    FILE *file;

    snprintf(path, sizeof(path), "%s/state.bin", root);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (!file)
        return;
    fclose(file);

    CHECK(FileWriteIfChanged(path, "first", 5, &changed) == 0 && changed);
    CHECK(FileWriteIfChanged(path, "first", 5, &changed) == 0 && !changed);
    CHECK(FileWriteIfChanged(path, "xy", 2, &changed) == 0 && changed);
    CHECK(FileReadAll(path, &content) == 0);
    CHECK(content.Size == 2 && !memcmp(content.Buffer, "xy", 2));
    CHECK(content.Buffer[content.Size] == 0);
    ByteBufferDeinit(&content);
    CHECK(FileWriteAll(path, "z", 1, true) == 0);
    CHECK(FileReadAll(path, &content) == 0);
    CHECK(content.Size == 3 && !memcmp(content.Buffer, "xyz", 3));
    ByteBufferDeinit(&content);
    remove(path);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <temporary-directory>\n", argv[0]);
        return 2;
    }

    TestDynamicArray();
    TestStringViews();
    TestStrings();
    TestPaths();
    TestByteBuffer();
    TestProfiles();
    TestOptionPolicy();
    TestFiles(argv[1]);

    if (Failures)
        fprintf(stderr, "%d ShaderTool unit assertion(s) failed\n", Failures);
    return Failures ? 1 : 0;
}
