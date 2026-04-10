/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/**
 * Scaling benchmark for Map<int,int> and Map<ptr,int>: sweeps N from 100 to 20000
 * using non-linear (exponential) sampling — denser at low N, sparser at high N —
 * with clusters of extra points around each power of 2 to capture rehash boundaries.
 *
 * Run: ./test_HashMapScalingBenchmark -r compact-bench [catch2 options]
 * e.g. ./test_HashMapScalingBenchmark -c "Map<int,int>" -r compact-bench
 *      BENCH_RESERVE=0 ./test_HashMapScalingBenchmark -r compact-bench
 */

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

#include "System/UnorderedMap.hpp"
#include "System/SpringHash.h"

#include <catch_amalgamated.hpp>

// =============================================================================
// Fast RNG (xorshift64)
// =============================================================================

struct Xorshift64 {
	uint64_t state;

	explicit Xorshift64(uint64_t seed = 0xDEADBEEFCAFEBABEull) : state(seed) {}

	uint64_t next() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}

	uint64_t nextBelow(uint64_t range) { return next() % range; }
};

// Fixed number of operations per benchmark call, regardless of container size.
constexpr int BENCH_OPS = 100000;

// Set BENCH_RESERVE=0 to disable .reserve() calls and measure rehashing impact.
static const bool USE_RESERVE = []() {
	const char* env = std::getenv("BENCH_RESERVE");
	return !env || std::string(env) != "0";
}();

// =============================================================================
// Key generation
//
// We grepped every spring::unordered_map/set and spring::unsynced_map/set in the
// engine to categorise the key types actually used at runtime. Results:
//
//   Key type              Usages  Representative containers (largest at runtime)
//   --------------------  ------  ------------------------------------------------
//   std::string            ~70+   FeatureDefHandler, WeaponDefHandler, NamedTextures,
//                                 TextureAtlas, S3OTextureHandler  (50-500 entries)
//   int  (entity IDs)      ~40    SelectedUnits, UnitHandler, WaitCommandsAI,
//                                 LosHandler, GroupHandler           (100-10k entries)
//   T*   (pointers)        ~17    ModelDrawerData, MoveMath, ModelsMemStorage,
//                                 LuaUtils, creg                     (100-5k entries)
//   uint32_t / unsigned    ~15    FBO, GroundDecalHandler, PathManager (misc)
//   uint64_t                ~3    PathCache, CFontTexture             (niche)
//
// String keys in the large containers are overwhelmingly short lowercase
// identifiers — unit/weapon/feature definition names like "armcom", "corllt",
// "armllt" (5-10 chars). Texture path strings are longer (20-80 chars) but
// occur in fewer containers. No UUID-style strings were found.
//
// The three makeKey<T> branches mirror these categories:
//   int       → sequential IDs 0..N        (matches unit/entity ID pattern)
//   uintptr_t → aligned heap addresses     (matches pointer-as-key pattern)
//   string    → faction+unittype combinator (matches definition name pattern)
// =============================================================================

// Faction prefixes and unit type suffixes matching real Spring/BAR naming
// conventions (e.g. "armcom", "corllt", "legfus", "clawbot")
static constexpr const char* kFactions[] = {
	"arm", "cor", "leg", "rap", "gok", "aven", "gear", "claw"
};
static constexpr const char* kUnitTypes[] = {
	"com", "llt", "hlt", "mex", "fus", "war", "flea", "flash",
	"pit", "lab", "fac", "air", "sea", "sub", "tank", "bot"
};
static constexpr int kNumFactions  = sizeof(kFactions) / sizeof(kFactions[0]);
static constexpr int kNumUnitTypes = sizeof(kUnitTypes) / sizeof(kUnitTypes[0]);

template<typename T>
T makeKey(int n) {
	if constexpr (std::is_same_v<T, uintptr_t>) {
		constexpr uintptr_t heapBase = 0x555555550000ULL;
		return heapBase + static_cast<uintptr_t>(n) * 128;
	} else if constexpr (std::is_integral_v<T>) {
		return static_cast<T>(n);
	} else {
		std::string key;
		key += kFactions[n % kNumFactions];
		key += kUnitTypes[(n / kNumFactions) % kNumUnitTypes];
		if (n >= kNumFactions * kNumUnitTypes)
			key += std::to_string(n / (kNumFactions * kNumUnitTypes));
		return key;
	}
}

template<typename T>
uint64_t toChecksum(const T& v) {
	if constexpr (std::is_integral_v<T>)
		return static_cast<uint64_t>(v);
	else
		return v.size();
}

enum class OpType : uint8_t { Find, Insert, Erase };
template<typename K, typename V> struct MapOp { OpType type; K key; V value; };

template<typename Map>
Map populateMap(int n) {
	using K = typename Map::key_type;
	using V = typename Map::mapped_type;
	Map map;
	if (USE_RESERVE) map.reserve(n * 2);
	for (int i = 0; i < n; ++i)
		map[makeKey<K>(i)] = makeKey<V>(i);
	return map;
}

// =============================================================================
// Workload preparation
// =============================================================================

template<typename Map>
std::pair<Map, std::vector<MapOp<typename Map::key_type, typename Map::mapped_type>>>
prepareMapWorkload(int n, int numOps, int findPct) {
	using K = typename Map::key_type;
	using V = typename Map::mapped_type;

	Xorshift64 rng;
	Map map = populateMap<Map>(n);

	const int insertPct = (100 - findPct) / 2;

	std::vector<MapOp<K, V>> ops;
	ops.reserve(numOps);
	for (int i = 0; i < numOps; ++i) {
		const int roll = static_cast<int>(rng.nextBelow(100));
		if (roll < findPct) {
			ops.push_back({OpType::Find, makeKey<K>(static_cast<int>(rng.nextBelow(2 * n))), {}});
		} else if (roll < findPct + insertPct) {
			const int k = n + static_cast<int>(rng.nextBelow(2 * n));
			ops.push_back({OpType::Insert, makeKey<K>(k), makeKey<V>(k)});
		} else {
			ops.push_back({OpType::Erase, makeKey<K>(static_cast<int>(rng.nextBelow(n))), {}});
		}
	}

	return {std::move(map), std::move(ops)};
}

// =============================================================================
// Benchmark implementations
// =============================================================================

template<typename Map>
void benchmarkMapMixed(Catch::Benchmark::Chronometer meter, int n, int findPct) {
	auto [baseMap, ops] = prepareMapWorkload<Map>(n, BENCH_OPS, findPct);
	std::vector<Map> maps(meter.runs(), baseMap);
	if (USE_RESERVE) { for (auto& m : maps) m.reserve(n * 2); }
	meter.measure([&](int i) {
		auto& map = maps[i];
		uint64_t checksum = 0;
		for (const auto& op : ops) {
			if (op.type == OpType::Find) {
				auto it = map.find(op.key);
				if (it != map.end())
					checksum += toChecksum(it->second);
			} else if (op.type == OpType::Insert) {
				map[op.key] = op.value;
			} else {
				map.erase(op.key);
			}
		}
		return checksum;
	});
}

template<typename Map>
void benchmarkMapIterate(Catch::Benchmark::Chronometer meter, int n) {
	Map map = populateMap<Map>(n);
	meter.measure([&] {
		uint64_t checksum = 0;
		auto it = map.begin();
		for (int j = 0; j < BENCH_OPS; ++j) {
			if (it == map.end())
				it = map.begin();
			checksum += toChecksum(it->first) + toChecksum(it->second);
			++it;
		}
		return checksum;
	});
}

// =============================================================================
// Non-linear sample points: exponential spacing + clusters around powers of 2
//
// The exponential sweep gives denser sampling at low N and sparser at high N.
// Extra points are inserted around each power of 2 because that is where
// hashmap resizes occur. Mixed workloads randomly insert/erase, so the actual
// map size can drift above the initial N — we therefore cluster across a wide
// range (3/4 to 3/2 of each power of 2) to catch the transition regardless.
// =============================================================================

[[maybe_unused]] static std::vector<int> scalingSamples() {
	std::vector<int> samples;
	// Exponential sweep: finer step above N=2000 where spacing otherwise grows too large
	for (double x = std::log2(100.0); x <= std::log2(2000.0); x += 0.2)
		samples.push_back(static_cast<int>(std::round(std::pow(2.0, x))));
	for (double x = std::log2(2000.0); x <= std::log2(20000.0); x += 0.05)
		samples.push_back(static_cast<int>(std::round(std::pow(2.0, x))));
	// Cluster around powers of 2 above N=2000 (below that the sweep is already dense enough)
	for (int p = 2048; p <= 16384; p *= 2) {
		for (int v : {p * 3 / 4, p * 7 / 8, p - 1, p, p + 1, p * 9 / 8, p * 3 / 2}) {
			if (v >= 100 && v <= 20000)
				samples.push_back(v);
		}
	}
	std::sort(samples.begin(), samples.end());
	samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
	return samples;
}

// =============================================================================
// Pad N to 5 characters for aligned output (e.g. "  100", "20000")
// =============================================================================

static std::string padN(int n) {
	auto s = std::to_string(n);
	return std::string(5 - std::min<size_t>(5, s.size()), ' ') + s;
}

// =============================================================================
// Container type aliases
// =============================================================================

using StdInt    = std::unordered_map<int, int, spring::synced_hash<int>>;
using SpringInt = spring::unordered_map<int, int>;
using UnsyncInt = spring::unsynced_map<int, int, spring::synced_hash<int>>;

using StdPtr    = std::unordered_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;
using SpringPtr = spring::unordered_map<uintptr_t, int>;
using UnsyncPtr = spring::unsynced_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;

using StdStr    = std::unordered_map<std::string, std::string, spring::synced_hash<std::string>>;
using SpringStr = spring::unordered_map<std::string, std::string>;
using UnsyncStr = spring::unsynced_map<std::string, std::string, spring::synced_hash<std::string>>;

// Helper macro to avoid repeating the N-sweep boilerplate
#define BENCH_SWEEP_MIXED(MapType, findPct) \
	for (int N : scalingSamples()) { \
		BENCHMARK_ADVANCED(std::string{"N=" + padN(N)})(Catch::Benchmark::Chronometer meter) { \
			benchmarkMapMixed<MapType>(meter, N, findPct); \
		}; \
	}

#define BENCH_SWEEP_ITER(MapType) \
	for (int N : scalingSamples()) { \
		BENCHMARK_ADVANCED(std::string{"N=" + padN(N)})(Catch::Benchmark::Chronometer meter) { \
			benchmarkMapIterate<MapType>(meter, N); \
		}; \
	}

// =============================================================================
// Scaling benchmarks
//
// Hierarchy: Container type → Workload → Implementation → N sweep
// This gives clean CSV columns: Container, Workload, Impl, Benchmark
// =============================================================================

// clang-format off
TEST_CASE("Map<int,int>")
{
	SECTION("90% find / 10% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdInt,    90) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringInt, 90) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncInt, 90) }
	}
	SECTION("50% find / 50% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdInt,    50) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringInt, 50) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncInt, 50) }
	}
	SECTION("iterate") {
		SECTION("std")      { BENCH_SWEEP_ITER(StdInt)    }
		SECTION("spring")   { BENCH_SWEEP_ITER(SpringInt) }
		SECTION("unsynced") { BENCH_SWEEP_ITER(UnsyncInt) }
	}
}

TEST_CASE("Map<ptr,int>")
{
	SECTION("90% find / 10% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtr,    90) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtr, 90) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtr, 90) }
	}
	SECTION("50% find / 50% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtr,    50) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtr, 50) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtr, 50) }
	}
	SECTION("iterate") {
		SECTION("std")      { BENCH_SWEEP_ITER(StdPtr)    }
		SECTION("spring")   { BENCH_SWEEP_ITER(SpringPtr) }
		SECTION("unsynced") { BENCH_SWEEP_ITER(UnsyncPtr) }
	}
}

TEST_CASE("Map<string,string>")
{
	SECTION("90% find / 10% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdStr,    90) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringStr, 90) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncStr, 90) }
	}
	SECTION("50% find / 50% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdStr,    50) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringStr, 50) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncStr, 50) }
	}
	SECTION("iterate") {
		SECTION("std")      { BENCH_SWEEP_ITER(StdStr)    }
		SECTION("spring")   { BENCH_SWEEP_ITER(SpringStr) }
		SECTION("unsynced") { BENCH_SWEEP_ITER(UnsyncStr) }
	}
}
// clang-format on
