#include "app/query_token.h"

#include <fstream>
#include <random>

#if !defined(_WIN32)
    #include <sys/stat.h>
#endif

namespace zg::app {

std::string generate_session_token() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;  // OS entropy; we never seed a PRNG from it here
    std::string out;
    out.reserve(32);
    // 4 draws x 4 bytes = 16 bytes = 128 bits.
    for (int i = 0; i < 4; ++i) {
        const unsigned v = rd();
        for (int b = 0; b < 4; ++b) {
            const unsigned byte = (v >> (b * 8)) & 0xFFu;
            out.push_back(hex[(byte >> 4) & 0xF]);
            out.push_back(hex[byte & 0xF]);
        }
    }
    return out;
}

bool write_token_file(const std::filesystem::path& path, const std::string& token) {
    {
        std::ofstream out(path, std::ios::trunc | std::ios::binary);
        if (!out) return false;
        out << token;
        if (!out) return false;
    }  // close before chmod
#if !defined(_WIN32)
    // 0600: only the owner reads the session secret. Best-effort.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

}  // namespace zg::app
