/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef DEDIMSG_HEADERS_H
#define DEDIMSG_HEADERS_H

#include <cstddef>
#include <cstdint>

/**
 * Shared constants for NETMSG_DEDIMSG record-type headers.
 *
 * The header byte range is partitioned:
 *   0x0001..0x0FFF  engine-reserved. Headers in this range may be intercepted
 *                   on the dedicated server (see CGameServer NETMSG_DEDIMSG
 *                   handler) and are not guaranteed to reach the autohost.
 *   0x1000..0xFFFF  game-private. Always forwarded to the autohost via
 *                   AutohostInterface::SendDediMsg.
 *
 * Engine-defined headers are listed below as named constants.
 */
namespace dedimsg {

constexpr uint16_t kEngineRangeMax   = 0x0FFF;
constexpr uint16_t kGameRangeMin     = 0x1000;

// Engine-defined headers (intercepted server-side, never reach autohost).
// Both METRIC_COUNTER and METRIC_GAUGE share the wire payload:
//   <uint8 teamNum> <uint8 nameLen> <char[nameLen] name> <double value>
// teamNum is the optional team scope: 0..254 = a real team, 0xFF = no team.
// On the dediserver, every observation appends one row to a JSONL sidecar at
// "{demoFilePath}.metrics.jsonl.gz":
//   {"frame":<f>,"player":<p>,"team":<t>,"type":"counter"|"gauge","name":"<n>","value":<v>}
// where "team" is -1 when the wire teamNum is 0xFF. Aggregation is a
// parse-time concern:
//   - Counter: cumulative monotonic running total.
//   - Gauge: instantaneous reading.
constexpr uint16_t METRIC_COUNTER    = 0x0001;
constexpr uint16_t METRIC_GAUGE      = 0x0002;

constexpr size_t   kMaxMetricNameLen = 64;
constexpr uint8_t  kNoTeam           = 0xFF;

} // namespace dedimsg

#endif // DEDIMSG_HEADERS_H
