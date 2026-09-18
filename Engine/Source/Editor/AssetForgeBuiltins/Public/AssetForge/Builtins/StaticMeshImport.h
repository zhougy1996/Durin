#pragma once

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
		std::optional<FStaticMeshSubmissionError> SubmissionCause;
		std::optional<FStaticMeshCompilationDiagnostic> CompletionCause;
	};
	struct FStaticMeshRebuildResult
	{
		FStaticMeshRebuildError Error;
		explicit operator bool() const { return Error.Code == EStaticMeshRebuildError::None; }
	};
	ASSETFORGEBUILTINS_API auto FormatStaticMeshRebuildError(const FStaticMeshRebuildError& Error) -> std::string;

	ASSETFORGEBUILTINS_API auto ReimportStaticMesh(
		DStaticMesh& Mesh,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> FStaticMeshRebuildResult;
	ASSETFORGEBUILTINS_API auto ReimportStaticMeshFromFile(
		DStaticMesh& Mesh,
		std::string_view FilePath,
		const FAssetBundleSaveOptions& SaveOptions = {}) -> FStaticMeshRebuildResult;
	ASSETFORGEBUILTINS_API auto CreateTransientStaticMeshFromFile(
		std::string_view FilePath,
		DObject* Outer,
		std::string_view ObjectName,
		DStaticMesh*& OutMesh,
		const FStaticMeshImportSettings& ImportSettings = {}) -> FStaticMeshRebuildResult;
}
