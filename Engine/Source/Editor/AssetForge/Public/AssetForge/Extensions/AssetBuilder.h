#pragma once

#include "Asset/Load.h"
#include "AssetForge/Extensions/SourceTranslator.h"
#include "AssetForge/Graph/BuildGraph.h"

namespace Durin::AssetForge
{
struct FImportProvenance;

struct FAssetBuilderDescriptor
	{
		FComponentIdentity Identity;
		std::string OutputClassName;
		int32 Priority = 0;
		EThreadCapability ProductBuildThread = EThreadCapability::WorkerSafe;
	};

	class IBuildProduct
	{
	public:
		virtual ~IBuildProduct() = default;
	};

	class IReconciliationContext
	{
	public:
		virtual ~IReconciliationContext() = default;
	};

	// Read-only view of the complete prospective authored graph. Factories use
	// it to bind relationships only after every candidate has been materialized.
	// ExistingTarget returns the currently published object for replacements;
	// ProspectiveObject returns the candidate that will be published.
	struct FMaterializationContext
	{
		std::function<DObject*(std::string_view)> ExistingTarget;
		std::function<DObject*(std::string_view)> ProspectiveObject;
	};

	class IAssetBuilder
	{
	public:
		virtual ~IAssetBuilder() = default;
		virtual auto BuildDetachedProduct(
			const FBuildNode& AssetBuilderNode,
			const FSourceGraph& SourceGraph,
			IImportProgressReporter* Progress,
			const std::function<bool()>& IsCancellationRequested,
			std::vector<FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<IBuildProduct> = 0;
		// Captures immutable replacement facts on the GameThread. The returned
		// context crosses to worker product construction and must not retain
		// Target references.
		virtual auto CaptureReconciliationContext(
			const FBuildNode&,
			const DObject&,
			std::vector<FImportDiagnostic>&) const
			-> std::unique_ptr<IReconciliationContext> { return {}; }
		// Applies the immutable target context during worker product construction.
		virtual auto ReconcileDetachedProduct(
			const FBuildNode&,
			const IReconciliationContext*,
			IBuildProduct&,
			std::vector<FImportDiagnostic>&) const -> bool { return true; }
		virtual auto MaterializeCandidate(
			const FBuildNode& AssetBuilderNode,
			std::unique_ptr<IBuildProduct> Product,
			std::vector<FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<ISingleAssetCandidate> = 0;
		// Providers may relax disposable derived-data loading while obtaining the
		// published identity that a replacement will update.
		virtual auto LoadExistingTarget(
			const FBuildNode& AssetBuilderNode,
			DObject*& OutTarget) const -> Asset::FAssetResult
		{
			return Asset::LoadAsset(AssetBuilderNode.Destination, OutTarget);
		}
		virtual auto ResolveCandidateDependencies(
			const FBuildNode&,
			ISingleAssetCandidate&,
			const FMaterializationContext&,
			std::vector<FImportDiagnostic>&) const -> bool { return true; }
		// Publishes a completed candidate into an existing asset on the GameThread.
		// Import task state is the authority for in-progress work; publication is
		// one-way and leaves the asset dirty for an independent save attempt.
		virtual auto PublishImportedState(
			DObject&,
			ISingleAssetCandidate&,
			std::vector<FImportDiagnostic>&) const -> bool { return false; }
		// Called after recovery publication. The candidate now contains the
		// displaced target state, allowing factories to distinguish authored
		// changes from disposable runtime/DDC reconstruction.
		virtual auto HasAuthoredRecoveryChanges(
			const DObject&,
			const ISingleAssetCandidate&) const -> bool { return false; }
		// Persists framework reproduction metadata after one-way publication and
		// authored fingerprinting, but before the independent save attempt.
		virtual auto ApplyProvenance(
			DObject&,
			const FImportProvenance&,
			std::vector<FImportDiagnostic>&) const -> bool { return true; }
	};
}
