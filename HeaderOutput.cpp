#include "HeaderOutput.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace {

// Byte-for-byte replica of ShaderMake's DataOutputContext text emission:
// the line length counter starts beyond the wrap threshold so the first byte
// already begins an indented line, "\n    " is inserted when a line passes
// 128 columns, and the indent itself is not counted.
void AppendDataAsText(std::string &text, const void *data, size_t size)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint32_t line_length = 129;

    for (size_t i = 0; i < size; i++) {
        uint8_t value = bytes[i];

        if (line_length > 128) {
            text += "\n    ";
            line_length = 0;
        }

        char formatted[8] = {};
        int formatted_length = std::snprintf(formatted, sizeof(formatted), "%u,", value);
        if (formatted_length > 0)
            text.append(formatted, static_cast<size_t>(formatted_length));

        if (value < 10)
            line_length += 2;
        else if (value < 100)
            line_length += 3;
        else
            line_length += 4;
    }
}

std::string BuildHeaderText(const std::string &symbol, const void *data, size_t size)
{
    std::string text;
    text.reserve(size * 4 + symbol.size() + 64);

    text += "// {}\n";
    text += "const uint8_t " + symbol + "[] = {";
    AppendDataAsText(text, data, size);
    text += "\n};\n";

    return text;
}

// ShaderMake writes headers through fopen(path, "w"), so on Windows every
// '\n' becomes "\r\n" on disk. Reproduce that translation explicitly so the
// content comparison and the binary write below see the exact final bytes.
std::string ToPlatformNewlines(const std::string &text)
{
#ifdef _WIN32
    std::string translated;
    translated.reserve(text.size() + text.size() / 16);

    for (char character : text) {
        if (character == '\n')
            translated += '\r';
        translated += character;
    }

    return translated;
#else
    return text;
#endif
}

bool ReadFileContent(const std::string &path, std::string &content)
{
    FILE *stream = std::fopen(path.c_str(), "rb");
    if (stream == nullptr)
        return false;

    bool success = true;
    char chunk[4096];
    size_t read_size = 0;

    content.clear();
    while ((read_size = std::fread(chunk, 1, sizeof(chunk), stream)) > 0)
        content.append(chunk, read_size);

    if (std::ferror(stream) != 0)
        success = false;

    std::fclose(stream);
    return success;
}

bool WriteFileContent(const std::string &path, const std::string &content)
{
    FILE *stream = std::fopen(path.c_str(), "wb");
    if (stream == nullptr)
        return false;

    bool success = content.empty() ||
        std::fwrite(content.data(), content.size(), 1, stream) == 1;

    if (std::fclose(stream) != 0)
        success = false;

    return success;
}

} // namespace

int HeaderOutputWrite(const std::string &path, const std::string &symbol,
                      const void *data, size_t size)
{
    if (path.empty() || symbol.empty() || (data == nullptr && size > 0))
        return -1;

    try {
        std::string content = ToPlatformNewlines(BuildHeaderText(symbol, data, size));

        std::error_code error;
        std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error && !std::filesystem::is_directory(parent, error))
                return -1;
        }

        std::string existing_content;
        if (ReadFileContent(path, existing_content) && existing_content == content)
            return 0; // unchanged: keep the file's timestamp

        return WriteFileContent(path, content) ? 0 : -1;
    } catch (...) {
        return -1;
    }
}
