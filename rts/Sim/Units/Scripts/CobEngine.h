/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef COB_ENGINE_H
#define COB_ENGINE_H

/*
 * Simple VM responsible for "scheduling" and running COB threads.
 * It also manages reading and caching of the actual .cob files.
 */

#include <cstdint>
#include <deque>
#include <vector>

#include "CobThread.h"
#include "CobDeferredCallin.h"
#include "System/creg/creg_cond.h"
#include "System/creg/STL_Queue.h"
#include "System/creg/STL_Map.h"
#include "System/creg/STL_Deque.h"
#include "System/Cpp11Compat.hpp"

class CCobThread;
class CCobInstance;
class CCobFile;
class CCobFileHandler;

// CobThreadID is declared in CobThread.h (see it for the encoding). High 32
// bits select a slot in CCobEngine::threadSlots; low 32 bits are a generation
// bumped on every slot reuse so stale ids don't alias a freshly-allocated
// thread. Generation 0 is skipped on wrap so the default-initialized slot
// state (gen=0, occupied=false) never validates any real id.

class CCobEngine
{
	CR_DECLARE_STRUCT(CCobEngine)

public:
	struct SleepingThread {
		CR_DECLARE_STRUCT(SleepingThread)

		CobThreadID id;
		int wt;
	};

	struct ThreadSlot {
		CR_DECLARE_STRUCT(ThreadSlot)

		CCobThread thread;
		uint32_t   generation = 0;
		bool       occupied   = false;
	};

	struct CCobThreadComp {
	public:
		bool operator() (const SleepingThread& a, const SleepingThread& b) const {
			return a.wt > b.wt || (a.wt == b.wt && a.id > b.id);
		}
	};

public:
	void Init() {
		// Target workload is ~10k units (~30-50k concurrent COB threads) with
		// headroom to MAX_UNITS (~100-160k threads). Pre-size the free-list and
		// scheduler queues for that; the slot deque grows by chunks without
		// invalidating existing slots.
		tickAddedThreads.reserve(128);

		freeSlots.reserve(8192);
		runningThreadIDs.reserve(8192);
		waitingThreadIDs.reserve(8192);

		sleepingThreadIDs = {};

		curThread = nullptr;

		currentTime = 0;
	}
	void Kill() {
		threadSlots.clear();
		freeSlots.clear();
		spring::clear_unordered_map(deferredCallins);
		tickAddedThreads.clear();

		runningThreadIDs.clear();
		waitingThreadIDs.clear();

		while (!sleepingThreadIDs.empty()) {
			sleepingThreadIDs.pop();
		}
	}

	void Tick(int deltaTime);
	void ShowScriptError(const std::string& msg);


	static constexpr uint32_t IDSlotIdx(CobThreadID id) { return static_cast<uint32_t>(static_cast<uint64_t>(id) >> 32); }
	static constexpr uint32_t IDGeneration(CobThreadID id) { return static_cast<uint32_t>(static_cast<uint64_t>(id) & 0xFFFFFFFFu); }
	static constexpr CobThreadID MakeID(uint32_t slotIdx, uint32_t gen) {
		return static_cast<CobThreadID>((static_cast<uint64_t>(slotIdx) << 32) | static_cast<uint64_t>(gen));
	}

	CCobThread* GetThread(CobThreadID threadID) {
		const uint32_t idx = IDSlotIdx(threadID);
		if (idx >= threadSlots.size())
			return nullptr;

		ThreadSlot& s = threadSlots[idx];
		if (!s.occupied || s.generation != IDGeneration(threadID))
			return nullptr;

		return &s.thread;
	}

	bool RemoveThread(CobThreadID threadID);
	CobThreadID AddThread(CCobThread&& thread);
	CobThreadID GenThreadID();
	// Release a slot that was reserved by GenThreadID but never handed to
	// AddThread (e.g. an inline-ticked thread that died on its first tick).
	void ReleaseReservation(CobThreadID threadID);

	void QueueAddThread(CCobThread&& thread) { tickAddedThreads.emplace_back(std::move(thread)); }
	void QueueRemoveThread(CobThreadID threadID) { tickRemovedThreads.emplace_back(threadID); }
	void ProcessQueuedThreads();

	void ScheduleThread(const CCobThread* thread);
	void SanityCheckThreads(const CCobInstance* owner);

	const auto& GetThreadSlots() const { return threadSlots; }
	const auto& GetWaitingThreadIDs() const { return waitingThreadIDs; }
	const auto& GetSleepingThreadIDs() const { return sleepingThreadIDs; }
	const auto  GetCurrTime() const { return currentTime; }

	void AddDeferredCallin(CCobDeferredCallin&& deferredCallin);
	void RunDeferredCallins();
private:
	void TickThread(CCobThread* thread);

	void WakeSleepingThreads();
	void TickRunningThreads();

private:
	// Slot pool for every CCobThread across all script instances. std::deque
	// guarantees pointer stability for all outstanding slots as it grows
	// (chunk-allocated), which is the entire point: scheduler queues and other
	// callers can hold a slot index + generation and look it up in O(1)
	// without the old unordered_map's rehash churn or scattered buckets.
	std::deque<ThreadSlot> threadSlots;
	// indices into threadSlots that are currently unoccupied
	std::vector<uint32_t> freeSlots;
	// threads that are spawned during Tick
	std::vector<CCobThread> tickAddedThreads;
	// threads that are killed during Tick
	std::vector<CobThreadID> tickRemovedThreads;

	std::vector<CobThreadID> runningThreadIDs;
	std::vector<CobThreadID> waitingThreadIDs;

	spring::unordered_map<int, std::vector<CCobDeferredCallin> > deferredCallins;

	// stores <id, waketime> pairs s.t. after waking up the ID can be checked
	// for validity; thread owner might get removed while a thread is sleeping
	std::priority_queue<SleepingThread, std::vector<SleepingThread>, CCobThreadComp> sleepingThreadIDs;

	CCobThread* curThread = nullptr;

	int currentTime = 0;
};


extern CCobEngine* cobEngine;

#endif // COB_ENGINE_H
