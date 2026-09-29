#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include "Depfile.h"

#include <cstdio>
#include <unordered_set>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

/* Path normalization with the same semantics as the legacy NormalizeAPath
   (ShaderToolUtils.c), reimplemented in C++: separators become '/', consecutive
   separators collapse, "." components are dropped and ".." folds away the preceding
   component. Divergence (deliberate, depfiles must not hard-fail): a ".." that cannot
   be folded is kept verbatim on relative paths (e.g. "../shared/x.h") and dropped at
   an absolute or drive root. */
bool IsSeparator(char c) {
    return c == '/' || c == '\\';
}

bool IsDriveComponent(const std::string &part) {
    return part.size() == 2 && part[1] == ':' &&
           ((part[0] >= 'A' && part[0] <= 'Z') || (part[0] >= 'a' && part[0] <= 'z'));
}

std::string NormalizeDependencyPath(const std::string &path) {
    const size_t length = path.size();
    const bool absolute = length != 0 && IsSeparator(path[0]);
    std::vector<std::string> parts;
    size_t i = 0;

    parts.reserve(8);
    while (i < length) {
        while (i < length && IsSeparator(path[i]))
            ++i;
        size_t j = i;
        while (j < length && !IsSeparator(path[j]))
            ++j;
        if (j > i) {
            std::string part = path.substr(i, j - i);
            if (part == ".") {
                /* drop */
            } else if (part == "..") {
                const bool poppable = !parts.empty() && parts.back() != ".." &&
                                      !IsDriveComponent(parts.back());
                if (poppable)
                    parts.pop_back();
                else if (!absolute)
                    parts.push_back(std::move(part));
                /* ".." above an absolute root is dropped */
            } else {
                parts.push_back(std::move(part));
            }
        }
        i = j;
    }

    std::string result;
    result.reserve(path.size());
    if (absolute)
        result += '/';
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k)
            result += '/';
        result += parts[k];
    }
    if (result.empty())
        result = ".";
    return result;
}

/* Make-style escaping: a space becomes "\ " and '#' becomes "\#" (both would otherwise
   terminate or comment the dependency list); '$' becomes "$$" so make does not expand
   it as a variable reference. */
void AppendMakeEscaped(std::string &text, const std::string &path) {
    for (char c : path) {
        if (c == ' ' || c == '#')
            text += '\\';
        else if (c == '$')
            text += '$';
        text += c;
    }
}

std::string DedupKey(const std::string &path) {
#ifdef _WIN32
    std::string key = path;
    for (char &c : key) {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return key;
#else
    return path;
#endif
}

FILE *OpenDepfile(const std::string &path, const wchar_t *wide_mode, const char *mode) {
#ifdef _WIN32
    (void)mode;
    /* Open through the wide API so UTF-8 paths survive on Windows. */
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1,
                                    NULL, 0);
    if (count <= 0)
        return NULL;
    std::wstring wide((size_t)count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, &wide[0],
                             count))
        return NULL;
    return _wfopen(wide.c_str(), wide_mode);
#else
    (void)wide_mode;
    return fopen(path.c_str(), mode);
#endif
}

/* Returns 0 and sets matches=true when the file already holds exactly `text`.
   A missing or unreadable file is not an error here; it simply does not match. */
void DepfileMatches(const std::string &path, const std::string &text, bool &matches) {
    matches = false;
    FILE *file = OpenDepfile(path, L"rb", "rb");
    if (!file)
        return;
    std::string old;
    char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        old.append(buffer, count);
        if (old.size() > text.size())
            break;
    }
    matches = !ferror(file) && old == text;
    fclose(file);
}

} // namespace

int DepfileWrite(const std::string &depfile_path, const std::string &target_path,
                 const std::vector<std::string> &dependencies) {
    try {
        std::vector<std::string> deps;
        std::unordered_set<std::string> seen;
        deps.reserve(dependencies.size());
        for (const std::string &dependency : dependencies) {
            std::string normalized = NormalizeDependencyPath(dependency);
            if (seen.insert(DedupKey(normalized)).second)
                deps.push_back(std::move(normalized));
        }

        std::string text;
        AppendMakeEscaped(text, NormalizeDependencyPath(target_path));
        text += ':';
        for (const std::string &dep : deps) {
            text += ' ';
            AppendMakeEscaped(text, dep);
        }
        text += '\n';

        bool matches = false;
        DepfileMatches(depfile_path, text, matches);
        if (matches)
            return 0;

        FILE *file = OpenDepfile(depfile_path, L"wb", "wb");
        if (!file) {
            fprintf(stderr, "ShaderTool: cannot open depfile '%s' for writing\n",
                    depfile_path.c_str());
            return -1;
        }
        const size_t written = fwrite(text.data(), 1, text.size(), file);
        const bool flushed = fclose(file) == 0;
        if (written != text.size() || !flushed) {
            fprintf(stderr, "ShaderTool: failed to write depfile '%s'\n",
                    depfile_path.c_str());
            return -1;
        }
        return 0;
    } catch (...) {
        fprintf(stderr, "ShaderTool: out of memory while writing depfile '%s'\n",
                depfile_path.c_str());
        return -1;
    }
}
