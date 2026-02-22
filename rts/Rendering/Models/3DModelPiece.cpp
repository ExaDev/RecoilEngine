#include "3DModelPiece.hpp"

#include "3DModel.hpp"
#include "3DModelVAO.hpp"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Game/GlobalUnsynced.h"
#include "System/Misc/TracyDefs.h"

/** ****************************************************************************************************
 * S3DModelPiece
 */

void S3DModelPiece::DrawStaticLegacy(bool bind, bool bindPosMat) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!HasGeometryData())
		return;

	if (bind) S3DModelHelpers::BindLegacyAttrVBOs();

	// Note: bindPosMat is now ignored because vertices are already in model space
	// The bposeTransform was already applied during TransferPiecesToSkinnedMesh()
	// For static rendering, we don't need any additional transform
	DrawElements();

	if (bind) S3DModelHelpers::UnbindLegacyAttrVBOs();
}

void S3DModelPiece::DrawStaticLegacyRecImpl(const float3& rootT) const
{
	// Build a per-piece transform that correctly positions it relative to the root piece:
	//   rotation + scale  come from this piece's own accumulated bposeTransform
	//   translation       is the offset from the root piece in model space (bpose.t - rootT),
	//                     so the chunk flies as a connected rigid body centered at drawPos.
	const CMatrix44f relMat = Transform(bposeTransform.r, bposeTransform.t - rootT, bposeTransform.s).ToMatrix();
	glPushMatrix();
	glMultMatrixf(relMat);
	DrawElements();
	glPopMatrix();

	for (const S3DModelPiece* childPiece : children) {
		childPiece->DrawStaticLegacyRecImpl(rootT);
	}
}

// only used by projectiles with the PF_Recursive flag
void S3DModelPiece::DrawStaticLegacyRec() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	S3DModelHelpers::BindLegacyAttrVBOs();
	DrawStaticLegacyRecImpl(bposeTransform.t);
	S3DModelHelpers::UnbindLegacyAttrVBOs();
}

void S3DModelPiece::CreateShatterPieces()
{
	RECOIL_DETAILED_TRACY_ZONE;
	tmpShIndcs.reserve(S3DModelPiecePart::SHATTER_VARIATIONS * tmpIndcs.size());
	for (int i = 0; i < S3DModelPiecePart::SHATTER_VARIATIONS; ++i) {
		CreateShatterPiecesVariation(i);
	}
}

void S3DModelPiece::SetGlobalOffset()
{
	// Calculate goffset (global offset from root piece)
	// Note: goffset only captures translation, not rotation/scale
	// Model bounds are calculated separately using bposeTransform in S3DModel::CalcModelBounds()
	const CMatrix44f scaleRotMat = ComposeTransform(ZeroVector, ZeroVector, scale).ToMatrix();
	goffset = scaleRotMat.Mul(offset) + (parent != nullptr ? parent->goffset : ZeroVector);
}

void S3DModelPiece::CreateShatterPiecesVariation(int num)
{
	RECOIL_DETAILED_TRACY_ZONE;
	using ShatterPartDataPair = std::pair<S3DModelPiecePart::RenderData, std::vector<uint32_t>>;
	using ShatterPartsBuffer  = std::array<ShatterPartDataPair, S3DModelPiecePart::SHATTER_MAX_PARTS>;

	ShatterPartsBuffer shatterPartsBuf;

	for (auto& [rd, idcs] : shatterPartsBuf) {
		rd.dir = (guRNG.NextVector()).ANormalize();
	}

	// helper
	const auto GetPolygonDir = [&](size_t idx)
	{
		float3 midPos;
		midPos += tmpVerts[tmpIndcs[idx + 0]].pos;
		midPos += tmpVerts[tmpIndcs[idx + 1]].pos;
		midPos += tmpVerts[tmpIndcs[idx + 2]].pos;
		midPos /= 3.0f;
		return midPos.ANormalize();
	};

	// add vertices to splitter parts
	for (size_t i = 0; i < tmpIndcs.size(); i += 3) {
		const float3& dir = GetPolygonDir(i);

		// find the closest shatter part (the one that points into same dir)
		float md = -2.0f;

		ShatterPartDataPair* mcp = nullptr;
		const S3DModelPiecePart::RenderData* rd = nullptr;

		for (ShatterPartDataPair& cp: shatterPartsBuf) {
			rd = &cp.first;

			if (rd->dir.dot(dir) < md)
				continue;

			md = rd->dir.dot(dir);
			mcp = &cp;
		}

		assert(mcp);

		//  + vertex offset + piece->relVertOff will be added in S3DModelVAO::ProcessIndicies()
		(mcp->second).push_back(tmpIndcs[i + 0]);
		(mcp->second).push_back(tmpIndcs[i + 1]);
		(mcp->second).push_back(tmpIndcs[i + 2]);
	}

	{
		const size_t mapSize = tmpIndcs.size();

		uint32_t indxPos = 0;

		for (auto& [rd, idcs] : shatterPartsBuf) {
			rd.indexCount = static_cast<uint32_t>(idcs.size());
			rd.indexStart = static_cast<uint32_t>(num * mapSize) + indxPos;

			if (rd.indexCount > 0) {
				tmpShIndcs.insert(tmpShIndcs.end(), idcs.begin(), idcs.end());
				indxPos += rd.indexCount;
			}
		}
	}

	{
		// delete empty splitter parts
		size_t backIdx = shatterPartsBuf.size() - 1;

		for (size_t j = 0; j < shatterPartsBuf.size() && j < backIdx; ) {
			const auto& [rd, idcs] = shatterPartsBuf[j];

			if (rd.indexCount == 0) {
				std::swap(shatterPartsBuf[j], shatterPartsBuf[backIdx--]);
				continue;
			}

			j++;
		}

		shatterParts[num].renderData.clear();
		shatterParts[num].renderData.reserve(backIdx + 1);

		// finish: copy buffer to actual memory
		for (size_t n = 0; n <= backIdx; n++) {
			shatterParts[num].renderData.push_back(shatterPartsBuf[n].first);
		}
	}
}


void S3DModelPiece::Shatter(float pieceChance, int modelType, int texType, int team, const float3 pos, const float3 speed, const CMatrix44f& m) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const float2  pieceParams = {float3::max(float3::fabs(aabb.maxs), float3::fabs(aabb.mins)).Length(), pieceChance};
	const   int2 renderParams = {texType, team};

	projectileHandler.AddFlyingPiece(modelType, this, m, pos, speed, pieceParams, renderParams);
}

void S3DModelPiece::SetPieceTransform(const Transform& parentTra)
{
	bposeTransform = parentTra * ComposeTransform(offset, ZeroVector, scale);
	bposeTransformInv = bposeTransform.InvertAffine();

	for (S3DModelPiece* c : children) {
		c->SetPieceTransform(bposeTransform);
	}
}

Transform S3DModelPiece::ComposeTransform(const float3& t, const float3& r, float s) const
{
	// NOTE:
	//   ORDER MATTERS (T(baked + script) * R(baked) * R(script) * S(baked))
	//   translating + rotating + scaling is faster than matrix-multiplying
	//   m is identity so m.SetPos(t)==m.Translate(t) but with fewer instrs
	Transform tra;
	tra.t = t;

	if (bakedTransform.has_value())
		tra *= bakedTransform.value();

	tra *= Transform(CQuaternion::FromEulerYPRNeg(-r), ZeroVector, s);
	return tra;
}

void S3DModelPiece::SetEmitters()
{
	const auto vertCount = tmpVerts.size();
	if (vertCount == 0) {
		emitPos = ZeroVector;
		emitDir = FwdVector;
	}
	else if (vertCount == 1) {
		emitPos = ZeroVector;
		emitDir = tmpVerts[0].pos;
	}
	else {
		emitPos = tmpVerts[0].pos;
		emitDir = tmpVerts[1].pos - tmpVerts[0].pos;
	}
	emitDir.SafeANormalize();
}

void S3DModelPiece::DrawElements(GLuint prim) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (relIndxCnt == 0)
		return;
	assert(relIndxCnt != ~0u);
	assert(model != nullptr);

	// Use model's global VBO offset + piece's relative offset
	S3DModelVAO::GetInstance().DrawElements(prim, model->indxStart + relIndxOff, relIndxCnt);
}

void S3DModelPiece::DrawShatterElements(uint32_t vboIndxStart, uint32_t vboIndxCount, GLuint prim)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (vboIndxCount == 0)
		return;

	S3DModelVAO::GetInstance().DrawElements(prim, vboIndxStart, vboIndxCount);
}