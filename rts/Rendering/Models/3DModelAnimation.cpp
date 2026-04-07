#include "3DModelAnimation.hpp"

#include <algorithm>
#include <cassert>

void ModelAnimation::Map::RemoveEmptyAnimations()
{
	const auto IsEmptySequence = [](ModelAnimation::Sequence& seq) {
		static const auto IsEmpty = []<typename T>(const ModelAnimation::TypedSequence<T>& ts) {
			return ts.timeFrames.empty();
		};

		static const auto RemoveDefault = []<typename T>(ModelAnimation::TypedSequence<T>& ts) -> void {
			if constexpr (std::is_same_v<T, float3>) {
				static constexpr auto def = float3();
				if (std::all_of(ts.values.begin(), ts.values.end(), [](const auto& val) { return val == def; })) {
					ts.timeFrames.clear();
					ts.values.clear();
				}
			}
			else if constexpr (std::is_same_v<T, CQuaternion>) {
				static constexpr auto def = CQuaternion();
				if (std::all_of(ts.values.begin(), ts.values.end(), [](const auto& val) { return val == def; })) {
					ts.timeFrames.clear();
					ts.values.clear();
				}
			}
			else if constexpr (std::is_same_v<T, float>) {
				static constexpr auto def = 1.0f;
				if (std::all_of(ts.values.begin(), ts.values.end(), [](const auto& val) { return val == def; })) {
					ts.timeFrames.clear();
					ts.values.clear();
				}
			}
		};

		std::apply([&](auto&... elems) {
			(RemoveDefault(elems), ...);
		}, seq);

		return std::apply([&](const auto&... elems) {
			return (IsEmpty(elems) && ...);
		}, seq);
	};

	for (auto& entry : animationMap) {
		for (auto it = entry.pieceData.begin(); it != entry.pieceData.end(); ) {
			if (IsEmptySequence(it->second))
				it = entry.pieceData.erase(it);
			else
				++it;
		}
	}

	animationMap.erase(
		std::remove_if(animationMap.begin(), animationMap.end(), [](const AnimationEntry& e) { return e.pieceData.empty(); }), animationMap.end()
	);

	for (auto& entry : animationMap) {
		entry.duration = 0.0f;
		for (const auto& [pieceIdx, seq] : entry.pieceData) {
			const auto& [tSeq, rSeq, sSeq] = seq;
			if (!tSeq.timeFrames.empty()) entry.duration = std::max(entry.duration, tSeq.timeFrames.back());
			if (!rSeq.timeFrames.empty()) entry.duration = std::max(entry.duration, rSeq.timeFrames.back());
			if (!sSeq.timeFrames.empty()) entry.duration = std::max(entry.duration, sSeq.timeFrames.back());
		}
	}
}

static size_t FindKeyframeBracket(const std::vector<float>& timeFrames, float time)
{
	assert(!timeFrames.empty());
	if (time <= timeFrames.front())
		return 0;
	if (time >= timeFrames.back())
		return timeFrames.size() - 1;

	// upper_bound gives first element > time; step back one to get lower frame
	auto it = std::upper_bound(timeFrames.begin(), timeFrames.end(), time);
	return static_cast<size_t>(std::distance(timeFrames.begin(), it)) - 1;
}


template<>
float3 ModelAnimation::SampleSequence(const TypedSequence<float3>& seq, float time)
{
	if (seq.timeFrames.empty())
		return float3{};

	const size_t i = FindKeyframeBracket(seq.timeFrames, time);
	if (i + 1 >= seq.timeFrames.size())
		return seq.values[i];

	const float t0 = seq.timeFrames[i];
	const float t1 = seq.timeFrames[i + 1];
	const float alpha = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
	const float3& v0 = seq.values[i];
	const float3& v1 = seq.values[i + 1];
	return v0 + (v1 - v0) * alpha;
}


template<>
CQuaternion ModelAnimation::SampleSequence(const TypedSequence<CQuaternion>& seq, float time)
{
	if (seq.timeFrames.empty())
		return CQuaternion{};

	const size_t i = FindKeyframeBracket(seq.timeFrames, time);
	if (i + 1 >= seq.timeFrames.size())
		return seq.values[i];

	const float t0 = seq.timeFrames[i];
	const float t1 = seq.timeFrames[i + 1];
	const float alpha = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
	return CQuaternion::SLerp(seq.values[i], seq.values[i + 1], alpha);
}


template<>
float ModelAnimation::SampleSequence(const TypedSequence<float>& seq, float time)
{
	if (seq.timeFrames.empty())
		return 1.0f; // default scale is 1, not 0

	const size_t i = FindKeyframeBracket(seq.timeFrames, time);
	if (i + 1 >= seq.timeFrames.size())
		return seq.values[i];

	const float t0 = seq.timeFrames[i];
	const float t1 = seq.timeFrames[i + 1];
	const float alpha = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
	return seq.values[i] + (seq.values[i + 1] - seq.values[i]) * alpha;
}
