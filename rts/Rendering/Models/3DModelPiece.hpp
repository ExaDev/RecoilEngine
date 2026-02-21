#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <optional>

#include "3DModelDefs.hpp"
#include "3DModelMisc.hpp"
#include "VertexData.hpp"
#include "Sim/Misc/CollisionVolume.h"
#include "System/Matrix44f.h"
#include "System/Transform.hpp"
#include "System/AABB.hpp"

struct S3DModel;
struct S3DModelPiece {
	S3DModelPiece() = default;

	virtual void Clear() {
		name.clear();
		children.clear();

		for (S3DModelPiecePart& p : shatterParts) {
			p.renderData.clear();
		}

		tmpVerts.clear();
		tmpIndcs.clear();
		shatterIndices.clear();

		parent = nullptr;
		colvol = {};

		bposeTransform.LoadIdentity();

		emitPos = ZeroVector;
		emitDir = ZeroVector;

		offset = ZeroVector;
		goffset = ZeroVector;
		scale = 1.0f;

		aabb.Reset();

		vertIndex = ~0u;
		indxStart = ~0u;
		indxCount = ~0u;

		relVertOff = ~0u;
		relVertCnt = 0;
		relIndxOff = ~0u;
		relIndxCnt = 0;
	}

	void SetEmitters();

	const auto& GetEmitPos() const { return emitPos; }
	const auto& GetEmitDir() const { return emitDir; }

	void DrawElements(uint32_t prim = 0x0004/*GL_TRIANGLES*/) const;
	static void DrawShatterElements(uint32_t vboIndxStart, uint32_t vboIndxCount, uint32_t prim = 0x0004/*GL_TRIANGLES*/);
public:
	void DrawStaticLegacy(bool bind, bool bindPosMat) const;
	void DrawStaticLegacyRec() const;

	void CreateShatterPieces();
	void Shatter(float, int, int, int, const float3, const float3, const CMatrix44f&) const;

	void SetPieceTransform(const Transform& parentTra);
	void SetBakedTransform(const Transform& tra) {
		if (tra.IsIdentity())
			bakedTransform = std::nullopt;
		else
			bakedTransform = tra;
	}

	Transform ComposeTransform(const float3& t, const float3& r, float s) const;

	void SetCollisionVolume(const CollisionVolume& cv) { colvol = cv; }
	const CollisionVolume* GetCollisionVolume() const { return &colvol; }
	      CollisionVolume* GetCollisionVolume()       { return &colvol; }

	bool HasGeometryData() const { return tmpIndcs.size() >= 3; }
	void SetParentModel(S3DModel* model_) { model = model_; }
	const S3DModel* GetParentModel() const { return model; }

	void ReleaseShatterIndices();

	bool HasBackedTra() const { return bakedTransform.has_value(); }

	void SetGlobalOffset();
private:
	void CreateShatterPiecesVariation(int num);
	void DrawStaticLegacyRecImpl(const float3& rootT) const;
public:
	std::string name;
	std::vector<S3DModelPiece*> children;
	std::array<S3DModelPiecePart, S3DModelPiecePart::SHATTER_VARIATIONS> shatterParts;

	S3DModelPiece* parent = nullptr;
	CollisionVolume colvol;

	// bind-pose transform, including baked rots
	Transform bposeTransform;

	// baked local-space rotations
	std::optional<Transform> bakedTransform;

	float3 emitPos = ZeroVector;
	float3 emitDir = ZeroVector;

	float3 offset;      /// local (piece-space) offset wrt. parent piece
	float3 goffset;     /// global (model-space) offset wrt. root piece
	float scale{1.0f};  /// baked uniform scaling factor (assimp-only)

	AABB aabb;

	uint32_t vertIndex = ~0u; // global vertex number offset
	uint32_t indxStart = ~0u; // global Index VBO offset
	uint32_t indxCount = ~0u;

	// Relative offset/count within model.skinnedMesh (set for pieces with geometry)
	uint32_t relVertOff = ~0u;
	uint32_t relVertCnt = 0;
	uint32_t relIndxOff = ~0u;
	uint32_t relIndxCnt = 0;

	// Temporary vertex and index data, cleared after upload to GPU
	std::vector<SVertexData> tmpVerts;
	std::vector<uint32_t> tmpIndcs;
	std::vector<uint32_t> shatterIndices;

	S3DModel* model = nullptr;
};