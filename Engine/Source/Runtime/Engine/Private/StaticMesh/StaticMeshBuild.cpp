#include "StaticMesh/StaticMeshBuild.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	auto InvokeStaticMeshCollisionBuildFeature(
		const FStaticMeshRenderData& RenderData,
		const FStaticMeshSourceImportData& SourceImportData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<IStaticMeshCollisionBuildFeature>(
			[&](IStaticMeshCollisionBuildFeature& Feature) {
				return Feature.BuildCollisionProduct(
					RenderData, SourceImportData, Mode, Policy, OutProduct, OutError);
			},
			{
				.Unavailable = "StaticMesh collision build capability is unavailable.",
				.Ambiguous = "StaticMesh collision build capability is ambiguous.",
				.VisitorFailed = "StaticMesh collision build provider failed."},
			OutError);
	}
}
