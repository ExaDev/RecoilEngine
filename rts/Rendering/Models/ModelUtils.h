#pragma once

#include <vector>
#include <optional>
#include <string>

#include "VertexData.hpp"

class LuaTable;
struct S3DModel;
struct S3DModelPiece;

namespace Skinning {
	struct SkinnedMesh {
		std::vector<SVertexData> verts;
		std::vector<uint32_t> indcs;
	};
};

namespace ModelUtils {
	struct ModelParams {
		std::array<std::optional<std::string>, 2> texs;
		std::optional<float3> relMidPos;
		std::optional<float3> mins;
		std::optional<float3> maxs;
		std::optional<float> radius;
		std::optional<float> height;
		std::optional<bool> flipTextures;
		std::optional<bool> invertTeamColor;
		std::optional<bool> s3oCompat;
	};

	// Iterate over the model and calculate its overall dimensions
	void CalculateModelDimensions(S3DModel* model, S3DModelPiece* piece);

	// Get ModelParams from the modelTable
	void GetModelParams(const LuaTable& modelTable, ModelParams& modelParams);

	// Calculate model properties from LuaTable (fills ModelParams and calls overload)
	void CalculateModelProperties(S3DModel* model, const LuaTable& modelTable);

	// Calculate model properties from ModelParams
	void CalculateModelProperties(S3DModel* model, const ModelParams& modelParams);

	// Calculate missing normals
	void CalculateNormals(std::vector<SVertexData>& verts, const std::vector<uint32_t>& indcs);

	// Calculate missing tangents
	void CalculateTangents(std::vector<SVertexData>& verts, const std::vector<uint32_t>& indcs);

	// Transfer all piece vertices/indices to model's skinnedMesh, transforming vertices to model space
	void TransferPiecesToSkinnedMesh(S3DModel* model);

	static constexpr uint32_t INVALID_INDEX = uint32_t(-1);
}

namespace ModelLog {
	// Verbose logging of model properties
	void LogModelProperties(const S3DModel& model);
}