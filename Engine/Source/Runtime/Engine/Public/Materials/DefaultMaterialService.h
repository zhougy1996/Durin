#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "DObject/StrongObjectPtr.h"
#include "Materials/MaterialRenderProxy.h"

namespace Durin
{
	class DMaterialInterface;

	class DMaterialInstance;
	struct FViewEnvironmentOverride;

	struct FCubeSkyViewEnvironment
	{
		FMaterialRenderProxyRef Material;
		FRHITextureReferenceRef TextureReference;
		FQuat Rotation;
	};

	// Game-thread capture owner. Render submissions retain only the published proxy and texture reference.
	class FCubeSkyCaptureMaterial
	{
	public:
		ENGINE_API static auto Create(const FViewEnvironmentOverride& Input)
			-> std::expected<std::unique_ptr<FCubeSkyCaptureMaterial>, std::string>;
		ENGINE_API ~FCubeSkyCaptureMaterial();
		FCubeSkyCaptureMaterial(const FCubeSkyCaptureMaterial&) = delete;
		auto operator=(const FCubeSkyCaptureMaterial&) -> FCubeSkyCaptureMaterial& = delete;
		auto GetEnvironment() const -> const std::shared_ptr<const FCubeSkyViewEnvironment>& { return Environment; }

	private:
		FCubeSkyCaptureMaterial() = default;
		TStrongObjectPtr<DMaterialInstance> Instance;
		std::shared_ptr<const FCubeSkyViewEnvironment> Environment;
	};

	ENGINE_API auto RequestProceduralSkyMaterialProxy_RenderThread() -> FMaterialRenderProxyRef;
	ENGINE_API auto GetDefaultMaterial() -> DMaterialInterface*;
	inline constexpr std::string_view DefaultMaterialPackagePath =
		"/Engine/Materials/DefaultMaterial";
	inline constexpr std::string_view DefaultMaterialObjectPath =
		"/Engine/Materials/DefaultMaterial.DefaultMaterial";

	// Engine-owned lifetime boundary for the authored default material. The
	// neutral fallback is prepared at startup; an empty fallback proxy means initialization
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
