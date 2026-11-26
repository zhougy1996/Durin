#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"

namespace Durin
{
	class DMaterialInterface;

	struct FDefaultCubeSkyBindings
	{
		static constexpr const char* TextureName = "SkyTexture";
		static constexpr const char* TintName = "SkyTint";
		static constexpr const char* IntensityName = "SkyIntensity";

		uint16 TextureIndex;
		uint32 TintOffset;
		uint32 IntensityOffset;
	};

	auto RefreshDefaultCubeSkyBindings_GameThread(const DMaterialInterface& Material, const FMaterialProgram* Program) -> void;
	ENGINE_API auto GetDefaultCubeSkyBindings_RenderThread(const FMaterialRenderLayoutIdentity& Layout)
		-> const FDefaultCubeSkyBindings*;
	ENGINE_API auto GetDefaultSkyMaterialProxy_RenderThread(bool bProcedural) -> FMaterialRenderProxyRef;
	ENGINE_API auto GetDefaultCubeSkyMaterial() -> DMaterialInterface*;
	ENGINE_API auto GetDefaultMaterial() -> DMaterialInterface*;
	inline constexpr std::string_view DefaultMaterialPackagePath =
		"/Engine/Materials/DefaultMaterial";
	inline constexpr std::string_view DefaultMaterialObjectPath =
		"/Engine/Materials/DefaultMaterial.DefaultMaterial";

	// Engine-owned lifetime boundary for the authored default material. The
	// service never performs lazy loads; an empty proxy means initialization
	// failed or shutdown has begun and callers must select ErrorMaterial.
	ENGINE_API auto InitializeDefaultMaterialService() -> bool;
	ENGINE_API auto ShutdownDefaultMaterialService() -> void;
	ENGINE_API auto GetDefaultMaterialRenderProxy() -> FMaterialRenderProxyRef;
	auto GetDefaultMaterialRenderProxy_RenderThread() -> FMaterialRenderProxy*;
	ENGINE_API auto IsDefaultMaterialServiceAvailable() -> bool;

	// Fixed Engine content roots which a cook front end must include even when
	// no project package serializes a reference to them.
	ENGINE_API auto GetEngineBuiltInCookRoots()
		-> std::span<const std::string_view>;
}
