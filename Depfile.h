#ifndef SHADER_TOOL_DEPFILE_H
#define SHADER_TOOL_DEPFILE_H

#include <string>
#include <vector>

/* Frozen interface (contract section 8b). Writes a single make-style rule
   "target: dep dep ..." for the given dependencies. Paths are normalized to forward
   slashes with "." and ".." folded, deduplicated (case-insensitively on Windows), and
   spaces are make-escaped. The file is only rewritten when its content changed.
   Returns 0 on success, -1 on failure (errors reported to stderr). No exceptions
   escape this API. */
int DepfileWrite(const std::string &depfile_path, const std::string &target_path,
                 const std::vector<std::string> &dependencies);

#endif
