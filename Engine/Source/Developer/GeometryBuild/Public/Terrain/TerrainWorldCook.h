#pragma once

#include "Terrain/TerrainWorldTile.h"
#include "Asset/Cook.h"

namespace Durin::Asset::Build
{
	struct FTerrainRegionKey
	{
		FTerrainWorldId WorldId;
		int64 RegionX = 0;
		int64 RegionY = 0;
		uint16 SchemeVersion = TerrainWorldTileSchemeVersion;
		auto operator<=>(const FTerrainRegionKey&) const = default;
	};

	struct FTerrainManifestProductEntry
	{
		FTerrainTileKey Tile;
		FGuid GenerationId;
		ETerrainTileProductClass ProductClass = ETerrainTileProductClass::Metadata;
		Asset::FCookedPayloadDescriptor Descriptor;
		FXxHash128 ProductHash;
		std::vector<FXxHash128> Dependencies;
		auto operator==(const FTerrainManifestProductEntry&) const -> bool = default;
	};

	struct FTerrainManifestRegion
	{
		FTerrainRegionKey Region;
		bool bInstalled = false;
		std::string VirtualPackagePath;
		std::vector<FTerrainManifestProductEntry> Products;
	};

	struct FTerrainWorldManifest
	{
		FTerrainWorldId WorldId;
		uint16 SchemaVersion = TerrainWorldSchemaVersion;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Win64;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Game;
		std::vector<FTerrainManifestRegion> Regions;
	};

	struct FTerrainWorldCookRequest
	{
		std::filesystem::path CookRoot;
		std::string VirtualWorldRoot;
		FTerrainWorldId WorldId;
		std::vector<FTerrainTileGeneration> Generations;
		std::vector<FTerrainRegionKey> InstalledRegions;
		std::vector<std::byte> PackageTemplateBytes;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Win64;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Game;
	};

	struct FTerrainCookedProductHandle
	{
		std::shared_ptr<const FTerrainWorldManifest> Manifest;
		std::shared_ptr<Asset::FCookedPackagePayload> PackagePayload;
		FTerrainManifestProductEntry Entry;
		auto GetBytes() const -> std::span<const std::byte>
		{
			return PackagePayload ? PackagePayload->Payload : std::span<const std::byte>{};
		}
	};

	GEOMETRYBUILD_API auto GetTerrainRegionKey(
		const FTerrainTileKey& Tile, FTerrainRegionKey& OutRegion,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	GEOMETRYBUILD_API auto EncodeTerrainWorldManifest(
		const FTerrainWorldManifest& Manifest, std::vector<std::byte>& OutBytes,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	GEOMETRYBUILD_API auto DecodeTerrainWorldManifest(
		std::span<const std::byte> Bytes, const FTerrainWorldId& ExpectedWorld,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FTerrainWorldManifest& OutManifest, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
	GEOMETRYBUILD_API auto CookTerrainWorld(
		const FTerrainWorldCookRequest& Request, FTerrainWorldManifest& OutManifest,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	GEOMETRYBUILD_API auto LoadCookedTerrainWorldManifest(
		const Asset::FAssetRuntimeConfiguration& RuntimeConfiguration,
		std::string_view VirtualWorldRoot, const FTerrainWorldId& WorldId,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		std::shared_ptr<const FTerrainWorldManifest>& OutManifest,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	GEOMETRYBUILD_API auto LoadCookedTerrainProduct(
		const Asset::FAssetRuntimeConfiguration& RuntimeConfiguration,
		const std::shared_ptr<const FTerrainWorldManifest>& Manifest,
		const FTerrainTileKey& Tile, const FGuid& GenerationId,
		ETerrainTileProductClass ProductClass, FTerrainCookedProductHandle& OutHandle,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
}
