#include "StaticMesh/StaticMeshAuthoring.h"

namespace Durin
{
	namespace
	{
		template<typename F>
		auto InvokeStaticMeshFeature(F&& Visitor, std::string_view Unavailable, std::string& OutError) -> bool
		{
			const auto Result = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshAuthoringFeature>(
				std::forward<F>(Visitor));
			if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value) return *Result.Value;
			if (Result.Status == EFeatureInvokeStatus::Unavailable) OutError = Unavailable;
			else if (Result.Status == EFeatureInvokeStatus::Ambiguous)
				OutError = "StaticMesh authoring capability is ambiguous.";
			else if (Result.Status == EFeatureInvokeStatus::VisitorFailed)
				OutError = "StaticMesh authoring provider failed.";
			return false;
		}
	}

	auto InvokeStaticMeshPostLoadFeature(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool
	{
		const bool bSucceeded = InvokeStaticMeshFeature([&](IStaticMeshAuthoringFeature& Feature) {
			return Feature.PostLoadUncooked(Mesh, OutDiagnostic, OutError);
		}, "StaticMesh uncooked load policy is unavailable.", OutError);
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
		const auto Result = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshCollisionBuildFeature>(
			[&](IStaticMeshCollisionBuildFeature& Feature) {
				return Feature.BuildCollisionProduct(
					RenderData, SourceImportData, Mode, Policy, OutProduct, OutError);
			});
		if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value) return *Result.Value;
		if (Result.Status == EFeatureInvokeStatus::Unavailable)
			OutError = "StaticMesh collision build capability is unavailable.";
		else if (Result.Status == EFeatureInvokeStatus::Ambiguous)
			OutError = "StaticMesh collision build capability is ambiguous.";
		else if (Result.Status == EFeatureInvokeStatus::VisitorFailed)
			OutError = "StaticMesh collision build provider failed.";
		return false;
	}

	auto InvokeStaticMeshSourceChangeHandler(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return InvokeStaticMeshFeature([&](IStaticMeshAuthoringFeature& Feature) {
			return Feature.ChangeSourceReference(Mesh, SourceVirtualPath, OutError);
		}, "StaticMesh source translation is unavailable.", OutError);
	}
}
