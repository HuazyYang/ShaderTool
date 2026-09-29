#ifndef SHADER_TOOL_HEADER_OUTPUT_H
#define SHADER_TOOL_HEADER_OUTPUT_H

#include <cstddef>
#include <string>

/*
 * Emits a shader binary as a C header in ShaderMake's exact -Fh shape:
 *
 *     // {}
 *     const uint8_t <symbol>[] = {
 *         77,68,...            <- decimal "%u," wrapping near 128 columns
 *     };
 *
 * No `_size` variable is emitted; <symbol> is used verbatim. Newlines follow
 * the platform text convention (CRLF on Windows), matching ShaderMake's
 * text-mode output. The file is only rewritten when its content changes;
 * parent directories are created as needed. Returns 0 on success, -1 on
 * failure. Never throws.
 */
int HeaderOutputWrite(const std::string &path, const std::string &symbol,
                      const void *data, size_t size);

#endif /* SHADER_TOOL_HEADER_OUTPUT_H */
