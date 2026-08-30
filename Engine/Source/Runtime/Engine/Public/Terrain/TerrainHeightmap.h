#pragma once

#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
#include "Asset/Cook.h"
#include "Asset/EditorBulkData.h"
#include "DObject/Object.h"
#include "EngineAPI.h"
#include "Hash/XxHash.h"

#include "TerrainHeightmap.gen.h"

namespace Durin
{
	inline constexpr FGuid TerrainHeightmapImportedSamplesPayloadId{
		0x5a7a7583, 0x8bb74d51, 0xa0df56f5, 0x2cd48376};
	inline constexpr uint32 TerrainHeightmapImportedDataSchemaVersion = 1;

	// Identifies the publication state of the immutable heightmap CPU payload.
	DENUM(DisplayName = "Terrain Heightmap Status")
	enum class ETerrainHeightmapStatus : uint8
	{
		Unavailable,
		Loading,
		Rebuilding,
		Ready,
		Failed
	};

	// Identifies the exact encoded source contract used to author a heightmap.
	DENUM(DisplayName = "Terrain Heightmap Source Format")
	enum class ETerrainHeightmapSourceFormat : uint8
	{
		Unknown,
		Png16,
		Raw16
	};

	// Carries the source contract transiently through heightmap build and recovery.
	struct FTerrainHeightmapSourceImportData
	{
		std::string SourceFilename;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;

		auto HasSource() const -> bool { return !SourceFilename.empty(); }
		auto HasContentHash() const -> bool
		{
			return SourceContentHashLow != 0 || SourceContentHashHigh != 0;
		}
		auto operator==(const FTerrainHeightmapSourceImportData&) const -> bool = default;
	};

	// Owns the decoder-free row-major uint16 samples used by every Terrain build.
	DSTRUCT()
	struct FTerrainHeightmapImportedData
	{
		GENERATED_BODY()

		DPROPERTY()
		Asset::FEditorBulkData Samples;

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint32 SchemaVersion = TerrainHeightmapImportedDataSchemaVersion;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto SetSamples(uint32 InWidth, uint32 InHeight,
			std::span<const uint16> InSamples) -> bool;
		ENGINE_API auto GetSamples() const -> std::vector<uint16>;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
	};

	// Stores one exact min/max node in level-major, row-major order.
	struct FTerrainHeightmapMinMaxNode
	{
		uint16 Minimum = 0;
		uint16 Maximum = 0;

		auto operator==(const FTerrainHeightmapMinMaxNode&) const -> bool = default;
	};

	// Describes one row-major hierarchy level in the shared node array.
	struct FTerrainHeightmapLevel
	{
		uint32 Width = 0;
		uint32 Height = 0;
		uint64 NodeOffset = 0;
		uint32 SampleRegionSize = 0;

		auto operator==(const FTerrainHeightmapLevel&) const -> bool = default;
	};

	// Owns exact samples and deterministic conservative regional extrema.
	struct FTerrainHeightmapPayload
	{
		std::vector<uint16> Samples;
		std::vector<FTerrainHeightmapMinMaxNode> Nodes;
		std::vector<FTerrainHeightmapLevel> Levels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint16 Minimum = 0;
		uint16 Maximum = 0;

		ENGINE_API auto Serialize(
			FArchive& Ar,
			Asset::ECookTargetPlatform TargetPlatform,
			Asset::ECookTargetProfile TargetProfile) -> void;

		// Checks bounded container and hierarchy layout without rebuilding canonical data.
		ENGINE_API auto HasValidLayout() const -> bool;
		// Rebuilds canonical data and verifies exact sample-derived extrema and hierarchy.
		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto GetSample(uint32 X, uint32 Y, uint16& OutSample) const -> bool;
		// Queries an exact half-open sample rectangle; empty/out-of-range rectangles fail.
		ENGINE_API auto QueryMinMax(
			uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY,
			uint16& OutMinimum, uint16& OutMaximum) const -> bool;
		ENGINE_API auto GetSampleBytes() const -> uint64;
		ENGINE_API auto GetHierarchyBytes() const -> uint64;
		ENGINE_API auto GetRetainedBytes() const -> uint64;
	};

	class DTerrainHeightmap;
	class FTerrainHeightmapRenderStateRecreateContext;

	struct FTerrainHeightmapImportSettings
	{
	};

	// Owns an exact revisioned uint16 height field without renderer or physics policy.
	DCLASS(DisplayName = "Terrain Heightmap")
	class DTerrainHeightmap : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTerrainHeightmap(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DTerrainHeightmap() override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

		ENGINE_API auto GetPayload() const -> std::shared_ptr<const FTerrainHeightmapPayload>;
		auto GetImportedData() const -> const FTerrainHeightmapImportedData& { return ImportedData; }
		auto GetImportedDataIdentity() const -> FXxHash128 { return ImportedData.GetIdentity(); }
		auto GetWidth() const -> uint32 { return Width; }
		auto GetHeight() const -> uint32 { return Height; }
		auto GetMinimum() const -> uint16 { return Minimum; }
		auto GetMaximum() const -> uint16 { return Maximum; }
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetStatus() const -> ETerrainHeightmapStatus { return Status; }
		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		ENGINE_API auto PublishAssetImportData(
			DAssetImportData& Value, std::string& OutError) -> bool;
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetLastDiagnostic() const -> const std::string& { return LastDiagnostic; }
		auto GetCookedPlatformData() const -> const Asset::FBulkData& { return CookedPlatformData; }
		auto WasLoadedFromDerivedDataCache() const -> bool { return bLoadedFromDerivedDataCache; }
		auto GetDerivedDataLoadGeneration() const -> uint64 { return DerivedDataLoadGeneration; }
		ENGINE_API auto BeginDerivedDataLoad(bool bRebuilding, std::string Diagnostic) -> uint64;
		ENGINE_API auto IsDerivedDataLoadCurrent(uint64 Generation) const -> bool;
		ENGINE_API auto FailDerivedDataLoad(
			uint64 Generation, ETerrainHeightmapStatus FailureStatus, std::string Diagnostic) -> bool;
		ENGINE_API auto GetSample(uint32 X, uint32 Y, uint16& OutSample) const -> bool;
		ENGINE_API auto QueryMinMax(
			uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY,
			uint16& OutMinimum, uint16& OutMaximum) const -> bool;
		ENGINE_API auto PublishDerivedDataLoadResult(
			std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
			std::string InDerivedDataKey,
			std::string InDiagnostic,
			bool bAdvanceRevision = true,
			bool bMarkPackageDirty = true,
			bool bInLoadedFromDerivedDataCache = false) -> void;

		ENGINE_API auto InitializeFromSamples(
			uint32 InWidth,
			uint32 InHeight,
			std::span<const uint16> InSamples,
			std::string& OutError) -> bool;
		ENGINE_API auto IsSemanticImportNoOp(const DTerrainHeightmap& Candidate) const -> bool;
		ENGINE_API auto PrepareCandidateRevision(DTerrainHeightmap& Candidate) const -> void;
		ENGINE_API auto ExchangeImportedState(DTerrainHeightmap& Other) noexcept -> void;
	private:
		friend auto Asset::ContributeEngineCookAsset(
			DObject&, std::string_view, Asset::FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	public:

	private:
		friend class FTerrainHeightmapRenderStateRecreateContext;
		auto LoadCookedPayload(std::string& OutError) -> bool;
		auto PublishPayload(
			std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
			bool bAdvanceRevision) -> void;

		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FTerrainHeightmapImportedData ImportedData;

		DPROPERTY(EditorOnly)
		uint32 SourceBitDepth = 0;

		DPROPERTY(EditorOnly)
		uint32 SourceChannelCount = 0;

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint16 Minimum = 0;

		DPROPERTY()
		uint16 Maximum = 0;

		DPROPERTY()
		uint64 Revision = 0;

		DPROPERTY()
		uint64 SampleBytes = 0;

		DPROPERTY()
		uint64 HierarchyBytes = 0;

		DPROPERTY()
		uint64 RetainedBytes = 0;

		Asset::FBulkData CookedPlatformData;

		DPROPERTY(Transient)
		ETerrainHeightmapStatus Status = ETerrainHeightmapStatus::Unavailable;

		DPROPERTY(Transient)
		std::string DerivedDataKey;

		DPROPERTY(Transient)
		std::string LastDiagnostic;

		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		bool bLoadedFromDerivedDataCache = false;
		uint64 DerivedDataLoadGeneration = 0;
	};

	ENGINE_API auto BuildTerrainHeightmapPayload(
		uint32 Width,
		uint32 Height,
		std::span<const uint16> Samples,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool;
}
