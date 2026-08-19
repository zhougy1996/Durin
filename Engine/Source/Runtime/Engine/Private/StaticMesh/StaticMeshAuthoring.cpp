#include "StaticMesh/StaticMeshAuthoring.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view StaticMeshAuthoringAmbiguousMessage =
			"StaticMesh authoring capability is ambiguous.";
		constexpr std::string_view StaticMeshAuthoringVisitorFailedMessage =
			"StaticMesh authoring provider failed.";
	}

	auto InvokeStaticMeshPostLoadFeature(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		const bool bSucceeded = Private::InvokeSingleModularFeature<IStaticMeshAuthoringFeature>(
			[&](IStaticMeshAuthoringFeature& Feature) {
				return Feature.PostLoadUncooked(Mesh, OutDiagnostic, OutError);
			},
			{
				.Unavailable = "StaticMesh uncooked load policy is unavailable.",
				.Ambiguous = StaticMeshAuthoringAmbiguousMessage,
				.VisitorFailed = StaticMeshAuthoringVisitorFailedMessage},
			OutError);
		if (!bSucceeded && OutError == "StaticMesh uncooked load policy is unavailable.")
		{
			OutDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
			OutDiagnostic.Message = OutError;
		}
		return bSucceeded;
	}

	auto InvokeStaticMeshCollisionBuildFeature(
		const FStaticMeshRenderData& RenderData,
		const FStaticMeshSourceImportData& SourceImportData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionAuthoringProduct& OutProduct,
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

	auto InvokeStaticMeshSourceChangeHandler(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<IStaticMeshAuthoringFeature>(
			[&](IStaticMeshAuthoringFeature& Feature) {
				return Feature.ChangeSourceReference(Mesh, SourceVirtualPath, OutError);
			},
			{
				.Unavailable = "StaticMesh source translation is unavailable.",
				.Ambiguous = StaticMeshAuthoringAmbiguousMessage,
				.VisitorFailed = StaticMeshAuthoringVisitorFailedMessage},
			OutError);
	}
}
