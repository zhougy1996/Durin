#include "PackageTools.h"
#include "Asset/PackageCollection.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	auto DPackageTools::UnloadPackages(FUnloadPackageParams& Params) -> bool
	{
		auto& Errors = Params.OutErrorMessage;
		Errors.clear();
		if (Params.Packages.empty()) return false;
		if (auto Guard = CheckPackageCollectionAllowed(); !Guard)
		{
			Errors = Guard.Message;
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
		if (!Result.Admission) { Errors += Result.Admission.Message; return false; }
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
