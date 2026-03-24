/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */
#include <cassert>
#include <cfloat>
#include <limits>

#include "ShadowHandler.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GameVersion.h"
#include "Game/Game.h"
#include "Map/BaseGroundDrawer.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Features/FeatureDrawer.h"
#include "Rendering/Env/Particles/ProjectileDrawer.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/Features/FeatureDrawer.h"
#include "Rendering/Env/GrassDrawer.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/GL/FBO.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/SubState.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/GL/RenderBuffers.h"
#include "System/Config/ConfigHandler.h"

#include "System/EventHandler.h"
#include "System/Matrix44f.h"
#include "System/SpringMath.h"
#include "System/StringUtil.h"
#include "System/Log/ILog.h"

#include <fmt/format.h>

CONFIG(int, Shadows).defaultValue(2).headlessValue(-1).minimumValue(-1).safemodeValue(-1).description(R"(
Sets whether shadows are rendered.
Use numbers:
	-1:=forceoff, 0:=off, 1:=full
Or use bitmask combination to disable particular parts of shadows rendering:
	2:=shadows for terrain, 4:=shadows for solid models, 8:=semitransparent shadows for particles, 16:=grass
)");

CONFIG(int, ShadowMapSize)
	.defaultValue(CShadowHandler::DEF_SHADOWMAP_SIZE)
	.minimumValue(CShadowHandler::MIN_SHADOWMAP_SIZE)
	.maximumValue(CShadowHandler::MAX_SHADOWMAP_SIZE)
	.description("Sets the resolution of shadows. Higher numbers increase quality at the cost of performance.");

CONFIG(int, ShadowProjectionMode).deprecated(true);
CONFIG(bool, ShadowColorMode).deprecated(true);

CShadowHandler shadowHandler;

void CShadowHandler::Reload(const char* argv)
{
	int nextShadowConfig = (shadowConfig + 1) & 0xF;
	int nextShadowMapSize = shadowMapSize;

	if (argv != nullptr)
		(void) sscanf(argv, "%i %i", &nextShadowConfig, &nextShadowMapSize);

	// do nothing without a parameter change
	if (nextShadowConfig == shadowConfig && nextShadowMapSize == shadowMapSize)
		return;

	configHandler->Set("Shadows", nextShadowConfig & 0xF);
	configHandler->Set("ShadowMapSize", nextShadowMapSize);

	Kill();
	Init();
}

void CShadowHandler::Init()
{
	const bool tmpFirstInit = firstInit;
	firstInit = false;

	shadowConfig  = configHandler->GetInt("Shadows");
	shadowMapSize = configHandler->GetInt("ShadowMapSize");
	shadowGenBits = SHADOWGEN_BIT_NONE;

	shadowsLoaded = false;
	inShadowPass = false;

	shadowDepthTexture = 0;
	shadowColorTexture = 0;

	if (!tmpFirstInit && !shadowsSupported)
		return;

	// possible values for the "Shadows" config-parameter:
	// < 0: disable and don't try to initialize
	//   0: disable, but create a fallback FBO
	// > 0: enabled (by default for all shadow-casting geometry if equal to 1)
	if (shadowConfig < 0) {
		LOG("[%s] shadow rendering is disabled (config-value %d)", __func__, shadowConfig);
		return;
	}

	if (shadowConfig > 0)
		shadowGenBits = SHADOWGEN_BIT_MODEL | SHADOWGEN_BIT_MAP | SHADOWGEN_BIT_PROJ | SHADOWGEN_BIT_TREE;

	if (shadowConfig > 1)
		shadowGenBits &= (~shadowConfig);

	// no warnings when running headless
	if (SpringVersion::IsHeadless())
		return;

	if (!InitFBOAndTextures()) {
		// free any resources allocated by InitFBOAndTextures()
		FreeFBOAndTextures();

		LOG_L(L_ERROR, "[%s] failed to initialize depth-texture FBO", __func__);
		return;
	}

	if (tmpFirstInit)
		shadowsSupported = true;

	if (shadowConfig > 0)
		LoadShadowGenShaders();
}

void CShadowHandler::Kill()
{
	FreeFBOAndTextures();
	shaderHandler->ReleaseProgramObjects("[ShadowHandler]");
	shadowGenProgs.fill(nullptr);
}


void CShadowHandler::Update()
{
	if (freezeFrustum)
		return;

	CCamera* playCam = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);
	CCamera* shadCam = CCameraHandler::GetCamera(CCamera::CAMTYPE_SHADOW);

	CalcShadowMatrices(playCam, shadCam);
}

void CShadowHandler::SaveShadowMapTextures() const
{
	glSaveTexture(shadowDepthTexture, fmt::format("smDepth_{}.png", globalRendering->drawFrame).c_str());
	glSaveTexture(shadowColorTexture, fmt::format("smColor_{}.png", globalRendering->drawFrame).c_str());
}

void CShadowHandler::DrawFrustumDebugMiniMap() const
{
	if (!debugFrustum || !shadowsLoaded)
		return;

	CCamera* shadCam = CCameraHandler::GetCamera(CCamera::CAMTYPE_SHADOW);

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_0>();
	rb.AssertSubmission();

	const auto ntl = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NTL) };
	const auto ntr = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NTR) };
	const auto nbr = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NBR) };
	const auto nbl = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NBL) };

	const auto ftl = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FTL) };
	const auto ftr = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FTR) };
	const auto fbr = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FBR) };
	const auto fbl = VA_TYPE_0{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FBL) };

	rb.AddVertices({ nbl, nbr }); // NBL - NBR
	rb.AddVertices({ nbr, ntr }); // NBR - NTR
	rb.AddVertices({ ntr, ntl }); // NTR - NTL
	rb.AddVertices({ ntl, nbl }); // NTL - NBL

	rb.AddVertices({ ntl, ftl }); // NTL - FTL
	rb.AddVertices({ ntr, ftr }); // NTR - FTR
	rb.AddVertices({ nbl, fbl }); // NBL - FBL
	rb.AddVertices({ nbr, fbr }); // NBR - FBR

	rb.AddVertices({ fbl, fbr }); // FBL - FBR
	rb.AddVertices({ fbr, ftr }); // FBR - FTR
	rb.AddVertices({ ftr, ftl }); // FTR - FTL
	rb.AddVertices({ ftl, fbl }); // FTL - FBL

	auto& sh = rb.GetShader();
	glLineWidth(2.0f);
	sh.Enable();
	sh.SetUniform("ucolor", 0.0f, 0.0f, 1.0f, 1.0f);
	rb.DrawArrays(GL_LINES);
	sh.SetUniform("ucolor", 1.0f, 1.0f, 1.0f, 1.0f);
	sh.Disable();
	glLineWidth(1.0f);
}

namespace {
	enum {
		NBL = 1,
		FBL = 0,
		NBR = 3,
		FBR = 2,
		NTL = 5,
		FTL = 4,
		NTR = 7,
		FTR = 6,
	};
}

void CShadowHandler::DrawFrustumDebugMap() const
{
	if (!debugFrustum || !shadowsLoaded || !freezeFrustum)
		return;

	static constexpr SColor SHVOL_BEG_COL  = SColor{ 255, 255,   0, 255 };
	static constexpr SColor SHVOL_FIN_COL  = SColor{ 255,   0,   0, 255 };
	static constexpr SColor WORLD_BNDS_COL = SColor{   0,   0, 255, 255 };
	static constexpr SColor WORLD_PCAM_COL = SColor{ 255, 255, 255, 255 };
	static constexpr SColor CLIPPD_CAM_COL = SColor{   0, 255,   0, 255 };


	CCamera* shadCam = CCameraHandler::GetCamera(CCamera::CAMTYPE_SHADOW);
	{
		auto mat = shadCam->GetViewMatrixInverse();
		mat.Scale(32.0f);
		GL::shapes.DrawSolidSphere(10, 10, mat, float4{ 1, 0, 0, 1 });
	}

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	rb.AssertSubmission();
	auto& sh = rb.GetShader();

	// shadow cuboid unextended (yellow) and final (red)
	{
		auto ntl = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NTL), SHVOL_FIN_COL };
		auto ntr = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NTR), SHVOL_FIN_COL };
		auto nbr = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NBR), SHVOL_FIN_COL };
		auto nbl = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_NBL), SHVOL_FIN_COL };

		auto ftl = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FTL), SHVOL_FIN_COL };
		auto ftr = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FTR), SHVOL_FIN_COL };
		auto fbr = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FBR), SHVOL_FIN_COL };
		auto fbl = VA_TYPE_C{ shadCam->GetFrustumVert(CCamera::FRUSTUM_POINT_FBL), SHVOL_FIN_COL };

		rb.AddVertices({ nbl, nbr }); // NBL - NBR
		rb.AddVertices({ nbr, ntr }); // NBR - NTR
		rb.AddVertices({ ntr, ntl }); // NTR - NTL
		rb.AddVertices({ ntl, nbl }); // NTL - NBL

		rb.AddVertices({ ntl, ftl }); // NTL - FTL
		rb.AddVertices({ ntr, ftr }); // NTR - FTR
		rb.AddVertices({ nbl, fbl }); // NBL - FBL
		rb.AddVertices({ nbr, fbr }); // NBR - FBR

		rb.AddVertices({ fbl, fbr }); // FBL - FBR
		rb.AddVertices({ fbr, ftr }); // FBR - FTR
		rb.AddVertices({ ftr, ftl }); // FTR - FTL
		rb.AddVertices({ ftl, fbl }); // FTL - FBL

		glLineWidth(6.0f);
		sh.Enable();
		sh.SetUniform("ucolor", 1.0f, 1.0f, 1.0f, 1.0f);
		rb.DrawArrays(GL_LINES);
		sh.Disable();

		float3 extHeightVec = shadCam->forward * extraShadowCamHeight;
		ntl.pos -= extHeightVec;
		ntr.pos -= extHeightVec;
		nbr.pos -= extHeightVec;
		nbl.pos -= extHeightVec;

		ntl.c = SHVOL_BEG_COL;
		ntr.c = SHVOL_BEG_COL;
		nbr.c = SHVOL_BEG_COL;
		nbl.c = SHVOL_BEG_COL;

		rb.AddVertices({ nbl, nbr }); // NBL - NBR
		rb.AddVertices({ nbr, ntr }); // NBR - NTR
		rb.AddVertices({ ntr, ntl }); // NTR - NTL
		rb.AddVertices({ ntl, nbl }); // NTL - NBL

		glLineWidth(3.0f);
		sh.Enable();
		rb.DrawArrays(GL_LINES);
		sh.Disable();
	}

	// world bounds
	{
		const auto wcs = game->GetWorldBounds().GetCorners();

		const auto ntl = VA_TYPE_C{ wcs[NTL], WORLD_BNDS_COL };
		const auto ntr = VA_TYPE_C{ wcs[NTR], WORLD_BNDS_COL };
		const auto nbr = VA_TYPE_C{ wcs[NBR], WORLD_BNDS_COL };
		const auto nbl = VA_TYPE_C{ wcs[NBL], WORLD_BNDS_COL };

		const auto ftl = VA_TYPE_C{ wcs[FTL], WORLD_BNDS_COL };
		const auto ftr = VA_TYPE_C{ wcs[FTR], WORLD_BNDS_COL };
		const auto fbr = VA_TYPE_C{ wcs[FBR], WORLD_BNDS_COL };
		const auto fbl = VA_TYPE_C{ wcs[FBL], WORLD_BNDS_COL };

		rb.AddVertices({ nbl, nbr }); // NBL - NBR
		rb.AddVertices({ nbr, ntr }); // NBR - NTR
		rb.AddVertices({ ntr, ntl }); // NTR - NTL
		rb.AddVertices({ ntl, nbl }); // NTL - NBL

		rb.AddVertices({ ntl, ftl }); // NTL - FTL
		rb.AddVertices({ ntr, ftr }); // NTR - FTR
		rb.AddVertices({ nbl, fbl }); // NBL - FBL
		rb.AddVertices({ nbr, fbr }); // NBR - FBR

		rb.AddVertices({ fbl, fbr }); // FBL - FBR
		rb.AddVertices({ fbr, ftr }); // FBR - FTR
		rb.AddVertices({ ftr, ftl }); // FTR - FTL
		rb.AddVertices({ ftl, fbl }); // FTL - FBL

		glLineWidth(2.0f);
		sh.Enable();
		rb.DrawArrays(GL_LINES);
		sh.Disable();
	}

	// player's camera frustum
	{
		const auto ntl = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_NTL], WORLD_PCAM_COL };
		const auto ntr = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_NTR], WORLD_PCAM_COL };
		const auto nbr = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_NBR], WORLD_PCAM_COL };
		const auto nbl = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_NBL], WORLD_PCAM_COL };

		const auto ftl = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_FTL], WORLD_PCAM_COL };
		const auto ftr = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_FTR], WORLD_PCAM_COL };
		const auto fbr = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_FBR], WORLD_PCAM_COL };
		const auto fbl = VA_TYPE_C{ playCamFrustum[CCamera::FRUSTUM_POINT_FBL], WORLD_PCAM_COL };

		rb.AddVertices({ nbl, nbr }); // NBL - NBR
		rb.AddVertices({ nbr, ntr }); // NBR - NTR
		rb.AddVertices({ ntr, ntl }); // NTR - NTL
		rb.AddVertices({ ntl, nbl }); // NTL - NBL

		rb.AddVertices({ ntl, ftl }); // NTL - FTL
		rb.AddVertices({ ntr, ftr }); // NTR - FTR
		rb.AddVertices({ nbl, fbl }); // NBL - FBL
		rb.AddVertices({ nbr, fbr }); // NBR - FBR

		rb.AddVertices({ fbl, fbr }); // FBL - FBR
		rb.AddVertices({ fbr, ftr }); // FBR - FTR
		rb.AddVertices({ ftr, ftl }); // FTR - FTL
		rb.AddVertices({ ftl, fbl }); // FTL - FBL

		glLineWidth(2.0f);
		sh.Enable();
		rb.DrawArrays(GL_LINES);
		sh.Disable();
	}

	// Phase 1 intersection vertices (frustum ∩ worldBounds)
	{
		for (const auto& p : debugPhase1Points)
			rb.AddVertex({ p, CLIPPD_CAM_COL });

		glPointSize(6.0f);
		sh.Enable();
		rb.DrawArrays(GL_POINTS);
		sh.Disable();
		glPointSize(1.0f);
	}
}
void CShadowHandler::FreeFBOAndTextures() {
	if (shadowsFBO.IsValid()) {
		shadowsFBO.Bind();
		shadowsFBO.DetachAll();
		shadowsFBO.Unbind();
	}

	shadowsFBO.Kill();

	glDeleteTextures(1, &shadowDepthTexture); shadowDepthTexture = 0;
	glDeleteTextures(1, &shadowColorTexture); shadowColorTexture = 0;
}

void CShadowHandler::LoadShadowGenShaders()
{
	#define sh shaderHandler
	static const std::string shadowGenProgHandles[SHADOWGEN_PROGRAM_COUNT] = {
		"ShadowGenShaderProgModel",
		"ShadowGenShaderProgModelGL4",
		"ShadowGenshaderProgMap",
		"ShadowGenshaderProgProjectileOpaque",
	};
	static const std::string shadowGenProgDefines[SHADOWGEN_PROGRAM_COUNT] = {
		"#define SHADOWGEN_PROGRAM_MODEL\n",
		"#define SHADOWGEN_PROGRAM_MODEL_GL4\n",
		"#define SHADOWGEN_PROGRAM_MAP\n",
		"#define SHADOWGEN_PROGRAM_PROJ_OPAQ\n",
	};

	// #version has to be added here because it is conditional
	static const std::string versionDefs[3] = {
		"#version 130\n",
		"#version " + IntToString(globalRendering->supportFragDepthLayout? 420: 130) + "\n",
	};

	static const std::string extraDefs =
		("#define SUPPORT_CLIP_CONTROL " + IntToString(globalRendering->supportClipSpaceControl) + "\n") +
		("#define SUPPORT_DEPTH_LAYOUT " + IntToString(globalRendering->supportFragDepthLayout) + "\n");

	for (int i = 0; i < SHADOWGEN_PROGRAM_COUNT; i++) {
		if (i == SHADOWGEN_PROGRAM_MODEL_GL4)
			continue; //special path

		if (i == SHADOWGEN_PROGRAM_MAP)
			continue; //special path

		Shader::IProgramObject* po = sh->CreateProgramObject("[ShadowHandler]", shadowGenProgHandles[i] + "GLSL");

		po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenVertProg.glsl", versionDefs[0] + shadowGenProgDefines[i] + extraDefs, GL_VERTEX_SHADER));
		po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenFragProg.glsl", versionDefs[1] + shadowGenProgDefines[i] + extraDefs, GL_FRAGMENT_SHADER));

		po->Link();
		po->Enable();
		po->SetUniform("alphaMaskTex", 0);
		po->SetUniform("alphaParams", mapInfo->map.voidAlphaMin, 0.0f);
		po->Disable();
		po->Validate();

		if (!po->IsValid()) {
			po->RemoveShaderObject(GL_FRAGMENT_SHADER);
			po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenFragProg.glsl", versionDefs[0] + shadowGenProgDefines[i] + extraDefs, GL_FRAGMENT_SHADER));
			po->Link();
			po->Enable();
			po->SetUniform("alphaMaskTex", 0);
			po->SetUniform("alphaParams", mapInfo->map.voidAlphaMin, 0.0f);
			po->Disable();
			po->Validate();
		}

		shadowGenProgs[i] = po;
	}
	{
		Shader::IProgramObject* po = sh->CreateProgramObject("[ShadowHandler]", shadowGenProgHandles[SHADOWGEN_PROGRAM_MAP] + "GLSL");

		po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenVertMapProg.glsl", versionDefs[0] + shadowGenProgDefines[SHADOWGEN_PROGRAM_MAP] + extraDefs, GL_VERTEX_SHADER));
		po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenFragProg.glsl"   , versionDefs[1] + shadowGenProgDefines[SHADOWGEN_PROGRAM_MAP] + extraDefs, GL_FRAGMENT_SHADER));
		po->BindAttribLocation("vertexPos", 0);
		po->Link();
		po->Enable();
		po->SetUniform("alphaMaskTex", 0);
		po->SetUniform("heightMapTex", 1);
		po->SetUniform("alphaParams", mapInfo->map.voidAlphaMin, 0.0f);
		po->SetUniform("mapSize",
			static_cast<float>(mapDims.mapx * SQUARE_SIZE), static_cast<float>(mapDims.mapy * SQUARE_SIZE),
					   1.0f / (mapDims.mapx * SQUARE_SIZE),            1.0f / (mapDims.mapy * SQUARE_SIZE)
		);
		po->SetUniform("texSquare", 0, 0);
		po->Disable();
		po->Validate();

		if (!po->IsValid()) {
			po->RemoveShaderObject(GL_FRAGMENT_SHADER);
			po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenFragProg.glsl", versionDefs[0] + shadowGenProgDefines[SHADOWGEN_PROGRAM_MAP] + extraDefs, GL_FRAGMENT_SHADER));
			po->Link();
			po->Enable();
			po->SetUniform("alphaMaskTex", 0);
			po->SetUniform("heightMapTex", 1);
			po->SetUniform("alphaParams", mapInfo->map.voidAlphaMin, 0.0f);
			po->SetUniform("mapSize",
				static_cast<float>(mapDims.mapx * SQUARE_SIZE), static_cast<float>(mapDims.mapy * SQUARE_SIZE),
						   1.0f / (mapDims.mapx * SQUARE_SIZE),            1.0f / (mapDims.mapy * SQUARE_SIZE)
			);
			po->SetUniform("texSquare", 0, 0);
			po->Disable();
			po->Validate();
		}

		shadowGenProgs[SHADOWGEN_PROGRAM_MAP] = po;
	}
	if (globalRendering->haveGL4) {
		Shader::IProgramObject* po = sh->CreateProgramObject("[ShadowHandler]", shadowGenProgHandles[SHADOWGEN_PROGRAM_MODEL_GL4] + "GLSL");

		po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenVertProgGL4.glsl", shadowGenProgDefines[SHADOWGEN_PROGRAM_MODEL_GL4] + extraDefs, GL_VERTEX_SHADER));
		po->AttachShaderObject(sh->CreateShaderObject("GLSL/ShadowGenFragProgGL4.glsl", shadowGenProgDefines[SHADOWGEN_PROGRAM_MODEL_GL4] + extraDefs, GL_FRAGMENT_SHADER));
		po->Link();
		po->Enable();
		po->SetUniform("alphaCtrl", 0.5f, 1.0f, 0.0f, 0.0f); // test > 0.5
		po->Disable();
		po->Validate();

		shadowGenProgs[SHADOWGEN_PROGRAM_MODEL_GL4] = po;
	}

	shadowsLoaded = true;
	#undef sh
}



bool CShadowHandler::InitFBOAndTextures()
{
	//create dummy textures / FBO in case shadowConfig is 0
	const int realShTexSize = shadowConfig > 0 ? shadowMapSize : 1;

	// shadowsFBO is no-op constructed, has to be initialized manually
	shadowsFBO.Init(false);

	if (!shadowsFBO.IsValid()) {
		LOG_L(L_ERROR, "[%s] framebuffer not valid", __func__);
		return false;
	}

	// TODO: add bit depth?
	static constexpr struct {
		GLint clampMode;
		GLint filterMode;
		const char* name;
	} presets[] = {
		{GL_CLAMP_TO_BORDER, GL_LINEAR , "SHADOW-BEST"  },
		{GL_CLAMP_TO_EDGE  , GL_NEAREST, "SHADOW-COMPAT"},
	};

	static constexpr float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	bool status = false;
	for (const auto& preset : presets)
	{
		if (FBO::GetCurrentBoundFBO() == shadowsFBO.GetId())
			shadowsFBO.DetachAll();

		//depth
		glDeleteTextures(1, &shadowDepthTexture);
		glGenTextures(1, &shadowDepthTexture);
		glBindTexture(GL_TEXTURE_2D, shadowDepthTexture);

		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, one);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, preset.clampMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, preset.clampMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, preset.filterMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, preset.filterMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); //no mips

		const int depthBits = std::min(globalRendering->supportDepthBufferBitDepth, 24);
		const GLint depthFormat = CGlobalRendering::DepthBitsToFormat(depthBits);

		glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
		glTexImage2D(GL_TEXTURE_2D, 0, depthFormat, realShTexSize, realShTexSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		glBindTexture(GL_TEXTURE_2D, 0);

		/// color
		glDeleteTextures(1, &shadowColorTexture);
		glGenTextures(1, &shadowColorTexture);
		glBindTexture(GL_TEXTURE_2D, shadowColorTexture);

		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, one);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, preset.clampMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, preset.clampMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, preset.filterMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, preset.filterMode);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); //no mips
		// TODO: Figure out if mips make sense here.

		// seems like GL_RGB8 has enough precision
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, realShTexSize, realShTexSize, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		static constexpr GLint swizzleMask[] = { GL_RED, GL_GREEN, GL_BLUE, GL_ONE };
		glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);

		glBindTexture(GL_TEXTURE_2D, 0);

		// Mesa complains about an incomplete FBO if calling Bind before TexImage (?)
		shadowsFBO.Bind();
		shadowsFBO.AttachTexture(shadowDepthTexture, GL_TEXTURE_2D, GL_DEPTH_ATTACHMENT);
		shadowsFBO.AttachTexture(shadowColorTexture, GL_TEXTURE_2D, GL_COLOR_ATTACHMENT0);

		glDrawBuffer(GL_COLOR_ATTACHMENT0);
		glReadBuffer(GL_COLOR_ATTACHMENT0);

		// test the FBO
		status = shadowsFBO.CheckStatus(preset.name);

		if (status) //exit on the first occasion
			break;
	}

	glClearDepth(1.0f);
	glClear(GL_DEPTH_BUFFER_BIT);
	EnableColorOutput(true);
	glClearColor(1.0f, 1.0f, 1.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	shadowsFBO.Unbind();

	// revert to FBO = 0 default
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	return status;
}

void CShadowHandler::DrawShadowPasses()
{
	inShadowPass = true;

	using namespace GL::State;
	auto state = GL::SubState(
		Blending(GL_FALSE),
		Lighting(GL_FALSE),
		TexTarget<GL_TEXTURE_2D>(GL_FALSE),
		ShadeModel(GL_FLAT),
		DepthMask(GL_TRUE),
		DepthTest(GL_TRUE),
		CullFace(GL_BACK),
		Culling(GL_TRUE),
		PolygonOffsetFill(GL_TRUE)
	);

	EnableColorOutput(false);

	eventHandler.DrawWorldShadow();

	{
		auto polyOffset = GL::SubState(
			PolygonOffset(objPolygonOffsetScale, objPolygonOffsetUnits) // factor * DZ + r * units
		);

		if ((shadowGenBits & SHADOWGEN_BIT_TREE) != 0) {
			grassDrawer->DrawShadow();
		}

		if ((shadowGenBits & SHADOWGEN_BIT_PROJ) != 0) {
			projectileDrawer->DrawShadowOpaque();
		}
		if ((shadowGenBits & SHADOWGEN_BIT_MODEL) != 0) {
			unitDrawer->DrawShadowPass();
			featureDrawer->DrawShadowPass();
		}
	}

	// cull front-faces during the terrain shadow pass: sun direction
	// can be set so oblique that geometry back-faces are visible (eg.
	// from hills near map edges) from its POV
	//
	// not the best idea, causes acne when projecting the shadow-map
	// (rasterizing back-faces writes different depth values) and is
	// no longer required since border geometry will fully hide them
	// (could just disable culling of terrain faces entirely, but we
	// also want to prevent overdraw in low-angle passes)
	// glCullFace(GL_FRONT);

	{
		auto polyOffset = GL::SubState(
			CullFace(GL_FRONT),
			PolygonOffset(mapPolygonOffsetScale, mapPolygonOffsetUnits) // factor * DZ + r * units
		);

		if ((shadowGenBits & SHADOWGEN_BIT_MAP) != 0) {
			ZoneScopedN("Draw::World::CreateShadows::Terrain");
			readMap->GetGroundDrawer()->DrawShadowPass();
		}
	}

	//transparent pass, comes last
	{
		auto polyOffset = GL::SubState(
			PolygonOffset(1.0f, 1.0f) // factor * DZ + r * units
		);

		if ((shadowGenBits & SHADOWGEN_BIT_PROJ) != 0) {
			projectileDrawer->DrawShadowTransparent();
			eventHandler.DrawShadowPassTransparent();
		}
	}

	inShadowPass = false;
}

namespace Impl {
	constexpr float EPS = 1e-5f;

	// 12 frustum edges as index pairs into Frustum::verts (NBL=0, NBR=1, NTR=2, NTL=3, FBL=4, FBR=5, FTR=6, FTL=7)
	constexpr std::array<std::pair<uint32_t, uint32_t>, 12> kFrustumEdges = {{
		// near face
		{CCamera::FRUSTUM_POINT_NBL, CCamera::FRUSTUM_POINT_NBR},
		{CCamera::FRUSTUM_POINT_NBR, CCamera::FRUSTUM_POINT_NTR},
		{CCamera::FRUSTUM_POINT_NTR, CCamera::FRUSTUM_POINT_NTL},
		{CCamera::FRUSTUM_POINT_NTL, CCamera::FRUSTUM_POINT_NBL},
		// far face
		{CCamera::FRUSTUM_POINT_FBL, CCamera::FRUSTUM_POINT_FBR},
		{CCamera::FRUSTUM_POINT_FBR, CCamera::FRUSTUM_POINT_FTR},
		{CCamera::FRUSTUM_POINT_FTR, CCamera::FRUSTUM_POINT_FTL},
		{CCamera::FRUSTUM_POINT_FTL, CCamera::FRUSTUM_POINT_FBL},
		// connecting
		{CCamera::FRUSTUM_POINT_NBL, CCamera::FRUSTUM_POINT_FBL},
		{CCamera::FRUSTUM_POINT_NBR, CCamera::FRUSTUM_POINT_FBR},
		{CCamera::FRUSTUM_POINT_NTR, CCamera::FRUSTUM_POINT_FTR},
		{CCamera::FRUSTUM_POINT_NTL, CCamera::FRUSTUM_POINT_FTL},
	}};

	// 12 AABB edges as index pairs into AABB::CalcCorners ordering
	// [0]=(mins.x,mins.y,mins.z) [1]=(mins.x,mins.y,maxs.z) [2]=(maxs.x,mins.y,mins.z) [3]=(maxs.x,mins.y,maxs.z)
	// [4]=(mins.x,maxs.y,mins.z) [5]=(mins.x,maxs.y,maxs.z) [6]=(maxs.x,maxs.y,mins.z) [7]=(maxs.x,maxs.y,maxs.z)
	constexpr std::array<std::pair<uint32_t, uint32_t>, 12> kAABBEdges = {{
		// X-axis
		{0, 2}, {1, 3}, {4, 6}, {5, 7},
		// Y-axis
		{0, 4}, {1, 5}, {2, 6}, {3, 7},
		// Z-axis
		{0, 1}, {2, 3}, {4, 5}, {6, 7},
	}};

	bool AABBContainsEps(const AABB& box, const float3& p) {
		return p.x >= box.mins.x - EPS && p.x <= box.maxs.x + EPS
		    && p.y >= box.mins.y - EPS && p.y <= box.maxs.y + EPS
		    && p.z >= box.mins.z - EPS && p.z <= box.maxs.z + EPS;
	}

	bool PointInsidePlanes(const float3& p, const float4* planes, int count) {
		for (int i = 0; i < count; ++i) {
			if (float3(planes[i]).dot(p) + planes[i].w < -EPS)
				return false;
		}
		return true;
	}

	// Collect intersection points of segment [a,b] with the 6 axis-aligned faces of an AABB
	void CollectSegmentAABBFaceHits(const float3& a, const float3& b, const AABB& box, std::vector<float3>& out) {
		const float faceVals[6] = { box.mins.x, box.maxs.x, box.mins.y, box.maxs.y, box.mins.z, box.maxs.z };
		const int   faceAxis[6] = { 0, 0, 1, 1, 2, 2 };

		for (int f = 0; f < 6; ++f) {
			const int axis = faceAxis[f];
			const float denom = b[axis] - a[axis];
			if (math::fabs(denom) < EPS)
				continue;

			const float t = (faceVals[f] - a[axis]) / denom;
			if (t < -EPS || t > 1.0f + EPS)
				continue;

			const float3 hit = a + (b - a) * t;

			// check the other 2 axes are within the face rectangle
			const int a1 = (axis + 1) % 3;
			const int a2 = (axis + 2) % 3;
			if (hit[a1] >= box.mins[a1] - EPS && hit[a1] <= box.maxs[a1] + EPS &&
			    hit[a2] >= box.mins[a2] - EPS && hit[a2] <= box.maxs[a2] + EPS)
			{
				out.push_back(hit);
			}
		}
	}

	// Collect intersection points of segment [a,b] with arbitrary planes, validated by a callback
	template<typename ValidateFn>
	void CollectSegmentPlaneHits(const float3& a, const float3& b, const float4* planes, int count, ValidateFn&& validate, std::vector<float3>& out) {
		const float3 ab = b - a;

		for (int i = 0; i < count; ++i) {
			const float3 n(planes[i]);
			const float d = planes[i].w;
			const float nDotAB = n.dot(ab);

			if (math::fabs(nDotAB) < EPS)
				continue;

			const float t = -(n.dot(a) + d) / nDotAB;
			if (t < -EPS || t > 1.0f + EPS)
				continue;

			const float3 hit = a + ab * t;
			if (validate(hit))
				out.push_back(hit);
		}
	}

	// Collect intersection points of 4 infinite tube edge lines with the 6 AABB faces
	void CollectTubeEdgeAABBFaceHits(const AABB& tubeAABB, const CMatrix44f& viewMatrixInv, const AABB& worldBounds, std::vector<float3>& out) {
		const float3 dir = viewMatrixInv.GetZ(); // light Z-axis in world space
		const float xCorners[2] = { tubeAABB.mins.x, tubeAABB.maxs.x };
		const float yCorners[2] = { tubeAABB.mins.y, tubeAABB.maxs.y };

		const float faceVals[6] = { worldBounds.mins.x, worldBounds.maxs.x, worldBounds.mins.y, worldBounds.maxs.y, worldBounds.mins.z, worldBounds.maxs.z };
		const int   faceAxis[6] = { 0, 0, 1, 1, 2, 2 };

		for (int xi = 0; xi < 2; ++xi) {
			for (int yi = 0; yi < 2; ++yi) {
				const float3 origin = viewMatrixInv * float3(xCorners[xi], yCorners[yi], 0.0f);

				for (int f = 0; f < 6; ++f) {
					const int axis = faceAxis[f];
					if (math::fabs(dir[axis]) < EPS)
						continue;

					const float t = (faceVals[f] - origin[axis]) / dir[axis];
					const float3 hit = origin + dir * t;

					// check the other 2 axes are within the face rectangle
					const int a1 = (axis + 1) % 3;
					const int a2 = (axis + 2) % 3;
					if (hit[a1] >= worldBounds.mins[a1] - EPS && hit[a1] <= worldBounds.maxs[a1] + EPS &&
					    hit[a2] >= worldBounds.mins[a2] - EPS && hit[a2] <= worldBounds.maxs[a2] + EPS)
					{
						out.push_back(hit);
					}
				}
			}
		}
	}

	float3 GetLightXDir(const CCamera* playerCam, const float3 toLightDir)
	{
#if 0
		// align with the world's X axis
		float3 xAxis = float3(1, 0, 0);
		return (xAxis - xAxis.dot(toLightDir) * toLightDir).Normalize();
#else
		// Try to rotate LM's X and Y around Z direction to fit playerCam tightest
		// find the most orthogonal vector to zDir and call it xDir
		float minDot = 1.0f;
		float3 xDir;
		for (const auto* dir : { &playerCam->forward, &playerCam->right, &playerCam->up }) {
			const float dp = toLightDir.dot(*dir);
			if (math::fabs(dp) < minDot) {
				xDir = *dir;
				minDot = math::fabs(dp);
			}
		}

		xDir = (xDir - xDir.dot(toLightDir) * toLightDir).Normalize();

		return xDir;
#endif
	}
};

void CShadowHandler::CalcShadowMatrices(CCamera* playerCam, CCamera* shadowCam)
{
	SCOPED_TIMER("CShadowHandler::CalcShadowMatrices");

	// save the player's camera frustum verts in case we need them in CShadowHandler::DrawFrustumDebugMap()
	playCamFrustum = playerCam->GetFrustum().verts;

	const auto& worldBounds = game->GetWorldBounds();
	const auto& frustum = playerCam->GetFrustum();
	const auto aabbCorners = worldBounds.GetCorners();

	// Collect all vertices of the frustum ∩ worldBounds intersection polytope
	std::vector<float3> polytopeVerts;
	polytopeVerts.reserve(64);

	// 1) Frustum corners inside AABB
	for (const auto& v : frustum.verts)
		if (Impl::AABBContainsEps(worldBounds, v))
			polytopeVerts.push_back(v);

	const size_t nStep1 = polytopeVerts.size();

	// 2) AABB corners inside frustum
	for (const auto& c : aabbCorners)
		if (Impl::PointInsidePlanes(c, frustum.planes.data(), frustum.planes.size()))
			polytopeVerts.push_back(c);

	const size_t nStep2 = polytopeVerts.size() - nStep1;

	// 3) 12 frustum edges × 6 AABB faces
	for (const auto& [i, j] : Impl::kFrustumEdges)
		Impl::CollectSegmentAABBFaceHits(frustum.verts[i], frustum.verts[j], worldBounds, polytopeVerts);

	const size_t nStep3 = polytopeVerts.size() - nStep1 - nStep2;

	// 4) 12 AABB edges × 6 frustum planes
	{
		auto frustumValidate = [&](const float3& p) {
			return Impl::AABBContainsEps(worldBounds, p) && Impl::PointInsidePlanes(p, frustum.planes.data(), frustum.planes.size());
		};

		// Debug: log each hit from step 4
		for (const auto& [i, j] : Impl::kAABBEdges) {
			const size_t before = polytopeVerts.size();
			Impl::CollectSegmentPlaneHits(aabbCorners[i], aabbCorners[j], frustum.planes.data(), frustum.planes.size(), frustumValidate, polytopeVerts);
			if (debugFrustum && polytopeVerts.size() > before) {
				for (size_t k = before; k < polytopeVerts.size(); ++k) {
					const auto& h = polytopeVerts[k];
					// Check which plane each hit is closest to (signed distance ~ 0)
					for (size_t pi = 0; pi < frustum.planes.size(); ++pi) {
						const float3 n(frustum.planes[pi]);
						float sd = n.dot(h) + frustum.planes[pi].w;
						if (math::fabs(sd) < 0.1f)
							LOG("  step4 hit (%.3f,%.3f,%.3f) edge[%u,%u] onPlane[%zu] sd=%.8f", h.x, h.y, h.z, i, j, pi, sd);
					}
					// Also log distance to each AABB face
					LOG("  step4 hit AABB margins: xlo=%.6f xhi=%.6f ylo=%.6f yhi=%.6f zlo=%.6f zhi=%.6f",
						h.x - worldBounds.mins.x, worldBounds.maxs.x - h.x,
						h.y - worldBounds.mins.y, worldBounds.maxs.y - h.y,
						h.z - worldBounds.mins.z, worldBounds.maxs.z - h.z);
				}
			}
		}
	}

	const size_t nStep4 = polytopeVerts.size() - nStep1 - nStep2 - nStep3;

	if (debugFrustum) {
		LOG("Phase1 steps: frustCorners=%zu aabbCorners=%zu frustEdge×aabbFace=%zu aabbEdge×frustPlane=%zu total=%zu",
			nStep1, nStep2, nStep3, nStep4, polytopeVerts.size());
		LOG("  worldBounds mins=(%.6f, %.6f, %.6f) maxs=(%.6f, %.6f, %.6f)",
			worldBounds.mins.x, worldBounds.mins.y, worldBounds.mins.z,
			worldBounds.maxs.x, worldBounds.maxs.y, worldBounds.maxs.z);
		LOG("  frustum near=%.3f far=%.3f", frustum.scales.z, frustum.scales.w);
		// Log frustum planes at high precision
		for (size_t pi = 0; pi < frustum.planes.size(); ++pi) {
			const auto& p = frustum.planes[pi];
			LOG("  frustPlane[%zu]=(%.10f, %.10f, %.10f, %.10f)", pi, p.x, p.y, p.z, p.w);
		}
	}

	debugPhase1Points = polytopeVerts; // for debug drawing

	// Early out: if camera doesn't intersect the map at all, skip shadow rendering
	if (polytopeVerts.empty())
		return;

	// construct Camera World Matrix & View Matrix
	CMatrix44f viewMatrixInv;
	{
		// Build orthogonal basis aligned with light direction (zAxis)
		float3 zAxis = float3{ ISky::GetSky()->GetLight()->GetLightDir().xyz };
		float3 xAxis = Impl::GetLightXDir(playerCam, zAxis);
		float3 yAxis = zAxis.cross(xAxis);

		viewMatrixInv.SetX(xAxis);
		viewMatrixInv.SetY(yAxis);
		viewMatrixInv.SetZ(zAxis);

		// convert camera "world" matrix into camera view matrix
		// https://www.3dgep.com/understanding-the-view-matrix/
		// note viewMatrixInv has default (0,0,0) position here, it's defined later
		viewMatrix = viewMatrixInv.InvertAffine();
	}

	// Project polytope vertices to light space
	lightAABB.Reset();
	for (const auto& p : polytopeVerts)
		lightAABB.AddPoint(viewMatrix * p);

	float3 lsMidPos = lightAABB.CalcCenter();
	float3 lsDims = lightAABB.CalcScales();

	// Position shadow camera at the far end of the bounding box, looking toward the scene
	viewMatrix.SetPos(-(lsMidPos + float3{ 0.0f, 0.0f, lsDims.z }));
	viewMatrixInv.Translate(-viewMatrix.GetPos());

	// Adjust AABB to account for new camera position
	lightAABB.mins += viewMatrix.GetPos();
	lightAABB.maxs += viewMatrix.GetPos();

	// The frustum∩AABB polytope above gives tight XY bounds but may miss geometry behind
	// the camera's near plane that still casts shadows into the visible area.
	// Find extraShadowCamHeight by intersecting the worldBounds AABB with the infinite
	// rectangular "tube" defined by lightAABB's XY extent along the light direction.
	extraShadowCamHeight = 0.0f;
	{
		assert(viewMatrix[3] == 0.0f && viewMatrix[7] == 0.0f
		    && viewMatrix[11] == 0.0f && viewMatrix[15] == 1.0f);

		// Extract viewMatrix rows (column-major: col[0].x, col[1].x, col[2].x = row 0)
		const float3 row0(viewMatrix[0], viewMatrix[4], viewMatrix[ 8]);
		const float3 row1(viewMatrix[1], viewMatrix[5], viewMatrix[ 9]);
		const float tx = viewMatrix[12], ty = viewMatrix[13];

		// 4 world-space half-planes forming the tube:
		//   (viewMatrix * p).x >= XLmin  ⟺  row0·p + tx >= XLmin  ⟺  row0·p + (tx - XLmin) >= 0
		const float4 tubePlanes[4] = {
			float4( row0,  tx - lightAABB.mins.x),  // x >= mins.x
			float4(-row0, -tx + lightAABB.maxs.x),  // x <= maxs.x
			float4( row1,  ty - lightAABB.mins.y),  // y >= mins.y
			float4(-row1, -ty + lightAABB.maxs.y),  // y <= maxs.y
		};

		auto tubeValidate = [&](const float3& p) {
			return Impl::AABBContainsEps(worldBounds, p) && Impl::PointInsidePlanes(p, tubePlanes, 4);
		};

		std::vector<float3> tubeVerts;
		tubeVerts.reserve(32);

		// 1) AABB corners inside tube
		for (const auto& c : aabbCorners)
			if (Impl::PointInsidePlanes(c, tubePlanes, 4))
				tubeVerts.push_back(c);

		// 2) 12 AABB edges × 4 tube planes
		for (const auto& [i, j] : Impl::kAABBEdges)
			Impl::CollectSegmentPlaneHits(aabbCorners[i], aabbCorners[j], tubePlanes, 4, tubeValidate, tubeVerts);

		// 3) 4 tube edge lines × 6 AABB faces
		Impl::CollectTubeEdgeAABBFaceHits(lightAABB, viewMatrixInv, worldBounds, tubeVerts);

		// Find max light-space Z among collected vertices
		// (maxZ is correct: camera at Z=0 looking down -Z, positive Z = behind camera toward light)
		float maxZ = std::numeric_limits<float>::lowest();
		for (const auto& v : tubeVerts)
			maxZ = std::max(maxZ, (viewMatrix * v).z);

		if (maxZ > 0.0f)
			extraShadowCamHeight = maxZ;

		if (debugFrustum) {
			LOG("Phase1: %zu verts, lightAABB X[%.1f, %.1f] Y[%.1f, %.1f] Z[%.1f, %.1f]",
				polytopeVerts.size(),
				lightAABB.mins.x, lightAABB.maxs.x,
				lightAABB.mins.y, lightAABB.maxs.y,
				lightAABB.mins.z, lightAABB.maxs.z);
			LOG("Phase2: %zu tubeVerts, maxZ=%.3f, extraShadowCamHeight=%.3f",
				tubeVerts.size(), maxZ, extraShadowCamHeight);
		}
	}

	// shift camera further away to account for the calculation above
	viewMatrix.col[3].z -= extraShadowCamHeight;
	// viewMatrixInv is local and no longer needed, so we don't care to update it with the change above

	// translate mins.z accordingly
	lightAABB.mins.z -= extraShadowCamHeight;
	// Make sure maxs.z is at the camera position
	lightAABB.maxs.z = 0.0f; // @ camPos

	// Negated Z values because OpenGL convention has camera looking down -Z axis
	projMatrix = CMatrix44f::ClipOrthoProj(
		 lightAABB.mins.x,  lightAABB.maxs.x,
		 lightAABB.mins.y,  lightAABB.maxs.y,
		-lightAABB.maxs.z, -lightAABB.mins.z,
		 globalRendering->supportClipSpaceControl
	);

	// doesn't seem to do much if anything effectively
	#if 0
	{
		// Texel snapping to prevent shadow shimmering
		const float texelScale = shadowMapSize * 0.5f;
		const float invTexelScale = 2.0f / shadowMapSize;

		CMatrix44f shadowMatrix = projMatrix * viewMatrix;
		float4 shadowOrigin = shadowMatrix.col[3];

		float offsetX = (std::roundf(shadowOrigin.x * texelScale) - shadowOrigin.x * texelScale) * invTexelScale;
		float offsetY = (std::roundf(shadowOrigin.y * texelScale) - shadowOrigin.y * texelScale) * invTexelScale;

		projMatrix.col[3][0] += offsetX;
		projMatrix.col[3][1] += offsetY;
	}
	#endif

	viewProjMatrix = projMatrix * viewMatrix;
}

void CShadowHandler::SetShadowCamera(CCamera* shadowCam)
{
	const int realShTexSize = shadowConfig > 0 ? shadowMapSize : 1;

	// first set matrices needed by shaders (including ShadowGenVertProg)
	shadowCam->SetProjMatrix(projMatrix);
	shadowCam->SetViewMatrix(viewMatrix);
	shadowCam->UpdateDerivedMatrices();

	// scales are in a space relative to the camera position and along worldspace camera's principal vectors
	// while lightAABB is in camera view space, so need to use relative (max - min) values
	float4 shadowProjScales{
		lightAABB.maxs.x - lightAABB.mins.x,
		lightAABB.maxs.y - lightAABB.mins.y,
		0.0f,
		-(lightAABB.maxs.z - lightAABB.mins.z) // shadowCam->forward is looking towards the light, so make far plane negative
	};

	// convert xy-length to half-length
	shadowCam->SetFrustumScales(shadowProjScales * float4(0.5f, 0.5f, 1.0f, 1.0f));
	shadowCam->UpdateFrustum();
	shadowCam->UpdateLoadViewport(0, 0, realShTexSize, realShTexSize);

	// load matrices into gl_{ModelView,Projection}Matrix
	shadowCam->Update({ false, false, false, false, false });

	shadowCam->SetAspectRatio(shadowProjScales.x / shadowProjScales.y);
}

void CShadowHandler::SetupShadowTexSampler(unsigned int texUnit, bool enable) const
{
	glActiveTexture(texUnit);
	glBindTexture(GL_TEXTURE_2D, shadowDepthTexture);

	// support FFP context
	if (enable)
		glEnable(GL_TEXTURE_2D);

	SetupShadowTexSamplerRaw();
}

void CShadowHandler::SetupShadowTexSamplerRaw() const
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
	// glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_INTENSITY);
	// glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_ALPHA);
}

void CShadowHandler::ResetShadowTexSampler(unsigned int texUnit, bool disable) const
{
	glActiveTexture(texUnit);
	glBindTexture(GL_TEXTURE_2D, 0);

	if (disable)
		glDisable(GL_TEXTURE_2D);

	ResetShadowTexSamplerRaw();
}

void CShadowHandler::ResetShadowTexSamplerRaw() const
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
}


void CShadowHandler::CreateShadows()
{
	// NOTE:
	//   we unbind later in WorldDrawer::GenerateIBLTextures() to save render
	//   context switches (which are one of the slowest OpenGL operations!)
	//   together with VP restoration
	shadowsFBO.Bind();

	glClearColor(1.0f, 1.0f, 1.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	CCamera* prvCam = CCameraHandler::GetSetActiveCamera(CCamera::CAMTYPE_SHADOW);
	SetShadowCamera(camera); // shadowCam here

	if (ISky::GetSky()->GetLight()->GetLightIntensity() > 0.0f)
		DrawShadowPasses();

	CCameraHandler::SetActiveCamera(prvCam->GetCamType());
	prvCam->Update();


	glShadeModel(GL_SMOOTH);

	//revert to default, EnableColorOutput(true) is not enough
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void CShadowHandler::EnableColorOutput(bool enable) const
{
	assert(FBO::GetCurrentBoundFBO() == shadowsFBO.GetId());

	const GLboolean b = static_cast<GLboolean>(enable);
	glColorMask(b, b, b, GL_FALSE);
}