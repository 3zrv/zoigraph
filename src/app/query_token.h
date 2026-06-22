#pragma once

#include <filesystem>
#include <string>

namespace zg::app {

// A per-session auth secret for the read query channel. The read channel
// exposes node content to any local process, so it carries the same posture as
// the DB file: a random token, written 0600 next to the project DB, that a
// legitimate client (the LLM bridge) reads to authenticate. Queries without
// the matching token are dropped (see answer_query).

// 128 bits of randomness as 32 lowercase hex chars. New every process start.
std::string generate_session_token();

// Writes `token` (no trailing newline) to `path`, then restricts it to owner
// read/write (0600) on POSIX. On Windows the default per-user ACL is relied on
// — strict ACL manipulation is out of scope. Returns false if the write fails;
// a failed chmod is best-effort and does not fail the call.
bool write_token_file(const std::filesystem::path& path, const std::string& token);

}  // namespace zg::app
