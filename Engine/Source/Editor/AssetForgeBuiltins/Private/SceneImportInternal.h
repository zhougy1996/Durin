#pragma once

#include "SceneSourceSnapshot.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "ImportedSceneInternal.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	enum class ESceneOutputKind : uint8
	{
		Skeleton, SkeletalMesh, AnimationClip, StaticMesh, MaterialInstance, Texture2D
	};
	enum class ESceneTextureDerivation : uint8
	{
		None, Red, Green, Blue, Alpha, ScaledNormal, ScaledColor
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
		ETextureUsage TextureUsage = ETextureUsage::Color;
		ESceneTextureDerivation TextureDerivation = ESceneTextureDerivation::None;
		float TextureDerivationScale = 1.0f;
		FVector3f TextureDerivationColorScale{1.0f};
		std::vector<FSceneMaterialTextureBinding> TextureBindings;
		std::string SkeletonIdentity;
	};
	// Carries decoded scene data and stable output descriptors into product construction.
	struct FSceneImportPlan
	{
		FImportedSceneData Scene;
		FStaticMeshImportSettings MeshSettings;
		std::vector<FSceneOutputData> Outputs;
		std::vector<std::string> Warnings;
	};
	struct FSceneTextureBuildProduct
	{
		FTexture2DBuildProduct Product;
		std::string SourceFilename;
		FByteArray GeneratedSourceBytes;
		uint64 SourceFileSize = 0;
	};

	auto BuildScenePlan(
		const FSourceSnapshot& Snapshot,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		FSceneImportPlan& OutPlan,
		std::vector<FImportOutputSummary>& OutOutputs,
		std::vector<FImportDiagnostic>& OutDiagnostics,
		std::string& OutError) -> bool;
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
		const FSceneImportPlan& Data,
		const FSceneOutputData& Descriptor,
		const std::function<bool()>& IsCancellationRequested,
		FSceneTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool;
}
