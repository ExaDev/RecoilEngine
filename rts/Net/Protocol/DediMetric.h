/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef DEDIMETRIC_H
#define DEDIMETRIC_H

#include <cstdint>
#include <string_view>

/**
 * C++ side entry points for the engine NETMSG_DEDIMSG metric pipeline.
 * Mirrors the Spring.Metrics.* Lua API. See DediMsgHeaders.h for the wire
 * format and CGameServer::AppendMetricRow for the sidecar JSONL schema.
 *
 * Counter is a cumulative monotonic running total: each call carries the
 * absolute total at the current frame. ("Team X's Metal generated so far 
 * is 57058")
 *
 * Gauge is an instantaneous reading ("Team X's Metal is currently 3059")
 *
 * Both functions are best-effort: they validate the inputs against the same
 * rules as the Lua bindings (1..kMaxMetricNameLen byte name without
 * `\0\t\n"\\`; finite double value), and on failure log at L_WARNING and
 * return false rather than throwing.
 *
 * `teamNum`: 0..254 attaches a team scope (the JSON row carries that value
 * in its "team" key); leave at the default -1 for player-only observations
 * (the JSON row will have "team":-1).
 *
 * Subject to the existing per-player 64 KiB/s NETMSG_DEDIMSG rate limit
 * (enforced server-side, see CGameServer::ProcessPacket).
 */
namespace dedimetric {

bool Counter(std::string_view name, double value, int teamNum = -1);
bool Gauge  (std::string_view name, double value, int teamNum = -1);

} // namespace dedimetric

#endif // DEDIMETRIC_H
