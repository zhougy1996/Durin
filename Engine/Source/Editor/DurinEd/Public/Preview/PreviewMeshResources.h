#pragma once

#include <expected>

#include "Asset/AssetRetention.h"

namespace Durin
{
	class DStaticMesh;
}

namespace Durin::Editor
{
	// Editor-session owner of the fixed material preview meshes. Consumer retention
	// handles share these objects, so closing a preview cannot evict their resources.
	class FPreviewMeshResources
	{
	public:
		static constexpr std::string_view SphereAssetPath = "/Engine/Models/Sphere.Sphere";
		static constexpr std::string_view BoxAssetPath = "/Engine/Models/Box.Box";

		FPreviewMeshResources() = default;
		FPreviewMeshResources(const FPreviewMeshResources&) = delete;
		auto operator=(const FPreviewMeshResources&) -> FPreviewMeshResources& = delete;

		// GameThread startup only: finishes mesh compilation/loading and, when RHI is
		// available, resource initialization. Repeated calls do no work until Reset.
		// A partial failure preserves successfully loaded meshes and reports a diagnostic.
		DURINED_API auto Initialize() -> std::expected<void, std::string>;
		// Call after preview consumers retire, before renderer/module shutdown.
		DURINED_API auto Reset() -> void;
		DURINED_API auto GetSphere() const -> DStaticMesh*;
		DURINED_API auto GetBox() const -> DStaticMesh*;

	private:
		std::array<FRetainedAsset, 2> Meshes;
		bool bInitialized = false;
		std::string Diagnostic;
	};
}
