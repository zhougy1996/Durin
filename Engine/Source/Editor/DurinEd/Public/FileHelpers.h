#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	class DPackage;
	class DWorld;

	// Observes this lexical batch's physical saves; nested batches require their own observer.
	class FScopedPackageSaveObserver
	{
	public:
		DURINED_API explicit FScopedPackageSaveObserver(std::function<void(const FPackagePath&)> OnSaved);
		DURINED_API ~FScopedPackageSaveObserver();
		FScopedPackageSaveObserver(const FScopedPackageSaveObserver&) = delete;
		auto operator=(const FScopedPackageSaveObserver&) -> FScopedPackageSaveObserver& = delete;
	private:
		friend class DEditorLoadingAndSavingUtils;
		FScopedPackageSaveObserver* Previous;
		std::function<void(const FPackagePath&)> Callback;
	};

	// Editor loading and saving policy. Filtering an empty set succeeds; failures preserve prior committed files.
	class DEditorLoadingAndSavingUtils
	{
	public:
		// Synchronous, noninteractive load; nullptr means failure, including blank-map recovery.
		DURINED_API static auto LoadMap(const std::string& Filename) -> DWorld*;
		DURINED_API static auto SavePackages(std::span<DPackage* const> Packages, bool bOnlyDirty) -> bool;
		DURINED_API static auto SavePackagesWithDialog(std::span<DPackage* const> Packages, bool bOnlyDirty) -> bool;
		DURINED_API static auto SaveDirtyPackages(bool bSaveMapPackages, bool bSaveContentPackages) -> bool;
		DURINED_API static auto SaveDirtyPackagesWithDialog(bool bSaveMapPackages, bool bSaveContentPackages) -> bool;
	};

	// Prompt outcomes describe the user decision separately from partial file persistence.
	class FEditorFileUtils
	{
	public:
		// Loaded, dirty asset packages eligible for saving; excludes graph-private packages.
		DURINED_API static auto GetDirtyPackages(bool bSaveMapPackages, bool bSaveContentPackages) -> std::vector<DPackage*>;
		// Loads a saved map by object or package path; never prompts to save dirty packages.
		DURINED_API static auto LoadMap(const std::string& Filename) -> bool;
		enum EPromptReturnCode { PR_Success, PR_Failure, PR_Declined, PR_Cancelled };
		struct FPromptForCheckoutAndSaveParams
		{
			bool bCheckDirty = false;
			bool bPromptToSave = false;
			bool bAlreadyCheckedOut = false;
			bool bCanBeDeclined = true;
			bool bIsExplicitSave = false;
			std::string Title = "Save Content";
			std::string Message = "Select Content to Save";
			std::vector<DPackage*>* OutFailedPackages = nullptr;
		};
		// The host must outlive its binding. An unavailable requested prompt cancels.
		using FSavePrompt = std::function<EPromptReturnCode(std::span<DPackage* const>, const FPromptForCheckoutAndSaveParams&)>;
		DURINED_API static auto SetSavePrompt(FSavePrompt Prompt) -> void;
		[[nodiscard]] DURINED_API static auto SaveDirtyPackages(bool bPromptUserToSave,
			bool bSaveMapPackages, bool bSaveContentPackages) -> bool;
		DURINED_API static auto PromptForCheckoutAndSave(std::span<DPackage* const> Packages,
			FPromptForCheckoutAndSaveParams& Params) -> EPromptReturnCode;
		DURINED_API static auto PromptForCheckoutAndSave(std::span<DPackage* const> Packages,
			bool bCheckDirty, bool bPromptToSave, std::vector<DPackage*>* OutFailedPackages = nullptr,
			bool bAlreadyCheckedOut = false, bool bCanBeDeclined = true) -> EPromptReturnCode;
		DURINED_API static auto PromptForCheckoutAndSave(std::span<DPackage* const> Packages,
			bool bCheckDirty, bool bPromptToSave, std::string_view Title, std::string_view Message,
			std::vector<DPackage*>* OutFailedPackages = nullptr, bool bAlreadyCheckedOut = false,
			bool bCanBeDeclined = true) -> EPromptReturnCode;
	};
}
