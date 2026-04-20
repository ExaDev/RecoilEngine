/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "CobEngine.h"

#include "CobDeferredCallin.h"
#include "CobThread.h"
#include "CobFile.h"

#include <cstdint>
#include "System/Misc/TracyDefs.h"
#include "Lua/LuaUI.h"

CR_BIND(CCobEngine, )

CR_REG_METADATA(CCobEngine, (
	CR_MEMBER(threadSlots),
	CR_MEMBER(freeSlots),
	CR_MEMBER(tickAddedThreads),
	CR_MEMBER(tickRemovedThreads),
	CR_MEMBER(runningThreadIDs),
	CR_MEMBER(sleepingThreadIDs),
	// always null/empty when saving
	CR_IGNORED(waitingThreadIDs),

	CR_IGNORED(curThread),
	CR_IGNORED(deferredCallins),

	CR_MEMBER(currentTime)
))

CR_BIND(CCobEngine::SleepingThread, )
CR_REG_METADATA(CCobEngine::SleepingThread, (
	CR_MEMBER(id),
	CR_MEMBER(wt)
))

CR_BIND(CCobEngine::ThreadSlot, )
CR_REG_METADATA(CCobEngine::ThreadSlot, (
	CR_MEMBER(thread),
	CR_MEMBER(generation),
	CR_MEMBER(occupied)
))

static const char* const numCobThreadsPlot = "CobThreads";

CobThreadID CCobEngine::GenThreadID()
{
	RECOIL_DETAILED_TRACY_ZONE;
	uint32_t idx;

	if (!freeSlots.empty()) {
		idx = freeSlots.back();
		freeSlots.pop_back();
		ThreadSlot& s = threadSlots[idx];
		// Bump generation; skip 0 on wrap so a default-initialized slot
		// (gen=0, occupied=false) never validates any real id.
		s.generation = s.generation + 1u;
		if (s.generation == 0u)
			s.generation = 1u;
		s.occupied = true;
	} else {
		idx = static_cast<uint32_t>(threadSlots.size());
		threadSlots.emplace_back();
		ThreadSlot& s = threadSlots[idx];
		s.generation = 1u;
		s.occupied = true;
	}

	return MakeID(idx, threadSlots[idx].generation);
}

CobThreadID CCobEngine::AddThread(CCobThread&& thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (thread.GetID() == kInvalidCobThreadID)
		thread.SetID(GenThreadID());

	const CobThreadID id = thread.GetID();
	const uint32_t idx = IDSlotIdx(id);
	assert(idx < threadSlots.size());

	ThreadSlot& slot = threadSlots[idx];
	assert(slot.occupied);
	assert(slot.generation == IDGeneration(id));

	CCobInstance* o = thread.cobInst;
	slot.thread = std::move(thread);
	o->AddThreadID(id);

	TracyPlot(numCobThreadsPlot, static_cast<int64_t>(threadSlots.size() - freeSlots.size()));

	return id;
}

bool CCobEngine::RemoveThread(CobThreadID threadID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const uint32_t idx = IDSlotIdx(threadID);
	if (idx >= threadSlots.size())
		return false;

	ThreadSlot& slot = threadSlots[idx];
	if (!slot.occupied || slot.generation != IDGeneration(threadID))
		return false;

	// Mirror the old unordered_map::erase semantics: run ~CCobThread so that
	// Stop() fires the thread callback and un-registers the id from its owner
	// CCobInstance's threadIDs list, and the data/call stacks return to the
	// static free pools. Then placement-new a fresh default-initialized body
	// back into the slot so it's safe for the next occupant.
	slot.thread.~CCobThread();
	new (&slot.thread) CCobThread();
	slot.occupied = false;
	freeSlots.push_back(idx);

	TracyPlot(numCobThreadsPlot, static_cast<int64_t>(threadSlots.size() - freeSlots.size()));
	return true;
}

void CCobEngine::ReleaseReservation(CobThreadID threadID)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const uint32_t idx = IDSlotIdx(threadID);
	if (idx >= threadSlots.size())
		return;

	ThreadSlot& slot = threadSlots[idx];
	if (!slot.occupied || slot.generation != IDGeneration(threadID))
		return;

	slot.occupied = false;
	freeSlots.push_back(idx);
}

void CCobEngine::ProcessQueuedThreads() {
	ZoneScoped;

	// Remove threads killed during Tick by other thread (SIGNAL), we do it
	// here as nothing is actively referencing any thread's memory here.
	for (CobThreadID threadID: tickRemovedThreads) {
		RemoveThread(threadID);
	}
	tickRemovedThreads.clear();

	// move new threads spawned by START into their reserved slots; their
	// ID's will already have been scheduled into either waitingThreadIDs or
	// sleepingThreadIDs, and their slots were reserved by GenThreadID at
	// START time.
	for (CCobThread& t: tickAddedThreads) {
		AddThread(std::move(t));
	}

	tickAddedThreads.clear();
}

// a thread wants to continue running at a later time, and adds itself to the scheduler
void CCobEngine::ScheduleThread(const CCobThread* thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	switch (thread->GetState()) {
		case CCobThread::Run: {
			waitingThreadIDs.push_back(thread->GetID());
		} break;
		case CCobThread::Sleep: {
			sleepingThreadIDs.push(SleepingThread{thread->GetID(), thread->GetWakeTime()});
		} break;
		default: {
			LOG_L(L_ERROR, "[COBEngine::%s] unknown state %d for thread %lld", __func__, thread->GetState(), static_cast<long long>(thread->GetID()));
		} break;
	}
}

void CCobEngine::SanityCheckThreads(const CCobInstance* owner)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (false) {
		// no threads belonging to owner should be left
		for (const ThreadSlot& s: threadSlots) {
			if (!s.occupied)
				continue;
			assert(s.thread.cobInst != owner);
		}
		for (const CCobThread& t: tickAddedThreads) {
			assert(t.cobInst != owner);
		}
	}
}


void CCobEngine::TickThread(CCobThread* thread)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// for error messages originating in CUnitScript
	curThread = thread;

	// NB: threadID is still in <runningThreadIDs> here, TickRunningThreads clears it
	if (thread != nullptr && !thread->Tick())
		RemoveThread(thread->GetID());

	curThread = nullptr;
}

void CCobEngine::WakeSleepingThreads()
{
	ZoneScoped;
	// check on the sleeping threads, remove any whose owner died
	while (!sleepingThreadIDs.empty()) {
		// PQ already knows the earliest wakeTime — bail before any hashmap probe
		// when nothing is due.
		const auto top = sleepingThreadIDs.top();
		if (top.wt >= currentTime)
			break;

		sleepingThreadIDs.pop();

		CCobThread* zzzThread = GetThread(top.id);
		if (zzzThread == nullptr)
			continue;

		// wake up the thread and tick it (if not dead)
		// this can quite possibly re-add the thread to <sleepingThreadIDs>
		// again, but any thread is guaranteed to sleep for at least 1 tick
		switch (zzzThread->GetState()) {
			case CCobThread::Sleep: {
				zzzThread->SetState(CCobThread::Run);
				TickThread(zzzThread);
			} break;
			case CCobThread::Dead: {
				RemoveThread(zzzThread->GetID());
			} break;
			default: {
				LOG_L(L_ERROR, "[COBEngine::%s] unknown state %d for thread %lld", __func__, zzzThread->GetState(), static_cast<long long>(zzzThread->GetID()));
			} break;
		}
	}
}

void CCobEngine::TickRunningThreads()
{
	ZoneScoped;
	// advance all currently running threads
	for (const CobThreadID threadID: runningThreadIDs) {
		TickThread(GetThread(threadID));
	}

	// a thread can never go from running->running, so clear the list
	// note: if preemption was to be added, this would no longer hold
	// however, TA scripts can not run preemptively anyway since there
	// aren't any synchronization methods available
	runningThreadIDs.clear();

	// prepare threads that will run next frame
	std::swap(runningThreadIDs, waitingThreadIDs);
}

void CCobEngine::Tick(int deltaTime)
{
	ZoneScoped;
	currentTime += deltaTime;

	TickRunningThreads();
	ProcessQueuedThreads();

	WakeSleepingThreads();
	ProcessQueuedThreads();
}


void CCobEngine::ShowScriptError(const std::string& msg)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (curThread != nullptr) {
		curThread->ShowError(msg.c_str());
		return;
	}

	LOG_L(L_ERROR, "[COBEngine::%s] \"%s\" outside script execution", __func__, msg.c_str());
}


void CCobEngine::AddDeferredCallin(CCobDeferredCallin&& deferredCallin)
{
	deferredCallins[deferredCallin.funcHash].push_back(deferredCallin);
}


void CCobEngine::RunDeferredCallins()
{
	std::vector<int> funcHashes;
	funcHashes.reserve(deferredCallins.size());
	for(auto& it: deferredCallins)
		funcHashes.push_back(it.first);

	for(auto funcHash: funcHashes) {
		auto it = deferredCallins.find(funcHash); // 'it' has to necessarily be present at this point

		auto callins = std::move(it->second);
		deferredCallins.erase(it);

		const LuaHashString cmdStr = LuaHashString(callins[0].funcName.c_str());
		luaRules->unsyncedLuaHandle.Cob2LuaBatch(cmdStr, callins);
		if (luaUI)
			luaUI->Cob2LuaBatch(cmdStr, callins);
	}
}
