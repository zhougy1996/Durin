#pragma once

#include "AssetRegistryAPI.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/PackageTypes.h"

namespace Durin
{
	namespace AssetPrivate
	{
		class FAssetRegistryState;
	}

	enum class EAssetReferenceKind : uint8
	{
		HardObject,
		SoftObject,
		Redirect
	};

	// Aggregates every asset's redirects. Mixed packages retain hard edges even when
	// an ordinary reference and a redirect share the same destination package.
	template <typename TVisitor>
	auto VisitAssetPackageReferences(const FAssetData& Data, TVisitor&& Visitor) -> void
	{
		const bool bOnlyRedirectors = !Data.TopLevelAssets.empty()
			&& std::ranges::all_of(Data.TopLevelAssets, &FTopLevelAssetData::IsRedirector);
		for (const FPackagePath& Target : Data.Dependencies)
			if (!bOnlyRedirectors || std::ranges::none_of(Data.TopLevelAssets,
				[&](const FTopLevelAssetData& Asset) {
					return Asset.RedirectDestination.GetPackagePath() == Target;
				})) Visitor(EAssetReferenceKind::HardObject, Target);
		for (const FPackagePath& Target : Data.SoftDependencies)
			Visitor(EAssetReferenceKind::SoftObject, Target);
		for (const FTopLevelAssetData& Asset : Data.TopLevelAssets)
			if (Asset.IsRedirector())
				Visitor(EAssetReferenceKind::Redirect, Asset.RedirectDestination.GetPackagePath());
	}

	// Persistent package-level dependency; exact object/property occurrences are transient tooling data.
	struct FAssetPackageReferenceEdge
	{
		FPackagePath SourcePackage;
		FAssetPackageFingerprint SourceFingerprint;
		EAssetReferenceKind Kind = EAssetReferenceKind::HardObject;
		FPackagePath TargetPath;

		auto operator==(const FAssetPackageReferenceEdge&) const -> bool = default;
	};

	class FAssetReferenceIndex
	{
	public:
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetEdges() const -> std::span<const FAssetPackageReferenceEdge> { return Edges; }
		auto GetSourceFingerprints() const
			-> const std::unordered_map<FPackagePath, FAssetPackageFingerprint>&
		{
			return SourceFingerprints;
		}
		ASSETREGISTRY_API auto FindReferencers(const FPackagePath& Target) const
			-> std::vector<FAssetPackageReferenceEdge>;
		ASSETREGISTRY_API auto FindTargets(const FPackagePath& Source) const
			-> std::vector<FPackagePath>;
		auto IsComplete() const -> bool { return bComplete; }
		auto GetErrors() const -> std::span<const FAssetRegistryResult> { return Errors; }

	private:
		uint64 Revision = 0;
		std::vector<FAssetPackageReferenceEdge> Edges;
		std::unordered_map<FPackagePath, FAssetPackageFingerprint> SourceFingerprints;
		std::vector<FAssetRegistryResult> Errors;
		bool bComplete = true;

		friend class AssetPrivate::FAssetRegistryState;
	};

	struct FAssetRegistrySnapshot
	{
		uint64 Revision = 0;
		FAssetCatalogSnapshot Catalog;
		FAssetReferenceIndex References;

		ASSETREGISTRY_API auto ResolveAssetPath(
			const FPackagePath& Path,
			const FAssetPathQueryOptions& Options = {}) const
			-> FAssetPathResolveResult;
		ASSETREGISTRY_API auto ResolveAssetObjectPath(
			const FObjectPath& Path, const FAssetPathQueryOptions& Options = {}) const
			-> FObjectPathResolveResult;
	};

	ASSETREGISTRY_API auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex;
	ASSETREGISTRY_API auto CaptureAssetRegistrySnapshot() -> FAssetRegistrySnapshot;
	ASSETREGISTRY_API auto FindRedirectorsTo(const FPackagePath& Destination)
		-> std::vector<FPackagePath>;
}
