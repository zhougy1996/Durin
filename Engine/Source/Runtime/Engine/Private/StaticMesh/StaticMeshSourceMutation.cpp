#include "StaticMesh/StaticMeshSourceMutation.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	auto InvokeStaticMeshSourceChangeHandler(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<IStaticMeshSourceMutationFeature>(
			[&](IStaticMeshSourceMutationFeature& Feature) {
				return Feature.ChangeSourceReference(Mesh, SourceVirtualPath, OutError);
			},
			{
				.Unavailable = "StaticMesh source translation is unavailable.",
				.Ambiguous = "StaticMesh source mutation capability is ambiguous.",
				.VisitorFailed = "StaticMesh source mutation provider failed."},
			OutError);
	}
}
