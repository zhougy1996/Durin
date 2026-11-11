#pragma once

#include "CoreMinimal.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "PackageCollection.h"

namespace Durin
{
	// Checked cleanup for isolated native fixtures, including non-editor targets.
	enum class EAssetPackageUnloadPolicy : uint8 { RejectUnsaved, DiscardUnsaved };

	inline auto UnloadPackage(DPackage* Package,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved) -> FAssetReadResult
	{
		if (auto Guard = AssetPrivate::CheckPackageUnloadAllowed(); !Guard) return Guard;
		if (!Package || !Package->IsAssetPackage()) return {EAssetReadError::InvalidPackageType, "Invalid package."};
		const auto Path = Package->GetPackagePathIdentity();
		if (FindResidentPackage(Path) != Package) return {EAssetReadError::NotFound, "Package is not resident."};
		if ((Package->IsDirty() || Package->IsNewlyCreated()) && Policy == EAssetPackageUnloadPolicy::RejectUnsaved)
			return {EAssetReadError::InUse, "Package has unsaved changes."};
		if (!AssetPrivate::CollectPackageExports({Path}))
			return {EAssetReadError::InUse, "Package remains referenced by live objects."};
		return {};
	}

	inline auto UnloadPackage(const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved) -> FAssetReadResult
	{
		if (auto Guard = AssetPrivate::CheckPackageUnloadAllowed(); !Guard) return Guard;
		if (IsPackageLoading(Path)) return {EAssetReadError::InUse, "Package loading is in progress."};
		auto* Package = FindResidentPackage(Path);
		if (!Package) return {EAssetReadError::NotFound, "Package is not resident."};
		return UnloadPackage(Package, Policy);
	}
}

namespace Durin::Testing
{
	// A prior cleanup's global GC can already have collected other clean fixtures.
	// Still use checked unloading for live packages, so owners/unsaved edits fail.
	inline auto EnsurePackageNotResident(const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved) -> FAssetReadResult
	{
		auto Result = UnloadPackage(Path, Policy);
		if (Path.IsValid() && Result.Error == EAssetReadError::NotFound) return {};
		return Result;
	}
}
