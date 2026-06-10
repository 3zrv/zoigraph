#include "persistence/secure_wipe.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zg::persistence {

namespace {

namespace fs = std::filesystem;

// Flush a stdio stream all the way to the device, not just to the kernel.
bool flush_to_device(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

// Sync a directory so a just-renamed/unlinked entry is durable. POSIX
// only — Windows can't fsync a directory from userspace; the unlink
// itself is the best we get there.
void sync_directory(const fs::path& dir) {
#ifndef _WIN32
    const int fd = open(dir.string().c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
#endif
}

// One full overwrite pass. `fill` writes the next chunk into the buffer;
// passing the same buffer back avoids re-randomising bytes we don't need.
template <typename Fill>
bool overwrite_pass(std::FILE* f, std::uintmax_t size, Fill fill) {
    std::array<unsigned char, 64 * 1024> buf;
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    std::uintmax_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk =
            remaining < buf.size() ? static_cast<std::size_t>(remaining)
                                   : buf.size();
        fill(buf.data(), chunk);
        if (std::fwrite(buf.data(), 1, chunk, f) != chunk) return false;
        remaining -= chunk;
    }
    return flush_to_device(f);
}

}  // namespace

bool secure_overwrite_file(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(p, ec))) return false;
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec) return false;
    if (size == 0) return true;  // nothing to scrub

    std::FILE* f = std::fopen(p.string().c_str(), "r+b");
    if (!f) return false;

    // Pass 1: pseudo-random. Doesn't need to be cryptographic — the point
    // is that the original bytes are gone, not that the replacement is
    // unpredictable. Pass 2: zeros, so a later inspection shows an
    // obviously-blank file rather than something that looks like
    // ciphertext worth attacking.
    std::mt19937_64 rng{std::random_device{}()};
    bool ok = overwrite_pass(f, size, [&](unsigned char* dst, std::size_t n) {
        for (std::size_t i = 0; i < n; i += sizeof(std::uint64_t)) {
            const std::uint64_t r = rng();
            for (std::size_t j = 0; j < sizeof(r) && i + j < n; ++j) {
                dst[i + j] = static_cast<unsigned char>(r >> (8 * j));
            }
        }
    });
    ok = overwrite_pass(f, size, [](unsigned char* dst, std::size_t n) {
             for (std::size_t i = 0; i < n; ++i) dst[i] = 0;
         }) && ok;

    if (std::fclose(f) != 0) ok = false;
    return ok;
}

bool secure_wipe_file(const fs::path& p) {
    if (!secure_overwrite_file(p)) return false;

    // Rename to a junk name so the original filename doesn't survive in
    // the live directory entry, then unlink. Rename failure isn't fatal —
    // the contents are already scrubbed, so fall back to unlinking under
    // the original name.
    std::error_code ec;
    const fs::path dir = p.parent_path().empty() ? fs::path(".")
                                                 : p.parent_path();
    fs::path doomed = p;
    std::mt19937_64 rng{std::random_device{}()};
    for (int attempt = 0; attempt < 4; ++attempt) {
        char name[20];
        std::snprintf(name, sizeof(name), "%016llx",
                      static_cast<unsigned long long>(rng()));
        const fs::path junk = dir / name;
        if (fs::exists(junk, ec)) continue;
        fs::rename(doomed, junk, ec);
        if (!ec) doomed = junk;
        break;
    }

    if (!fs::remove(doomed, ec) || ec) return false;
    sync_directory(dir);
    return true;
}

int panic_wipe(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;

    int wiped = 0;
    // Wipe file contents first, then tear the tree down in one sweep —
    // remove_all also catches anything that wasn't a regular file
    // (symlinks, stray subdirectories).
    for (fs::recursive_directory_iterator it(
             dir, fs::directory_options::skip_permission_denied, ec), end{};
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && !it->is_symlink()) {
            if (secure_wipe_file(it->path())) ++wiped;
        }
    }
    fs::remove_all(dir, ec);
    sync_directory(dir.parent_path().empty() ? fs::path(".")
                                             : dir.parent_path());
    return wiped;
}

}  // namespace zg::persistence
