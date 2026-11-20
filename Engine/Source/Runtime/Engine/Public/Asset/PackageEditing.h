#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "EngineAPI.h"
#include "Asset/PackageInspection.h"
#include "DObject/SoftObjectPtr.h"

namespace Durin
{
	// Emits one committed live identity event; file reconciliation does not repeat it.
	ENGINE_API auto PublishAssetRenamed(DObject& Asset, const FSoftObjectPath& OldPath) -> void;
	// Byte-level path substitutions shared by Cook and authoring tools.
	struct FAssetPackageReferenceMapping
	{
		FPackagePath SourcePath;
		FPackagePath DestinationPath;
		std::vector<std::pair<FTopLevelAssetPath, FTopLevelAssetPath>> Objects;
		auto operator==(const FAssetPackageReferenceMapping&) const -> bool = default;
	};

	// Checks the live-load boundary, runtime lifecycle, and authored-package mode.
	// Call on the owner thread before preparing or publishing authored edits.
	ENGINE_API auto CheckAssetPackageMutationAllowed() -> std::expected<void, UnifiedError::FError>;
	ENGINE_API auto RewriteAssetPackageReferences(
		FByteView Bytes, FByteView BulkBytes, const FPackagePath& PackagePath,
		std::span<const FAssetPackageReferenceMapping> Mappings,
		uint64 ExpectedRewriteCount, FByteBuffer& OutBytes) -> std::expected<void, UnifiedError::FError>;
	ENGINE_API auto CollectLoadedAssetPackageSoftReferences(
		DPackage* Package, const FPackagePath& TargetPath,
		std::vector<FSoftObjectPtr*>& OutValues) -> std::expected<void, Durin::UnifiedError::FError>;
	ENGINE_API auto FingerprintAssetPackageBytes(
		std::string_view PhysicalPath, FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> std::expected<void, Durin::UnifiedError::FError>;
}
