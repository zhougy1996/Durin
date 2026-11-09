#pragma once

#include "CoreMinimal.h"
#include "DObject/AssetPath.h"
#include "DObject/DObjectFwd.h"
#include "EngineAPI.h"

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

	// Detailed evidence is collected only during an explicitly scoped synchronous audit.
	struct FAssetLoadAudit
	{
		std::vector<FAssetLoadMutation> Mutations;
		std::vector<FAssetCanonicalizationEvidence> CanonicalizationEvidence;
		std::vector<FAssetDeprecatedRouteEvidence> DeprecatedRouteEvidence;
	};

	// GameThread-only. Resets Output on entry and restores the previous collector on exit.
	// Nested scopes are independent. Queued async work does not inherit this scope.
	class FScopedAssetLoadAudit final
	{
	public:
		ENGINE_API explicit FScopedAssetLoadAudit(FAssetLoadAudit& Output);
		ENGINE_API ~FScopedAssetLoadAudit();
		FScopedAssetLoadAudit(const FScopedAssetLoadAudit&) = delete;
		auto operator=(const FScopedAssetLoadAudit&) -> FScopedAssetLoadAudit& = delete;
	private:
		FAssetLoadAudit* Previous;
	};

	ENGINE_API auto ReportAssetLoadMutation(
		DObject* Object,
		std::string HandlerId,
		std::string Summary,
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade
	) -> void;

}
