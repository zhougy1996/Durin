#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetReadResult.h"
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
		EAssetReadError Error = EAssetReadError::None;
		std::string ErrorMessage;
		uint64 PackageFileReadCount = 0;
		std::vector<FAssetLoadMutation> Mutations;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence;
		std::vector<FAssetDeprecatedRouteEvidence> DeprecatedRouteEvidence;
		// Removed authored fields skipped in memory; source bytes change only on save.
		uint64 DiscardedFieldCount = 0;

		ENGINE_API auto HasNonUpgradeMutations() const -> bool;
	};

	ENGINE_API auto ReportAssetLoadMutation(
		DObject* Object,
		std::string HandlerId,
		std::string Summary,
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade
	) -> void;

	[[nodiscard]] ENGINE_API auto LoadPackage(
		const FPackagePath& Path,
		FAssetLoadReport* OutReport = nullptr) -> std::expected<DPackage*, FAssetReadError>;
	[[nodiscard]] ENGINE_API auto LoadObject(
		const FObjectPath& Path,
		const DClass* ExpectedClass,
		FAssetLoadReport* OutReport = nullptr) -> std::expected<DObject*, FAssetReadError>;
	[[nodiscard]] inline auto LoadObject(
		const FTopLevelAssetPath& Path,
		const DClass* ExpectedClass,
		FAssetLoadReport* OutReport = nullptr) -> std::expected<DObject*, FAssetReadError>
	{
		FObjectPath ObjectPath;
		if (!FObjectPath::TryCreate(
			Path, std::span<const std::string>{}, ObjectPath))
		{
			return std::unexpected(FAssetReadError{EAssetReadError::InvalidPath,
				"A top-level asset load requires a valid exact asset path."});
		}
		return LoadObject(ObjectPath, ExpectedClass, OutReport);
	}

	template<typename T>
	[[nodiscard]] auto LoadObject(const FObjectPath& Path,
		FAssetLoadReport* OutReport = nullptr) -> std::expected<T*, FAssetReadError>
	{
		static_assert(std::is_base_of_v<DObject, T>);
		auto Result = LoadObject(Path, T::StaticClass(), OutReport);
		if (!Result) return std::unexpected(std::move(Result.error()));
		return static_cast<T*>(*Result);
	}

	template<typename T>
	[[nodiscard]] auto LoadObject(const FTopLevelAssetPath& Path,
		FAssetLoadReport* OutReport = nullptr) -> std::expected<T*, FAssetReadError>
	{
		static_assert(std::is_base_of_v<DObject, T>);
		auto Result = LoadObject(
			Path, T::StaticClass(), OutReport);
		if (!Result) return std::unexpected(std::move(Result.error()));
		return static_cast<T*>(*Result);
	}

	ENGINE_API auto IsPackageLoading(const FPackagePath& Path) -> bool;
	ENGINE_API auto FindResidentPackage(const FPackagePath& Path) -> DPackage*;
	ENGINE_API auto ShutdownAssetManager() -> void;
	ENGINE_API auto InitializeAssetManager(
		FAssetRuntimeConfiguration Configuration = FAssetRuntimeConfiguration::Authored()
	)
		-> FAssetWriteResult;
	ENGINE_API auto GetAssetRuntimeConfiguration()
		-> const FAssetRuntimeConfiguration&;
} // namespace Durin
