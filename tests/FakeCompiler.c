#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <time.h>
#endif

static void SleepMilliseconds(unsigned long milliseconds) {
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec duration;
    duration.tv_sec = (time_t)(milliseconds / 1000);
    duration.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&duration, &duration)) {}
#endif
}

int main(int argc, char **argv) {
    const char *include_path = getenv("FAKE_INCLUDE");
    const char *argument_log = getenv("FAKE_ARGUMENT_LOG");
    const char *output_bytes = getenv("FAKE_OUTPUT_BYTES");
    const char *sleep_ms = getenv("FAKE_SLEEP_MS");
    const char *exit_code = getenv("FAKE_EXIT_CODE");
    FILE *log = NULL;
    const char *header_path = NULL;
    const char *entry_point = NULL;
    const char *variable_name = NULL;
    int fxc_header = 0;
    unsigned long count = output_bytes ? strtoul(output_bytes, NULL, 10) : 0;
    int i;

    for (i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "-Fh") || !strcmp(argv[i], "/Fh")) {
            header_path = argv[i + 1];
            fxc_header = argv[i][0] == '/';
        }
        if (!strcmp(argv[i], "-E") || !strcmp(argv[i], "/E"))
            entry_point = argv[i + 1];
        if (!strcmp(argv[i], "-Vn") || !strcmp(argv[i], "/Vn"))
            variable_name = argv[i + 1];
    }
    if (header_path) {
        char default_name[256];
        if (!variable_name && entry_point) {
            if (snprintf(default_name, sizeof(default_name), "g_%s", entry_point) < 0)
                return 125;
            variable_name = default_name;
        }
        if (!variable_name) variable_name = "g_main";
        log = fopen(header_path, "wb");
        if (!log) return 125;
        if (fprintf(log, "const %s %s[] = { 0x01, 0x02, 0x03 };\n",
                    fxc_header ? "BYTE" : "unsigned char", variable_name) < 0 ||
            fclose(log))
            return 125;
        log = NULL;
    }
    if (argument_log) {
        log = fopen(argument_log, "wb");
        if (!log) return 125;
        for (i = 0; i < argc; ++i) fprintf(log, "[%s]\n", argv[i]);
        if (fclose(log)) return 125;
    }
    if (include_path) fprintf(stderr, "; Opening file [%s]\n", include_path);
    while (count--) fputc('x', stdout);
    if (output_bytes) fputc('\n', stdout);
    fflush(stdout);
    fflush(stderr);
    if (sleep_ms) SleepMilliseconds(strtoul(sleep_ms, NULL, 10));
    return exit_code ? atoi(exit_code) : 0;
}
