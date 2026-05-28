#pragma once

#include <mutex>
#include <string>
#include <thread>

namespace zg::app {

// "Ask about selection" — fires the LLM at the currently selected node and
// lets the resulting phantom land via the normal UDP listener. One instance
// per main(); the inspector calls start() on click, snapshot() each frame
// to decide whether to disable the button and what status text to show.
//
// The actual LLM call is delegated to scripts/llm_phantom.py, shelled out
// in a detached worker. The C++ side does a fast TCP-probe against
// 127.0.0.1:11434 before spawning the subprocess so a missing Ollama
// daemon fails in milliseconds rather than after a subprocess startup.
//
// Threading: single producer (the inspector main thread) calls start();
// single consumer (the worker thread) updates state. Mutex guards the
// fields and serialises with the destructor's join().
class LlmAsk {
public:
    enum class State {
        Idle,             // never asked, or last ask fully consumed
        Thinking,         // worker thread is running (TCP probe + subprocess)
        Done,             // phantom emitted; arrives via UDP listener
        ErrNoOllama,      // TCP probe failed -- Ollama not reachable
        ErrSubprocess,    // python script exited non-zero
    };

    struct Snapshot {
        State        state;
        std::string  msg;
    };

    LlmAsk()  = default;
    ~LlmAsk();

    LlmAsk(const LlmAsk&)            = delete;
    LlmAsk& operator=(const LlmAsk&) = delete;

    // Kick off a background ask anchored on `anchor_id` from `db_path`.
    // No-op if an ask is already in flight (caller should also disable
    // the UI button when state == Thinking). Returns immediately.
    void start(std::string db_path, long long anchor_id);

    // Cheap, frame-safe read. Returns a copy so the caller can branch on
    // state without holding the lock.
    Snapshot snapshot() const;

private:
    void run(std::string db_path, long long anchor_id);

    mutable std::mutex  mu_;
    State               state_ = State::Idle;
    std::string         msg_;
    std::thread         worker_;
};

}  // namespace zg::app
