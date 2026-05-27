#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "telemetry/phantom_buffer.h"
#include "telemetry/phantom_parse.h"

#include <atomic>
#include <chrono>
#include <thread>

using zg::telemetry::Phantom;
using zg::telemetry::PhantomBuffer;
using zg::telemetry::parse_phantom;

TEST_CASE("parse_phantom: minimal valid payload") {
    const auto p = parse_phantom(R"({"id":42,"x":1.0,"y":-2.5,"z":3.0})");
    REQUIRE(p.has_value());
    CHECK(p->id == 42);
    CHECK(p->position.x == doctest::Approx(1.0f));
    CHECK(p->position.y == doctest::Approx(-2.5f));
    CHECK(p->position.z == doctest::Approx(3.0f));
    CHECK(p->label.empty());
    CHECK(p->spawn_time == 0.0);
}

TEST_CASE("parse_phantom: optional label is picked up when present") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"label":"scan-host-1.2.3.4"})");
    REQUIRE(p.has_value());
    CHECK(p->label == "scan-host-1.2.3.4");
}

TEST_CASE("parse_phantom: malformed json returns nullopt") {
    CHECK_FALSE(parse_phantom("not json at all").has_value());
    CHECK_FALSE(parse_phantom("{").has_value());
    CHECK_FALSE(parse_phantom("").has_value());
}

TEST_CASE("parse_phantom: missing required field returns nullopt") {
    CHECK_FALSE(parse_phantom(R"({"id":1,"x":0,"y":0})").has_value());     // no z
    CHECK_FALSE(parse_phantom(R"({"x":0,"y":0,"z":0})").has_value());      // no id
    CHECK_FALSE(parse_phantom(R"([1,2,3])").has_value());                  // array, not object
}

TEST_CASE("parse_phantom: wrong type for required field returns nullopt") {
    // id must be integer, not string.
    CHECK_FALSE(parse_phantom(R"({"id":"x","x":0,"y":0,"z":0})").has_value());
    // x/y/z must be numbers.
    CHECK_FALSE(parse_phantom(R"({"id":1,"x":"a","y":0,"z":0})").has_value());
}

TEST_CASE("phantom_buffer: add and snapshot returns the same phantom") {
    PhantomBuffer buf;
    Phantom p{};
    p.id = 7;
    p.position = {1, 2, 3};
    p.label = "x";
    p.spawn_time = 100.0;
    buf.add(p);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, /*ttl=*/60.0f, /*now=*/100.5);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 7);
    CHECK(out[0].label == "x");
}

TEST_CASE("phantom_buffer: expired phantoms are dropped from storage") {
    PhantomBuffer buf;
    Phantom fresh{}; fresh.id = 1; fresh.spawn_time = 100.0;
    Phantom stale{}; stale.id = 2; stale.spawn_time = 10.0;
    buf.add(fresh);
    buf.add(stale);
    REQUIRE(buf.size() == 2);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, /*ttl=*/60.0f, /*now=*/120.0);

    // stale spawned at 10, now=120, age=110 > ttl=60 → expired.
    // fresh spawned at 100, now=120, age=20 < ttl=60 → kept.
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 1);
    CHECK(buf.size() == 1);  // stale was removed from internal storage too.
}

TEST_CASE("phantom_buffer: snapshot returning empty after every phantom expires") {
    PhantomBuffer buf;
    Phantom p{}; p.id = 99; p.spawn_time = 0.0;
    buf.add(p);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 1000.0);  // age=1000 >> ttl=60
    CHECK(out.empty());
    CHECK(buf.size() == 0);
}

TEST_CASE("parse_phantom: integer values in float slots are accepted") {
    const auto p = parse_phantom(R"({"id":1,"x":5,"y":-10,"z":0})");
    REQUIRE(p.has_value());
    CHECK(p->position.x == doctest::Approx(5.0f));
    CHECK(p->position.y == doctest::Approx(-10.0f));
    CHECK(p->position.z == doctest::Approx(0.0f));
}

TEST_CASE("parse_phantom: scientific notation parses") {
    const auto p = parse_phantom(R"({"id":1,"x":1e3,"y":-2.5e-1,"z":0})");
    REQUIRE(p.has_value());
    CHECK(p->position.x == doctest::Approx(1000.0f));
    CHECK(p->position.y == doctest::Approx(-0.25f));
}

TEST_CASE("parse_phantom: negative id is allowed") {
    const auto p = parse_phantom(R"({"id":-42,"x":0,"y":0,"z":0})");
    REQUIRE(p.has_value());
    CHECK(p->id == -42);
}

TEST_CASE("parse_phantom: unicode label survives") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"label":"αβγ-日本語"})");
    REQUIRE(p.has_value());
    CHECK(p->label == "αβγ-日本語");
}

TEST_CASE("parse_phantom: unknown extra fields are ignored, not rejected") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"foo":"bar","weight":3.14})");
    REQUIRE(p.has_value());
    CHECK(p->id == 1);
}

TEST_CASE("parse_phantom: empty json object fails (no required fields)") {
    CHECK_FALSE(parse_phantom("{}").has_value());
}

TEST_CASE("parse_phantom: required field explicitly null is rejected") {
    CHECK_FALSE(parse_phantom(R"({"id":null,"x":0,"y":0,"z":0})").has_value());
    CHECK_FALSE(parse_phantom(R"({"id":1,"x":null,"y":0,"z":0})").has_value());
}

TEST_CASE("parse_phantom: id supplied as a float (1.0) is rejected") {
    // is_number_integer() distinguishes "1" from "1.0" in nlohmann/json;
    // we want strict id semantics so external tools can rely on the parser
    // refusing fractional ids rather than truncating them.
    CHECK_FALSE(parse_phantom(R"({"id":1.0,"x":0,"y":0,"z":0})").has_value());
    CHECK_FALSE(parse_phantom(R"({"id":1.5,"x":0,"y":0,"z":0})").has_value());
}

TEST_CASE("parse_phantom: leading/trailing whitespace around the JSON is fine") {
    const auto p = parse_phantom("   \t\n{\"id\":7,\"x\":0,\"y\":0,\"z\":0}   \n");
    REQUIRE(p.has_value());
    CHECK(p->id == 7);
}

TEST_CASE("parse_phantom: label that isn't a string is silently dropped") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"label":42})");
    REQUIRE(p.has_value());
    CHECK(p->label.empty());
}

TEST_CASE("parse_phantom: connections array of integers is picked up") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"connections":[12,87,400]})");
    REQUIRE(p.has_value());
    REQUIRE(p->connections.size() == 3);
    CHECK(p->connections[0] == 12);
    CHECK(p->connections[1] == 87);
    CHECK(p->connections[2] == 400);
}

TEST_CASE("parse_phantom: missing connections field yields empty vector") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0})");
    REQUIRE(p.has_value());
    CHECK(p->connections.empty());
}

TEST_CASE("parse_phantom: empty connections array yields empty vector") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"connections":[]})");
    REQUIRE(p.has_value());
    CHECK(p->connections.empty());
}

TEST_CASE("parse_phantom: non-integer entries inside connections are silently skipped") {
    const auto p = parse_phantom(
        R"({"id":1,"x":0,"y":0,"z":0,"connections":[5,"hello",3.14,null,9]})");
    REQUIRE(p.has_value());
    REQUIRE(p->connections.size() == 2);
    CHECK(p->connections[0] == 5);
    CHECK(p->connections[1] == 9);
}

TEST_CASE("parse_phantom: connections that isn't an array is silently dropped") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"connections":"oops"})");
    REQUIRE(p.has_value());
    CHECK(p->connections.empty());
}

TEST_CASE("parse_phantom: connections may include negative ids (no sign filter)") {
    // The id space is just long long — negative ids are syntactically valid
    // and we'd rather accept-and-discard-out-of-bounds in the renderer than
    // reject the whole payload at parse time.
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"connections":[-5,7,-99]})");
    REQUIRE(p.has_value());
    REQUIRE(p->connections.size() == 3);
    CHECK(p->connections[0] == -5);
    CHECK(p->connections[1] == 7);
    CHECK(p->connections[2] == -99);
}

TEST_CASE("parse_phantom: connections with very large id values are accepted") {
    // long long can hold up to ~9.2e18.
    const auto p = parse_phantom(
        R"({"id":1,"x":0,"y":0,"z":0,"connections":[9223372036854775806]})");
    REQUIRE(p.has_value());
    REQUIRE(p->connections.size() == 1);
    CHECK(p->connections[0] == 9223372036854775806LL);
}

TEST_CASE("parse_phantom: 100-entry connections array round-trips") {
    std::string payload = R"({"id":1,"x":0,"y":0,"z":0,"connections":[)";
    for (int i = 0; i < 100; ++i) {
        if (i) payload += ',';
        payload += std::to_string(i);
    }
    payload += "]}";

    const auto p = parse_phantom(payload);
    REQUIRE(p.has_value());
    REQUIRE(p->connections.size() == 100);
    CHECK(p->connections[0]  == 0);
    CHECK(p->connections[99] == 99);
}

TEST_CASE("phantom_buffer: size is zero before any add") {
    PhantomBuffer buf;
    CHECK(buf.size() == 0);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 100.0);
    CHECK(out.empty());
}

TEST_CASE("phantom_buffer: mixed expiry keeps only the in-TTL phantoms") {
    PhantomBuffer buf;
    Phantom p1{}; p1.id = 1; p1.spawn_time = 100.0;
    Phantom p2{}; p2.id = 2; p2.spawn_time =  50.0;
    Phantom p3{}; p3.id = 3; p3.spawn_time = 105.0;
    buf.add(p1); buf.add(p2); buf.add(p3);
    REQUIRE(buf.size() == 3);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, /*ttl=*/60.0f, /*now=*/120.0);
    // p1: age 20 < 60 -> keep. p2: age 70 > 60 -> expire. p3: age 15 < 60 -> keep.
    REQUIRE(out.size() == 2);
    CHECK(buf.size() == 2);
    const bool has_1 = (out[0].id == 1 || out[1].id == 1);
    const bool has_3 = (out[0].id == 3 || out[1].id == 3);
    CHECK(has_1);
    CHECK(has_3);
}

TEST_CASE("phantom_buffer: duplicate ids are all retained (no de-duplication)") {
    PhantomBuffer buf;
    Phantom p1{}; p1.id = 42; p1.spawn_time = 100.0; p1.position = {1, 0, 0};
    Phantom p2{}; p2.id = 42; p2.spawn_time = 100.0; p2.position = {2, 0, 0};
    buf.add(p1);
    buf.add(p2);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 100.5);
    REQUIRE(out.size() == 2);
    CHECK(out[0].id == 42);
    CHECK(out[1].id == 42);
}

TEST_CASE("phantom_buffer: add after expire restores a populated snapshot") {
    PhantomBuffer buf;
    Phantom stale{}; stale.id = 1; stale.spawn_time = 0.0;
    buf.add(stale);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 1000.0);
    REQUIRE(out.empty());
    REQUIRE(buf.size() == 0);

    Phantom fresh{}; fresh.id = 2; fresh.spawn_time = 1000.0;
    buf.add(fresh);
    buf.snapshot_and_expire(out, 60.0f, 1000.1);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 2);
}

TEST_CASE("phantom_buffer: clear() followed by add() leaves only the new phantom") {
    PhantomBuffer buf;
    for (int i = 0; i < 3; ++i) {
        Phantom old{};
        old.id = i;
        old.spawn_time = 50.0;
        buf.add(old);
    }
    buf.clear();
    Phantom fresh{};
    fresh.id         = 99;
    fresh.spawn_time = 200.0;
    buf.add(fresh);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 200.5);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 99);
}

TEST_CASE("phantom_buffer: clear() drops every phantom") {
    PhantomBuffer buf;
    for (int i = 0; i < 5; ++i) {
        Phantom p{};
        p.id = i;
        p.spawn_time = 100.0;
        buf.add(p);
    }
    REQUIRE(buf.size() == 5);

    buf.clear();
    CHECK(buf.size() == 0);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 100.5);
    CHECK(out.empty());
}

TEST_CASE("phantom_buffer: snapshot returns phantoms in insertion order") {
    PhantomBuffer buf;
    for (int i = 0; i < 5; ++i) {
        Phantom p{};
        p.id         = i;
        p.spawn_time = 100.0;
        buf.add(p);
    }

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 100.5);
    REQUIRE(out.size() == 5);
    for (int i = 0; i < 5; ++i) {
        CHECK(out[static_cast<std::size_t>(i)].id == i);
    }
}

TEST_CASE("phantom_buffer: connections survive add and snapshot intact") {
    PhantomBuffer buf;
    Phantom p{};
    p.id          = 1;
    p.spawn_time  = 100.0;
    p.connections = {12, 87, -3, 400};
    buf.add(p);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 100.5);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].connections.size() == 4);
    CHECK(out[0].connections[0] == 12);
    CHECK(out[0].connections[1] == 87);
    CHECK(out[0].connections[2] == -3);
    CHECK(out[0].connections[3] == 400);
}

TEST_CASE("phantom_buffer: remove by id drops every matching entry") {
    PhantomBuffer buf;
    Phantom a{}; a.id = 1; a.spawn_time = 100.0;
    Phantom b{}; b.id = 2; b.spawn_time = 100.0;
    Phantom c{}; c.id = 1; c.spawn_time = 100.0;  // duplicate id with a
    buf.add(a); buf.add(b); buf.add(c);
    REQUIRE(buf.size() == 3);

    buf.remove(1);
    REQUIRE(buf.size() == 1);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 100.5);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 2);
}

TEST_CASE("phantom_buffer: remove of a non-existent id is a silent no-op") {
    PhantomBuffer buf;
    Phantom p{}; p.id = 1; p.spawn_time = 100.0;
    buf.add(p);

    buf.remove(99);  // never added; should not throw or affect existing rows.
    CHECK(buf.size() == 1);

    std::vector<Phantom> out;
    buf.snapshot_and_expire(out, 60.0f, 100.5);
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 1);
}

TEST_CASE("phantom_buffer: concurrent add/snapshot doesn't crash or corrupt") {
    // Mirrors the graph_buffer producer/consumer hammer: a producer thread
    // continuously adds phantoms while we keep snapshotting on the main
    // thread, asserting per-element invariants on each observed copy.
    PhantomBuffer buf;
    std::atomic<bool> stop{false};
    std::atomic<int>  added{0};

    std::thread producer([&]() {
        long long id = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            Phantom p{};
            p.id         = id++;
            p.position   = {1.0f, 2.0f, 3.0f};
            p.spawn_time = 100.0;  // all in-TTL relative to now=100.5
            buf.add(std::move(p));
            added.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150)) {
        std::vector<Phantom> out;
        buf.snapshot_and_expire(out, /*ttl=*/60.0f, /*now=*/100.5);
        for (const auto& p : out) {
            REQUIRE(p.id >= 0);
            REQUIRE(p.position.x == doctest::Approx(1.0f));
            REQUIRE(p.position.y == doctest::Approx(2.0f));
            REQUIRE(p.position.z == doctest::Approx(3.0f));
            REQUIRE(p.spawn_time == 100.0);
        }
    }

    stop.store(true);
    producer.join();

    CHECK(added.load() > 0);
}
