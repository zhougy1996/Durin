#include "ObjectTools.h"
#include "AssetDeleteModel.h"
#include "AssetTools/DeleteItemsHost.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "Misc/FeedbackContext.h"
#include "Threading/RunnableThread.h"

namespace Durin::ObjectTools
{
	namespace
	{
		FDeletePrompt DeletePrompt;
		Private::FFolderDeleteHandler FolderDeleteHandler;
		bool Deleting = false;
	}
	auto SetDeletePrompt(FDeletePrompt Prompt) -> void { CheckGameThread(); DeletePrompt = std::move(Prompt); }
	auto Private::SetFolderDeleteHandler(FFolderDeleteHandler Handler) -> void
	{ CheckGameThread(); FolderDeleteHandler = std::move(Handler); }
	auto ConfirmDelete(FAssetDeleteModel& Model, std::span<const std::string> Files, EAllowCancelDuringDelete AllowCancel) -> bool
	{
		CheckGameThread();
		const auto Prompt = DeletePrompt;
		if (!Prompt) { FFeedbackContext::Get().Error("Deletion requires a confirmation host."); return false; }
		return Prompt(Model, Files, AllowCancel);
	}
	auto DeleteObjects(std::span<DObject* const> Objects, bool ShowConfirmation, EAllowCancelDuringDelete AllowCancel) -> int32
	{
		CheckGameThread();
		if (Deleting) return 0;
		Deleting = true;
		struct FReset { ~FReset() { Deleting = false; } } Reset;
		FAssetDeleteModel Model(Objects);
		while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
		if (!Model.CanDelete()) { FFeedbackContext::Get().Error(Model.GetProgressText()); return 0; }
		if (ShowConfirmation && !ConfirmDelete(Model, {}, AllowCancel)) return 0;
		Model.DoDelete();
		return Model.GetDeletedObjectCount();
	}
	auto DeleteAssets(std::span<const FAssetData> Assets, bool ShowConfirmation) -> int32
	{
		CheckGameThread();
		if (Deleting) return 0;
		Deleting = true;
		struct FReset { ~FReset() { Deleting = false; } } Reset;
		const std::vector<FAssetData> Selection(Assets.begin(), Assets.end());
		FAssetDeleteModel Model(std::span<DObject* const>{});
		for (const auto& Asset : Selection)
		{
			auto* Package = FindResidentPackage(Asset.AssetPath.GetPackagePath());
			if (!Package) Package = LoadPackage(Asset.AssetPath.GetPackagePath());
			auto* Object = Package ? Package->FindTopLevelAsset(FName(Asset.AssetPath.GetAssetName())) : nullptr;
			if (!Object) return 0;
			Model.AddObjectToDelete(Object);
		}
		while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
		if (!Model.CanDelete()) { FFeedbackContext::Get().Error(Model.GetProgressText()); return 0; }
		if (ShowConfirmation && !ConfirmDelete(Model, {})) return 0;
		Model.DoDelete();
		return Model.GetDeletedObjectCount();
	}
	auto DeleteItems(const FDeleteItemsParameters& Parameters) -> FDeleteItemsResult
	{
		CheckGameThread();
		const auto Selection = Parameters;
		if (Selection.FoldersToDelete.empty())
			return {.ObjectsDeleted = DeleteObjects(Selection.ObjectsToDelete, Selection.bShowConfirmation, Selection.AllowCancelDuringDelete)};
		const auto Handler = FolderDeleteHandler;
		if (!Handler || Deleting) return {};
		Deleting = true;
		struct FReset { ~FReset() { Deleting = false; } } Reset;
		return Handler(Selection);
	}
}
