#pragma once

#include "CoreMinimal.h"
#include "StaticMesh/StaticMeshImportSettings.h"

#include "SceneSourceSnapshot.h"
#include "AssetForge/Builtins/AssetImport.h"
#include "AssetForge/Builtins/ImportedSurfaceRecipe.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "ImportedSceneInternal.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DBuild.h"

namespace Durin::AssetForge::Builtins
{
	enum class ESceneOutputKind : uint8
	{
		StaticMesh, Material, MaterialInstance, Texture2D
	};
	inline auto IsSceneMaterial(ESceneOutputKind Kind) -> bool
	{
		return Kind == ESceneOutputKind::Material || Kind == ESceneOutputKind::MaterialInstance;
	}
	enum class ESceneTextureDerivation : uint8
	{
		None, Red, Green, Blue, Alpha, ScaledNormal, ScaledColor, ScaledOcclusion
	};
	struct FSceneMaterialTextureBinding
	{
		uint32 MaterialRole = 0;
		std::string TextureIdentity;
		FImportedTextureBinding Binding;
	};
	struct FSceneOutputData
	{
		std::string StableIdentity;
		ESceneOutputKind Kind = ESceneOutputKind::StaticMesh;
		uint32 SourceIndex = 0;
		bool bCombinedMesh = true;
		std::vector<std::string> Dependencies;
		ETextureUsage TextureUsage = ETextureUsage::Color;
		ESceneTextureDerivation TextureDerivation = ESceneTextureDerivation::None;
		float TextureDerivationScale = 1.0f;
		FVector3f TextureDerivationColorScale{1.0f};
		std::vector<FSceneMaterialTextureBinding> TextureBindings;
		TStrongObjectPtr<DMaterial> Parent;
		bool bStandardPBRParent = false;
		bool bExistingMaterialMapping = false;
		TStrongObjectPtr<DMaterialInterface> PreservedMaterial;
		uint64 PreviousRevision = 0;
	};
	// Carries decoded scene data and stable output descriptors into product construction.
	struct FAssetImportPlan
	{
		FImportedDocument Document;
		FImportedSceneData Scene;
		FStaticMeshImportSettings MeshSettings;
		std::vector<uint32> SelectedMeshes;
		std::vector<FSceneOutputData> Outputs;
		std::vector<std::string> Warnings;
	};
	struct FSceneTextureBuildProduct
	{
		FTextureSource SourceData;
		FTexture2DBuildSettings Settings;
		FTexturePlatformData PlatformData;
		FXxHash128 EncodedSourceHash;
		std::string SourceFilename;
		FByteBuffer GeneratedSourceBytes;
		uint64 SourceFileSize = 0;
	};
	auto ConfigureSceneMaterials(FAssetImportPlan& Plan,
		std::vector<FImportOutputSummary>& Outputs,
		const FPackagePath& Destination, const FSceneMaterialImportOptions& Options,
		std::vector<FSceneMaterialPreview>& Preview, std::string& Error) -> bool;
	auto MakeSceneSurfaceRoles(const FAssetImportPlan& Plan, const FSceneOutputData& Output)
		-> std::array<FImportedSurfaceRole, 8>;

	auto BuildScenePlan(
		const FSourceSnapshot& Snapshot,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		FAssetImportPlan& OutPlan,
		std::vector<FImportOutputSummary>& OutOutputs,
		std::vector<FImportDiagnostic>& OutDiagnostics,
		std::string& OutError,
		const FAssetImportOptions& Options = FAssetImportOptions::LegacyCombined(),
		FImportedDocument* PreparedDocument = nullptr) -> bool;
	auto DiscoverSceneImportDependencies(
		std::span<const FSourceSnapshotEntry> Sources,
		FDependencyRequestSink& Sink,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
	auto DecodeSceneSnapshotForImport(
		const FSourceSnapshot& Snapshot,
		const FStaticMeshImportSettings& Settings,
		FImportedSceneData& OutScene,
		std::string& OutError) -> bool;
	auto BuildSceneImportTextureProduct(
		const FSourceSnapshot& Snapshot,
		const FAssetImportPlan& Data,
		const FSceneOutputData& Descriptor,
		const std::function<bool()>& IsCancellationRequested,
		FSceneTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool;
}
