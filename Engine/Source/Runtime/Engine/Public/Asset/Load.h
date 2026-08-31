#pragma once

#include "Asset/Result.h"

#include "EngineAPI.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/CookedAsset.h"
#include "AssetRegistry/PackageTypes.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectFwd.h"
#include "DObject/SoftObjectPtr.h"
#include "Misc/Guid.h"

namespace Durin::Asset
{
	enum class ESoftObjectNullPolicy : uint8
	{
		Reject,
		Allow
	};

	enum class ESoftObjectResolveState : uint8
	{
		Null,
		NotLoaded,
		Loaded
	};

	struct FSoftObjectResolveResult
	{
		FAssetResult Result;
		ESoftObjectResolveState State = ESoftObjectResolveState::Null;
		DObject* Object = nullptr;
		FObjectPath ResolvedPath;
		bool bRedirected = false;

		auto Succeeded() const -> bool { return Result.Succeeded(); }
		explicit operator bool() const { return Succeeded(); }
	};

	template<typename T>
	struct TSoftObjectResolveResult
	{
		FAssetResult Result;
		ESoftObjectResolveState State = ESoftObjectResolveState::Null;
		T* Object = nullptr;
		FObjectPath ResolvedPath;
		bool bRedirected = false;

		auto Succeeded() const -> bool { return Result.Succeeded(); }
		explicit operator bool() const { return Succeeded(); }
	};

	enum class EAssetLoadMutationKind : uint8
	{
		Upgrade,
		NonUpgrade
	};

	struct FAssetLoadMutation
	{
		FPackagePath PackagePath;
		std::string ObjectPath;
		std::string HandlerId;
		std::string Summary;
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade;
	};

	enum class EAssetReflectedIdentityKind : uint8
	{
		Class,
		Struct,
		Enum,
		Property
	};
	enum class EAssetSerializedIdentityLocation : uint8
	{
		PackageHeader,
		ObjectRecord,
		Schema,
		TypeDescriptor
	};

	struct FAssetCanonicalizationEvidence
	{
		FPackagePath PackagePath;
		std::string StoredIdentity;
		std::string CurrentIdentity;
		EAssetReflectedIdentityKind Kind = EAssetReflectedIdentityKind::Class;
		EAssetSerializedIdentityLocation Location =
			EAssetSerializedIdentityLocation::PackageHeader;
		std::string LogicalPath;

		auto operator==(const FAssetCanonicalizationEvidence&) const -> bool = default;
	};

	struct FAssetDeprecatedRouteEvidence
	{
		FPackagePath PackagePath;
		std::string ObjectPath;
		std::string DeclaringType;
		std::string StoredFieldName;
		std::string DeprecatedPropertyName;
		std::vector<std::string> MigrationTargets;
		FGuid CustomVersionGuid;
		int32 SourceVersion = -1;
		int32 DeprecatedBefore = 0;

		auto operator==(const FAssetDeprecatedRouteEvidence&) const -> bool = default;
	};

	struct FAssetLoadReport
	{
		FPackagePath RequestedPath;
		FPackagePath FinalPath;
		FPackagePath PackagePath;
		uint64 CatalogRevision = 0;
		std::vector<FPackagePath> RedirectChain;
		std::string FinalAssetClassName;
		EAssetError Error = EAssetError::None;
		std::string ErrorMessage;
		uint64 PackageFileReadCount = 0;
		std::vector<FAssetLoadMutation> Mutations;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence;
		std::vector<FAssetDeprecatedRouteEvidence> DeprecatedRouteEvidence;

		ENGINE_API auto HasNonUpgradeMutations() const -> bool;
	};

	ENGINE_API auto ReportAssetLoadMutation(
		DObject* Object,
		std::string HandlerId,
		std::string Summary,
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade
	) -> void;

	struct FAssetPackageLoadSnapshot
	{
		std::vector<FPackagePath> ResidentPackages;
	};

	enum class EAssetPackageUnloadPolicy : uint8
	{
		RejectUnsaved,
		DiscardUnsaved,
	};

	ENGINE_API auto LoadPackage(
		const FPackagePath& Path,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
	ENGINE_API auto LoadObject(
		const FObjectPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
	inline auto LoadObject(
		const FTopLevelAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult
	{
		FObjectPath ObjectPath;
		if (!FObjectPath::TryCreate(
			Path, std::span<const std::string>{}, ObjectPath))
		{
			OutObject = nullptr;
			return {EAssetError::InvalidPath,
				"A top-level asset load requires a valid exact asset path."};
		}
		return LoadObject(ObjectPath, ExpectedClass, OutObject, OutReport);
	}

	template<typename T>
	auto LoadObject(const FObjectPath& Path, T*& OutObject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = LoadObject(Path, T::StaticClass(), Object, OutReport);
		OutObject = Result ? static_cast<T*>(Object) : nullptr;
		return Result;
	}

	template<typename T>
	auto LoadObject(const FTopLevelAssetPath& Path, T*& OutObject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = LoadObject(
			Path, T::StaticClass(), Object, OutReport);
		OutObject = Result ? static_cast<T*>(Object) : nullptr;
		return Result;
	}

	ENGINE_API auto ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject
	)
		-> FSoftObjectResolveResult;
	ENGINE_API auto LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject,
		FAssetLoadReport* OutReport = nullptr
	) -> FAssetResult;

	template<typename T>
	auto ResolveSoftObject(
		TSoftObjectPtr<T>& Reference,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject
	)
		-> TSoftObjectResolveResult<T>
	{
		static_assert(std::is_base_of_v<DObject, T>);
		FSoftObjectResolveResult Result = ResolveSoftObject(
			Reference.GetBase(), T::StaticClass(), NullPolicy
		);
		return {
			.Result = std::move(Result.Result),
			.State = Result.State,
			.Object = static_cast<T*>(Result.Object),
			.ResolvedPath = std::move(Result.ResolvedPath),
			.bRedirected = Result.bRedirected
		};
	}

	template<typename T>
	auto LoadSoftObject(
		TSoftObjectPtr<T>& Reference,
		T*& OutObject,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject,
		FAssetLoadReport* OutReport = nullptr
	) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = LoadSoftObject(
			Reference.GetBase(), T::StaticClass(), Object, NullPolicy, OutReport
		);
		OutObject = Result ? static_cast<T*>(Object) : nullptr;
		return Result;
	}

	ENGINE_API auto FindResidentPackage(const FPackagePath& Path) -> DPackage*;
	// Attempts to release Standalone residency and collect an unreferenced package.
	// Returns InUse and restores residency when a live strong reference keeps it reachable.
	ENGINE_API auto UnloadPackage(
		const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved
	)
		-> FAssetResult;
	ENGINE_API auto UnloadPackage(
		DPackage* Package,
		EAssetPackageUnloadPolicy Policy = EAssetPackageUnloadPolicy::RejectUnsaved
	)
		-> FAssetResult;
	ENGINE_API auto CapturePackageLoadSnapshot() -> FAssetPackageLoadSnapshot;
	ENGINE_API auto ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot
	) -> FAssetResult;
	ENGINE_API auto ShutdownAssetManager() -> void;
	ENGINE_API auto InitializeAssetManager(
		FAssetRuntimeConfiguration Configuration = FAssetRuntimeConfiguration::Authored()
	)
		-> FAssetResult;
	ENGINE_API auto GetAssetRuntimeConfiguration()
		-> const FAssetRuntimeConfiguration&;
} // namespace Durin::Asset
