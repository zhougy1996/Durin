#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"

#include <span>
#include <string_view>

namespace Durin
{
	inline constexpr std::string_view DefaultMaterialAssetPath =
		"/Engine/Materials/DefaultMaterial";

	// Engine-owned lifetime boundary for the authored default material. The
	// service never performs lazy loads; an empty proxy means initialization
	// failed or shutdown has begun and callers must select ErrorMaterial.
	ENGINE_API auto InitializeDefaultMaterialService() -> bool;
	ENGINE_API auto ShutdownDefaultMaterialService() -> void;
	ENGINE_API auto GetDefaultMaterialRenderProxy() -> FMaterialRenderProxyRef;
	ENGINE_API auto IsDefaultMaterialServiceAvailable() -> bool;

	// Fixed Engine content roots which a cook front end must include even when
	// no project package serializes a reference to them.
	ENGINE_API auto GetEngineBuiltInCookRoots()
		-> std::span<const std::string_view>;
}
