#pragma once

#include "AssetForge/Persistence/ImportRecordIndex.h"
#include "AssetForge/ImportRequest.h"
#include "AssetForge/Operations/ImportOperation.h"
#include "AssetForgeBuiltinsAPI.h"
#include "StaticMesh/StaticMesh.h"
#include "Asset/MountedSource.h"

namespace Durin
{
	class DMaterial;
	class DMaterialInstance;
	class DTexture2D;
	class DSkeleton;
	class DSkeletalMesh;
	class DAnimationClip;
}

namespace Durin::AssetForge::Builtins
{
	inline constexpr std::string_view ImportedSurfaceMaterialPath =
		"/Engine/Materials/ImportedSurface";

	ASSETFORGEBUILTINS_API auto MakeSceneImportRequest(
		const FSourcePath& MountedRootSource,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		EImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto MakeSceneRecordImportRequest(
		const DImportRecord& Record,
		EImportRecordAction Action,
		FImportOperationOwner Owner,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool;

	struct FPreparedSceneSourceBundle
	{
		std::vector<Asset::FScopedMountedSourceFile> Sources;
		FSourcePath RootSource;
	};
	struct FSceneSourceBundleAsyncState;
	class FSceneSourceBundleAsyncHandle
	{
	public:
		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }

	private:
		std::shared_ptr<FSceneSourceBundleAsyncState> State;

		explicit FSceneSourceBundleAsyncHandle(
			std::shared_ptr<FSceneSourceBundleAsyncState> InState)
			: State(std::move(InState)) {}

		friend ASSETFORGEBUILTINS_API auto BeginSceneSourceBundlePreparation(
			std::filesystem::path, std::string, std::string, bool)
			-> FSceneSourceBundleAsyncHandle;
		friend ASSETFORGEBUILTINS_API auto PollSceneSourceBundlePreparation(
			FSceneSourceBundleAsyncHandle&, FPreparedSceneSourceBundle&,
			std::string&) -> EAsyncImportPlanStatus;
		friend ASSETFORGEBUILTINS_API auto CancelAndDrainSceneSourceBundlePreparation(
			FSceneSourceBundleAsyncHandle&) -> void;
	};

	ASSETFORGEBUILTINS_API auto PrepareSceneSourceBundle(
		const std::filesystem::path& InputRoot,
		std::string_view ReferencingContentPath,
		std::string_view ExternalIngestDestination,
		FPreparedSceneSourceBundle& OutBundle,
		std::string& OutError,
		bool bEngineAuthoringContext = false) -> bool;
	ASSETFORGEBUILTINS_API auto BeginSceneSourceBundlePreparation(
		std::filesystem::path InputRoot,
		std::string ReferencingContentPath,
		std::string ExternalIngestDestination,
		bool bEngineAuthoringContext = false) -> FSceneSourceBundleAsyncHandle;
	ASSETFORGEBUILTINS_API auto PollSceneSourceBundlePreparation(
		FSceneSourceBundleAsyncHandle& Handle,
		FPreparedSceneSourceBundle& OutBundle,
		std::string& OutError) -> EAsyncImportPlanStatus;
	ASSETFORGEBUILTINS_API auto CancelAndDrainSceneSourceBundlePreparation(
		FSceneSourceBundleAsyncHandle& Handle) -> void;
	ASSETFORGEBUILTINS_API auto CommitSceneSourceBundle(
		FPreparedSceneSourceBundle& Bundle) -> void;
	ASSETFORGEBUILTINS_API auto RollbackSceneSourceBundle(
		FPreparedSceneSourceBundle& Bundle) -> void;
	ASSETFORGEBUILTINS_API auto FindSceneImportRecordForOutput(
		const DObject& Output,
		std::string& OutError) -> DImportRecord*;
	ASSETFORGEBUILTINS_API auto EnsureImportedSurfaceMaterial(
		std::string& OutError) -> DMaterial*;
}
