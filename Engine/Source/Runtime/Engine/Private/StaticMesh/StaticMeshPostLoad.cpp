#include "StaticMesh/StaticMeshPostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	auto InvokeStaticMeshPostLoadFeature(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		const bool bSucceeded = Private::InvokeSingleModularFeature<IStaticMeshPostLoadFeature>(
			[&](IStaticMeshPostLoadFeature& Feature) {
				return Feature.PostLoadUncooked(Mesh, OutDiagnostic, OutError);
			},
			{
				.Unavailable = "StaticMesh uncooked load policy is unavailable.",
				.Ambiguous = "StaticMesh post-load capability is ambiguous.",
				.VisitorFailed = "StaticMesh post-load provider failed."},
			OutError);
		if (!bSucceeded && OutError == "StaticMesh uncooked load policy is unavailable.")
		{
			OutDiagnostic.Status = EStaticMeshDerivedDataStatus::Incompatible;
			OutDiagnostic.Message = OutError;
		}
		return bSucceeded;
	}
}
