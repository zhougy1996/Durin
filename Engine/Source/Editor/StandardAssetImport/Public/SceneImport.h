#pragma once

#include "ImportRecordIndex.h"
#include "MultiOutputImport.h"
#include "StandardAssetImportAPI.h"
#include "StaticMesh/StaticMesh.h"
#include "Source/SourcePath.h"

namespace Durin
{
	class DMaterial;
	class DMaterialInstance;
	class DTexture2D;

	inline constexpr std::string_view SceneImportProviderId = "Durin.Scene";
	inline constexpr uint32 SceneImportProviderContractVersion = 1;
	inline constexpr std::string_view StandardImportedSurfaceMaterialPath =
		"/Engine/Materials/ImportedSurface";

	struct FSceneImportRequest
	{
		// Scene import plans consume an already mounted source. Explicit ingestion
		// of an external authoring file is a separate editor operation.
		FSourcePath RootSource;
		FAssetPath PrimaryOutput;
		FStaticMeshImportSettings MeshSettings;
		AssetImport::DImportRecord* ExistingRecord = nullptr;
		bool bRecreateMissingManagedOutputs = false;
		AssetImport::IImportProgressReporter* Progress = nullptr;
	};

	struct FPreparedSceneSourceBundle
	{
		std::vector<FMountedSourceFile> Sources;
		FSourcePath RootSource;
	};
	struct FSceneImportPlanResult;
	struct FSceneImportExecutionResult;

	class FSceneImportPlan
	{
	public:
		auto GetMultiOutputPlan() const
			-> const AssetImport::FMultiOutputImportPlan& { return MultiOutputPlan; }
		auto GetWarnings() const -> std::span<const std::string> { return Warnings; }

	private:
		AssetImport::FMultiOutputImportPlan MultiOutputPlan;
		std::vector<std::string> Warnings;

		friend STANDARDASSETIMPORT_API auto PlanSceneImport(
			const FSceneImportRequest&) -> FSceneImportPlanResult;
		friend STANDARDASSETIMPORT_API auto ExecuteSceneImport(
			const FSceneImportPlan&,
			const AssetImport::FMultiOutputExecutionOptions&)
			-> FSceneImportExecutionResult;
	};

	struct FSceneImportPlanResult
	{
		bool bSucceeded = false;
		std::string Message;
		FSceneImportPlan Plan;
		std::vector<AssetImport::FImportDiagnostic> Diagnostics;

		explicit operator bool() const { return bSucceeded; }
	};

	struct FSceneImportExecutionResult
	{
		bool bSucceeded = false;
		std::string Message;
		AssetImport::DImportRecord* Record = nullptr;
		DStaticMesh* StaticMesh = nullptr;
		std::vector<DMaterialInstance*> Materials;
		std::vector<DTexture2D*> Textures;
		std::vector<FAssetPath> OrphanedAssets;
		std::vector<AssetImport::FImportDiagnostic> Diagnostics;
		AssetImport::FProviderLease Provider;

		explicit operator bool() const { return bSucceeded; }
	};

	enum class ELegacySceneMigrationStatus : uint8
	{
		NotLegacy,
		Migrated,
		RepairRequired,
		Failed
	};

	struct FLegacySceneMigrationResult
	{
		ELegacySceneMigrationStatus Status = ELegacySceneMigrationStatus::NotLegacy;
		std::string Message;
		AssetImport::DImportRecord* Record = nullptr;

		explicit operator bool() const
		{
			return Status == ELegacySceneMigrationStatus::Migrated;
		}
	};

	STANDARDASSETIMPORT_API auto PlanSceneImport(
		const FSceneImportRequest& Request) -> FSceneImportPlanResult;
	STANDARDASSETIMPORT_API auto PrepareSceneSourceBundle(
		const std::filesystem::path& InputRoot,
		std::string_view ReferencingAssetPath,
		std::string_view ExternalIngestDestination,
		FPreparedSceneSourceBundle& OutBundle,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto CommitSceneSourceBundle(
		FPreparedSceneSourceBundle& Bundle) -> void;
	STANDARDASSETIMPORT_API auto RollbackSceneSourceBundle(
		FPreparedSceneSourceBundle& Bundle) -> void;
	STANDARDASSETIMPORT_API auto PlanSceneReimport(
		AssetImport::DImportRecord& Record,
		bool bRecreateMissingManagedOutputs = false,
		AssetImport::IImportProgressReporter* Progress = nullptr) -> FSceneImportPlanResult;
	STANDARDASSETIMPORT_API auto ExecuteSceneImport(
		const FSceneImportPlan& Plan,
		const AssetImport::FMultiOutputExecutionOptions& Options = {})
		-> FSceneImportExecutionResult;
	STANDARDASSETIMPORT_API auto FindSceneImportRecordForOutput(
		const DObject& Output,
		std::string& OutError) -> AssetImport::DImportRecord*;
	STANDARDASSETIMPORT_API auto MigrateLegacySceneImport(
		DStaticMesh& StaticMesh) -> FLegacySceneMigrationResult;
	STANDARDASSETIMPORT_API auto EnsureStandardImportedSurfaceMaterial(
		std::string& OutError) -> DMaterial*;
}
