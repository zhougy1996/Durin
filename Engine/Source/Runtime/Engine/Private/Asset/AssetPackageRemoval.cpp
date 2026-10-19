#include "Asset/AssetWriteResult.h"
#include "AssetRegistryResultAdapter.h"
#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageRemoval.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/Function.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/Level.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto ValidatePackageRemovalReferences(std::span<const FPackagePath> Paths,
		FPackagePath* OutBlockedPackage) -> FAssetWriteResult
	{
		CheckGameThread();
		if (OutBlockedPackage) *OutBlockedPackage = {};
		std::unordered_set<DPackage*> Packages;
		for (const auto& Path : Paths)
			if (auto* Package = FindResidentPackage(Path)) Packages.insert(Package);
		if (Packages.empty()) return {};
		auto Reject = [&](DPackage& Package, std::string Reason) {
			if (OutBlockedPackage) *OutBlockedPackage = Package.GetPackagePathIdentity();
			return Error(EAssetWriteError::InUse, std::move(Reason));
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
		class FRemovalReferenceCollector final : public FReferenceCollector
		{
		public:
			explicit FRemovalReferenceCollector(const std::unordered_set<DPackage*>& InPackages) : Packages(InPackages) {}
			auto AddReferencedObject(DObject*& Object) -> void override
			{
				if (!BlockedPackage && IsValid(Object) && Packages.contains(Object->GetPackage()))
					BlockedPackage = Object->GetPackage();
			}
			const std::unordered_set<DPackage*>& Packages;
			DPackage* BlockedPackage = nullptr;
		} Collector(Packages);
		Private::AddStrongObjectReferences(Collector);
		Private::AddFunctionParameterReferences(Collector);
		if (Collector.BlockedPackage)
			return Reject(*Collector.BlockedPackage, std::format("Asset {} is retained by an active native owner or function call. Close its editor or finish the operation before deleting it.", Collector.BlockedPackage->GetPackagePath()));
		for (DObject* Object : Objects)
		{
			if (!IsValid(Object) || Packages.contains(Object->GetPackage())) continue;
			Object->AddReferencedObjects(Collector);
			if (Collector.BlockedPackage)
				return Reject(*Collector.BlockedPackage, std::format("Asset {} is referenced by live object {}. Close its level or editor before deleting it.", Collector.BlockedPackage->GetPackagePath(), Object->GetObjectPath()));
		}
		return {};
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
		std::vector<FPackagePath> Paths;
		for (const auto& Entry : Entries) Paths.push_back(Entry.PackagePath);
		const auto References = ValidatePackageRemovalReferences(Paths);
		if (!References) return References;
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
		return AssetWriteResultFromRead(AssetPrivate::ToAssetResult(PublishAssetRegistryDelta(std::move(Delta))));
	}

}
