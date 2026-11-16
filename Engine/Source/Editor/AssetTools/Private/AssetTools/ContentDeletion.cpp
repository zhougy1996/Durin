#include "AssetTools/FileDeletionState.h"
#include "AssetTools/DeleteItemsHost.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Package.h"

namespace Durin
{
	using namespace ContentStorage;
	auto ObjectTools::Private::DeleteObjectsAndFolders(const ObjectTools::FDeleteItemsParameters& Parameters) -> ObjectTools::FDeleteItemsResult
	{
		std::vector<FFileDeletionInput> Inputs;
		for (const auto& Folder : Parameters.FoldersToDelete)
		{
			const auto Mapped = FContentMountPaths::VirtualToPhysical(Folder);
			Inputs.push_back({std::filesystem::path(Folder).filename().generic_string(), Mapped.empty() ? Folder : Mapped});
		}
		FAssetDeleteModel Objects(Parameters.ObjectsToDelete);
		const auto Assets = CaptureAssetQuerySnapshot();
		for (const auto& [Identity, Asset] : Assets.Assets)
		{
			const auto Resolved = FMountPaths::ResolveAssetPath(Identity.GetPackagePath().GetView());
			if (!Resolved) continue;
			const auto Physical = Resolved->PhysicalPath.generic_string() + ".dasset";
			const bool Covered = std::ranges::any_of(Inputs, [&](const auto& Folder) {
				std::filesystem::path Relative;
				return FPaths::TryMakeLexicalRelativePath(Physical, Folder.PhysicalPath, Relative);
			});
			if (!Covered) continue;
			auto* Package = FindResidentPackage(Identity.GetPackagePath());
			if (!Package) Package = LoadPackage(Identity.GetPackagePath());
			auto* Object = Package ? Package->FindTopLevelAsset(FName(Identity.GetAssetName())) : nullptr;
			if (!Object) return ObjectTools::FDeleteItemsResult{};
			Objects.AddObjectToDelete(Object);
		}
		if (!ObjectTools::Private::ScanDeleteModel(Objects, Parameters.AllowCancelDuringDelete)) return {};
		const bool HasObjects = !Objects.GetPendingDeletedAssets().empty();
		if (HasObjects && !Objects.CanDelete()) return ObjectTools::FDeleteItemsResult{};
		// File retains the immutable ordinary-file scope while this one model
		// retires the selected assets. Only confirmed asset removals may differ.
		auto Files = AssetToolsPrivate::FAssetDeleteModelAccess::Create(Inputs, {}, false, true);
		auto& State = AssetToolsPrivate::FAssetDeleteModelAccess::GetState(*Files);
		const auto& Snapshot = State.Snapshot;
		if (!Files->CanDelete()) return ObjectTools::FDeleteItemsResult{};
		std::vector<std::string> Paths;
		std::unordered_set<std::string> Folders;
		for (const auto& Entry : Snapshot.Entries)
		{
			Paths.push_back(Entry.PhysicalPath);
			if (Entry.Kind == EFileDeletionEntryKind::Directory && std::ranges::any_of(Inputs, [&](const auto& Input) {
				return AreSamePath(Input.PhysicalPath, Entry.PhysicalPath);
			})) Folders.insert(Entry.PhysicalPath);
		}
		for (const auto& Warning : Snapshot.Warnings) Paths.push_back(Warning.Details);
		if (Parameters.bShowConfirmation && !ObjectTools::Private::ConfirmDelete(Objects, Paths, Parameters.AllowCancelDuringDelete)) return ObjectTools::FDeleteItemsResult{};
		if (!State.ValidateScope()) return ObjectTools::FDeleteItemsResult{};
		State.PeerScopeValidated = true;
		if (HasObjects && !Objects.DoDelete()) return ObjectTools::FDeleteItemsResult{};
		Files->DoDelete();
		const auto& Effects = State.Effects;
		ObjectTools::FDeleteItemsResult Result{.ObjectsDeleted = Objects.GetDeletedObjectCount()};
		for (const auto& Effect : Effects.Effects)
			if (Effect.Status == EFileDeletionEffectStatus::Committed && Folders.contains(Effect.PhysicalPath)) ++Result.FoldersDeleted;
		return Result;
	}

}
namespace Durin::ContentStorage
{
	auto DeleteFiles(std::span<const FFileDeletionInput> Inputs, const FContentMutationPolicy& Policy, bool Assets, bool Files, bool ShowConfirmation, ObjectTools::EAllowCancelDuringDelete AllowCancel) -> FFileDeletionResult
	{
		auto Model = AssetToolsPrivate::FAssetDeleteModelAccess::Create(Inputs, Policy, Assets, Files, AllowCancel);
		auto& State = AssetToolsPrivate::FAssetDeleteModelAccess::GetState(*Model);
		if (!Model->CanDelete())
		{
			for (const auto& Blocker : State.Snapshot.Blockers) State.Report(ELogLevel::Error, Blocker.Details);
			return {.bStopped = true};
		}
		std::vector<std::string> Paths;
		for (const auto& Entry : State.Snapshot.Entries) Paths.push_back(Entry.PhysicalPath);
		for (const auto& Warning : State.Snapshot.Warnings) Paths.push_back(Warning.Details);
		if (ShowConfirmation && !ObjectTools::Private::ConfirmDelete(*Model, Paths, AllowCancel)) return {.bCancelled = true};
		Model->DoDelete();
		return State.Effects;
	}
}
