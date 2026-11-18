#include "PackageTools.h"
#include "Asset/PackageCollection.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/EditorEngine.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Asset/PackageReload.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace { DPackageTools::FReloadPrompt ReloadPrompt; }
	auto DPackageTools::SetReloadPrompt(FReloadPrompt Prompt) -> void
	{ check(IsInGameThread()); ReloadPrompt = std::move(Prompt); }
	auto DPackageTools::ReloadPackages(std::span<DPackage* const> Packages) -> bool
	{
		std::string Error;
		const bool Changed = ReloadPackages(Packages, Error);
		if (!Error.empty()) DURIN_WARN("{}", Error);
		return Changed;
	}
	auto DPackageTools::ReloadPackages(std::span<DPackage* const> Packages, std::string& Error,
		EReloadPackagesInteractionMode Mode) -> bool
	{
		check(IsInGameThread());
		Error.clear();
		FPackageReloadRequest Request;
		for (auto* Package : Packages)
			if (Package && Package->IsAssetPackage() && !Package->IsNewlyCreated()
				&& FindResidentPackage(Package->GetPackagePathIdentity()) == Package
				&& !std::ranges::contains(Request.Packages, Package)) Request.Packages.push_back(Package);
		if (Request.Packages.empty()) return false;
		std::vector<FStrongObjectPtr> PromptOwners;
		std::vector<FPackagePath> Paths;
		for (auto* Package : Request.Packages)
		{
			PromptOwners.emplace_back(Package);
			Paths.push_back(Package->GetPackagePathIdentity());
			for (auto* Asset : Package->GetTopLevelAssets()) PromptOwners.emplace_back(Asset);
		}
		std::vector<DPackage*> Dirty;
		for (auto* Package : Request.Packages) if (Package->IsDirty()) Dirty.push_back(Package);
		if (!Dirty.empty() && Mode != EReloadPackagesInteractionMode::AssumePositive)
		{
			const auto Prompt = ReloadPrompt;
			if (Mode == EReloadPackagesInteractionMode::AssumeNegative || !Prompt || !Prompt(Dirty))
			{ Error = "Reload declined; packages have unsaved changes."; return false; }
		}
		if (auto* Editor = GetTransactionEditor())
		{
			if (auto* Editors = Editor->GetEditorSubsystem<DAssetEditorSubsystem>())
			{
				if (!Editors->AddPackageReloadParticipants(Request))
				{ Error = "Reload requires idle editor and transaction state."; return false; }
			}
			else if (Editor->GetTransactor())
			{
				auto Participant = CreateTransactorReloadParticipant(*Editor->GetTransactor(), Request.Packages);
				if (!Participant) { Error = "Reload requires idle transaction history."; return false; }
				Request.Participants.push_back(std::move(Participant));
			}
		}
		for (size_t Index = 0; Index < Paths.size(); ++Index)
			if (FindResidentPackage(Paths[Index]) != Request.Packages[Index]) { Error = "Reload selection changed during confirmation."; return false; }
		PromptOwners.clear();
		auto Operation = Durin::ReloadPackages(Request);
		const auto Result = Operation.Wait();
		if (!Result)
		{
			for (const auto& Diagnostic : Result.Diagnostics) Error += FormatPackageReloadDiagnostic(Diagnostic) + "\n";
			return false;
		}
		return true;
	}
	auto DPackageTools::UnloadPackages(FUnloadPackageParams& Params) -> bool
	{
		auto& Errors = Params.OutErrorMessage;
		Errors.clear();
		if (Params.Packages.empty()) return false;
		if (auto Guard = CheckPackageCollectionAllowed(); !Guard)
		{
			Errors = Guard.error().ToString();
			return false;
		}
		std::vector<TWeakObjectPtr<DPackage>> Candidates;
		std::vector<FStrongObjectPtr> Skipped;
		std::unordered_set<DPackage*> Seen;
		for (DPackage* Package : Params.Packages)
		{
			if (!Package || !Seen.insert(Package).second || !Package->IsAssetPackage()
				|| FindResidentPackage(Package->GetPackagePathIdentity()) != Package) continue;
			if (!Params.bUnloadDirtyPackages && (Package->IsDirty() || Package->IsNewlyCreated()))
			{
				Errors += Package->GetPackagePath() + " has unsaved changes.\n";
				Skipped.emplace_back(Package);
				for (DObject* Asset : Package->GetTopLevelAssets()) Skipped.emplace_back(Asset);
				continue;
			}
			Candidates.emplace_back(Package);
		}
		if (Candidates.empty()) return false;
		if (Params.bResetTransBuffer)
			if (auto* Editor = GetTransactionEditor(); Editor && Editor->GetTransactor())
			{
				const auto Reset = Editor->GetTransactor()->TryReset("Unload Packages");
				if (!Reset && Reset.Code != ETransactorResultCode::NoOp)
				{
					Errors += FormatTransactorResult(Reset);
					return false;
				}
			}
		const auto Result = CollectPackageExports(Candidates);
		if (!Result.Admission) { Errors += Result.Admission.error().ToString(); return false; }
		for (const auto& Path : Result.ReferencedPackages)
			Errors += Path.ToString() + " remains referenced.\n";
		return Result.CollectedCount != 0;
	}

	auto DPackageTools::UnloadPackages(std::span<DPackage* const> Packages, std::string& Errors,
		bool bUnloadDirtyPackages) -> bool
	{
		FUnloadPackageParams Params(Packages);
		Params.bUnloadDirtyPackages = bUnloadDirtyPackages;
		const bool bChanged = UnloadPackages(Params);
		Errors = std::move(Params.OutErrorMessage);
		return bChanged;
	}

	auto DPackageTools::UnloadPackages(std::span<DPackage* const> Packages) -> bool
	{
		std::string Errors;
		const bool bChanged = UnloadPackages(Packages, Errors);
		if (!Errors.empty()) DURIN_WARN("{}", Errors);
		return bChanged;
	}
}
