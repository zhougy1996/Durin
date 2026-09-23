#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshBuildVersion.h"

namespace Durin
{
	class FMeshBuilderModule final : public IMeshBuilderModule
	{
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
