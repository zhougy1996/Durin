#include "Asset/References.h"
#include "Misc/FileHelper.h"
#include "DObject/Function.h"
#include "AssetTools/IAssetTools.h"
#include "AssetMutationInternal.h"
#include "AssetTools/AssetRenameReferences.h"
#include "FileHelpers.h"
#include "PackageTools.h"
#include "Asset/Load.h"
#include "Asset/PackageEditing.h"
#include "Asset/PackageSerialization.h"
#include "Asset/Redirector.h"
#include "Asset/RegistryOperations.h"
#include "AssetRegistry/Publication.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Import/AssetDestinationValidation.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	using AssetToolsPrivate::FAssetReferenceFixup;
	using AssetToolsPrivate::FAssetRenameDataWithReferencers;
	using AssetToolsPrivate::CollectSoftObjectPathFixups;
	FAssetRenameData::FAssetRenameData(TWeakObjectPtr<DObject> Object, std::string_view Directory,
		std::string_view Name, bool OnlySoft)
		: Asset(Object), NewPackagePath(Directory), NewName(Name), bOnlyFixSoftReferences(OnlySoft) {}
	FAssetRenameData::FAssetRenameData(const FSoftObjectPath& Old, const FSoftObjectPath& New, bool OnlySoft)
		: OldObjectPath(Old), NewObjectPath(New), bOnlyFixSoftReferences(OnlySoft) {}

	namespace
	{
		bool Renaming = false, Fixing = false;
		auto Reject(std::string_view Message) -> bool
		{
			static thread_local bool Reporting = false;
			if (Reporting) return false;
			Reporting = true;
			struct FReset { bool& Value; ~FReset() { Value = false; } } Reset{Reporting};
			FFeedbackContext::Get().Error(Message);
			return false;
		}
		auto KeepPackage(DPackage* Package, std::vector<FStrongObjectPtr>& Owners) -> void
		{
			Owners.emplace_back(Package);
			for (auto* Object : Package->GetTopLevelAssets()) Owners.emplace_back(Object);
		}
		auto HasPersistentAliasReferences(const FSoftObjectPath& Alias, FAssetReferenceFixup& Facts) -> bool
		{
			std::unordered_set<FPackagePath> Seen;
			for (const auto& Edge : Facts.GetRegistry().References.FindReferencers(Alias.GetPackagePath()))
			{
				if (!Seen.insert(Edge.SourcePackage).second) continue;
				const auto Data = FindPackageExact(Edge.SourcePackage);
				if (!Data) return true;
				const auto Occurrences = Facts.GetPackageReferences(*Data);
				if (!Occurrences) { Reject(Occurrences.error().ToString()); return true; }
				if (std::ranges::any_of(**Occurrences, [&](const auto& Occurrence) { return Occurrence.TargetPath.GetAssetPath() == Alias.GetAssetPath(); })) return true;
			}
			return false;
		}
		auto SaveRenamed(DObject* Asset) -> bool
		{
			auto* Package = Asset->GetPackage();
			FAssetWriteResult Result;
			const bool Saved = (!Package->IsDirty() && !Package->IsNewlyCreated() && !IsAssetRegistryProjectionFenced(Package->GetPackagePathIdentity())) || SavePackage(Package, {}, &Result);
			const bool Committed = Saved || Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending;
			if (!Saved) FFeedbackContext::Get().Error(Result.Error ? Result.Error->ToString() : "Renamed package save failed; the live asset remains available for retry.");
			if (!Committed) return false;
			bool Success = Saved;
			// A retry of an already committed identity also retries its unsaved aliases.
			std::vector<FStrongObjectPtr> Owners;
			std::vector<DPackage*> Aliases;
			for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
				if (auto* Alias = Cast<DAssetRedirector>(Object); Alias && Alias->GetDestinationObject() == Asset
					&& Alias->GetPackage() != Package && (Alias->GetPackage()->IsDirty() || IsAssetRegistryProjectionFenced(Alias->GetPackage()->GetPackagePathIdentity()))
					&& !std::ranges::contains(Aliases, Alias->GetPackage()))
				{ Aliases.push_back(Alias->GetPackage()); KeepPackage(Alias->GetPackage(), Owners); }
			if (!DEditorLoadingAndSavingUtils::SavePackages(Aliases, false)) Success = false;
			return Success;
		}
		// Follow-up persistence gates fix-up retirement independently of rename success.
		auto RenameAndPersist(FAssetRenameDataWithReferencers& Item, FAssetReferenceFixup& Facts, bool SaveHardReferencers = false) -> bool
		{
			auto& Data = static_cast<FAssetRenameData&>(Item);
			DObject* Object = Data.Asset.Get();
			if (!Data.OldObjectPath.IsValid() && Object) FSoftObjectPath::TryCreate(Object->GetObjectPath(), Data.OldObjectPath);
			if (!Data.NewObjectPath.IsValid())
			{
				std::string Directory = Data.NewPackagePath;
				if (!Directory.ends_with('/')) Directory += '/';
				if (!FSoftObjectPath::TryCreate(Directory + Data.NewName + "." + Data.NewName, Data.NewObjectPath)) return Reject("The new asset identity is invalid.");
			}
			if (!Data.OldObjectPath.IsValid()) return Reject("Rename requires a live object or an explicit old object path.");
			if (!Data.bOnlyFixSoftReferences && (!Data.OldObjectPath.IsTopLevelAsset() || !Data.NewObjectPath.IsTopLevelAsset()))
				return Reject("Physical rename requires top-level assets; subobject paths are supported only for soft reference repair.");
			std::optional<UnifiedError::FError> Error;
			if (!Object && !Data.bOnlyFixSoftReferences) Object = LoadObject(Data.OldObjectPath, nullptr, nullptr, &Error);
			if (!Data.bOnlyFixSoftReferences && (!Object || Cast<DAssetRedirector>(Object))) return Reject(Error ? Error->ToString() : "The rename source is unavailable or is a redirector.");
			if (Data.NewPackagePath.empty()) { const auto Text = Data.NewObjectPath.GetPackagePath().ToString(); Data.NewPackagePath = Text.substr(0, Text.find_last_of('/')); }
			if (Data.NewName.empty()) Data.NewName = Data.NewObjectPath.GetAssetPath().GetAssetName();
			Item.Object = TStrongObjectPtr<DObject>(Object);
			if (Object) Data.Asset = TWeakObjectPtr<DObject>(Object);
			if (!Data.bOnlyFixSoftReferences && Object->GetObjectPath() == Data.NewObjectPath.ToString())
			{
				Item.bRenameFailed = false;
				return SaveRenamed(Object);
			}
			if (!Data.bOnlyFixSoftReferences && Object->GetObjectPath() != Data.OldObjectPath.ToString()) return Reject("The rename source identity changed.");
			const auto OldPackagePath = Data.OldObjectPath.GetPackagePath();
			const auto NewPackagePath = Data.NewObjectPath.GetPackagePath();
			const bool CaseOnly = Data.OldObjectPath == Data.NewObjectPath;
			const auto SourceDestination = InspectAssetDestination(OldPackagePath.GetView());
			const auto Destination = InspectAssetDestination(NewPackagePath.GetView());
			if (!SourceDestination.bMountedDestination || !SourceDestination.bContentWritable || !Destination.bMountedDestination || !Destination.bContentWritable)
				return Reject("Rename requires writable mounted source and destination packages.");
			auto* SourcePackage = Object ? Object->GetPackage() : nullptr;
			if (SourcePackage && (Object->IsTemplateObject() || Object->HasAnyObjectFlags(EObjectFlags::Transient) || SourcePackage->IsGraphPrivate() || SourcePackage->IsPlayInEditor() || !SourcePackage->IsFullyLoaded() || IsPackageLoading(OldPackagePath))) return Reject("Rename requires a published fully loaded source.");
			const auto SourceRevision = SourcePackage ? SourcePackage->GetEditRevision() : 0;
			std::optional<FXxHash128> SourceFingerprint;
			if (!Data.bOnlyFixSoftReferences && SourcePackage && !SourcePackage->IsNewlyCreated())
			{
				const auto Fingerprint = FFileHelper::HashFileXx128(SourceDestination.PhysicalPath);
				if (!Fingerprint) return Reject(Fingerprint.error().ToString());
				const auto Base = SourcePackage->GetDiskImageDigest();
				if (Base != FXxHash128{} && Base != *Fingerprint) return Reject("The saved source changed outside the loaded package; reload it before renaming.");
				SourceFingerprint = *Fingerprint;
			}
			auto* DestinationPackage = FindResidentPackage(NewPackagePath);
			std::error_code FilesystemError;
			if (!Data.bOnlyFixSoftReferences && !CaseOnly && NewPackagePath != OldPackagePath
				&& (DestinationPackage || FindPackageExact(NewPackagePath) || std::filesystem::exists(Destination.PhysicalPath, FilesystemError) || FilesystemError))
				return Reject("The rename destination package is occupied.");
			if (!Data.bOnlyFixSoftReferences && SourcePackage && SourcePackage->FindTopLevelAsset(Object->GetFName()) != Object) return Reject("Rename requires a registered top-level object.");
			std::vector<FStrongObjectPtr> Owners;
			std::vector<DPackage*> Referencers, ToSave;
			if (SourcePackage) { KeepPackage(SourcePackage, Owners); Referencers.push_back(SourcePackage); }
			const auto& References = Facts.GetRegistry().References;
			// Load disk referencers while the old identity still resolves. Their hard pointers survive rename.
			for (const auto& Edge : References.FindReferencers(OldPackagePath))
				if (Edge.Kind != EAssetReferenceKind::Redirect && !std::ranges::contains(Item.ReferencingPackageNames, Edge.SourcePackage)) Item.ReferencingPackageNames.push_back(Edge.SourcePackage);
			for (const auto& Path : Item.ReferencingPackageNames)
			{
				if (std::ranges::any_of(Referencers, [&](auto* P) { return P->GetPackagePathIdentity() == Path; })) continue;
				auto* Package = LoadPackage(Path, nullptr, &Error);
				if (Package) { Referencers.push_back(Package); if (!Data.bOnlyFixSoftReferences || SaveHardReferencers) ToSave.push_back(Package); KeepPackage(Package, Owners); }
				else FFeedbackContext::Get().Warning("A referencer could not be loaded; its old identity will remain supported by a redirector.");
			}
			for (auto* Candidate : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
				if (auto* Package = Cast<DPackage>(Candidate); Package && Package->IsAssetPackage() && !Package->IsGraphPrivate() && !Package->IsPlayInEditor() && Package->IsFullyLoaded() && !Package->GetTopLevelAssets().empty() && !std::ranges::contains(Referencers, Package))
				{ Referencers.push_back(Package); KeepPackage(Package, Owners); }
			auto& Soft = Item.SoftReferences;
			for (auto* Package : Referencers)
				if (auto Collected = CollectSoftObjectPathFixups(Package, std::span{&Data, 1}, Soft); !Collected) return Reject(Collected.error().ToString());
			FAssetOwnedPayloadRelocation Payload;
			std::vector<std::filesystem::path> CopiedPayloads;
			bool PayloadCommitted = false;
			struct FPayloadPreparation
			{
				std::vector<std::filesystem::path>& Files; bool& Committed;
				~FPayloadPreparation() { if (!Committed) for (const auto& File : Files) { std::error_code Ignored; std::filesystem::remove(File, Ignored); } }
			} PayloadPreparation{CopiedPayloads, PayloadCommitted};
			if (!Data.bOnlyFixSoftReferences && OldPackagePath != NewPackagePath)
				if (const auto Relocator = FindAssetOwnedPayloadRelocator(Object->GetClass()))
				{
					if (auto Prepared = Relocator(Object, OldPackagePath, NewPackagePath, Payload); !Prepared) return Reject(Prepared.error().ToString());
					for (const auto& [From, To] : Payload.Files)
					{
						if (From == To) continue;
						std::filesystem::create_directories(To.parent_path(), FilesystemError);
						if (!FilesystemError) std::filesystem::copy_file(From, To, std::filesystem::copy_options::none, FilesystemError);
						if (FilesystemError)
						{
							for (const auto& Created : CopiedPayloads) { std::error_code Ignored; std::filesystem::remove(Created, Ignored); }
							return Reject("Owned payload preparation failed: " + FilesystemError.message());
						}
						CopiedPayloads.push_back(To);
					}
				}
			const auto Stores = Facts.GetStores();
			if (!Stores) return Reject(Stores.error().ToString());
			const bool ExternalStore = std::ranges::any_of((*Stores)->Stores, [&](const auto& Store) { return std::ranges::any_of(Store.Occurrences, [&](const auto& Occurrence) { return Occurrence.TargetPath == OldPackagePath; }); });
			Item.bCreateRedirector = !CaseOnly && SourcePackage && (!SourcePackage->IsNewlyCreated() || !References.FindReferencers(OldPackagePath).empty() || !Soft.empty() || ExternalStore);
			if (SourcePackage && (Object->GetPackage() != SourcePackage || SourcePackage->GetEditRevision() != SourceRevision)) return Reject("The source changed while rename was preparing.");
			for (const auto& Rewrite : Soft)
				if (Rewrite.Value->ToSoftObjectPath() != Rewrite.Before) return Reject("A soft reference changed while rename was preparing.");
			if (!Data.bOnlyFixSoftReferences)
			{
				if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return Reject(Guard.error().ToString());
				const auto CurrentSource = InspectAssetDestination(OldPackagePath.GetView()), CurrentTarget = InspectAssetDestination(NewPackagePath.GetView());
				if (!CurrentSource.bContentWritable || !CurrentTarget.bContentWritable || CurrentSource.PhysicalPath != SourceDestination.PhysicalPath || CurrentTarget.PhysicalPath != Destination.PhysicalPath)
					return Reject("A source or destination mount changed while rename was preparing.");
				if (SourceFingerprint)
				{
					const auto Current = FFileHelper::HashFileXx128(SourceDestination.PhysicalPath);
					if (!Current) return Reject(Current.error().ToString());
					if (*Current != *SourceFingerprint) return Reject("The source file changed while rename was preparing.");
				}
			}
			DAssetRedirector* Alias = nullptr;
			if (!Data.bOnlyFixSoftReferences)
			{
				if (CaseOnly || (!Item.bCreateRedirector && SourcePackage->GetTopLevelAssets().size() == 1))
				{
					if (SourcePackage->GetTopLevelAssets().size() != 1 && OldPackagePath.ToString() != NewPackagePath.ToString()) return Reject("A package spelling change cannot silently rename sibling assets.");
					if (!SourcePackage->RelocateAssetPackage(NewPackagePath)) return Reject("The package identity could not be updated.");
					SourcePackage->Rename(FName(NewPackagePath.GetPackageName()));
					DestinationPackage = SourcePackage;
				}
				else if (NewPackagePath == OldPackagePath) DestinationPackage = SourcePackage;
				else DestinationPackage = CreatePackage(NewPackagePath);
				if (!DestinationPackage) return Reject("The destination package could not be created.");
				KeepPackage(DestinationPackage, Owners);
				const auto PreviousName = Object->GetFName();
				if (!Object->Rename(FName(Data.NewObjectPath.GetAssetPath().GetAssetName()), DestinationPackage)) return Reject("The object identity could not be updated.");
				Object->SetFlags(EObjectFlags::Public | EObjectFlags::Standalone);
				if (Payload.Apply) Payload.Apply();
				PayloadCommitted = true;
				if (Item.bCreateRedirector)
				{
					Alias = NewObject<DAssetRedirector>(SourcePackage, PreviousName, EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional);
					if (!Alias) return Reject("The old identity redirector could not be created.");
					Alias->SetDestinationObject(Object);
					Owners.emplace_back(Alias);
				}

			}
			for (auto& Rewrite : Soft) { *Rewrite.Value = Rewrite.After; Rewrite.Package->MarkDirty(); if (!std::ranges::contains(ToSave, Rewrite.Package)) ToSave.push_back(Rewrite.Package); }
			Item.bRenameFailed = false;
			if (!Data.bOnlyFixSoftReferences) { Item.bIdentityChanged = true; PublishAssetRenamed(*Object, Data.OldObjectPath); }
			bool Success = true;
			if (!Data.bOnlyFixSoftReferences) Success = SaveRenamed(Object);
			// Save referencers only after the real destination is persistent. Failed saves keep aliases.
			if (Success)
				for (auto* Package : ToSave)
				{
					if (!Data.bOnlyFixSoftReferences && (Package == SourcePackage || (Object && Package == Object->GetPackage()))) continue;
					const auto Writeable = InspectAssetDestination(Package->GetPackagePathIdentity().GetView());
					if (!Writeable.bMountedDestination || !Writeable.bContentWritable) continue;
					if (!DEditorLoadingAndSavingUtils::SavePackages(std::span{&Package, 1}, false)) Success = false;
				}
			if (Data.bOnlyFixSoftReferences) return Success;
			if (Alias && SourcePackage == DestinationPackage && !DEditorLoadingAndSavingUtils::SavePackages(std::span{&SourcePackage, 1}, false)) Success = false;
			if (Success && Alias && ExternalStore && std::ranges::all_of(SourcePackage->GetTopLevelAssets(), [&](auto* Export) {
				auto* Redirector = Cast<DAssetRedirector>(Export);
				return Redirector && Redirector->GetDestinationObject() && Redirector->GetDestinationObject()->GetPackage() == Object->GetPackage();
			}))
			{
				const auto Fixed = FixupPackageAliases(std::span{&OldPackagePath, 1}, EPackageAliasFixupMode::RewriteOnly, &Facts);
				if (!Fixed.Result)
				{
					Success = false;
					Reject(Fixed.Result.Error ? Fixed.Result.Error->ToString() : "Reference store update failed; the old alias remains valid.");
					for (const auto& Backup : Fixed.BackupLocations) FFeedbackContext::Get().Warning("Recovery backup: " + Backup.generic_string());
				}
			}
			if (Success)
				for (const auto& [From, To] : Payload.Files)
					if (From != To)
					{
						std::error_code Error; std::filesystem::remove(From, Error);
						if (Error) { Success = false; FFeedbackContext::Get().Warning("The renamed asset is saved; remove the retained source payload after inspection: " + From.generic_string()); }
					}
			return Success;
		}
	}
	auto RenameAssetsWithEditorPolicy(std::span<const FAssetRenameData> Input,
		TMulticastDelegate<void(std::span<const FAssetRenameData>)>& PostRename) -> bool
	{
		check(IsInGameThread());
		if (Renaming || Fixing) return Reject("An asset identity operation is already running.");
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return Reject(Guard.error().ToString());
		if (!CaptureAssetReferenceIndex().IsComplete()) return Reject("Rename requires complete asset discovery and reference indexing.");
		Renaming = true;
		struct FReset { ~FReset() { Renaming = false; } } Reset;
		std::vector<FAssetRenameDataWithReferencers> Items;
		for (const auto& Request : Input)
		{
			auto Data = Request;
			auto* Object = Request.Asset.Get();
			if (!Data.OldObjectPath.IsValid() && Object) FSoftObjectPath::TryCreate(Object->GetObjectPath(), Data.OldObjectPath);
			Items.emplace_back(Data);
		}
		bool Success = true;
		std::vector<FAssetRenameData> Completed;
		for (auto& Item : Items)
		{
			FAssetReferenceFixup Facts;
			(void)RenameAndPersist(Item, Facts);
			Success &= !Item.bRenameFailed;
			if (Item.bIdentityChanged || (!Item.bRenameFailed && Item.bOnlyFixSoftReferences)) Completed.push_back(Item);
		}
		if (!Completed.empty()) PostRename.Broadcast(Completed);
		return Success;
	}
	auto IsEditorFixupInProgress() -> bool { check(IsInGameThread()); return Fixing; }
	auto FixupReferencersWithEditorPolicy(std::span<DAssetRedirector* const> Objects, ERedirectFixupMode Mode) -> void
	{
		check(IsInGameThread());
		if (Fixing || Renaming) { Reject("An asset identity operation is already running."); return; }
		if (Objects.empty()) return;
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) { Reject(Guard.error().ToString()); return; }
		if (!CaptureAssetReferenceIndex().IsComplete()) { Reject("Fix-up requires complete asset discovery and reference indexing."); return; }
		Fixing = true;
		struct FReset { ~FReset() { Fixing = false; } } Reset;
		std::vector<TStrongObjectPtr<DAssetRedirector>> Selection;
		std::vector<FStrongObjectPtr> SiblingOwners;
		for (auto* Object : Objects)
			if (Object && !Object->IsGarbage() && std::ranges::none_of(Selection, [&](const auto& Owner) { return Owner.Get() == Object; })) Selection.emplace_back(Object);
		std::vector<FPackagePath> Paths;
		FAssetReferenceFixup Facts;
		for (const auto& Owner : Selection)
		{
			auto* Alias = Owner.Get();
			auto* Package = Alias->GetPackage();
			auto* Destination = Alias->GetDestinationObject();
			if (!Package || !Destination) { Reject("A selected redirector has no live destination."); continue; }
			FSoftObjectPath Old, New;
			if (!FSoftObjectPath::TryCreate(Alias->GetObjectPath(), Old) || !FSoftObjectPath::TryCreate(Destination->GetObjectPath(), New)) { Reject("A selected redirector has an invalid identity."); continue; }
			for (auto* Export : Package->GetTopLevelAssets())
				if (std::ranges::none_of(Selection, [&](const auto& Selected) { return Selected.Get() == Export; })) SiblingOwners.emplace_back(Export);
			if (!SaveRenamed(Destination)) continue;
			FAssetRenameDataWithReferencers Repair{FAssetRenameData(Old, New, true)};
			if (!RenameAndPersist(Repair, Facts, true)) continue; // Preserve the alias after any failed referencer save.
			const auto Exports = Package->GetTopLevelAssets();
			const bool WholeAliasPackage = std::ranges::all_of(Exports, [&](auto* Export) {
				auto* Other = Cast<DAssetRedirector>(Export);
				return Other && Other->GetDestinationObject() && Other->GetDestinationObject()->GetPackage() == Destination->GetPackage()
					&& std::ranges::any_of(Selection, [&](const auto& Selected) { return Selected.Get() == Export; });
			});
			if (WholeAliasPackage)
			{
				if (!std::ranges::contains(Paths, Old.GetPackagePath())) Paths.push_back(Old.GetPackagePath());
				continue;
			}
			if (Mode == ERedirectFixupMode::LeaveFixedUpRedirectors) continue;
			const auto Stores = Facts.GetStores();
			if (!Stores) { Reject(Stores.error().ToString()); continue; }
			if (std::ranges::any_of((*Stores)->Stores, [&](const auto& Store) { return std::ranges::any_of(Store.Occurrences, [&](const auto& Occurrence) { return Occurrence.TargetPath == Old.GetPackagePath(); }); }))
			{ Reject("A package-valued external store cannot distinguish the selected redirector from its siblings; the alias is retained."); continue; }
			if (Alias->HasAnyInternalFlags(EObjectInternalFlags::RootSet) || Private::GetStrongObjectReferenceCount(FObjectKey(Alias)) > 1)
			{ Reject("A selected redirector is retained by a native owner."); continue; }
			class FReferences final : public FReferenceCollector
			{
			public:
				DObject* Target = nullptr;
				bool Found = false;
				auto AddReferencedObject(DObject*& Value) -> void override { Found |= Value == Target; }
			} References;
			References.Target = Alias;
			Private::AddFunctionParameterReferences(References);
			for (auto* Live : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly)) if (Live != Alias) Live->AddReferencedObjects(References);
			if (References.Found) { Reject("A selected redirector is still referenced by a live object."); continue; }
			if (HasPersistentAliasReferences(Old, Facts)) { Reject("Persistent references still require the selected redirector; it is retained."); continue; }
			Alias->SetTransient(true);
			Package->UnregisterTopLevelAsset(Alias);
			Package->MarkDirty();
			FAssetWriteResult Written;
			const bool Saved = SavePackage(Package, {}, &Written);
			if (!Saved && Written.Effect != EAssetWriteEffect::ContentCommittedProjectionPending)
			{
				Alias->SetTransient(false);
				Alias->Rename(Alias->GetFName(), Package);
				Reject(Written.Error ? Written.Error->ToString() : "Could not save the remaining sibling assets.");
				continue;
			}
			MarkObjectHierarchyAsGarbage(Alias);
			RequestGarbageCollection();
			if (!Saved) Reject(Written.Error ? Written.Error->ToString() : "Sibling package saved; catalog synchronization is pending.");
		}
		Selection.clear();
		SiblingOwners.clear();
		if (Paths.empty()) return;
		if (Mode == ERedirectFixupMode::DeleteFixedUpRedirectors)
		{
			std::vector<DPackage*> Packages;
			for (const auto& Path : Paths) if (auto* Package = FindResidentPackage(Path)) Packages.push_back(Package);
			std::string Error;
			DPackageTools::UnloadPackages(Packages, Error);
		}
		const auto Result = FixupPackageAliases(Paths, Mode == ERedirectFixupMode::DeleteFixedUpRedirectors
			? EPackageAliasFixupMode::RewriteAndDelete : EPackageAliasFixupMode::RewriteOnly, &Facts);
		if (!Result.Result) FFeedbackContext::Get().Error(Result.Result.Error ? Result.Result.Error->ToString() : "Redirector fix-up failed.");
		for (const auto& Backup : Result.BackupLocations) FFeedbackContext::Get().Warning("Recovery backup: " + Backup.generic_string());
	}
}
