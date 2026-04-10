/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/**
 * Benchmarks comparing std::unordered_map/set against spring::unordered_map/set
 * and spring::unsynced_map/set (all emilib open-addressing, linear probing).
 *
 * Run: ./test_HashMapBenchmark [catch2 options]
 * e.g. ./test_HashMapBenchmark "Map<int,int>"
 */

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

#include "System/UnorderedMap.hpp"
#include "System/UnorderedSet.hpp"
#include "System/SpringHash.h"

#include <catch_amalgamated.hpp>

// =============================================================================
// Fast RNG (xorshift64)
//    std::rand() is MUCH slower than PCG/xorshift and otherwise
//    dominates the runtime in these benchmarks
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
// This ensures Catch2's auto-estimated iterations-per-sample is consistent
// across different N values, making results directly comparable.
constexpr int BENCH_OPS = 1000000;

// Set BENCH_RESERVE=0 to disable .reserve() calls and measure rehashing impact.
static const bool USE_RESERVE = []() {
	const char* env = std::getenv("BENCH_RESERVE");
	return !env || std::string(env) != "0";
}();

// =============================================================================
// Operation types -
// =============================================================================



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

// Generate a key of type T from an integer index.
// int       → the integer itself (matches unit/entity ID patterns)
// uintptr_t → simulated heap pointer (matches pointer-as-key patterns)
// string    → RTS-style unit def name (matches feature/weapon/unit def lookups)
template<typename T>
T makeKey(int n) {
	if constexpr (std::is_same_v<T, uintptr_t>) {
		// Simulate heap pointers: base + n * 128, 16-byte aligned
		// Matches real usage: CSolidObject*, CWorldObject*, lua_State*, etc.
		constexpr uintptr_t heapBase = 0x555555550000ULL;
		return heapBase + static_cast<uintptr_t>(n) * 128;
	} else if constexpr (std::is_integral_v<T>) {
		return static_cast<T>(n);
	} else {
		// RTS unit def names: "armcom", "corllt", "gokflea", "armcom1"
		std::string key;
		key += kFactions[n % kNumFactions];
		key += kUnitTypes[(n / kNumFactions) % kNumUnitTypes];
		if (n >= kNumFactions * kNumUnitTypes)
			key += std::to_string(n / (kNumFactions * kNumUnitTypes));
		return key;
	}
}

// Convert an element into something easy to checksum to force the compiler
// to actually read it (ie. it prevents dead-code elimination)
// This should hopefully be inlined for minimal overhead :)
template<typename T>
uint64_t toChecksum(const T& v) {
	if constexpr (std::is_integral_v<T>)
		return static_cast<uint64_t>(v);
	else
		return v.size(); // string: length is a cheap, non-trivial proxy
}

// enums used to represent insert/find/erase operations
enum class OpType : uint8_t { Find, Insert, Erase };
template<typename K, typename V> struct MapOp { OpType type; K key; V value; };
template<typename K>             struct SetOp { OpType type; K key; };

// For filling a container with elements before the benchmark starts
template<typename Map>
Map populateMap(int n) {
	using K = typename Map::key_type;
	using V = typename Map::mapped_type;
	Map map;
	if (USE_RESERVE) map.reserve(n*2); // pre-size to avoid rehashing during measurement
	for (int i = 0; i < n; ++i)
		map[makeKey<K>(i)] = makeKey<V>(i);
	return map;
}

template<typename Set>
Set populateSet(int n) {
	using K = typename Set::value_type;
	Set set;
	if (USE_RESERVE) set.reserve(n*2); // pre-size to avoid rehashing during measurement
	for (int i = 0; i < n; ++i)
		set.insert(makeKey<K>(i));
	return set;
}

// =============================================================================
// Workload preparation
//
// Pre-populates the container with N entries (keys 0..N-1), then generates
// N operations according to the requested find percentage:
//
//   Find   — keys from [0, 2N) → ~50% hit rate on the initial contents
//   Insert — keys from [N, 3N) → new keys not present in the initial map
//   Erase  — keys from [0, N)  → keys that exist in the initial map
//
// findPct: percentage of operations that are finds (0–100).
// The remainder is split evenly between inserts and erases.
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

template<typename Set>
std::pair<Set, std::vector<SetOp<typename Set::value_type>>>
prepareSetWorkload(int n, int numOps, int findPct) {
	using K = typename Set::value_type;

	Xorshift64 rng;
	Set set = populateSet<Set>(n);

	const int insertPct = (100 - findPct) / 2;

	std::vector<SetOp<K>> ops;
	ops.reserve(numOps);
	for (int i = 0; i < numOps; ++i) {
		const int roll = static_cast<int>(rng.nextBelow(100));
		if (roll < findPct) {
			ops.push_back({OpType::Find, makeKey<K>(static_cast<int>(rng.nextBelow(2 * n)))});
		} else if (roll < findPct + insertPct) {
			ops.push_back({OpType::Insert, makeKey<K>(n + static_cast<int>(rng.nextBelow(2 * n)))});
		} else {
			ops.push_back({OpType::Erase, makeKey<K>(static_cast<int>(rng.nextBelow(n)))});
		}
	}

	return {std::move(set), std::move(ops)};
}

// =============================================================================
// Actual benchmark implementation.
//   We calculate a running checksum to prevent the compiler
//   from optimizing the code away
// =============================================================================
template<typename Map>
void benchmarkMapMixed(Catch::Benchmark::Chronometer meter, int n, int findPct) {
	auto [baseMap, ops] = prepareMapWorkload<Map>(n, BENCH_OPS, findPct);
	std::vector<Map> maps(meter.runs(), baseMap);
	if (USE_RESERVE) { for (auto& m : maps) m.reserve(n*2); } // match populateMap's reserve
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

template<typename Set>
void benchmarkSetMixed(Catch::Benchmark::Chronometer meter, int n, int findPct) {
	auto [baseSet, ops] = prepareSetWorkload<Set>(n, BENCH_OPS, findPct);
	std::vector<Set> sets(meter.runs(), baseSet);
	if (USE_RESERVE) { for (auto& s : sets) s.reserve(n*2); } // match populateSet's reserve
	meter.measure([&](int i) {
		auto& set = sets[i];
		uint64_t checksum = 0;
		for (const auto& op : ops) {
			if (op.type == OpType::Find) {
				checksum += set.count(op.key);
			} else if (op.type == OpType::Insert) {
				set.insert(op.key);
			} else {
				set.erase(op.key);
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

template<typename Set>
void benchmarkSetIterate(Catch::Benchmark::Chronometer meter, int n) {
	Set set = populateSet<Set>(n);
	meter.measure([&] {
		uint64_t checksum = 0;
		auto it = set.begin();
		for (int j = 0; j < BENCH_OPS; ++j) {
			if (it == set.end())
				it = set.begin();
			checksum += toChecksum(*it);
			++it;
		}
		return checksum;
	});
}


// =============================================================================
// Actual Benchmarks
// =============================================================================

// clang-format off
TEST_CASE("Map<int,int>")
{
	using Std    = std::unordered_map<int, int, spring::synced_hash<int>>;
	using Spring = spring::unordered_map<int, int>;
	using Unsync = spring::unsynced_map<int, int, spring::synced_hash<int>>;

	SECTION("90% find / 10% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 10000, 90); };
	}
	SECTION("50% find / 50% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 10000, 50); };
	}
	SECTION("iterate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 100); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 100); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 100); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 1000); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 10000); };
	}
}

TEST_CASE("Map<string,string>")
{
	using Std    = std::unordered_map<std::string, std::string, spring::synced_hash<std::string>>;
	using Spring = spring::unordered_map<std::string, std::string>;
	using Unsync = spring::unsynced_map<std::string, std::string, spring::synced_hash<std::string>>;

	SECTION("90% find / 10% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 10000, 90); };
	}
	SECTION("50% find / 50% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 10000, 50); };
	}
	SECTION("iterate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 100); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 100); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 100); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 1000); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 10000); };
	}
}

TEST_CASE("Set<int>")
{
	using Std    = std::unordered_set<int, spring::synced_hash<int>>;
	using Spring = spring::unordered_set<int>;
	using Unsync = spring::unsynced_set<int, spring::synced_hash<int>>;

	SECTION("90% find / 10% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 10000, 90); };
	}
	SECTION("50% find / 50% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 10000, 50); };
	}
	SECTION("iterate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 100); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 100); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 100); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 1000); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 10000); };
	}
}

TEST_CASE("Set<string>")
{
	using Std    = std::unordered_set<std::string, spring::synced_hash<std::string>>;
	using Spring = spring::unordered_set<std::string>;
	using Unsync = spring::unsynced_set<std::string, spring::synced_hash<std::string>>;

	SECTION("90% find / 10% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 10000, 90); };
	}
	SECTION("50% find / 50% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 10000, 50); };
	}
	SECTION("iterate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 100); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 100); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 100); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 1000); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 10000); };
	}
}

TEST_CASE("Map<uintptr_t,int>")
{
	using Std    = std::unordered_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;
	using Spring = spring::unordered_map<uintptr_t, int>;
	using Unsync = spring::unsynced_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;

	SECTION("90% find / 10% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 100, 90); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 1000, 90); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 10000, 90); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 10000, 90); };
	}
	SECTION("50% find / 50% mutate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 100, 50); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 1000, 50); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 10000, 50); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 10000, 50); };
	}
	SECTION("iterate") {
		BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 100); };
		BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 100); };
		BENCHMARK_ADVANCED("N=100    unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 100); };
		BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 1000); };
		BENCHMARK_ADVANCED("N=1000   unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 1000); };
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 10000); };
		BENCHMARK_ADVANCED("N=10000  unsynced")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 10000); };
	}
}
// clang-format on
