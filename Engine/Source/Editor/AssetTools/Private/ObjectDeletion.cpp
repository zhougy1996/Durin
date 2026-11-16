#include "ObjectTools.h"
#include "AssetDeleteModel.h"
#include "AssetTools/DeleteItemsHost.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "Misc/FeedbackContext.h"
#include "Misc/ScopedSlowTask.h"
#include "Threading/RunnableThread.h"

namespace Durin::ObjectTools
{
	namespace
	{
		FDeletePrompt DeletePrompt;
		bool Deleting = false;
		// Keeps loading, scanning and confirmation callbacks inside one non-reentrant attempt.
		struct FDeleteScope
		{
			bool Entered = !Deleting;
			FDeleteScope() { if (Entered) Deleting = true; }
			~FDeleteScope() { if (Entered) Deleting = false; }
		};

		auto DeleteModel(FAssetDeleteModel& Model, const FDeleteItemsParameters& Selection,
			const Private::FDeleteAdmission& CanExecute) -> FDeleteItemsResult
		{
			if (!Private::ScanDeleteModel(Model, Selection.AllowCancelDuringDelete)) return {};
			if (!Model.CanDelete()) { FFeedbackContext::Get().Error(Model.GetProgressText()); return {}; }
			if (CanExecute && !CanExecute()) return {};
			if (Selection.bShowConfirmation && !Private::ConfirmDelete(Model, {}, Selection.AllowCancelDuringDelete)) return {};
			if (CanExecute && !CanExecute()) return {};
			Model.DoDelete();
			return {.ObjectsDeleted = Model.GetDeletedObjectCount()};
		}
	}
	auto SetDeletePrompt(FDeletePrompt Prompt) -> void { CheckGameThread(); DeletePrompt = std::move(Prompt); }
	auto Private::ScanDeleteModel(FAssetDeleteModel& Model, EAllowCancelDuringDelete AllowCancel) -> bool
	{
		CheckGameThread();
		if (Model.GetState() == FAssetDeleteModel::EState::Finished) return true;
		const bool CanCancel = AllowCancel == EAllowCancelDuringDelete::AllowCancel;
		FScopedSlowTask Progress(1., "Verifying Delete");
		Progress.MakeDialog(CanCancel);
		while (Model.GetState() != FAssetDeleteModel::EState::Finished)
		{
			if (CanCancel && Progress.ShouldCancel()) { Progress.Cancel(); return false; }
			Model.Tick(0.f);
			Progress.SetProgress(Model.GetProgress(), Model.GetProgressText());
		}
		if (CanCancel && Progress.ShouldCancel()) { Progress.Cancel(); return false; }
		return true;
	}
	auto Private::ConfirmDelete(FAssetDeleteModel& Model, std::span<const std::string> Files, EAllowCancelDuringDelete AllowCancel) -> bool
	{
		CheckGameThread();
		const auto Prompt = DeletePrompt;
		if (!Prompt) { FFeedbackContext::Get().Error("Deletion requires a confirmation host."); return false; }
		return Prompt(Model, Files, AllowCancel);
	}
	auto DeleteObjects(std::span<DObject* const> Objects, bool ShowConfirmation, EAllowCancelDuringDelete AllowCancel) -> int32
	{
		CheckGameThread();
		FDeleteItemsParameters Parameters;
		Parameters.ObjectsToDelete.assign(Objects.begin(), Objects.end());
		Parameters.bShowConfirmation = ShowConfirmation;
		Parameters.AllowCancelDuringDelete = AllowCancel;
		return DeleteItems(Parameters).ObjectsDeleted;
	}
	auto DeleteAssets(std::span<const FAssetData> Assets, bool ShowConfirmation) -> int32
	{
		CheckGameThread();
		FDeleteScope Scope;
		if (!Scope.Entered) return 0;
		const std::vector<FAssetData> Selection(Assets.begin(), Assets.end());
		// Retain exact exports during loading callbacks, then share DeleteItems orchestration.
		FAssetDeleteModel Model(std::span<DObject* const>{});
		for (const auto& Asset : Selection)
		{
			auto* Package = FindResidentPackage(Asset.AssetPath.GetPackagePath());
			if (!Package) Package = LoadPackage(Asset.AssetPath.GetPackagePath());
			auto* Object = Package ? Package->FindTopLevelAsset(FName(Asset.AssetPath.GetAssetName())) : nullptr;
			if (!Object) return 0;
			Model.AddObjectToDelete(Object);
		}
		FDeleteItemsParameters Parameters;
		Parameters.bShowConfirmation = ShowConfirmation;
		return DeleteModel(Model, Parameters, {}).ObjectsDeleted;
	}
	auto DeleteItems(const FDeleteItemsParameters& Parameters) -> FDeleteItemsResult
	{
		CheckGameThread();
		FDeleteScope Scope;
		if (!Scope.Entered) return {};
		const auto Selection = Parameters;
		if (!Selection.FoldersToDelete.empty())
			return Private::DeleteObjectsAndFolders(Selection);
		FAssetDeleteModel Model(Selection.ObjectsToDelete);
		return DeleteModel(Model, Selection, {});
	}
	auto Private::DeleteObjects(FAssetDeleteModel& Model, const FDeleteAdmission& CanExecute) -> int32
	{
		CheckGameThread();
		FDeleteScope Scope;
		if (!Scope.Entered || (CanExecute && !CanExecute())) return 0;
		return DeleteModel(Model, {}, CanExecute).ObjectsDeleted;
	}
}
