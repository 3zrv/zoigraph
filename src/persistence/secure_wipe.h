#pragma once

#include <filesystem>

namespace zg::persistence {

// Best-effort secure deletion for the /panic CLI command.
//
// "Best-effort" is doing honest work in that sentence: an in-place
// overwrite defeats casual recovery (undelete tools, hex-dumping free
// blocks), but CoW filesystems (btrfs/ZFS/APFS), SSD wear-leveling, and
// filesystem journals can all keep stale copies of the old blocks that
// userspace cannot reach. True at-rest protection arrives with SQLCipher
// (plan T6), where a wipe only has to zero the key. Until then this is
// the strongest wipe an unprivileged process can do.

// Overwrites the file's bytes in place — one pseudo-random pass, then one
// zero pass — flushing to the device after each. The file itself survives
// (size preserved). Split out from secure_wipe_file so tests can observe
// that the overwrite really happened; there's nothing left to inspect
// after the unlink. Returns false if the file can't be opened or a write
// fails (contents may then be partially intact).
bool secure_overwrite_file(const std::filesystem::path& p);

// secure_overwrite_file, then rename to a random junk name (so the
// original filename doesn't survive in the live directory entry) and
// unlink, syncing the parent directory afterwards. Returns false if any
// step failed.
bool secure_wipe_file(const std::filesystem::path& p);

// Wipes every regular file under `dir` (recursively; symlinks are not
// followed) with secure_wipe_file, then removes the directory tree.
// Returns the number of files successfully wiped — 0 when `dir` doesn't
// exist. A file that fails to wipe is still counted out (not in) but
// doesn't stop the sweep.
int panic_wipe(const std::filesystem::path& dir);

}  // namespace zg::persistence
