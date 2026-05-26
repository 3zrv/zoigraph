#pragma once

#include <optional>
#include <string_view>

#include "telemetry/phantom.h"

namespace zg::telemetry {

// Parses one JSON payload of the shape
//   {"id": <int>, "x": <num>, "y": <num>, "z": <num>, "label": "<str>"}
// "label" is optional. The returned Phantom carries spawn_time == 0; the
// listener stamps it before publishing.
//
// Returns std::nullopt on malformed JSON or missing required fields. Never
// throws.
std::optional<Phantom> parse_phantom(std::string_view payload);

}  // namespace zg::telemetry
