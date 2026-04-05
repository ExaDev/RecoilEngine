#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "System/BranchPrediction.h"
#include "System/UnorderedMap.hpp"
#include "System/float3.h"
#include "System/Quaternion.h"

namespace ModelAnimation {
	template<typename T>
	struct TypedSequence {
		std::vector<float> timeFrames;
		std::vector<T> values;
	};

	using Sequence = std::tuple<
		TypedSequence<float3>,
		TypedSequence<CQuaternion>,
		TypedSequence<float>
	>;

	using PieceInfoDataMap = spring::unordered_map<size_t, Sequence>;
	using MapType = spring::unordered_map<std::string, PieceInfoDataMap>;

	class Map {
	public:
		auto GetNamedAnimationIterator(const std::string& animName) {
			auto it = animationMap.find(animName);
			if likely(it == animationMap.end())
				it = animationMap.emplace(animName, {}).first;

			return it;
		}

		auto GetNamedAnimationIterator(const std::string& animName) const {
			return animationMap.find(animName);
		}

		template<typename T>
		decltype(auto) GetPieceAnimationVectors(MapType::iterator animMapIt, size_t animPieceIdx) {
			assert(animMapIt != animationMap.end());
			return std::get<TypedSequence<T>>(animMapIt->second[animPieceIdx]);
		}

		template<typename T>
		const TypedSequence<T>* GetPieceAnimationVectors(MapType::const_iterator animMapIt, size_t animPieceIdx) const {
			if (animMapIt == animationMap.end())
				return nullptr;

			auto pieceAnimMapIt = animMapIt->second.find(animPieceIdx);
			if (pieceAnimMapIt == animMapIt->second.end())
				return nullptr;

			return &std::get<TypedSequence<T>>(pieceAnimMapIt->second);
		}

		void RemoveEmptyAnimations();

		bool HasAnimation(const std::string& name) const {
			return animationMap.find(name) != animationMap.end();
		}

		// Returns the duration (seconds) of the named animation, or 0.0 if not found.
		float GetAnimationDuration(const std::string& name) const;

		MapType::const_iterator cbegin() const { return animationMap.cbegin(); }
		MapType::const_iterator cend() const { return animationMap.cend(); }
		size_t size() const { return animationMap.size(); }
	private:
		MapType animationMap;
	};

	// Sample a keyframe sequence at the given time using linear interpolation (SLERP for quaternions).
	// Returns the default value for the type if the sequence is empty.
	// Time is clamped to [first, last] keyframe (no looping — caller handles that).
	template<typename T>
	T SampleSequence(const TypedSequence<T>& seq, float time);
}