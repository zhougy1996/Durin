#pragma once

#include "Asset/SourcePath.h"
#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "EngineAPI.h"

#include "TerrainHeightmap.gen.h"

namespace Durin
{
	// Identifies the publication state of the immutable heightmap CPU payload.
	DENUM(DisplayName = "Terrain Heightmap Status")
	enum class ETerrainHeightmapStatus : uint8
	{
		Unavailable,
		Ready,
		SourceUnavailable,
		Failed
	};

	// Stores portable source provenance for heightmap rebuild and reimport.
	DSTRUCT()
	struct FTerrainHeightmapSourceImportData
	{
		GENERATED_BODY()

		DPROPERTY()
		FSourcePath SourcePath;

		DPROPERTY()
		uint64 SourceContentHashLow = 0;

		DPROPERTY()
		uint64 SourceContentHashHigh = 0;

		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;

		auto HasSource() const -> bool { return !SourcePath.IsEmpty(); }
		auto HasContentHash() const -> bool
		{
			return SourceContentHashLow != 0 || SourceContentHashHigh != 0;
		}
		auto operator==(const FTerrainHeightmapSourceImportData&) const -> bool = default;
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
	struct FTerrainHeightmapBuildOperations;

	struct FTerrainHeightmapImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DTerrainHeightmap* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};

	struct FTerrainHeightmapImportSettings
	{
		// Empty stores external source under TerrainHeightmaps using the asset name.
		std::string SourceDestination;
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
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

		auto GetPayload() const -> std::shared_ptr<const FTerrainHeightmapPayload> { return Payload; }
		auto GetWidth() const -> uint32 { return Width; }
		auto GetHeight() const -> uint32 { return Height; }
		auto GetMinimum() const -> uint16 { return Minimum; }
		auto GetMaximum() const -> uint16 { return Maximum; }
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetStatus() const -> ETerrainHeightmapStatus { return Status; }
		auto GetSourceImportData() const -> const FTerrainHeightmapSourceImportData& { return SourceImportData; }
		auto GetSourceFile() const -> const std::string& { return SourceImportData.SourcePath.Path; }
		auto GetDerivedDataKey() const -> const std::string& { return DerivedDataKey; }
		auto GetLastDiagnostic() const -> const std::string& { return LastDiagnostic; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		auto WasLoadedFromDerivedDataCache() const -> bool { return bLoadedFromDerivedDataCache; }
		ENGINE_API auto GetSample(uint32 X, uint32 Y, uint16& OutSample) const -> bool;
		ENGINE_API auto QueryMinMax(
			uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY,
			uint16& OutMinimum, uint16& OutMaximum) const -> bool;
		ENGINE_API auto PublishAuthoringCandidate(
			FTerrainHeightmapSourceImportData InSourceImportData,
			uint64 InSourceFileSize,
			int64 InSourceLastWriteTime,
			std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
			std::string InDerivedDataKey,
			std::string InDiagnostic) -> void;

		ENGINE_API auto InitializeFromSamples(
			uint32 InWidth,
			uint32 InHeight,
			std::span<const uint16> InSamples,
			std::string& OutError) -> bool;
		ENGINE_API auto BuildFromEncodedBytes(
			std::span<const uint8> EncodedBytes,
			const FSourcePath& SourcePath,
			std::string& OutError) -> bool;
		ENGINE_API auto ReimportSource(std::string_view FilePath, std::string& OutError) -> bool;
		ENGINE_API auto ChangeSourceReference(
			std::string_view SourceVirtualPath, std::string& OutError) -> bool;
		ENGINE_API auto IsSemanticImportNoOp(const DTerrainHeightmap& Candidate) const -> bool;
		ENGINE_API auto PrepareCandidateRevision(DTerrainHeightmap& Candidate) const -> void;
		ENGINE_API auto ExchangeImportedState(DTerrainHeightmap& Other) noexcept -> void;
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError,
			bool bRetainDiagnosticSourceMetadata = false) -> bool;

	private:
		friend class FTerrainHeightmapRenderStateRecreateContext;
		friend struct FTerrainHeightmapBuildOperations;
		auto LoadCookedPayload(std::string& OutError) -> bool;
		auto PublishPayload(
			std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
			bool bAdvanceRevision) -> void;

		DPROPERTY()
		FTerrainHeightmapSourceImportData SourceImportData;

		DPROPERTY()
		uint64 SourceFileSize = 0;

		DPROPERTY()
		int64 SourceLastWriteTime = 0;

		DPROPERTY()
		uint32 SourceBitDepth = 0;

		DPROPERTY()
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

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		DPROPERTY(Transient)
		ETerrainHeightmapStatus Status = ETerrainHeightmapStatus::Unavailable;

		DPROPERTY(Transient)
		std::string DerivedDataKey;

		DPROPERTY(Transient)
		std::string LastDiagnostic;

		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		bool bLoadedFromDerivedDataCache = false;
	};

	ENGINE_API auto BuildTerrainHeightmapPayload(
		uint32 Width,
		uint32 Height,
		std::span<const uint16> Samples,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool;
}
