#pragma once

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
	private:
		MapType animationMap;
	};
}