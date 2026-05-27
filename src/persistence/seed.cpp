#include "persistence/seed.h"

namespace zg::persistence {

InitialGraph make_initial_graph(double now_unix) {
    InitialGraph out;
    out.nodes.reserve(3);

    StoredNode self_n{};
    self_n.id           = 0;
    self_n.position     = {0.0f, 0.0f, 0.0f};
    self_n.title        = "self";
    self_n.content      = "the operator";
    self_n.first_seen   = now_unix;
    self_n.last_touched = now_unix;
    self_n.tier         = "self";
    out.nodes.push_back(std::move(self_n));

    StoredNode alice{};
    alice.id           = 1;
    alice.position     = {-5.0f, 0.0f, 0.0f};
    alice.title        = "alice";
    alice.content      = "";
    alice.first_seen   = now_unix;
    alice.last_touched = now_unix;
    alice.tier         = "confirmed";
    out.nodes.push_back(std::move(alice));

    StoredNode bob{};
    bob.id           = 2;
    bob.position     = {5.0f, 0.0f, 0.0f};
    bob.title        = "bob";
    bob.content      = "";
    bob.first_seen   = now_unix;
    bob.last_touched = now_unix;
    bob.tier         = "confirmed";
    out.nodes.push_back(std::move(bob));

    return out;
}

}  // namespace zg::persistence
