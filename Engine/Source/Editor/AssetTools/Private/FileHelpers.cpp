#include "FileHelpers.h"
#include "Asset/PackageSerialization.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/Level.h"
#include "Misc/FeedbackContext.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		thread_local FScopedPackageSaveObserver* SaveObserver = nullptr;
		FEditorFileUtils::FSavePrompt SavePrompt;

		auto FilterPackages(std::span<DPackage* const> Input, bool OnlyDirty) -> std::vector<DPackage*>
		{
			std::vector<DPackage*> Result;
			for (auto* Package : Input)
				if (Package && Package->IsAssetPackage() && !Package->IsGraphPrivate()
					&& (!OnlyDirty || Package->IsDirty()) && !std::ranges::contains(Result, Package)) Result.push_back(Package);
			return Result;
		}
		auto DirtyPackages(bool Maps, bool Content) -> std::vector<DPackage*>
		{
			std::vector<DPackage*> Result;
			for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
				if (auto* Package = Cast<DPackage>(Object); Package && Package->IsAssetPackage() && Package->IsDirty())
				{
					const bool Map = std::ranges::any_of(Package->GetTopLevelAssets(), [](auto* Asset) { return Cast<DLevel>(Asset) != nullptr; });
					if (Map ? Maps : Content) Result.push_back(Package);
				}
			return Result;
		}
	}

	FScopedPackageSaveObserver::FScopedPackageSaveObserver(std::function<void(const FPackagePath&)> OnSaved)
		: Previous(std::exchange(SaveObserver, this)), Callback(std::move(OnSaved)) {}
	FScopedPackageSaveObserver::~FScopedPackageSaveObserver() { SaveObserver = Previous; }

	auto DEditorLoadingAndSavingUtils::SavePackages(std::span<DPackage* const> Input, bool OnlyDirty) -> bool
	{
		check(IsInGameThread());
		const auto Packages = FilterPackages(Input, OnlyDirty);
		std::vector<FStrongObjectPtr> Owners;
		for (auto* Package : Packages)
		{
			Owners.emplace_back(Package);
			for (auto* Asset : Package->GetTopLevelAssets()) Owners.emplace_back(Asset);
		}
		// Detach the observer before any writer/feedback callback may start another batch.
		auto* Observer = std::exchange(SaveObserver, nullptr);
		struct FRestore { FScopedPackageSaveObserver* Value; ~FRestore() { SaveObserver = Value; } } Restore{Observer};
		bool Success = true;
		for (auto* Package : Packages)
		{
			const auto Path = Package->GetPackagePathIdentity();
			FAssetWriteResult Written;
			const bool Saved = SavePackage(Package, {}, &Written);
			if (Observer && Observer->Callback && (Saved || Written.Effect == EAssetWriteEffect::ContentCommittedProjectionPending)) Observer->Callback(Path);
			if (!Saved)
			{
				Success = false;
				FFeedbackContext::Get().Error(Path.ToString() + ": " +
					(Written.Effect == EAssetWriteEffect::ContentCommittedProjectionPending ? "Content saved; catalog synchronization pending. " : "") +
					(Written.Error ? Written.Error->ToString() : "Package save failed."));
			}
		}
		return Success;
	}

	auto FEditorFileUtils::SetSavePrompt(FSavePrompt Prompt) -> void
	{ check(IsInGameThread()); SavePrompt = std::move(Prompt); }

	auto FEditorFileUtils::PromptForCheckoutAndSave(std::span<DPackage* const> Input,
		FPromptForCheckoutAndSaveParams& Params) -> EPromptReturnCode
	{
		check(IsInGameThread());
		const auto Packages = FilterPackages(Input, Params.bCheckDirty);
		// A prompt may invoke another save. Reserve this invocation's observer
		// before handing control to the host, then expose it only to our writes.
		auto* Observer = std::exchange(SaveObserver, nullptr);
		struct FRestore { FScopedPackageSaveObserver* Value; ~FRestore() { SaveObserver = Value; } } Restore{Observer};
		if (Params.OutFailedPackages) Params.OutFailedPackages->clear();
		std::vector<FStrongObjectPtr> Owners;
		for (auto* Package : Packages)
		{
			Owners.emplace_back(Package);
			for (auto* Asset : Package->GetTopLevelAssets()) Owners.emplace_back(Asset);
		}
		if (Packages.empty()) return PR_Success;
		if (Params.bPromptToSave)
		{
			const auto Prompt = SavePrompt;
			const auto Decision = Prompt ? Prompt(Packages, Params) : PR_Cancelled;
			if (Decision != PR_Success) return Decision == PR_Declined && !Params.bCanBeDeclined ? PR_Cancelled : Decision;
		}
		bool Success = true;
		for (auto* Package : Packages)
		{
			SaveObserver = Observer;
			if (!DEditorLoadingAndSavingUtils::SavePackages(std::span{&Package, 1}, false))
			{
				Success = false;
				if (Params.OutFailedPackages) Params.OutFailedPackages->push_back(Package);
			}
			SaveObserver = nullptr;
		}
		return Success ? PR_Success : PR_Failure;
	}

	auto FEditorFileUtils::PromptForCheckoutAndSave(std::span<DPackage* const> Packages,
		bool Dirty, bool Prompt, std::vector<DPackage*>* Failed, bool CheckedOut, bool Decline) -> EPromptReturnCode
	{
		FPromptForCheckoutAndSaveParams Params;
		Params.bCheckDirty = Dirty; Params.bPromptToSave = Prompt;
		Params.bAlreadyCheckedOut = CheckedOut; Params.bCanBeDeclined = Decline; Params.OutFailedPackages = Failed;
		return PromptForCheckoutAndSave(Packages, Params);
	}
	auto FEditorFileUtils::PromptForCheckoutAndSave(std::span<DPackage* const> Packages,
		bool Dirty, bool Prompt, std::string_view Title, std::string_view Message,
		std::vector<DPackage*>* Failed, bool CheckedOut, bool Decline) -> EPromptReturnCode
	{
		FPromptForCheckoutAndSaveParams Params;
		Params.bCheckDirty = Dirty; Params.bPromptToSave = Prompt; Params.Title = Title; Params.Message = Message;
		Params.bAlreadyCheckedOut = CheckedOut; Params.bCanBeDeclined = Decline; Params.OutFailedPackages = Failed;
		return PromptForCheckoutAndSave(Packages, Params);
	}
	auto DEditorLoadingAndSavingUtils::SavePackagesWithDialog(std::span<DPackage* const> Packages, bool Dirty) -> bool
	{ return FEditorFileUtils::PromptForCheckoutAndSave(Packages, Dirty, true) == FEditorFileUtils::PR_Success; }
	auto DEditorLoadingAndSavingUtils::SaveDirtyPackages(bool Maps, bool Content) -> bool
	{ return SavePackages(DirtyPackages(Maps, Content), true); }
	auto DEditorLoadingAndSavingUtils::SaveDirtyPackagesWithDialog(bool Maps, bool Content) -> bool
	{ return SavePackagesWithDialog(DirtyPackages(Maps, Content), true); }
}
