#pragma once

#include "AssetRegistryAPI.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/PackageTypes.h"

namespace Durin::Asset
{
	namespace Private
	{
		class FAssetRegistryState;
	}

	enum class EAssetReferenceKind : uint8
	{
		HardObject,
		SoftObject,
		Redirect
	};

	// Persistent package-level dependency; exact object/property occurrences are transient tooling data.
	struct FAssetPackageReferenceEdge
	{
		FAssetPath SourcePackage;
		FAssetPackageFingerprint SourceFingerprint;
		EAssetReferenceKind Kind = EAssetReferenceKind::HardObject;
		FAssetPath TargetPath;

		auto operator==(const FAssetPackageReferenceEdge&) const -> bool = default;
	};

	class FAssetReferenceIndex
	{
	public:
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetEdges() const -> std::span<const FAssetPackageReferenceEdge> { return Edges; }
		auto GetSourceFingerprints() const
			-> const std::unordered_map<FAssetPath, FAssetPackageFingerprint>&
		{
			return SourceFingerprints;
		}
		ASSETREGISTRY_API auto FindReferencers(const FAssetPath& Target) const
			-> std::vector<FAssetPackageReferenceEdge>;
		ASSETREGISTRY_API auto FindTargets(const FAssetPath& Source) const
			-> std::vector<FAssetPath>;
		auto IsComplete() const -> bool { return bComplete; }
		auto GetErrors() const -> std::span<const FAssetResult> { return Errors; }

	private:
		uint64 Revision = 0;
		std::vector<FAssetPackageReferenceEdge> Edges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> SourceFingerprints;
		std::vector<FAssetResult> Errors;
		bool bComplete = true;

		friend class Private::FAssetRegistryState;
	};

	struct FAssetRegistrySnapshot
	{
		uint64 Revision = 0;
		FAssetCatalogSnapshot Catalog;
		FAssetReferenceIndex References;

		ASSETREGISTRY_API auto ResolveAssetPath(
			const FAssetPath& Path,
			const FAssetPathResolveOptions& Options = {}) const
			-> FAssetPathResolveResult;
	};

	ASSETREGISTRY_API auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex;
	ASSETREGISTRY_API auto CaptureAssetRegistrySnapshot() -> FAssetRegistrySnapshot;
	ASSETREGISTRY_API auto FindRedirectorsTo(const FAssetPath& Destination)
		-> std::vector<FAssetPath>;
}
