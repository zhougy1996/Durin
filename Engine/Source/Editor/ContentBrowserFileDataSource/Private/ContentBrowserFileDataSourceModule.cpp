#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserFileProviderSubsystem.h"
#include "Editor/EditorEngine.h"
#include "AssetTools/DeleteItemsHost.h"
#include "AssetDeleteModel.h"
#include "Data/FileDeletionRequest.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Asset/Load.h"
#include "Misc/Paths.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/MountPaths.h"
namespace Durin
{
	class FContentBrowserFileDataSourceModule final : public IModuleInterface
	{
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
	public:
		auto StartupModule() -> void override {
			ObjectTools::Private::SetFolderDeleteHandler([](const ObjectTools::FDeleteItemsParameters& Parameters) {
				std::vector<ContentStorage::FFileDeletionInput> Inputs;
				for (const auto& Folder : Parameters.FoldersToDelete)
				{
					const auto Mapped = ContentStorage::FContentMountPaths::VirtualToPhysical(Folder);
					Inputs.push_back({std::filesystem::path(Folder).filename().generic_string(), Mapped.empty() ? Folder : Mapped});
				}
				FAssetDeleteModel Objects(Parameters.ObjectsToDelete);
				const auto Snapshot = CaptureAssetQuerySnapshot();
				for (const auto& [Identity, Asset] : Snapshot.Assets)
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
				while (Objects.GetState() != FAssetDeleteModel::EState::Finished) Objects.Tick(0.f);
				const bool HasObjects = !Objects.GetPendingDeletedAssets().empty();
				if (HasObjects && !Objects.CanDelete()) return ObjectTools::FDeleteItemsResult{};
				// File retains the immutable ordinary-file scope while this one model
				// retires the selected assets. Only confirmed asset removals may differ.
				ContentStorage::FFileDeletionRequest Files({}, false, true);
				auto Plan = Files.PrepareDeleteItems(Inputs);
				if (!Plan || !Plan->CanExecute()) return ObjectTools::FDeleteItemsResult{};
				std::vector<std::string> Paths;
				std::unordered_set<std::string> Folders;
				for (const auto& Entry : Plan->Entries)
				{
					Paths.push_back(Entry.PhysicalPath);
					if (Entry.Kind == ContentStorage::EFileDeletionEntryKind::Directory) Folders.insert(Entry.PhysicalPath);
				}
				for (const auto& Warning : Plan->Warnings) Paths.push_back(Warning.Details);
				if (Parameters.bShowConfirmation && !ObjectTools::ConfirmDelete(Objects, Paths, Parameters.AllowCancelDuringDelete)) return ObjectTools::FDeleteItemsResult{};
				if (!Files.ValidateForExecution()) return ObjectTools::FDeleteItemsResult{};
				if (HasObjects && !Objects.DoDelete()) return ObjectTools::FDeleteItemsResult{};
				const auto Effects = Files.DeleteItems(Plan);
				ObjectTools::FDeleteItemsResult Result{.ObjectsDeleted = Objects.GetDeletedObjectCount()};
				for (const auto& Effect : Effects.Effects)
					if (Effect.Status == ContentStorage::EFileDeletionEffectStatus::Committed && Folders.contains(Effect.PhysicalPath)) ++Result.FoldersDeleted;
				return Result;
			});
			Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = DContentBrowserFileProviderSubsystem::StaticClass(), .Provider = FName("ContentBrowserFileDataSource")}); }
		auto ShutdownModule() -> void override { ObjectTools::Private::SetFolderDeleteHandler({}); if (GEditor) GEditor->RemoveEditorSubsystems(FName("ContentBrowserFileDataSource")); Registration.reset(); }
	};
	IMPLEMENT_MODULE(FContentBrowserFileDataSourceModule, ContentBrowserFileDataSource)
}
