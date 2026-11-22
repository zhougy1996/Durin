#include "FileHelpers.h"
#include "Asset/PackageCollection.h"
#include "EditorModeManager.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
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

		auto IsSaveCandidate(const DPackage* Package) -> bool
		{
			return Package && !Package->IsPendingKill() && Package->IsAssetPackage()
				&& !Package->IsGraphPrivate() && !Package->HasAnyObjectFlags(EObjectFlags::Transient);
		}

		auto FilterPackages(std::span<DPackage* const> Input, bool OnlyDirty) -> std::vector<DPackage*>
		{
			std::vector<DPackage*> Result;
			for (auto* Package : Input)
				if (IsSaveCandidate(Package)
					&& (!OnlyDirty || Package->IsDirty()) && !std::ranges::contains(Result, Package)) Result.push_back(Package);
			return Result;
		}
	}

	auto FEditorFileUtils::GetDirtyPackages(bool Maps, bool Content) -> std::vector<DPackage*>
	{
		std::vector<DPackage*> Result;
		for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (auto* Package = Cast<DPackage>(Object); IsSaveCandidate(Package) && Package->IsDirty())
			{
				const bool Map = std::ranges::any_of(Package->GetTopLevelAssets(), [](auto* Asset) { return Cast<DLevel>(Asset) != nullptr; });
				if (Map ? Maps : Content) Result.push_back(Package);
			}
		return Result;
	}

	auto FEditorFileUtils::LoadMap(const std::string& Filename) -> bool
	{
		CheckGameThread();
		auto* Editor = GEditor;
		if (!Editor) return false;
		if (!Editor->GetCapabilities().bRunTools || Editor->IsPlaying())
		{ FFeedbackContext::Get().Error("Map loading requires an idle editor outside Play in Editor."); return false; }
		FSoftObjectPath Path;
		if (!FSoftObjectPath::TryCreate(Filename, Path))
		{
			FPackagePath PackagePath;
			if (!FPackagePath::TryCreate(Filename, PackagePath))
			{ FFeedbackContext::Get().Error("Invalid map path: " + Filename); return false; }
			if (const auto Resolved = ResolveLevelPackage(PackagePath, Path); !Resolved)
			{ FFeedbackContext::Get().Error(Resolved.error().ToString()); return false; }
		}
		FSoftObjectPath Resolved;
		if (const auto Validation = ResolveLevelPackage(Path.GetPackagePath(), Resolved); !Validation)
		{ FFeedbackContext::Get().Error(Validation.error().ToString()); return false; }
		if (Resolved != Path)
		{ FFeedbackContext::Get().Error("The requested object is not the map package's level."); return false; }
		if (const auto Admission = CheckPackageCollectionAllowed(); !Admission)
		{ FFeedbackContext::Get().Error(Admission.error().ToString()); return false; }
		if (!Editor->GetRendererModule())
		{ FFeedbackContext::Get().Error("Renderer services are unavailable."); return false; }
		// As in UE FileHelpers, map loading explicitly deactivates editor modes.
		try
		{
			GLevelEditorModeTools(*Editor).DeactivateAllModes();
		}
		catch (const std::exception& Exception) { FFeedbackContext::Get().Error(Exception.what()); return false; }
		if (const auto* Trans = Editor->GetTransactor(); Trans && (!Trans->CanTransact() || Trans->IsActive()))
		{ FFeedbackContext::Get().Error("Finish the current transaction or recovery before opening a level."); return false; }
		std::string Error;
		bool Succeeded = false;
		try { Succeeded = Editor->Map_Load(Path, Error); }
		catch (const std::exception& Exception) { Error = Exception.what(); }
		catch (...) { Error = "Unknown map loading exception."; }
		if ((!Succeeded || !Editor->GetEditorWorld()) && !Editor->IsShutdownRequested())
		{
			Succeeded = false;
			try { if (!Editor->NewMap()) Error += " Could not create a blank map."; }
			catch (const std::exception& Exception) { Error += std::format(" Blank map creation failed: {}", Exception.what()); }
			catch (...) { Error += " Blank map creation failed."; }
		}
		if (!Succeeded) FFeedbackContext::Get().Error(Error.empty() ? "Map loading failed." : Error);
		return Succeeded;
	}

	auto DEditorLoadingAndSavingUtils::LoadMap(const std::string& Filename) -> DWorld*
	{
		return FEditorFileUtils::LoadMap(Filename) ? GEditor->GetEditorWorld() : nullptr;
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

	auto FEditorFileUtils::SaveDirtyPackages(bool bPromptUserToSave, bool bSaveMapPackages, bool bSaveContentPackages) -> bool
	{
		check(IsInGameThread());
		FPromptForCheckoutAndSaveParams Params;
		Params.bCheckDirty = true;
		Params.bPromptToSave = bPromptUserToSave;
		Params.bIsExplicitSave = bPromptUserToSave;
		const auto Result = PromptForCheckoutAndSave(GetDirtyPackages(bSaveMapPackages, bSaveContentPackages), Params);
		return Result == PR_Success || Result == PR_Declined;
	}

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
	{ return SavePackages(FEditorFileUtils::GetDirtyPackages(Maps, Content), true); }
	auto DEditorLoadingAndSavingUtils::SaveDirtyPackagesWithDialog(bool Maps, bool Content) -> bool
	{ return SavePackagesWithDialog(FEditorFileUtils::GetDirtyPackages(Maps, Content), true); }
}
