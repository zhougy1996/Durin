#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshBuildVersion.h"

namespace Durin
{
	class FMeshBuilderModule final : public IMeshBuilderModule
	{
		// Resident until normal editor shutdown; dynamic reloading is unsupported.

		auto GetRenderBuilderVersion() const -> uint32 override
		{
			return StaticMeshBuilderVersion;
		}

		auto BuildRender(FStaticMeshRenderData& OutRenderData,
			const FStaticMeshBuildParameters& Parameters) -> bool override
		{
			return FStaticMeshBuilder::Build(OutRenderData, Parameters);
		}
	};

	IMPLEMENT_MODULE(FMeshBuilderModule, MeshBuilder)
}
