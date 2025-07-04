#include "3DModelAnimation.hpp"

#include <algorithm>

void ModelAnimation::Map::RemoveEmptyAnimations()
{
	const auto IsEmptySequence = [](ModelAnimation::Sequence& seq) {
		static const auto IsEmpty = []<typename T>(const ModelAnimation::TypedSequence<T>&ts) {
			return ts.timeFrames.empty();
		};

		static const auto RemoveDefault = []<typename T>(ModelAnimation::TypedSequence<T>&ts) -> void {
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

	for (auto animMapIt = animationMap.begin(); animMapIt != animationMap.end(); /*NOOP*/) {
		for (auto pieceAnimMapIt = animMapIt->second.begin(); pieceAnimMapIt != animMapIt->second.end(); /*NOOP*/) {
			if (IsEmptySequence(pieceAnimMapIt->second))
				pieceAnimMapIt = animMapIt->second.erase(pieceAnimMapIt);
			else
				pieceAnimMapIt++;
		}

		// check if it has no pieces
		if (animMapIt->second.empty())
			animMapIt = animationMap.erase(animMapIt);
		else
			animMapIt++;
	}
}
