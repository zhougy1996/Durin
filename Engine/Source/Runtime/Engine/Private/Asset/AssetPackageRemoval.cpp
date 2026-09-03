#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageRemoval.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto FAssetMutationCoordinator::ValidatePackageRemoval(
		std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetResult
	{
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked packages cannot be removed.");
		if (GetAssetCatalogRevision() != ExpectedRevision)
			return Error(EAssetError::StaleData, "The package catalog changed before removal.");
		FAssetPublicationState Prepared = Registry.CapturePreparedState();
		std::unordered_set<FPackagePath> DeletionSet;
		for (const FAssetData& Entry : Entries)
			DeletionSet.insert(Entry.PackagePath);
		for (const FAssetData& Entry : Entries)
		{
			const auto Current = Prepared.Assets.find(Entry.PackagePath);
			if (Current == Prepared.Assets.end() || !(Current->second == Entry))
				return Error(EAssetError::InUse, std::format(
					"Asset {} changed before registry removal.",
					Entry.PackagePath.ToString()));
		}
		for (const auto& [OtherPath, OtherData] : Prepared.Assets)
		{
			if (DeletionSet.contains(OtherPath)) continue;
			for (const FPackagePath& Dependency : OtherData.Dependencies)
				if (DeletionSet.contains(Dependency))
					return Error(EAssetError::InUse, std::format(
						"Asset {} gained external referencer {}.",
						Dependency.ToString(), OtherPath.ToString()));
		}
		return {};
	}

	auto FAssetMutationCoordinator::ReleasePackagesForRemoval(
		std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetResult
	{
		const FAssetResult Validated = ValidatePackageRemoval(Entries, ExpectedRevision);
		if (!Validated) return Validated;
		std::vector<DPackage*> Packages;
		for (const FAssetData& Entry : Entries)
		{
			const FPackagePath& Path = Entry.PackagePath;
			if (LoadingPackages.contains(Path))
				return Error(EAssetError::InUse, std::format(
					"Asset {} is currently loading.", Path.ToString()));
			DPackage* Loaded = FindResidentPackage(Path);
			if (!Loaded) continue;
			if (Loaded->IsDirty())
				return Error(EAssetError::InUse, std::format(
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
		std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetResult
	{
		const FAssetResult Validated = ValidatePackageRemoval(Entries, ExpectedRevision);
		if (!Validated) return Validated;
		for (const FAssetData& Entry : Entries)
		{
			std::error_code Ec;
			const bool bExists = std::filesystem::exists(Entry.PhysicalPath, Ec);
			if (Ec || bExists)
				return Error(EAssetError::IoError, "The removed package file is still present or cannot be inspected.");
		}
		FAssetRegistryDelta Delta{.ExpectedRevision = ExpectedRevision};
		for (const FAssetData& Entry : Entries)
		{
			Delta.Removes.push_back(Entry.PackagePath);
			Delta.ReferenceInvalidations.push_back(Entry.PackagePath);
		}
		return Registry.PublishDelta(std::move(Delta));
	}

}
