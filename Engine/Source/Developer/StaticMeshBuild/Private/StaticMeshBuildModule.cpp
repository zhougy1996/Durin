#include "StaticMesh/IStaticMeshBuildModule.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshBuildVersion.h"

namespace Durin
{
	class FStaticMeshBuildModule final : public IStaticMeshBuildModule
	{
		auto GetDescriptor() const -> FStaticMeshBuilderDescriptor override
		{
			return {.ProducerIdentity = "Durin.StaticMeshBuild", .RenderBuilderVersion = StaticMeshBuilderVersion};
		}

		auto BuildRender(const FStaticMeshRenderBuildRequest& Request,
			const FAssetBuildTaskContext& Control)
			-> std::expected<FStaticMeshRenderBuildProduct, FStaticMeshRenderBuildError> override
		{
			return FStaticMeshBuilder::Build(Request, Control);
		}
	};

	IMPLEMENT_MODULE(FStaticMeshBuildModule, StaticMeshBuild)
}
