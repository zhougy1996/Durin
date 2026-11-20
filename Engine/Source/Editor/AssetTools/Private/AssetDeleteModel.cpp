#include "AssetDeleteModel.h"
#include "AssetTools/AssetCompanionOwnership.h"
#include "AssetTools/NativeAssetDeletion.h"
#include "AssetTools/AssetDeletionInternal.h"
#include "AssetTools/FileDeletionState.h"
#include "Asset/Load.h"
#include "Asset/MutationExtensions.h"
#include "Asset/PackageEditing.h"
#include "Asset/PackageSerialization.h"
#include "AssetRegistry/Publication.h"
#include "AssetRegistry/References.h"
#include "DObject/Package.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/FileHelper.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto IsWithin(const DObject* Value, const DObject* Root) -> bool
		{ for (; Value; Value = Value->GetOuter()) if (Value == Root) return true; return false; }
		auto SafeFile(const std::filesystem::path& Path) -> bool
		{
			const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Path.generic_string());
			if (!Mount || !Mount.Mount->bContentWritable) return false;
			std::error_code Error;
			std::filesystem::path Reparse;
			if (ContentStorage::FindReparsePointInPath(Mount.Mount->GetContentDir(), Path, Reparse, Error) || Error) return false;
			const auto Status = std::filesystem::status(Path, Error);
			return !Error && std::filesystem::is_regular_file(Status)
				&& (Status.permissions() & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
		}
	}

	FPendingDelete::FPendingDelete(DObject* InObject) : Object(InObject) {}
	auto FPendingDelete::CheckForReferences(std::span<DObject* const> Roots,
		std::span<const FStrongObjectPtr* const> Owners) -> void
	{
		const std::array DefaultRoots{GetObject()};
		const std::array<const FStrongObjectPtr*, 1> DefaultOwners{&Object};
		if (Roots.empty()) Roots = DefaultRoots;
		if (Owners.empty()) Owners = DefaultOwners;
		DiskReferences.clear();
		RemainingDiskReferences = RemainingMemoryReferences = 0;
		auto* Target = GetObject();
		if (!IsValid(Target)) { bNativeOwner = true; return; }
		bNativeOwner = FindObjectReferencers(Target, Roots, MemoryReferences, Owners);
		for (const auto& Ref : MemoryReferences.ExternalReferences) RemainingMemoryReferences += Ref.TotalReferences;
		auto* Package = Target->GetPackage();
		if (!Package) { bNativeOwner = true; return; }
		FTopLevelAssetPath TargetPath;
		if (!FTopLevelAssetPath::TryCreate(Package->GetPackagePathIdentity(), Target->GetName(), TargetPath))
		{ bNativeOwner = true; return; }
		const auto Snapshot = CaptureAssetRegistrySnapshot();
		std::unordered_set<FPackagePath> Sources{Package->GetPackagePathIdentity()};
		for (const auto& Edge : Snapshot.References.FindReferencers(Package->GetPackagePathIdentity()))
			if (Edge.Kind != EAssetReferenceKind::SoftObject) Sources.insert(Edge.SourcePackage);
		for (const auto& Source : Sources)
		{
			const auto* Entry = Snapshot.Catalog.FindExact(Source);
			if (!Entry) continue;
			FAssetPackageInspection Inspection;
			if (!InspectAssetPackage(Entry->PhysicalPath, Source, Inspection))
			{ DiskReferences.push_back(Source); continue; }
			bool Referenced = false;
			const auto TopLevel = [&](const FAssetPackageObjectInspection* Record) {
				for (size_t Depth = 0; Record && Record->OuterId && Depth < Inspection.Objects.size(); ++Depth)
					Record = Inspection.FindObject(Record->OuterId);
				return Record;
			};
			std::function<void(const FAssetPackageField&)> Visit = [&](const auto& Field) {
				const auto Check = [&](const FAssetPackageObjectReference& Ref) {
					if (Ref.Kind == EAssetPackageObjectReferenceKind::External)
						Referenced |= Ref.ExternalPath.GetAssetPath() == TargetPath;
					else if (Source == Package->GetPackagePathIdentity() && Ref.Kind == EAssetPackageObjectReferenceKind::Internal)
						if (const auto* ObjectRecord = TopLevel(Inspection.FindObject(Ref.ObjectId)))
							Referenced |= ObjectRecord->ObjectName == Target->GetName();
				};
				FAssetPackageObjectReference Ref;
				if (Field.TryReadObjectReference(Ref)) Check(Ref);
				std::vector<FAssetPackageObjectReference> Refs;
				if (Field.TryReadObjectReferenceArray(Refs)) for (const auto& Value : Refs) Check(Value);
				std::vector<FAssetPackageField> Fields;
				if (Field.TryInspectStructFields(Fields)) for (const auto& Value : Fields) Visit(Value);
				std::vector<std::vector<FAssetPackageField>> Elements;
				if (Field.TryInspectStructArray(Elements)) for (const auto& Element : Elements) for (const auto& Value : Element) Visit(Value);
			};
			for (const auto& Record : Inspection.Objects)
			{
				if (std::ranges::any_of(Roots, [&](auto* Root) {
					const auto* Export = TopLevel(&Record);
					return Root->GetPackage()->GetPackagePathIdentity() == Source && Export && Export->ObjectName == Root->GetName();
				})) continue;
				for (const auto& Field : Record.Fields) Visit(Field);
			}
			if (Referenced) DiskReferences.push_back(Source);
		}
		RemainingDiskReferences = static_cast<int32>(DiskReferences.size());
	}

	struct FAssetDeleteModel::FImpl
	{
		EState State = EState::Waiting;
		std::vector<std::unique_ptr<FPendingDelete>> Pending;
		std::vector<DObject*> Roots;
		std::vector<const FStrongObjectPtr*> Owners;
		FAssetDeletionOperation WholePackages;
		struct FPartial { FWeakObjectPtr Package; std::vector<FWeakObjectPtr> Objects; uint64 EditRevision; bool bDirty; std::vector<std::filesystem::path> Files; };
		std::vector<FPartial> Partial;
		struct FShape { FWeakObjectPtr Package; uint64 Revision; std::vector<DObject*> Exports; };
		std::vector<FShape> Shapes;
		std::unordered_map<std::string, FXxHash128> Bytes;
		FAssetReferenceStoreCapture Stores;
		uint64 RegistryRevision = 0;
		size_t Scanned = 0;
		int32 Deleted = 0;
		bool bAllowed = false, bAttempted = false, bDispatching = false, bNativeTransferred = false;
		std::string Diagnostic;
		std::unique_ptr<AssetToolsPrivate::FFileDeletionState> FileDeletion;
	};
	FAssetDeleteModel::FAssetDeleteModel(std::span<DObject* const> Objects) : Impl(std::make_unique<FImpl>())
	{ for (auto* Object : Objects) AddObjectToDelete(Object); }
	FAssetDeleteModel::~FAssetDeleteModel() = default;
	auto AssetToolsPrivate::FAssetDeleteModelAccess::Create(std::span<const ContentStorage::FFileDeletionInput> Inputs, const ContentStorage::FContentMutationPolicy& Policy, bool Assets, bool Files, ObjectTools::EAllowCancelDuringDelete AllowCancel) -> std::unique_ptr<FAssetDeleteModel>
	{
		const std::vector<ContentStorage::FFileDeletionInput> Selection(Inputs.begin(), Inputs.end());
		auto Model = std::make_unique<FAssetDeleteModel>(std::span<DObject* const>{});
		auto State = std::make_unique<FFileDeletionState>(Policy, Assets, Files);
		State->Analyze(Selection, *Model, AllowCancel);
		Model->Impl->FileDeletion = std::move(State);
		Model->SetState(FAssetDeleteModel::EState::Finished);
		return Model;
	}
	auto AssetToolsPrivate::FAssetDeleteModelAccess::GetState(FAssetDeleteModel& Model) -> FFileDeletionState& { return *Model.Impl->FileDeletion; }
	auto FAssetDeleteModel::TakeWholePackageOperation() -> FAssetDeletionOperation
	{
		check(Impl->Partial.empty());
		Impl->bAttempted = true;
		Impl->bNativeTransferred = true;
		return std::move(Impl->WholePackages);
	}
	auto FAssetDeleteModel::AddObjectToDelete(DObject* Object) -> void
	{
		CheckGameThread();
		if (Impl->FileDeletion) return;
		if (Impl->bAttempted || Impl->bDispatching || !IsValid(Object) || !Object->GetPackage()
			|| Object->GetOuter() != Object->GetPackage()) return;
		if (std::ranges::any_of(Impl->Pending, [Object](const auto& P) { return P->GetObject() == Object; })) return;
		Impl->Pending.push_back(std::make_unique<FPendingDelete>(Object));
		Impl->Scanned = 0;
		Impl->bAllowed = false;
		SetState(EState::Waiting);
	}
	auto FAssetDeleteModel::SetState(EState State) -> void
	{
		if (Impl->State == State) return;
		Impl->State = State;
		Impl->bDispatching = true;
		StateChanged.Broadcast(State);
		Impl->bDispatching = false;
	}
	auto FAssetDeleteModel::Tick(float) -> void
	{
		CheckGameThread();
		if (Impl->FileDeletion) return;
		if (Impl->bDispatching || Impl->bAttempted) return;
		if (Impl->State != EState::Waiting && Impl->State != EState::StartScanning)
		{
			Impl->Roots.clear();
			for (const auto& Pending : Impl->Pending)
				if (auto* Object = Pending->GetObject()) Impl->Roots.push_back(Object); else Impl->bAllowed = false;
		}
		auto& S = *Impl;
		if (S.State == EState::Waiting) { SetState(EState::StartScanning); return; }
		if (S.State == EState::StartScanning)
		{
			S.Roots.clear(); S.Owners.clear(); S.Partial.clear(); S.Shapes.clear(); S.Bytes.clear(); S.Diagnostic.clear();
			for (const auto& Pending : S.Pending) { S.Roots.push_back(Pending->GetObject()); S.Owners.push_back(&Pending->Object); }
			for (auto* Object : S.Roots)
			{
				auto* Package = Object ? Object->GetPackage() : nullptr;
				if (!Package || std::ranges::any_of(S.Shapes, [Package](const auto& Shape) { return Shape.Package.Get() == Package; })) continue;
				S.Shapes.push_back({FWeakObjectPtr(Package), Package->GetEditRevision(), {Package->GetTopLevelAssets().begin(), Package->GetTopLevelAssets().end()}});
			}
			S.RegistryRevision = GetAssetCatalogRevision();
			S.bAllowed = !S.Pending.empty() && bool(CheckAssetPackageMutationAllowed()) && bool(CaptureAssetReferenceStores(S.Stores));
			SetState(EState::Scanning); return;
		}
		if (S.State == EState::Scanning)
		{
			if (S.Scanned < S.Pending.size())
			{
				auto& Pending = *S.Pending[S.Scanned++];
				Pending.CheckForReferences(S.Roots, S.Owners);
				auto* Object = Pending.GetObject();
				S.bAllowed &= IsValid(Object) && !Pending.bNativeOwner && Pending.RemainingMemoryReferences == 0 && Pending.RemainingDiskReferences == 0;
				if (IsValid(Object))
				{
					if (Pending.bNativeOwner) S.Diagnostic += Object->GetObjectPath() + " is retained by an active native owner.\n";
					for (const auto& Path : Pending.DiskReferences) S.Diagnostic += Object->GetObjectPath() + " is referenced by " + Path.ToString() + ".\n";
					for (const auto& Ref : Pending.MemoryReferences.ExternalReferences)
						if (auto* Owner = Ref.Referencer.Get()) S.Diagnostic += Object->GetObjectPath() + " is referenced by live object " + Owner->GetObjectPath() + ".\n";
					S.bAllowed &= Object->GetPackage()->IsAssetPackage() && !Object->GetPackage()->HasAnyInternalFlags(EObjectInternalFlags::RootSet);
					S.bAllowed &= !IsPackageLoading(Object->GetPackage()->GetPackagePathIdentity());
					for (auto* Child : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
						if (IsWithin(Child, Object))
							S.bAllowed &= !Child->HasAnyInternalFlags(EObjectInternalFlags::RootSet)
								&& !(Cast<DLevel>(Child) && Cast<DLevel>(Child)->GetWorld());
				}
				return;
			}
			SetState(EState::UpdateActions); return;
		}
		if (S.State == EState::UpdateActions)
		{
			FAssetDeletionRequest Request;
			Request.RetainedOwners = S.Owners;
			std::unordered_set<DPackage*> Packages;
			for (auto* Object : S.Roots) if (IsValid(Object)) Packages.insert(Object->GetPackage());
			for (auto* Package : Packages)
			{
				std::vector<DObject*> Selected;
				for (auto* Object : S.Roots) if (Object->GetPackage() == Package) Selected.push_back(Object);
				const auto Entry = FindPackageExact(Package->GetPackagePathIdentity());
				if (Entry && Selected.size() == Package->GetTopLevelAssets().size()) Request.AssetPaths.push_back(Entry->PackagePath);
				else
				{
					FImpl::FPartial Partial{FWeakObjectPtr(Package), {}, Package->GetEditRevision(), Package->IsDirty()};
					for (auto* Object : Selected) Partial.Objects.emplace_back(Object);
					S.Partial.push_back(std::move(Partial));
					if (Entry)
					{
						std::vector<std::filesystem::path> Companions;
						if (!AssetToolsPrivate::InspectAssetCompanionFiles(*Entry, Companions)) S.bAllowed = false;
						for (const auto& File : Companions)
						{
							FAssetCompanionOwnership Ownership;
							const auto Identity = FFileHelper::HashFileXx128(File);
							if (!QueryAssetCompanionOwnership(File, Ownership) || Ownership.Owners.size() != 1
								|| Ownership.Owners.front() != Entry->PackagePath || !SafeFile(File) || !Identity) S.bAllowed = false;
							else { S.Bytes.emplace(File.generic_string(), *Identity); S.Partial.back().Files.push_back(File); }
						}
						const auto Hash = FFileHelper::HashFileXx128(Entry->PhysicalPath);
						if (!Hash || !SafeFile(Entry->PhysicalPath)) S.bAllowed = false;
						else { S.Bytes.emplace(Entry->PhysicalPath, *Hash); S.Partial.back().Files.emplace_back(Entry->PhysicalPath); }
					}
				}
			}
			FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) { if (Level >= ELogLevel::Error) S.Diagnostic += std::string(Message) + "\n"; });
			S.bAllowed &= PrepareAssetDeletionWithEditorPolicy(Request, S.WholePackages, &Feedback);
			for (const auto& Entry : S.WholePackages.GetEntries())
			{
				S.bAllowed &= SafeFile(Entry.RegistryEntry.PhysicalPath);
				for (const auto& File : Entry.CompanionFiles) S.bAllowed &= SafeFile(File);
			}
			if (!S.bAllowed && S.Diagnostic.empty()) S.Diagnostic = "Deletion is blocked by a reference, active native owner, loading state or file protection.";
			SetState(EState::Finished);
		}
	}
	auto FAssetDeleteModel::CanDelete() const -> bool
	{ return Impl->FileDeletion ? Impl->FileDeletion->CanDelete() : Impl->State == EState::Finished && Impl->bAllowed && !Impl->bAttempted; }
	auto FAssetDeleteModel::DoDelete() -> bool
	{
		CheckGameThread();
		if (Impl->FileDeletion) return Impl->FileDeletion->DoDelete();
		if (!CanDelete() || Impl->bDispatching) return false;
		auto& S = *Impl;
		// Freeze the attempt before invoking extensible admission callbacks.
		S.bAttempted = true;
		S.Roots.clear();
		for (const auto& Pending : S.Pending)
			if (auto* Object = Pending->GetObject()) S.Roots.push_back(Object); else return false;
		if (S.RegistryRevision != GetAssetCatalogRevision()) return false;
		for (const auto& Shape : S.Shapes)
		{
			auto* Package = Cast<DPackage>(Shape.Package.Get());
			if (!Package || Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet) || IsPackageLoading(Package->GetPackagePathIdentity()) || Package->GetEditRevision() != Shape.Revision
				|| !std::ranges::equal(Package->GetTopLevelAssets(), Shape.Exports)) return false;
		}
		FAssetReferenceStoreCapture Stores;
		if (!CheckAssetPackageMutationAllowed() || !CaptureAssetReferenceStores(Stores) || Stores != S.Stores) return false;
		for (const auto& Pending : S.Pending)
		{
			Pending->CheckForReferences(S.Roots, S.Owners);
			if (Pending->bNativeOwner || Pending->RemainingDiskReferences || Pending->RemainingMemoryReferences) return false;
		}
		for (const auto& [Path, Expected] : S.Bytes)
		{
			const auto Actual = FFileHelper::HashFileXx128(Path);
			if (!SafeFile(Path) || !Actual || *Actual != Expected) return false;
		}
		for (const auto& Shape : S.Shapes)
		{
			auto* Package = Cast<DPackage>(Shape.Package.Get());
			if (!Package || Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet) || IsPackageLoading(Package->GetPackagePathIdentity()) || Package->GetEditRevision() != Shape.Revision
				|| !std::ranges::equal(Shape.Exports, Package->GetTopLevelAssets())) return false;
		}
		for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (std::ranges::any_of(S.Roots, [&](auto* Root) { return IsWithin(Object, Root); })
				&& (Object->HasAnyInternalFlags(EObjectInternalFlags::RootSet)
					|| (Cast<DLevel>(Object) && Cast<DLevel>(Object)->GetWorld()))) return false;
		for (const auto& Entry : S.WholePackages.GetEntries())
		{
			if (!SafeFile(Entry.RegistryEntry.PhysicalPath)) return false;
			for (const auto& Path : Entry.CompanionFiles) if (!SafeFile(Path)) return false;
		}
		bool Accepted = false;
		S.WholePackages.Delete({.Delete = [&](auto& Removed) -> FAssetWriteResult {
			Accepted = true;
			for (const auto& Entry : S.WholePackages.GetEntries())
			{
				std::vector<std::filesystem::path> Files{Entry.RegistryEntry.PhysicalPath};
				Files.insert(Files.end(), Entry.CompanionFiles.begin(), Entry.CompanionFiles.end());
				for (const auto& File : Files)
				{
					const auto Expected = S.WholePackages.GetFileIdentities().find(std::filesystem::absolute(File).lexically_normal().generic_string());
					const auto Actual = FFileHelper::HashFileXx128(File);
					if (!SafeFile(File) || !Actual || Expected == S.WholePackages.GetFileIdentities().end() || *Actual != Expected->second)
						return {.Error = UnifiedError::FError{AssetWriteErrors::StaleData, "Deletion file changed immediately before destruction."},
							.Effect = Removed.empty() ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten};
					std::error_code Error;
					if (!std::filesystem::remove(File, Error) || Error)
						return {.Error = UnifiedError::FError{AssetWriteErrors::IoError, "Could not remove " + File.generic_string()},
							.Effect = Removed.empty() ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten};
					Removed.push_back(File);
				}
			}
			return {};
		}});
		if (!Accepted) return false;
		for (const auto& Partial : S.Partial)
		{
			auto* Package = Cast<DPackage>(Partial.Package.Get());
			if (!Package) break;
			FAssetReferenceStoreCapture CurrentStores;
			bool Safe = bool(CheckAssetPackageMutationAllowed()) && bool(CaptureAssetReferenceStores(CurrentStores)) && CurrentStores == S.Stores;
			Package = Cast<DPackage>(Partial.Package.Get());
			if (!Package) break;
			S.Roots.clear();
			for (const auto& Pending : S.Pending) if (auto* Object = Pending->GetObject()) S.Roots.push_back(Object);
			for (const auto& Object : Partial.Objects) Safe &= Object.IsValid();
			Safe &= !Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet)
				&& Package->GetEditRevision() == Partial.EditRevision && !IsPackageLoading(Package->GetPackagePathIdentity());
			for (const auto& Shape : S.Shapes)
				if (Shape.Package.Get() == Package) Safe &= std::ranges::equal(Shape.Exports, Package->GetTopLevelAssets());
			for (const auto& Pending : S.Pending)
				if (Pending->GetObject() && Pending->GetObject()->GetPackage() == Package)
				{
					Pending->CheckForReferences(S.Roots, S.Owners);
					Safe &= !Pending->bNativeOwner && !Pending->RemainingDiskReferences && !Pending->RemainingMemoryReferences;
				}
			for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
				if (std::ranges::any_of(Partial.Objects, [&](const auto& Root) { return IsWithin(Object, Root.Get()); }))
					Safe &= !Object->HasAnyInternalFlags(EObjectInternalFlags::RootSet)
						&& !(Cast<DLevel>(Object) && Cast<DLevel>(Object)->GetWorld());
			for (const auto& File : Partial.Files)
			{
				const auto Identity = FFileHelper::HashFileXx128(File);
				const auto Expected = S.Bytes.find(File.generic_string());
				Safe &= SafeFile(File) && Identity && Expected != S.Bytes.end() && *Identity == Expected->second;
			}
			if (!Safe) { FFeedbackContext::Get().Error("Remaining deletion participants changed after an earlier committed effect."); break; }
			const FStrongObjectPtr PackageLifetime(Package);
			for (const auto& Object : Partial.Objects) MarkObjectHierarchyAsGarbage(Object.Get());
			Package->MarkDirty();
			if (FindPackageExact(Package->GetPackagePathIdentity()))
			{
				FAssetWriteResult Report;
				if (!SavePackage(Package, {}, &Report))
				{
					const std::array Paths{Package->GetPackagePathIdentity()};
					FenceAssetRegistryProjection(Paths);
					if (Report.Error) FFeedbackContext::Get().Error(Report.Error->ToString());
				}
				if (Partial.bDirty) Package->MarkDirty();
			}
		}

		for (const auto& Pending : S.Pending) if (!Pending->Object.Get()) ++S.Deleted;
		RequestGarbageCollection();
		return Accepted;
	}
	auto FAssetDeleteModel::GetPendingDeletedAssets() const -> std::span<const std::unique_ptr<FPendingDelete>> { return Impl->Pending; }
	auto FAssetDeleteModel::GetDeletedObjectCount() const -> int32
	{
		if (Impl->FileDeletion && !Impl->FileDeletion->Started) return 0;
		return Impl->bNativeTransferred ? static_cast<int32>(std::ranges::count_if(Impl->Pending, [](const auto& Pending) { return !Pending->Object.Get(); })) : Impl->Deleted;
	}
	auto FAssetDeleteModel::GetProgress() const -> float
	{ return Impl->State == EState::Finished ? 1.f : Impl->Pending.empty() ? 0.f : float(Impl->Scanned) / float(Impl->Pending.size() + 1); }
	auto FAssetDeleteModel::GetProgressText() const -> std::string
	{
		if (Impl->FileDeletion)
		{
			const auto& Snapshot = Impl->FileDeletion->Snapshot;
			std::string Text = Snapshot.DisplayName;
			for (const auto& Blocker : Snapshot.Blockers) Text += "\n" + Blocker.Details;
			for (const auto& Warning : Snapshot.Warnings) Text += "\n" + Warning.Details;
			return Text;
		}
		std::string Text = Impl->Diagnostic.empty() ? std::format("{} of {} objects analyzed. Selected unsaved changes will be discarded.", Impl->Scanned, Impl->Pending.size()) : Impl->Diagnostic;
		for (const auto& Warning : Impl->WholePackages.GetWarnings()) Text += "\n" + Warning.Details;
		return Text;
	}
	auto FAssetDeleteModel::GetState() const -> EState { return Impl->State; }
}
