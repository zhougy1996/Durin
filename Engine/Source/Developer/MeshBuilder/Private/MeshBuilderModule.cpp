#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshBuildVersion.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"
#include <glm/detail/setup.hpp>

namespace Durin
{
	class FMeshBuilderModule final : public IMeshBuilderModule
	{
		// Resident until normal editor shutdown; dynamic reloading is unsupported.

		auto GetBuildVersion() const -> uint64 override
		{
			static const uint64 Version = [] {
				FBinaryWriter Writer;
				Writer.WriteString("Durin.MeshBuilder.StaticMesh");
				Writer.WriteU32(StaticMeshBuilderVersion);
				Writer.WriteU32(GLM_VERSION_MAJOR);
				Writer.WriteU32(GLM_VERSION_MINOR);
				Writer.WriteU32(GLM_VERSION_PATCH);
				return FXxHash64::HashBuffer(Writer.TakeBytes()).HashValue;
			}();
			return Version;
		}

		auto BuildRender(FStaticMeshRenderData& OutRenderData,
			const FStaticMeshBuildParameters& Parameters) -> bool override
		{
			return FStaticMeshBuilder::Build(OutRenderData, Parameters);
		}
	};

	IMPLEMENT_MODULE(FMeshBuilderModule, MeshBuilder)
}
