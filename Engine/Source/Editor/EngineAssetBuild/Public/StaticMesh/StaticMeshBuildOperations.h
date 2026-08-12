#pragma once

#include "EngineAssetBuildAPI.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin::AssetBuild
{
	inline constexpr uint32 MaximumStaticMeshImportedUVChannels = 4;

	struct FStaticMeshImportedMaterialSlot
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
		std::string SourceName;
	};

	struct FStaticMeshImportedMesh
	{
		std::string Name;
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaximumStaticMeshImportedUVChannels> UVChannels;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		uint32 SourceMaterialIndex = 0;
	};

	struct FStaticMeshImportedData
	{
		std::vector<FStaticMeshImportedMaterialSlot> MaterialSlots;
		std::vector<FStaticMeshImportedMesh> Meshes;
	};

	using FStaticMeshBuildProduct = FStaticMeshAuthoringProduct;

	class ENGINEASSETBUILD_API FStaticMeshBuildOperations
	{
	public:
		static auto BuildAndPublishImported(
			DStaticMesh& Mesh,
			const FStaticMeshImportedData& ImportedData,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceLabel,
			std::string& OutError) -> bool;

		static auto BuildImportedProduct(
			DStaticMesh& ReconciliationOwner,
			const FStaticMeshImportedData& ImportedData,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceLabel,
			FStaticMeshBuildProduct& OutProduct,
			std::string& OutError) -> bool;

		static auto PublishImportedProduct(
			DStaticMesh& Mesh,
			FStaticMeshBuildProduct Product,
			std::string& OutError) -> bool;

		static auto LoadDerivedDataProduct(
			DStaticMesh& Mesh,
			FStaticMeshSourceImportData SourceImportData,
			bool bSourceAvailable,
			FStaticMeshBuildProduct& OutProduct,
			EStaticMeshDerivedDataStatus& OutStatus,
			std::string& OutMessage,
			std::string& OutError) -> bool;

		static auto BuildCollisionProduct(
			const FStaticMeshRenderData& RenderData,
			const FStaticMeshSourceImportData& SourceImportData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionAuthoringProduct& OutProduct,
			std::string& OutError) -> bool;
	};
}
