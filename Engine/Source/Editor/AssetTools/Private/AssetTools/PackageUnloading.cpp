#include "AssetTools/PackageUnloading.h"
#include "PackageCollection.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin::PackageTools
{
	auto UnloadPackages(std::span<DPackage* const> Packages, std::string& Errors,
		bool bUnloadDirtyPackages) -> bool
	{
		Errors.clear();
		if (Packages.empty()) return true;
		if (auto Guard = AssetPrivate::CheckPackageUnloadAllowed(); !Guard)
		{
			Errors = Guard.Message;
			return false;
		}
		std::unordered_set<FPackagePath> Candidates;
		std::vector<FStrongObjectPtr> Skipped;
		for (DPackage* Package : Packages)
		{
			if (!Package || !Package->IsAssetPackage()
				|| FindResidentPackage(Package->GetPackagePathIdentity()) != Package) continue;
			if (!bUnloadDirtyPackages && (Package->IsDirty() || Package->IsNewlyCreated()))
			{
				Errors += Package->GetPackagePath() + " has unsaved changes.\n";
				Skipped.emplace_back(Package);
				for (DObject* Asset : Package->GetTopLevelAssets()) Skipped.emplace_back(Asset);
				continue;
			}
			Candidates.insert(Package->GetPackagePathIdentity());
		}
		AssetPrivate::CollectPackageExports(Candidates);
		for (const auto& Path : Candidates)
			if (FindResidentPackage(Path)) Errors += Path.ToString() + " remains referenced.\n";
		return !Candidates.empty();
	}

	auto UnloadPackages(std::span<DPackage* const> Packages) -> bool
	{
		std::string Errors;
		const bool bProcessed = UnloadPackages(Packages, Errors);
		if (!Errors.empty()) DURIN_WARN("{}", Errors);
		return bProcessed;
	}
}
