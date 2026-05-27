#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace zg::graph {

// Extracts every `[[target]]` occurrence in `content` as a target string.
// Order is the order of appearance. Duplicates are preserved (the caller
// deduplicates per-source if it cares).
//
// Brackets are matched literally — no escaping, no nesting. `[[ ]]` with
// inner whitespace is treated as a valid (whitespace-only) target and
// returned verbatim; callers can decide to drop empties.
std::vector<std::string> extract_wikilinks(std::string_view content);

}  // namespace zg::graph
