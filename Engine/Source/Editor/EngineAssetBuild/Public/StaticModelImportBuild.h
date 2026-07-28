#pragma once

#include "Asset/SourcePath.h"
#include "EngineAssetBuildAPI.h"
#include "DObject/AssetPath.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	class DMaterial;
	struct FMultiAssetImportTransactionTestAccess;
	struct FStaticModelImportPlanData;
	struct FStaticModelImportPlanResult;

	inline constexpr std::string_view StandardImportedSurfaceMaterialPath =
		"/Engine/Materials/ImportedSurface";

	enum class EStaticModelPlannedAssetKind : uint8
	{
		StaticMesh,
		MaterialInstance,
		Texture2D
	};

	enum class EStaticModelPlannedSourceAction : uint8
	{
		Reference,
		Ingest,
		Extract
	};

	struct FStaticModelPlannedAsset
	{
		EStaticModelPlannedAssetKind Kind = EStaticModelPlannedAssetKind::StaticMesh;
		FAssetPath AssetPath;
		uint32 SourceIndex = 0;
		std::optional<uint32> TextureAssetIndex;
	};

	struct FStaticModelPlannedSource
	{
		EStaticModelPlannedSourceAction Action = EStaticModelPlannedSourceAction::Reference;
		FSourcePath SourcePath;
		std::string StableIdentity;
		uint64 ByteCount = 0;
	};

	struct FStaticModelImportPlanRequest
	{
		std::filesystem::path SourceFile;
		FAssetPath RootAssetPath;
		FSourcePath RootSourceDestination;
		FStaticMeshImportSettings ImportSettings;
	};

	struct FStaticModelImportPlan
	{
		FAssetPath RootAssetPath;
		FSourcePath RootSource;
		std::string StandardMaterialPath;
		std::vector<FStaticModelPlannedAsset> Assets;
		std::vector<FStaticModelPlannedSource> Sources;
		std::vector<std::string> Warnings;

	private:
		std::shared_ptr<const FStaticModelImportPlanData> Data;

		friend ENGINEASSETBUILD_API auto PlanStaticModelImport(
			const FStaticModelImportPlanRequest& Request) -> FStaticModelImportPlanResult;
	};

	struct FStaticModelImportPlanResult
	{
		bool bSucceeded = false;
		std::string Message;
		FStaticModelImportPlan Plan;

		explicit operator bool() const { return bSucceeded; }
	};

	ENGINEASSETBUILD_API auto PlanStaticModelImport(
		const FStaticModelImportPlanRequest& Request) -> FStaticModelImportPlanResult;
	ENGINEASSETBUILD_API auto EnsureStandardImportedSurfaceMaterial(
		std::string& OutError) -> DMaterial*;

	struct FPortableTextureBuildRequest
	{
		FAssetPath AssetPath;

		// Exactly one of ExternalSource and EncodedBytes must be populated.
		// Mounted external sources are referenced in place. Other external
		// sources and embedded bytes require an explicit writable destination.
		std::filesystem::path ExternalSource;
		std::vector<uint8> EncodedBytes;
		FSourcePath SourceDestination;
		FTexture2DImportSettings Settings;
		bool bRootPackage = false;
	};

	struct FPortableSourceBuildRequest
	{
		FAssetPath AuthoringAssetPath;

		// Exactly one payload is required. A mounted external source is referenced
		// in place; other payloads require SourceDestination.
		std::filesystem::path ExternalSource;
		std::vector<uint8> EncodedBytes;
		FSourcePath SourceDestination;
	};

	struct FImportTransactionResult
	{
		bool bSucceeded = false;
		std::string Message;
		std::vector<DTexture2D*> Textures;

		explicit operator bool() const { return bSucceeded; }
	};

	ENGINEASSETBUILD_API auto BuildEmbeddedImageSourcePath(
		const FSourcePath& RootModelSource,
		std::string_view ImageIdentity,
		std::string_view SuggestedName,
		std::string_view Extension,
		FSourcePath& OutPath,
		std::string& OutError) -> bool;

	class ENGINEASSETBUILD_API FMultiAssetImportTransaction
	{
	public:
		FMultiAssetImportTransaction();
		~FMultiAssetImportTransaction();

		FMultiAssetImportTransaction(const FMultiAssetImportTransaction&) = delete;
		auto operator=(const FMultiAssetImportTransaction&) -> FMultiAssetImportTransaction& = delete;
		FMultiAssetImportTransaction(FMultiAssetImportTransaction&&) = delete;
		auto operator=(FMultiAssetImportTransaction&&) -> FMultiAssetImportTransaction& = delete;

		auto AddTexture(FPortableTextureBuildRequest Request) -> void;
		auto AddSource(FPortableSourceBuildRequest Request) -> void;
		auto AddPackage(DPackage* Package, bool bRootPackage = false) -> void;
		auto AddLoadedObjectMutation(
			std::function<bool(std::string&)> Apply,
			std::function<void()> Rollback) -> void;
		auto Prepare(std::string& OutError) -> bool;
		auto Stage(std::string& OutError) -> bool;
		auto Publish(std::string& OutError) -> bool;
		auto Rollback() -> void;
		auto Execute() -> FImportTransactionResult;
		auto GetTextures() const -> std::span<DTexture2D* const>;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;

		friend struct FMultiAssetImportTransactionTestAccess;
	};
}
