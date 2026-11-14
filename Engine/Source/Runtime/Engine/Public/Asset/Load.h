#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "Asset/AssetWriteResult.h"

#include "EngineAPI.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/CookedAsset.h"
#include "AssetRegistry/PackageTypes.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectFwd.h"
#include "DObject/SoftObjectPtr.h"
#include "Misc/Guid.h"

namespace Durin
{
	// Identifies a dependency failure without parsing the loader's diagnostic text.
	struct FAssetLoadDependencyFailure
	{
		FPackagePath PackagePath;
		FSoftObjectPath ObjectPath;
		UnifiedError::FError Cause;
	};

	// Request summary only. Errors belong to the load result; detailed evidence to LoadAudit.h.
	struct FAssetLoadReport
	{
		FPackagePath RequestedPath;
		FPackagePath FinalPath;
		FPackagePath PackagePath;
		uint64 CatalogRevision = 0;
		std::vector<FPackagePath> RedirectChain;
		std::string FinalAssetClassName;
		uint64 PackageFileReadCount = 0;
		uint64 UpgradeMutationCount = 0;
		uint64 NonUpgradeMutationCount = 0;
		uint64 CanonicalizedIdentityCount = 0;
		uint64 DeprecatedFieldRouteCount = 0;
		// Removed authored fields skipped in memory; source bytes change only on save.
		uint64 DiscardedFieldCount = 0;
		FSoftObjectPath RequestedObjectPath;
		FSoftObjectPath ResolvedObjectPath;
		std::vector<FSoftObjectPath> ObjectRedirectChain;
		std::vector<FAssetLoadDependencyFailure> DependencyFailures;
	};

	// Nullable observations. No new GC owner or implicit logging.
	// Optional outputs are reset for every call, including success/resident paths.
	[[nodiscard]] ENGINE_API auto LoadPackage(const FPackagePath& Path,
		FAssetLoadReport* OutReport = nullptr,
		std::optional<UnifiedError::FError>* OutError = nullptr) -> DPackage*;
	[[nodiscard]] ENGINE_API auto LoadObject(const FSoftObjectPath& Path,
		const DClass* ExpectedClass, FAssetLoadReport* OutReport = nullptr,
		std::optional<UnifiedError::FError>* OutError = nullptr) -> DObject*;
	[[nodiscard]] ENGINE_API auto LoadObject(const FTopLevelAssetPath& Path,
		const DClass* ExpectedClass, FAssetLoadReport* OutReport = nullptr,
		std::optional<UnifiedError::FError>* OutError = nullptr) -> DObject*;

	template<typename T>
	[[nodiscard]] auto LoadObject(const FSoftObjectPath& Path,
		FAssetLoadReport* OutReport = nullptr,
		std::optional<UnifiedError::FError>* OutError = nullptr) -> T*
	{
		static_assert(std::is_base_of_v<DObject, T>);
		return static_cast<T*>(LoadObject(Path, T::StaticClass(), OutReport, OutError));
	}

	template<typename T>
	[[nodiscard]] auto LoadObject(const FTopLevelAssetPath& Path,
		FAssetLoadReport* OutReport = nullptr,
		std::optional<UnifiedError::FError>* OutError = nullptr) -> T*
	{
		static_assert(std::is_base_of_v<DObject, T>);
		return static_cast<T*>(LoadObject(Path, T::StaticClass(), OutReport, OutError));
	}

	ENGINE_API auto IsPackageLoading(const FPackagePath& Path) -> bool;
	ENGINE_API auto FindResidentPackage(const FPackagePath& Path) -> DPackage*;
	// Close requests and retire the current package lifetime; safe before initialization.
	ENGINE_API auto ShutdownAssetRuntime() -> void;
	// Host bootstrap: select the final domain before DObjectInit constructs CDOs.
	// Running initialization is idempotent only for the same configuration.
	ENGINE_API auto InitializeAssetRuntime(
		FAssetRuntimeConfiguration Configuration = FAssetRuntimeConfiguration::Authored()
	)
		-> FAssetWriteResult;
	// Requires a successful initialization; retains the last configuration after shutdown.
	ENGINE_API auto GetAssetRuntimeConfiguration()
		-> const FAssetRuntimeConfiguration&;
} // namespace Durin
