#pragma once

#include "ImportRecordIndex.h"
#include "Interchange.h"
#include "MultiOutputImport.h"
#include "AsyncImport.h"
#include "AssetForgeAPI.h"
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

namespace Durin::Asset::Forge
{
	inline constexpr std::string_view SceneImportProviderId = "Durin.Scene";
	inline constexpr uint32 SceneImportProviderContractVersion = 3;
	inline constexpr std::string_view ImportedSurfaceMaterialPath =
		"/Engine/Materials/ImportedSurface";

	ASSETFORGE_API auto MakeSceneInterchangeRequest(
		const FSourcePath& MountedRootSource,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		EInterchangeImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto MakeSceneRecordInterchangeRequest(
		const DImportRecord& Record,
		EImportRecordAction Action,
		FImportOperationOwner Owner,
		FInterchangeImportRequest& OutRequest,
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

		friend ASSETFORGE_API auto BeginSceneSourceBundlePreparation(
			std::filesystem::path, std::string, std::string, bool)
			-> FSceneSourceBundleAsyncHandle;
		friend ASSETFORGE_API auto PollSceneSourceBundlePreparation(
			FSceneSourceBundleAsyncHandle&, FPreparedSceneSourceBundle&,
			std::string&) -> EAsyncImportPlanStatus;
		friend ASSETFORGE_API auto CancelAndDrainSceneSourceBundlePreparation(
			FSceneSourceBundleAsyncHandle&) -> void;
	};

	ASSETFORGE_API auto PrepareSceneSourceBundle(
		const std::filesystem::path& InputRoot,
		std::string_view ReferencingContentPath,
		std::string_view ExternalIngestDestination,
		FPreparedSceneSourceBundle& OutBundle,
		std::string& OutError,
		bool bEngineAuthoringContext = false) -> bool;
	ASSETFORGE_API auto BeginSceneSourceBundlePreparation(
		std::filesystem::path InputRoot,
		std::string ReferencingContentPath,
		std::string ExternalIngestDestination,
		bool bEngineAuthoringContext = false) -> FSceneSourceBundleAsyncHandle;
	ASSETFORGE_API auto PollSceneSourceBundlePreparation(
		FSceneSourceBundleAsyncHandle& Handle,
		FPreparedSceneSourceBundle& OutBundle,
		std::string& OutError) -> EAsyncImportPlanStatus;
	ASSETFORGE_API auto CancelAndDrainSceneSourceBundlePreparation(
		FSceneSourceBundleAsyncHandle& Handle) -> void;
	ASSETFORGE_API auto CommitSceneSourceBundle(
		FPreparedSceneSourceBundle& Bundle) -> void;
	ASSETFORGE_API auto RollbackSceneSourceBundle(
		FPreparedSceneSourceBundle& Bundle) -> void;
	ASSETFORGE_API auto FindSceneImportRecordForOutput(
		const DObject& Output,
		std::string& OutError) -> DImportRecord*;
	ASSETFORGE_API auto EnsureImportedSurfaceMaterial(
		std::string& OutError) -> DMaterial*;
}
