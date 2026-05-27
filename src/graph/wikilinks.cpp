#include "graph/wikilinks.h"

namespace zg::graph {

std::vector<std::string> extract_wikilinks(std::string_view content) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i + 1 < content.size()) {
        if (content[i] == '[' && content[i + 1] == '[') {
            // Scan forward for the closing ]] — but if we hit another [[
            // first, treat the current opening as never-closed noise and
            // restart from the nested opening.
            const std::size_t start = i + 2;
            std::size_t j = start;
            bool extracted = false;
            bool restart_here = false;
            while (j + 1 < content.size()) {
                if (content[j] == '[' && content[j + 1] == '[') {
                    restart_here = true;
                    break;
                }
                if (content[j] == ']' && content[j + 1] == ']') {
                    out.emplace_back(content.substr(start, j - start));
                    i = j + 2;
                    extracted = true;
                    break;
                }
                ++j;
            }
            if (extracted)    continue;
            if (restart_here) { i = j; continue; }
            // Reached end without closing or nested opening — drop and stop.
            return out;
        }
        ++i;
    }
    return out;
}

}  // namespace zg::graph
