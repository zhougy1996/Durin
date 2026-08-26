#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Terrain/TerrainHeightmap.h"
#include "AssetForge/ImportRequest.h"
#include "AssetForge/Operations/ImportExecution.h"

namespace Durin
{
	class DTerrainHeightmap;
}

namespace Durin::AssetForge::Builtins
{
	ASSETFORGEBUILTINS_API auto IsTerrainHeightmapSourceExtension(
		std::string_view Extension) -> bool;
	// Carries one admitted terrain source into the source-format-neutral canonical builder.
	struct FTerrainHeightmapSourceData
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;

		auto IsValid() const -> bool
		{
			return Width != 0 && Height != 0
				&& Samples.size() == static_cast<size_t>(Width) * Height;
		}
	};

	ASSETFORGEBUILTINS_API auto TranslateTerrainHeightmapSource(
		std::string_view Extension,
		std::span<const std::byte> EncodedBytes,
		FTerrainHeightmapSourceData& OutSource,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings = {},
		bool bAllowEngineContentWrite = false) -> FTerrainHeightmapImportResult;
	ASSETFORGEBUILTINS_API auto ChangeTerrainHeightmapSourceReference(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ReimportTerrainHeightmapSource(
		DTerrainHeightmap& Heightmap,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto MakeTerrainHeightmapImportRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		AssetForge::EImportMode Mode,
		AssetForge::FImportOperationOwner Owner,
		std::optional<AssetForge::FImportProvenance> ExistingProvenance,
		AssetForge::FImportRequest& OutRequest,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto InspectTerrainHeightmapImportProvenance(
		const DTerrainHeightmap& Heightmap,
		AssetForge::FImportProvenance& OutProvenance,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SubmitTerrainHeightmapImport(
		std::string_view FilePath, const FAssetPath& Destination,
		std::string_view SourceDestination, bool bAllowEngineContentWrite,
		AssetForge::FImportCompletion Completion,
		std::string& OutError) -> AssetForge::FImportHandle;
}
