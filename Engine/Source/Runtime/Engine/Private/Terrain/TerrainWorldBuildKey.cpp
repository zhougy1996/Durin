#include "Terrain/TerrainWorldBuildKey.h"

#if DURIN_WITH_EDITOR
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		auto WriteGuid(FBinaryWriter& Writer, const FGuid& Guid) -> void
		{
			for (uint32 Word : {Guid.A, Guid.B, Guid.C, Guid.D})
				for (int Shift : {24, 16, 8, 0})
					Writer.WriteU8(static_cast<uint8>(Word >> Shift));
		}

		auto WriteHash(FBinaryWriter& Writer, const FXxHash128& Hash) -> void
		{
			Writer.WriteU64(Hash.HashLow);
			Writer.WriteU64(Hash.HashHigh);
		}

		auto WriteTileKey(FBinaryWriter& Writer, const FTerrainTileKey& Tile) -> void
		{
			WriteGuid(Writer, Tile.WorldId.Value);
			Writer.WriteI64(Tile.TileX);
			Writer.WriteI64(Tile.TileY);
			Writer.WriteU16(Tile.SchemeVersion);
		}

		auto WriteDouble(FBinaryWriter& Writer, double Value) -> void
		{
			Writer.WriteU64(std::bit_cast<uint64>(Value));
		}

		auto IsValidProductClass(ETerrainTileProductClass ProductClass) -> bool
		{
			const uint8 Value = static_cast<uint8>(ProductClass);
			return Value >= static_cast<uint8>(ETerrainTileProductClass::Metadata)
				&& Value <= static_cast<uint8>(ETerrainTileProductClass::Query);
		}

		auto EncodeCommonKey(const FTerrainTileRecipeInput& Input,
			ETerrainTileProductClass ProductClass, FBinaryWriter& Writer) -> void
		{
			Writer.WriteString("TerrainWorldTileBuildKey");
			Writer.WriteU16(TerrainWorldSchemaVersion);
			Writer.WriteU8(static_cast<uint8>(ProductClass));
			WriteTileKey(Writer, Input.Tile);
			Writer.WriteI64(Input.WorldExtent.Min.X);
			Writer.WriteI64(Input.WorldExtent.Min.Y);
			Writer.WriteI64(Input.WorldExtent.Max.X);
			Writer.WriteI64(Input.WorldExtent.Max.Y);
			WriteGuid(Writer, Input.CompositionPolicyId);
			Writer.WriteU32(Input.CompositionPolicyVersion);
			Writer.WriteU32(Input.BuilderVersion);
			Writer.WriteU16(Input.ProductSchemaVersion);
			Writer.WriteU32(static_cast<uint32>(Input.TargetPlatform));
			Writer.WriteU32(static_cast<uint32>(Input.TargetProfile));
			Writer.WriteU32(Input.OutputPolicy);
		}

		auto SourceAffects(ETerrainTileProductClass ProductClass,
			const FTerrainCompositionSource& Source) -> bool
		{
			if (ProductClass == ETerrainTileProductClass::Coverage)
				return (Source.AffectedProductMask & TerrainSourceAffectsCoverage) != 0;
			if (ProductClass == ETerrainTileProductClass::Query)
				return Source.AffectedProductMask != 0;
			return (Source.AffectedProductMask & TerrainSourceAffectsHeight) != 0;
		}

		auto WriteSources(FBinaryWriter& Writer,
			std::span<const FTerrainCompositionSource> Sources,
			ETerrainTileProductClass ProductClass) -> void
		{
			const uint32 Count = static_cast<uint32>(std::ranges::count_if(Sources,
				[&](const auto& Source) { return SourceAffects(ProductClass, Source); }));
			Writer.WriteU32(Count);
			for (const FTerrainCompositionSource& Source : Sources)
			{
				if (!SourceAffects(ProductClass, Source)) continue;
				WriteGuid(Writer, Source.SourceId);
				WriteHash(Writer, Source.ContentHash);
				Writer.WriteI64(Source.AffectedSamples.Min.X);
				Writer.WriteI64(Source.AffectedSamples.Min.Y);
				Writer.WriteI64(Source.AffectedSamples.Max.X);
				Writer.WriteI64(Source.AffectedSamples.Max.Y);
				Writer.WriteU8(Source.BlendOperation);
				Writer.WriteI32(Source.Strength);
				Writer.WriteU8(Source.bEnabled ? 1 : 0);
				Writer.WriteU8(Source.AffectedProductMask);
			}
		}

		auto WriteHeights(FBinaryWriter& Writer, std::span<const int16> Heights) -> void
		{
			Writer.WriteU32(static_cast<uint32>(Heights.size()));
			for (int16 Height : Heights) Writer.WriteU16(static_cast<uint16>(Height));
		}

		auto WriteCoverage(FBinaryWriter& Writer, std::span<const FGuid> LayerIds,
			std::span<const FTerrainCoverageSample> Coverage) -> void
		{
			Writer.WriteU32(static_cast<uint32>(LayerIds.size()));
			for (const FGuid& Layer : LayerIds) WriteGuid(Writer, Layer);
			Writer.WriteU32(static_cast<uint32>(Coverage.size()));
			for (const FTerrainCoverageSample& Sample : Coverage)
			{
				Writer.WriteU8(Sample.LayerCount);
				for (uint8 Index = 0; Index < Sample.LayerCount; ++Index)
				{
					const auto It = std::ranges::find(LayerIds, Sample.Layers[Index].LayerId);
					Writer.WriteU8(static_cast<uint8>(std::distance(LayerIds.begin(), It)));
					Writer.WriteU8(Sample.Layers[Index].Weight);
				}
			}
		}

		auto WriteNeighbors(FBinaryWriter& Writer,
			const std::array<FTerrainNeighborEvidence, TerrainWorldMaximumNeighbors>& Neighbors,
			bool bCoverage) -> void
		{
			for (const FTerrainNeighborEvidence& Neighbor : Neighbors)
			{
				Writer.WriteU8(Neighbor.bPresent ? 1 : 0);
				if (!Neighbor.bPresent) continue;
				WriteTileKey(Writer, Neighbor.Tile);
				WriteHash(Writer, bCoverage ? Neighbor.CoverageEdgeHash : Neighbor.HeightEdgeHash);
			}
		}

	}

	static auto MakeTerrainTileBuildKeyUnchecked(const FTerrainTileRecipeInput& Input,
		ETerrainTileProductClass ProductClass, std::string& OutError) -> std::string
	{
		FBinaryWriter Writer;
		EncodeCommonKey(Input, ProductClass, Writer);
		WriteSources(Writer, Input.Sources, ProductClass);
		switch (ProductClass)
		{
		case ETerrainTileProductClass::Height:
			WriteHeights(Writer, Input.Heights);
			break;
		case ETerrainTileProductClass::Collision:
			WriteHeights(Writer, Input.Heights);
			break;
		case ETerrainTileProductClass::Metadata:
			WriteDouble(Writer, Input.Coordinates.OriginX);
			WriteDouble(Writer, Input.Coordinates.OriginY);
			WriteDouble(Writer, Input.Coordinates.OriginZ);
			WriteDouble(Writer, Input.Coordinates.SampleSpacingMeters);
			WriteDouble(Writer, Input.Coordinates.HeightDatumMeters);
			WriteHeights(Writer, Input.Heights);
			WriteHeights(Writer, Input.HeightHalo);
			WriteNeighbors(Writer, Input.Neighbors, false);
			break;
		case ETerrainTileProductClass::Coverage:
			WriteCoverage(Writer, Input.LayerIds, Input.Coverage);
			WriteCoverage(Writer, Input.LayerIds, Input.CoverageHalo);
			WriteNeighbors(Writer, Input.Neighbors, true);
			break;
		case ETerrainTileProductClass::Query:
			WriteHeights(Writer, Input.Heights);
			WriteHeights(Writer, Input.HeightHalo);
			WriteCoverage(Writer, Input.LayerIds, Input.Coverage);
			WriteCoverage(Writer, Input.LayerIds, Input.CoverageHalo);
			WriteNeighbors(Writer, Input.Neighbors, false);
			WriteNeighbors(Writer, Input.Neighbors, true);
			break;
		}
		OutError.clear();
		return FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
	}

	auto MakeTerrainTileBuildKey(const FTerrainTileRecipeInput& Input,
		ETerrainTileProductClass ProductClass, std::string& OutError) -> std::string
	{
		ETerrainWorldOutcome Outcome;
		if (!IsValidProductClass(ProductClass)
			|| !ValidateTerrainNormalizedTileInput(Input, Outcome, OutError)) return {};
		return MakeTerrainTileBuildKeyUnchecked(Input, ProductClass, OutError);
	}

}
#endif
