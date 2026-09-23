#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshBuildVersion.h"

namespace Durin
{
	class FMeshBuilderModule final : public IMeshBuilderModule
	{
		// Build sessions retain code leases until their work and results are released.
		auto SupportsDynamicReloading() const -> bool override { return true; }

		auto GetRenderBuilderVersion() const -> uint32 override
		{
			return StaticMeshBuilderVersion;
		}

		auto BuildRender(const FStaticMeshRenderBuildRequest& Request,
			const FAssetBuildTaskContext& Control)
			-> std::expected<FStaticMeshRenderBuildProduct, FStaticMeshRenderBuildError> override
		{
			return FStaticMeshBuilder::Build(Request, Control);
		}
	};

	IMPLEMENT_MODULE(FMeshBuilderModule, MeshBuilder)
}
