#pragma once

#include "TerrainBuildAPI.h"
#include "Asset/CookedAsset.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"

namespace Durin::Asset::Build
{
	inline constexpr uint16 TerrainWorldSchemaVersion = 1;
	inline constexpr uint16 TerrainWorldTileSchemeVersion = 1;
	inline constexpr int64 TerrainWorldTileCells = 256;
	inline constexpr int64 TerrainWorldTileSamples = 257;
	inline constexpr uint32 TerrainWorldSampleCount = 257u * 257u;
	inline constexpr uint32 TerrainWorldMaximumLayers = 64;
	inline constexpr uint32 TerrainWorldMaximumSources = 1024;
	inline constexpr uint32 TerrainWorldMaximumTileSources = 64;
	inline constexpr uint32 TerrainWorldMaximumTileLayers = 16;
	inline constexpr uint32 TerrainWorldMaximumActiveLayers = 4;
	inline constexpr uint32 TerrainWorldMaximumDependencies = 64;
	inline constexpr uint32 TerrainWorldMaximumNeighbors = 8;
	inline constexpr uint64 TerrainWorldMaximumRegionStoredBytes = 64ull * 1024ull * 1024ull;
	inline constexpr uint64 TerrainWorldMaximumRegionLogicalBytes = 256ull * 1024ull * 1024ull;
	inline constexpr uint8 TerrainSourceAffectsHeight = 1u << 0;
	inline constexpr uint8 TerrainSourceAffectsCoverage = 1u << 1;

	enum class ETerrainWorldOutcome : uint8
	{
		Ready,
		Unavailable,
		InvalidDefinition,
		UnsupportedLegacySchema,
		MissingDependency,
		BorderMismatch,
		Overflow,
		BudgetRejected,
		Cancelled,
		Superseded,
		Corrupt,
		Incompatible,
		PublicationFailed
	};

	enum class ETerrainTileProductClass : uint8
	{
		Metadata = 1,
		Height = 2,
		Coverage = 3,
		Collision = 4,
		Query = 5
	};

	enum class ETerrainTileBuildOrigin : uint8
	{
		LocalBuild,
		DerivedData
	};

	enum class ETerrainCompositionBlendOperation : uint8
	{
		Replace = 1,
		Add = 2,
		Minimum = 3,
		Maximum = 4
	};

	struct FTerrainWorldId
	{
		FGuid Value;
		auto IsValid() const -> bool { return Value.IsValid(); }
		auto operator<=>(const FTerrainWorldId&) const = default;
	};

	struct FTerrainGlobalSample
	{
		int64 X = 0;
		int64 Y = 0;
		auto operator<=>(const FTerrainGlobalSample&) const = default;
	};

	struct FTerrainLocalSample
	{
		uint16 X = 0;
		uint16 Y = 0;
		auto IsValid() const -> bool { return X <= 256 && Y <= 256; }
		auto operator<=>(const FTerrainLocalSample&) const = default;
	};

	struct FTerrainSampleExtent
	{
		FTerrainGlobalSample Min;
		FTerrainGlobalSample Max;
		auto operator<=>(const FTerrainSampleExtent&) const = default;
	};

	struct FTerrainTileKey
	{
		FTerrainWorldId WorldId;
		int64 TileX = 0;
		int64 TileY = 0;
		uint16 SchemeVersion = TerrainWorldTileSchemeVersion;
		auto operator<=>(const FTerrainTileKey&) const = default;
	};

	struct FTerrainTileAddress
	{
		FTerrainTileKey Tile;
		FTerrainLocalSample Local;
	};

	struct FTerrainWorldCoordinates
	{
		double OriginX = 0.0;
		double OriginY = 0.0;
		double OriginZ = 0.0;
		double SampleSpacingMeters = 1.0;
		double HeightDatumMeters = 0.0;
	};

	struct FTerrainLayerDefinition
	{
		FGuid LayerId;
		std::string DisplayName;
		FGuid PhysicalSurfaceId;
	};

	struct FTerrainCompositionSource
	{
		FGuid SourceId;
		FXxHash128 ContentHash;
		FTerrainSampleExtent AffectedSamples;
		uint8 BlendOperation = 0;
		int32 Strength = 0;
		bool bEnabled = true;
		uint8 AffectedProductMask = TerrainSourceAffectsHeight | TerrainSourceAffectsCoverage;
	};

	struct FTerrainWorldDefinition
	{
		FTerrainWorldId WorldId;
		FTerrainWorldCoordinates Coordinates;
		FTerrainSampleExtent SampleExtent;
		uint16 TileSchemeVersion = TerrainWorldTileSchemeVersion;
		uint8 ProductProfile = 0;
		uint64 PeakBuildBudgetBytes = 0;
		std::vector<FTerrainLayerDefinition> Layers;
		std::vector<FTerrainCompositionSource> Sources;
		FGuid BuildPolicyId;
		uint32 BuildPolicyVersion = 0;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		uint8 RegionTileDimension = 8;
	};

	struct FTerrainCoverageWeight
	{
		FGuid LayerId;
		uint8 Weight = 0;
		auto operator<=>(const FTerrainCoverageWeight&) const = default;
	};

	struct FTerrainCoverageSample
	{
		std::array<FTerrainCoverageWeight, TerrainWorldMaximumActiveLayers> Layers{};
		uint8 LayerCount = 0;
	};

	struct FTerrainNeighborEvidence
	{
		bool bPresent = false;
		FTerrainTileKey Tile;
		FXxHash128 HeightEdgeHash;
		FXxHash128 CoverageEdgeHash;
	};

	struct FTerrainNormalizedTileInput
	{
		FTerrainTileKey Tile;
		FTerrainSampleExtent WorldExtent;
		FTerrainWorldCoordinates Coordinates;
		std::vector<FTerrainCompositionSource> Sources;
		std::vector<FGuid> LayerIds;
		std::vector<int16> Heights;
		std::vector<FTerrainCoverageSample> Coverage;
		std::vector<int16> HeightHalo;
		std::vector<FTerrainCoverageSample> CoverageHalo;
		std::array<FTerrainNeighborEvidence, TerrainWorldMaximumNeighbors> Neighbors{};
		FGuid CompositionPolicyId;
		uint32 CompositionPolicyVersion = 0;
		uint32 BuilderVersion = 1;
		uint16 ProductSchemaVersion = TerrainWorldSchemaVersion;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		uint32 OutputPolicy = 0;
		bool bQueryDerivedData = true;
		bool bPersistDerivedData = true;
		std::function<bool()> ShouldCancel;
	};

	struct FTerrainComposedTileValues
	{
		std::vector<int16> Heights;
		std::vector<FTerrainCoverageSample> Coverage;
		std::vector<int16> HeightHalo;
		std::vector<FTerrainCoverageSample> CoverageHalo;
		std::array<FTerrainNeighborEvidence, TerrainWorldMaximumNeighbors> Neighbors{};
		std::function<bool()> ShouldCancel;
	};

	struct FTerrainTileSourceContribution
	{
		FGuid SourceId;
		FXxHash128 ContentHash;
		std::vector<int16> Heights;
		std::vector<FTerrainCoverageSample> Coverage;
	};

	struct FTerrainTileProduct
	{
		ETerrainTileProductClass ProductClass = ETerrainTileProductClass::Metadata;
		FTerrainTileKey Tile;
		FGuid GenerationId;
		FXxHash128 BodyHash;
		std::vector<FXxHash128> Dependencies;
		std::vector<std::byte> Bytes;
		std::string DerivedDataKey;
		ETerrainTileBuildOrigin Origin = ETerrainTileBuildOrigin::LocalBuild;
	};

	struct FTerrainTileGeneration
	{
		FTerrainTileKey Tile;
		FGuid GenerationId;
		std::array<FTerrainTileProduct, 5> Products;
	};

	class FTerrainTileGenerationPublisher
	{
	public:
		TERRAINBUILD_API auto BeginRequest() -> uint64;
		TERRAINBUILD_API auto Publish(
			uint64 RequestId, FTerrainTileGeneration Candidate,
			ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
		TERRAINBUILD_API auto GetCurrent() const
			-> std::shared_ptr<const FTerrainTileGeneration>;
		TERRAINBUILD_API auto Retire() -> void;

		FTerrainTileGenerationPublisher() = default;
		FTerrainTileGenerationPublisher(const FTerrainTileGenerationPublisher&) = delete;
		auto operator=(const FTerrainTileGenerationPublisher&)
			-> FTerrainTileGenerationPublisher& = delete;

	private:
		mutable std::mutex Mutex;
		uint64 LatestRequestId = 0;
		std::shared_ptr<const FTerrainTileGeneration> Current;
	};

	TERRAINBUILD_API auto TerrainFloorDiv(int64 Value, int64 Divisor, int64& OutResult) -> bool;
	TERRAINBUILD_API auto TerrainFloorMod(int64 Value, int64 Divisor, int64& OutResult) -> bool;
	TERRAINBUILD_API auto ValidateTerrainWorldDefinition(
		const FTerrainWorldDefinition& Definition, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
	TERRAINBUILD_API auto ResolveTerrainSampleAddress(
		const FTerrainWorldId& WorldId, const FTerrainSampleExtent& Extent,
		FTerrainGlobalSample Sample, FTerrainTileAddress& OutAddress,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto GetTerrainTileSampleExtent(
		const FTerrainTileKey& Tile, FTerrainSampleExtent& OutExtent,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto TerrainSampleToWorldPosition(
		const FTerrainWorldCoordinates& Coordinates, FTerrainGlobalSample Sample,
		int16 Height, std::array<double, 3>& OutPosition,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto NormalizeTerrainHeightQuantum(
		int32 Height, int16& OutHeight, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
	TERRAINBUILD_API auto ValidateTerrainNormalizedTileInput(
		const FTerrainNormalizedTileInput& Input, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
	TERRAINBUILD_API auto NormalizeTerrainTileInput(
		const FTerrainWorldDefinition& Definition, int64 TileX, int64 TileY,
		const FTerrainComposedTileValues& ComposedValues,
		FTerrainNormalizedTileInput& OutInput,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto ComposeTerrainTileInput(
		const FTerrainWorldDefinition& Definition, int64 TileX, int64 TileY,
		std::span<const FTerrainTileSourceContribution> Contributions,
		FTerrainNormalizedTileInput& OutInput,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError,
		std::function<bool()> ShouldCancel = {}) -> bool;
	TERRAINBUILD_API auto EstimateTerrainTileBuildBytes(
		const FTerrainNormalizedTileInput& Input) -> uint64;
	TERRAINBUILD_API auto BuildTerrainNeighborEvidence(
		const FTerrainNormalizedTileInput& Tile,
		const FTerrainNormalizedTileInput& Neighbor,
		FTerrainNeighborEvidence& OutEvidence,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto MakeTerrainTileBuildKey(
		const FTerrainNormalizedTileInput& Input, ETerrainTileProductClass ProductClass,
		std::string& OutError) -> std::string;
	TERRAINBUILD_API auto EncodeTerrainTileProduct(
		ETerrainTileProductClass ProductClass, const FTerrainTileKey& Tile,
		const FGuid& GenerationId, std::span<const FXxHash128> Dependencies,
		std::span<const std::byte> Body, std::vector<std::byte>& OutBytes,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto DecodeTerrainTileProduct(
		std::span<const std::byte> Bytes, ETerrainTileProductClass ExpectedClass,
		FTerrainTileProduct& OutProduct, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
	TERRAINBUILD_API auto BuildTerrainTileGeneration(
		const FTerrainNormalizedTileInput& Input, const FGuid& GenerationId,
		FTerrainTileGeneration& OutGeneration, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
}
