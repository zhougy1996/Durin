#pragma once

#include "AssetImportCore.h"
#include "ImportedScene.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"

namespace Durin::Asset::Forge
{
	enum class ESceneOutputKind : uint8
	{
		Skeleton, SkeletalMesh, AnimationClip, StaticMesh, MaterialInstance, Texture2D,
		ImportRecord
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
	struct FSceneProviderPlanData
	{
		FImportedSceneData Scene;
		FStaticMeshImportSettings MeshSettings;
		std::vector<FSceneOutputData> Outputs;
		std::vector<std::string> Warnings;
	};
	struct FSceneInterchangeTextureProduct
	{
		Asset::Build::FTexture2DBuildProduct Product;
		FSourcePath Source;
		std::vector<std::byte> SourceBytesToMount;
		uint64 SourceFileSize = 0;
	};

	auto BuildSceneInterchangePlanData(
		const FSourceSnapshot& Snapshot,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		std::shared_ptr<const FSceneProviderPlanData>& OutData,
		std::vector<FImportOutputPreview>& OutOutputs,
		std::vector<FImportDiagnostic>& OutDiagnostics,
		std::string& OutError) -> bool;
	auto DiscoverSceneInterchangeDependencies(
		std::span<const FSourceSnapshotEntry> Sources,
		FDependencyRequestSink& Sink,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
	auto DecodeSceneSnapshotForInterchange(
		const FSourceSnapshot& Snapshot,
		const FStaticMeshImportSettings& Settings,
		FImportedSceneData& OutScene,
		std::string& OutError) -> bool;
	auto BuildSceneInterchangeTextureProduct(
		const FSourceSnapshot& Snapshot,
		const FSceneProviderPlanData& Data,
		const FSceneOutputData& Descriptor,
		const std::function<bool()>& IsCancellationRequested,
		FSceneInterchangeTextureProduct& OutProduct,
		std::string& OutError) -> bool;
}
