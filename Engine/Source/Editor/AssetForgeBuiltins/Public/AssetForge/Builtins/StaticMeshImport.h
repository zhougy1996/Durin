#pragma once

#include <expected>

#include "AssetForgeBuiltinsAPI.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "Asset/PackageSerialization.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Asset/SourceHint.h"
#include "Asset/AssetImportData.h"
#include "Misc/MountPaths.h"

namespace Durin::AssetForge::Builtins
{
	// Import policy: preserve existing slot identities and bindings while matching the new source.
	ASSETFORGEBUILTINS_API auto ReconcileStaticMeshMaterialSlots(
		std::span<const FMeshMaterialSlotDefinition> PreviousMaterialSlots,
		std::span<const FStaticMeshImportedMaterialSlot> ImportedSlots) -> std::vector<FMeshMaterialSlotDefinition>;

	struct FEncodedSourceError;
	enum class EStaticMeshRebuildError : uint8
	{
		None, Settings, Package, Mount, SourceHint, SourceFile, Capture, Decode,
		Source, ImportValidation, Submission, Completion, ImportData, MissingSource, Path, ObjectCreation, ObjectType, SourceCount
	};
	struct FStaticMeshRebuildError
	{
		EStaticMeshRebuildError Code = EStaticMeshRebuildError::None;
		std::string ObjectPath;
		std::string Filename;
		std::string ObjectName;
		uint64 SourceCount = 0;
		std::error_code SystemError;
		EMountPathError MountCause = EMountPathError::None;
		std::optional<FStaticMeshImportSettingsError> SettingsCause;
		std::optional<FSourceHintError> SourceHintCause;
		std::shared_ptr<const FEncodedSourceError> CaptureCause;
		std::vector<FSceneImportDiagnostic> DecodeCauses;
		std::optional<FStaticMeshSourceError> SourceCause;
		std::shared_ptr<const FAssetImportDataError> ImportCause;
		std::vector<std::string> SubmissionErrors;
		std::optional<FStaticMeshCompilationResult> CompletionCause;
	};
	using FStaticMeshRebuildResult = std::expected<void, FStaticMeshRebuildError>;
	ASSETFORGEBUILTINS_API auto FormatStaticMeshRebuildError(const FStaticMeshRebuildError& Error) -> std::string;

	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportStaticMesh(
		DStaticMesh& Mesh,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> FStaticMeshRebuildResult;
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportStaticMeshFromFile(
		DStaticMesh& Mesh,
		std::string_view FilePath,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> FStaticMeshRebuildResult;
	[[nodiscard]] ASSETFORGEBUILTINS_API auto CreateTransientStaticMeshFromFile(
		std::string_view FilePath,
		DObject* Outer,
		std::string_view ObjectName,
		const FStaticMeshImportSettings& ImportSettings = {}) -> std::expected<DStaticMesh*, FStaticMeshRebuildError>;
}
