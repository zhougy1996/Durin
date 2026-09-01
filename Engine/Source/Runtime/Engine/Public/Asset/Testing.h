#pragma once

#include "Asset/AssetDefinitions.h"

#include "EngineAPI.h"
#include "Asset/Mutation.h"
#include "Asset/Redirector.h"

namespace Durin
{
	ENGINE_API auto CreateAssetRedirectorForTesting(
		const FPackagePath& RedirectorPath,
		const FPackagePath& DestinationPath,
		DAssetRedirector*& OutRedirector
	) -> FAssetResult;
	ENGINE_API auto DeleteAssetForTesting(const FPackagePath& Path)
		-> FAssetResult;
	enum class EAssetRedirectorFixupFailurePoint : uint8
	{
		None,
		PreparePackage,
		PrepareStore,
		StageOriginal,
		PublishPackage,
		ApplyStore,
		Verify,
		DeleteRedirector,
		PublishRegistry,
	};

	ENGINE_API auto SetAssetRedirectorFixupFailurePointForTesting(
		EAssetRedirectorFixupFailurePoint Point,
		uint32 Occurrence = 1
	) -> void;

	enum class EAssetRelocationFailurePoint : uint8
	{
		None,
		PrepareOutput,
		StageOriginal,
		PublishRealAsset,
		PublishOwnedPayload,
		PublishRedirector,
		UpdateLoadedPackage,
		PublishRegistry,
	};

	ENGINE_API auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence = 1
	) -> void;

	enum class EAssetMutationRecoveryFailurePoint : uint8
	{
		None,
		AfterParticipantPublication,
		AfterProgressPersistence,
		BeforeProjectionReconcile,
	};

	ENGINE_API auto SetAssetMutationRecoveryFailurePointForTesting(
		EAssetMutationRecoveryFailurePoint Point,
		uint32 Occurrence = 1
	) -> void;
	ENGINE_API auto SetAssetMutationRecoveryDirectoryForTesting(
		std::filesystem::path Directory
	) -> void;
	ENGINE_API auto FlushAssetCatalogSnapshotForTesting() -> void;
	ENGINE_API auto IsAssetCatalogSnapshotDirtyForTesting() -> bool;
	ENGINE_API auto GetAssetCatalogCacheWarningForTesting() -> std::string;
} // namespace Durin
