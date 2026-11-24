#include "Asset/AssetWriteResult.h"
#include "AssetRegistry/RegistryResult.h"
#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageRemoval.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ReferencerInformation.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/Function.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/Level.h"
#include "Threading/RunnableThread.h"
#include "HAL/PlatformFile.h"

namespace Durin
{
	auto OnPackageDeleted() -> TMulticastDelegate<void(DPackage&)>&
	{
		static TMulticastDelegate<void(DPackage&)> Event;
		return Event;
	}

	class FStrongObjectPtr;
	namespace
	{
		auto WriteFailure(UnifiedError::FErrorId Code, std::string Message) -> std::expected<void, UnifiedError::FError>
		{ return std::unexpected(UnifiedError::FError{Code, std::move(Message)});
		}
	}

	auto ValidatePackageRemovalReferences(std::span<const FPackagePath> Paths,
		FPackagePath* OutBlockedPackage, std::span<const FStrongObjectPtr* const> ExcludedOwners) -> std::expected<void, UnifiedError::FError>
	{
		CheckGameThread();
		if (OutBlockedPackage) *OutBlockedPackage = {};
		std::unordered_set<DPackage*> Packages;
		for (const auto& Path : Paths)
			if (auto* Package = FindResidentPackage(Path)) Packages.insert(Package);
		if (Packages.empty()) return {};
		auto Reject = [&](DPackage& Package, std::string Reason) {
			if (OutBlockedPackage) *OutBlockedPackage = Package.GetPackagePathIdentity();
			return WriteFailure(AssetWriteErrors::InUse, std::move(Reason));
		};
		const auto Objects = GDObjectArray.GetAll(EObjectQueryScope::LiveOnly);
		for (DObject* Object : Objects)
		{
			if (!IsValid(Object)) continue;
			DPackage* Package = Object->GetPackage();
			if (!Packages.contains(Package)) continue;
			if (const auto* Level = Cast<DLevel>(Object); Level && Level->GetWorld())
				return Reject(*Package, std::format("Level {} is open in a world. Close the level before deleting it.", Object->GetObjectPath()));
			if (Object->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
				return Reject(*Package, std::format("Asset {} contains rooted object {} and cannot be deleted.", Package->GetPackagePath(), Object->GetObjectPath()));
		}
		std::vector<DObject*> Roots(Packages.begin(), Packages.end());
		for (auto* Package : Packages)
		{
			FReferencerInformationList References;
			if (FindObjectReferencers(Package, Roots, References, ExcludedOwners)
				|| !References.ExternalReferences.empty())
				return Reject(*Package, "The deletion is retained by an external live or native owner.");
		}
		return {};
	}

	auto FAssetMutationCoordinator::ValidatePackageRemoval(
		std::span<const FPackageData> Entries, uint64 ExpectedRevision) -> std::expected<void, UnifiedError::FError>
	{
		if (RuntimeConfiguration.IsCooked())
			return WriteFailure(AssetWriteErrors::ReadOnlyMode, "Cooked packages cannot be removed.");
		if (GetAssetCatalogRevision() != ExpectedRevision)
			return WriteFailure(AssetWriteErrors::StaleData, "The package catalog changed before removal.");
		const FAssetRegistrySnapshot Prepared = CaptureAssetRegistrySnapshot();
		if (Prepared.Revision != ExpectedRevision)
			return WriteFailure(AssetWriteErrors::StaleData, "The package catalog changed before removal.");
		std::unordered_set<FPackagePath> DeletionSet;
		for (const FPackageData& Entry : Entries)
			DeletionSet.insert(Entry.PackagePath);
		for (const FPackageData& Entry : Entries)
		{
			const auto* Current = Prepared.Catalog.FindExact(Entry.PackagePath);
			if (!Current || !(*Current == Entry))
				return WriteFailure(AssetWriteErrors::InUse, std::format(
					"Asset {} changed before registry removal.",
					Entry.PackagePath.ToString()));
		}
		for (const auto& Entry : Entries)
			for (const auto& Edge : Prepared.References.FindReferencers(Entry.PackagePath))
				if (Edge.Kind != EAssetReferenceKind::SoftObject && !DeletionSet.contains(Edge.SourcePackage))
					return WriteFailure(AssetWriteErrors::InUse, std::format(
						"Asset {} gained external referencer {}.",
						Entry.PackagePath.ToString(), Edge.SourcePackage.ToString()));
		return {};
	}

	auto FAssetMutationCoordinator::ReleasePackagesForRemoval(
		std::span<const FPackageData> Entries, uint64 ExpectedRevision,
			std::span<const FStrongObjectPtr* const> ExcludedOwners, bool bAllowDirty) -> std::expected<void, UnifiedError::FError>
	{
		const auto Validated = ValidatePackageRemoval(Entries, ExpectedRevision);
		if (!Validated) return Validated;
		std::vector<DPackage*> Packages;
		for (const FPackageData& Entry : Entries)
		{
			const FPackagePath& Path = Entry.PackagePath;
			if (Loader.IsPackageLoading(Path))
				return WriteFailure(AssetWriteErrors::InUse, std::format(
					"Asset {} is currently loading.", Path.ToString()));
			DPackage* Loaded = FindResidentPackage(Path);
			if (!Loaded) continue;
			if (!bAllowDirty && Loaded->IsDirty())
				return WriteFailure(AssetWriteErrors::InUse, std::format(
					"Asset {} has unsaved changes.", Path.ToString()));
			Packages.push_back(Loaded);
		}
		std::vector<FPackagePath> Paths;
		for (const auto& Entry : Entries) Paths.push_back(Entry.PackagePath);
		const auto References = ValidatePackageRemovalReferences(Paths, nullptr, ExcludedOwners);
		if (!References) return References;
		for (DPackage* Package : Packages)
		{
			OnPackageDeleted().Broadcast(*Package);
			MarkObjectHierarchyAsGarbage(Package);
			Package->RetireGarbageRegistration();
		}
		if (!Packages.empty()) RequestGarbageCollection();
		return {};
	}

	auto FAssetMutationCoordinator::PublishPackageRemoval(
		std::span<const FPackageData> Entries, uint64 ExpectedRevision) -> std::expected<void, UnifiedError::FError>
	{
		const auto Validated = ValidatePackageRemoval(Entries, ExpectedRevision);
		if (!Validated) return Validated;
		for (const FPackageData& Entry : Entries)
		{
			std::error_code Ec;
			const bool bExists = std::filesystem::exists(Entry.PhysicalPath, Ec);
			if (Ec) return std::unexpected(ToError(FFileSystemError{std::format("Failed to inspect: {}.", FFilePath(Entry.PhysicalPath).generic_string()), Ec}));
			if (bExists)
				return WriteFailure(AssetWriteErrors::StaleData, "The removed package file is still present.");
		}
		FAssetRegistryDelta Delta{.ExpectedRevision = ExpectedRevision};
		for (const FPackageData& Entry : Entries)
		{
			Delta.Removes.push_back(Entry.PackagePath);
			Delta.ReferenceInvalidations.push_back(Entry.PackagePath);
		}
		if (auto Published = PublishAssetRegistryDelta(std::move(Delta)); !Published)
			return std::unexpected(Durin::ToError(Published));
		return {};
	}

}
