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

	struct FSceneImportRequest
	{
		// Scene import plans consume an already mounted source. Explicit ingestion
		// of an external authoring file is a separate editor operation.
		FSourcePath RootSource;
		FAssetPath DestinationDirectory;
		FStaticMeshImportSettings MeshSettings;
		DImportRecord* ExistingRecord = nullptr;
		bool bRecreateMissingManagedOutputs = false;
		IImportProgressReporter* Progress = nullptr;
	};

	ASSETFORGE_API auto MakeSceneInterchangeRequest(
		const FSourcePath& MountedRootSource,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		EInterchangeImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest,
		std::string& OutError) -> bool;

	struct FPreparedSceneSourceBundle
	{
		std::vector<Asset::FScopedMountedSourceFile> Sources;
		FSourcePath RootSource;
	};
	struct FSceneImportPlanResult;
	struct FSceneImportExecutionResult;
	struct FSceneSourceBundleAsyncState;
	struct FSceneImportAsyncExecutionState;

	class FSceneImportPlan
	{
	public:
		auto GetMultiOutputPlan() const
			-> const FMultiOutputImportPlan& { return MultiOutputPlan; }
		auto GetWarnings() const -> std::span<const std::string> { return Warnings; }

	private:
		FMultiOutputImportPlan MultiOutputPlan;
		std::vector<std::string> Warnings;

		friend ASSETFORGE_API auto PlanSceneImport(
			const FSceneImportRequest&) -> FSceneImportPlanResult;
		friend auto FinalizeSceneImportPlan(
			const FSceneImportRequest&,
			FImportPlanResult) -> FSceneImportPlanResult;
		friend ASSETFORGE_API auto ExecuteSceneImport(
			const FSceneImportPlan&,
			const FMultiOutputExecutionOptions&)
			-> FSceneImportExecutionResult;
	};

	struct FSceneImportPlanResult
	{
		bool bSucceeded = false;
		std::string Message;
		FSceneImportPlan Plan;
		std::vector<FImportDiagnostic> Diagnostics;

		explicit operator bool() const { return bSucceeded; }
	};

	struct FSceneImportExecutionResult
	{
		bool bSucceeded = false;
		std::string Message;
		DImportRecord* Record = nullptr;
		std::vector<DStaticMesh*> Meshes;
		std::vector<DSkeleton*> Skeletons;
		std::vector<DSkeletalMesh*> SkeletalMeshes;
		std::vector<DAnimationClip*> AnimationClips;
		std::vector<DMaterialInstance*> Materials;
		std::vector<DTexture2D*> Textures;
		std::vector<FAssetPath> OrphanedAssets;
		std::vector<FImportDiagnostic> Diagnostics;
		FProviderLease Provider;

		explicit operator bool() const { return bSucceeded; }
	};

	class FSceneImportAsyncPlanHandle
	{
	public:
		auto IsValid() const -> bool
		{
			return GenericHandle.IsValid() || ImmediateResult.has_value();
		}
		explicit operator bool() const { return IsValid(); }
		auto GetStatus() const -> EAsyncImportPlanStatus;
		auto GetOperationHandle() const -> FAsyncImportPlanHandle
		{
			return GenericHandle;
		}

	private:
		FSceneImportRequest Request;
		FAsyncImportPlanHandle GenericHandle;
		std::optional<FSceneImportPlanResult> ImmediateResult;
		bool bConsumed = false;

		friend ASSETFORGE_API auto BeginSceneImportPlan(
			const FSceneImportRequest&, std::string_view, bool)
			-> FSceneImportAsyncPlanHandle;
		friend ASSETFORGE_API auto PollSceneImportPlan(
			FSceneImportAsyncPlanHandle&, FSceneImportPlanResult&)
			-> EAsyncImportPlanStatus;
		friend ASSETFORGE_API auto CancelAndDrainSceneImportPlan(
			FSceneImportAsyncPlanHandle&) -> void;
	};

	class FSceneImportAsyncExecutionHandle
	{
	public:
		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }

	private:
		std::shared_ptr<FSceneImportAsyncExecutionState> State;

		explicit FSceneImportAsyncExecutionHandle(
			std::shared_ptr<FSceneImportAsyncExecutionState> InState)
			: State(std::move(InState)) {}

		friend ASSETFORGE_API auto BeginSceneImportExecution(
			const FSceneImportPlan&,
			FMultiOutputExecutionOptions,
			FTaskScopeToken) -> FSceneImportAsyncExecutionHandle;
		friend ASSETFORGE_API auto PollSceneImportExecution(
			FSceneImportAsyncExecutionHandle&,
			FSceneImportExecutionResult&) -> EAsyncImportPlanStatus;
		friend ASSETFORGE_API auto CancelAndDrainSceneImportExecution(
			FSceneImportAsyncExecutionHandle&) -> void;
	};

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

	ASSETFORGE_API auto PlanSceneImport(
		const FSceneImportRequest& Request) -> FSceneImportPlanResult;
	ASSETFORGE_API auto BeginSceneImportPlan(
		const FSceneImportRequest& Request,
		std::string_view OwnerId,
		bool bKeepOperationOpenAfterPlan = false) -> FSceneImportAsyncPlanHandle;
	ASSETFORGE_API auto PollSceneImportPlan(
		FSceneImportAsyncPlanHandle& Handle,
		FSceneImportPlanResult& OutResult)
		-> EAsyncImportPlanStatus;
	ASSETFORGE_API auto CancelAndDrainSceneImportPlan(
		FSceneImportAsyncPlanHandle& Handle) -> void;
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
	ASSETFORGE_API auto PlanSceneReimport(
		DImportRecord& Record,
		bool bRecreateMissingManagedOutputs = false,
		IImportProgressReporter* Progress = nullptr) -> FSceneImportPlanResult;
	ASSETFORGE_API auto ExecuteSceneImport(
		const FSceneImportPlan& Plan,
		const FMultiOutputExecutionOptions& Options = {})
		-> FSceneImportExecutionResult;
	ASSETFORGE_API auto BeginSceneImportExecution(
		const FSceneImportPlan& Plan,
		FMultiOutputExecutionOptions Options = {},
		FTaskScopeToken OperationScope = {})
		-> FSceneImportAsyncExecutionHandle;
	ASSETFORGE_API auto PollSceneImportExecution(
		FSceneImportAsyncExecutionHandle& Handle,
		FSceneImportExecutionResult& OutResult) -> EAsyncImportPlanStatus;
	ASSETFORGE_API auto CancelAndDrainSceneImportExecution(
		FSceneImportAsyncExecutionHandle& Handle) -> void;
	ASSETFORGE_API auto FindSceneImportRecordForOutput(
		const DObject& Output,
		std::string& OutError) -> DImportRecord*;
	ASSETFORGE_API auto EnsureImportedSurfaceMaterial(
		std::string& OutError) -> DMaterial*;
}
