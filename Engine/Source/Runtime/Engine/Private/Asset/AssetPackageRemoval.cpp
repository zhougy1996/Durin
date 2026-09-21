#include "Asset/AssetWriteResult.h"
#include "AssetRegistryResultAdapter.h"
#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageRemoval.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto FAssetMutationCoordinator::ValidatePackageRemoval(
		std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult
	{
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be removed.");
		if (GetAssetCatalogRevision() != ExpectedRevision)
			return Error(EAssetWriteError::StaleData, "The package catalog changed before removal.");
		const FAssetRegistrySnapshot Prepared = CaptureAssetRegistrySnapshot();
		if (Prepared.Revision != ExpectedRevision)
			return Error(EAssetWriteError::StaleData, "The package catalog changed before removal.");
		std::unordered_set<FPackagePath> DeletionSet;
		for (const FAssetData& Entry : Entries)
			DeletionSet.insert(Entry.PackagePath);
		for (const FAssetData& Entry : Entries)
		{
			const auto* Current = Prepared.Catalog.FindExact(Entry.PackagePath);
			if (!Current || !(*Current == Entry))
				return Error(EAssetWriteError::InUse, std::format(
					"Asset {} changed before registry removal.",
					Entry.PackagePath.ToString()));
		}
		for (const auto& Entry : Entries)
			for (const auto& Edge : Prepared.References.FindReferencers(Entry.PackagePath))
				if (Edge.Kind != EAssetReferenceKind::SoftObject && !DeletionSet.contains(Edge.SourcePackage))
					return Error(EAssetWriteError::InUse, std::format(
						"Asset {} gained external referencer {}.",
						Entry.PackagePath.ToString(), Edge.SourcePackage.ToString()));
		return {};
	}

	auto FAssetMutationCoordinator::ReleasePackagesForRemoval(
		std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult
	{
		const FAssetWriteResult Validated = ValidatePackageRemoval(Entries, ExpectedRevision);
		if (!Validated) return Validated;
		std::vector<DPackage*> Packages;
		for (const FAssetData& Entry : Entries)
		{
			const FPackagePath& Path = Entry.PackagePath;
			if (Loader.IsPackageLoading(Path))
				return Error(EAssetWriteError::InUse, std::format(
					"Asset {} is currently loading.", Path.ToString()));
			DPackage* Loaded = FindResidentPackage(Path);
			if (!Loaded) continue;
			if (Loaded->IsDirty())
				return Error(EAssetWriteError::InUse, std::format(
					"Asset {} has unsaved changes.", Path.ToString()));
			Packages.push_back(Loaded);
		}
		for (DPackage* Package : Packages)
		{
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!Packages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetMutationCoordinator::PublishPackageRemoval(
		std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult
	{
		const FAssetWriteResult Validated = ValidatePackageRemoval(Entries, ExpectedRevision);
		if (!Validated) return Validated;
		for (const FAssetData& Entry : Entries)
		{
			std::error_code Ec;
			const bool bExists = std::filesystem::exists(Entry.PhysicalPath, Ec);
			if (Ec || bExists)
				return Error(EAssetWriteError::IoError, "The removed package file is still present or cannot be inspected.");
		}
		FAssetRegistryDelta Delta{.ExpectedRevision = ExpectedRevision};
		for (const FAssetData& Entry : Entries)
		{
			Delta.Removes.push_back(Entry.PackagePath);
			Delta.ReferenceInvalidations.push_back(Entry.PackagePath);
		}
		return AssetPrivate::ToAssetResult(PublishAssetRegistryDelta(std::move(Delta)));
	}

}
