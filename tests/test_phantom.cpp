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

TEST_CASE("parse_phantom: label that isn't a string is silently dropped") {
    const auto p = parse_phantom(R"({"id":1,"x":0,"y":0,"z":0,"label":42})");
    REQUIRE(p.has_value());
    CHECK(p->label.empty());
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
