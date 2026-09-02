#include "Terrain/TerrainWorld.h"

#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 ProductEnvelopeMagic = 0x44505754; // TWPD
		constexpr uint32 RequiredFlags = 1;
		constexpr uint64 MaximumLatticeContributionMeters = 1ull << 40;
		constexpr uint64 MaximumCellsPerAxis = 1ull << 31;
		constexpr uint64 HeaderBytes = 108;

		auto TerrainWorldFail(ETerrainWorldOutcome Outcome, std::string Message,
			ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
		{
			OutOutcome = Outcome;
			OutError = std::move(Message);
			return false;
		}

		auto ProductIndex(ETerrainTileProductClass ProductClass) -> size_t
		{
			return static_cast<size_t>(ProductClass) - 1;
		}

		auto ProductCeiling(ETerrainTileProductClass ProductClass) -> uint64
		{
			switch (ProductClass)
			{
			case ETerrainTileProductClass::Metadata: return 16ull * 1024ull;
			case ETerrainTileProductClass::Height: return 160ull * 1024ull;
			case ETerrainTileProductClass::Coverage: return 320ull * 1024ull;
			case ETerrainTileProductClass::Collision: return 96ull * 1024ull;
			case ETerrainTileProductClass::Query: return 160ull * 1024ull;
			}
			return 0;
		}

		auto WriteGuid(FBinaryWriter& Writer, const FGuid& Guid) -> void
		{
			for (uint32 Word : {Guid.A, Guid.B, Guid.C, Guid.D})
				for (int Shift : {24, 16, 8, 0})
					Writer.WriteU8(static_cast<uint8>(Word >> Shift));
		}

		auto ReadGuid(FBinaryReader& Reader, FGuid& OutGuid) -> bool
		{
			std::array<uint32, 4> Words{};
			for (uint32& Word : Words)
				for (int Shift : {24, 16, 8, 0})
				{
					uint8 Byte = 0;
					if (!Reader.ReadU8(Byte)) return false;
					Word |= static_cast<uint32>(Byte) << Shift;
				}
			OutGuid = {Words[0], Words[1], Words[2], Words[3]};
			return true;
		}

		auto WriteHash(FBinaryWriter& Writer, const FXxHash128& Hash) -> void
		{
			Writer.WriteU64(Hash.HashLow);
			Writer.WriteU64(Hash.HashHigh);
		}

		auto ReadHash(FBinaryReader& Reader, FXxHash128& OutHash) -> bool
		{
			return Reader.ReadU64(OutHash.HashLow) && Reader.ReadU64(OutHash.HashHigh);
		}

		auto WriteTileKey(FBinaryWriter& Writer, const FTerrainTileKey& Tile) -> void
		{
			WriteGuid(Writer, Tile.WorldId.Value);
			Writer.WriteI64(Tile.TileX);
			Writer.WriteI64(Tile.TileY);
			Writer.WriteU16(Tile.SchemeVersion);
		}

		auto ReadTileKey(FBinaryReader& Reader, FTerrainTileKey& OutTile) -> bool
		{
			return ReadGuid(Reader, OutTile.WorldId.Value)
				&& Reader.ReadI64(OutTile.TileX) && Reader.ReadI64(OutTile.TileY)
				&& Reader.ReadU16(OutTile.SchemeVersion);
		}

		auto WriteDouble(FBinaryWriter& Writer, double Value) -> void
		{
			Writer.WriteU64(std::bit_cast<uint64>(Value));
		}

		auto IsFinite(double Value) -> bool
		{
			return std::isfinite(Value);
		}

		auto IsCanonicalPositive(double Value) -> bool
		{
			return IsFinite(Value) && Value >= 0.01 && Value <= 4096.0
				&& !(Value == 0.0 && std::signbit(Value));
		}

		auto CheckedAxisCells(int64 Min, int64 Max, uint64& OutCells) -> bool
		{
			if (Max <= Min) return false;
			const uint64 Difference = static_cast<uint64>(Max) - static_cast<uint64>(Min);
			if (Difference == 0 || Difference > MaximumCellsPerAxis) return false;
			OutCells = Difference;
			return true;
		}

		auto CheckedAdd(int64 Value, int64 Delta, int64& OutValue) -> bool
		{
			if ((Delta > 0 && Value > std::numeric_limits<int64>::max() - Delta)
				|| (Delta < 0 && Value < std::numeric_limits<int64>::min() - Delta)) return false;
			OutValue = Value + Delta;
			return true;
		}

		auto GuidLess(const FGuid& A, const FGuid& B) -> bool
		{
			// RFC 4122 order compares the four big-endian words lexicographically.
			return std::tuple(A.A, A.B, A.C, A.D) < std::tuple(B.A, B.B, B.C, B.D);
		}

		auto IsValidProductClass(ETerrainTileProductClass ProductClass) -> bool
		{
			const uint8 Value = static_cast<uint8>(ProductClass);
			return Value >= static_cast<uint8>(ETerrainTileProductClass::Metadata)
				&& Value <= static_cast<uint8>(ETerrainTileProductClass::Query);
		}

		auto AppendBorderSample(FBinaryWriter& HeightWriter,
			FBinaryWriter& CoverageWriter, const FTerrainTileRecipeInput& Input,
			uint32 X, uint32 Y) -> void
		{
			const uint32 Offset = Y * 257 + X;
			HeightWriter.WriteU16(static_cast<uint16>(Input.Heights[Offset]));
			const FTerrainCoverageSample& Coverage = Input.Coverage[Offset];
			CoverageWriter.WriteU8(Coverage.LayerCount);
			for (uint8 Index = 0; Index < Coverage.LayerCount; ++Index)
			{
				WriteGuid(CoverageWriter, Coverage.Layers[Index].LayerId);
				CoverageWriter.WriteU8(Coverage.Layers[Index].Weight);
			}
		}

		auto ValidateGeneration(const FTerrainTileGeneration& Candidate,
			ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
		{
			if (!Candidate.Tile.WorldId.IsValid() || !Candidate.GenerationId.IsValid())
				return TerrainWorldFail(ETerrainWorldOutcome::PublicationFailed,
					"Terrain tile generation identity is incomplete.", OutOutcome, OutError);
			std::array<FXxHash128, 5> Hashes{};
			for (uint8 Value = 1; Value <= 5; ++Value)
			{
				const size_t Index = Value - 1;
				const auto Class = static_cast<ETerrainTileProductClass>(Value);
				const FTerrainTileProduct& Product = Candidate.Products[Index];
				FTerrainTileProduct Decoded;
				if (Product.ProductClass != Class || Product.Tile != Candidate.Tile
					|| Product.GenerationId != Candidate.GenerationId
					|| !DecodeTerrainTileProduct(Product.Bytes, Class, Decoded, OutOutcome, OutError))
					return TerrainWorldFail(ETerrainWorldOutcome::PublicationFailed,
						"Terrain tile generation contains an invalid or mismatched product.", OutOutcome, OutError);
				Hashes[Index] = Decoded.BodyHash;
			}
			const auto& Metadata = Candidate.Products[ProductIndex(ETerrainTileProductClass::Metadata)];
			const auto& Collision = Candidate.Products[ProductIndex(ETerrainTileProductClass::Collision)];
			const auto& Query = Candidate.Products[ProductIndex(ETerrainTileProductClass::Query)];
			if (Metadata.Dependencies != std::vector{Hashes[ProductIndex(ETerrainTileProductClass::Height)]}
				|| Collision.Dependencies != std::vector{Hashes[ProductIndex(ETerrainTileProductClass::Height)]}
				|| Query.Dependencies != std::vector{Hashes[ProductIndex(ETerrainTileProductClass::Height)],
					Hashes[ProductIndex(ETerrainTileProductClass::Coverage)]})
				return TerrainWorldFail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile generation product dependencies are incomplete.", OutOutcome, OutError);
			return true;
		}

		auto MaximumBodyBytes(ETerrainTileProductClass ProductClass) -> uint64
		{
			switch (ProductClass)
			{
			case ETerrainTileProductClass::Metadata: return 16ull * 1024ull - 108ull;
			case ETerrainTileProductClass::Height: return 160ull * 1024ull - 108ull;
			case ETerrainTileProductClass::Coverage: return 320ull * 1024ull - 108ull;
			case ETerrainTileProductClass::Collision: return 96ull * 1024ull - 108ull;
			case ETerrainTileProductClass::Query: return 160ull * 1024ull - 108ull;
			}
			return 0;
		}

		auto ValidateFixedGrid(std::span<const std::byte> Bytes, uint16 Dimension,
			uint32 BytesPerSample, std::string& OutError) -> bool
		{
			const uint64 Expected = 4ull + static_cast<uint64>(Dimension) * Dimension * BytesPerSample;
			uint16 Width = 0, Height = 0;
			if (Bytes.size() != Expected || !ReadLittleEndianAt(Bytes, 0, Width)
				|| !ReadLittleEndianAt(Bytes, 2, Height)
				|| Width != Dimension || Height != Dimension)
			{
				OutError = "Terrain World fixed-grid product body is malformed.";
				return false;
			}
			return true;
		}

		auto ValidateCoverage(std::span<const std::byte> Bytes, std::string& OutError) -> bool
		{
			uint16 Width = 0, Height = 0;
			if (Bytes.size() < 5 || !ReadLittleEndianAt(Bytes, 0, Width)
				|| !ReadLittleEndianAt(Bytes, 2, Height))
			{
				OutError = "Terrain World coverage header is malformed.";
				return false;
			}
			const uint8 PaletteCount = std::to_integer<uint8>(Bytes[4]);
			if (Width != 257 || Height != 257
				|| PaletteCount == 0 || PaletteCount > TerrainWorldMaximumTileLayers)
			{
				OutError = "Terrain World coverage header is malformed.";
				return false;
			}
			size_t Offset = 5 + static_cast<size_t>(PaletteCount) * 16;
			if (Offset > Bytes.size())
			{
				OutError = "Terrain World coverage palette is truncated.";
				return false;
			}
			for (uint32 Sample = 0; Sample < TerrainWorldSampleCount; ++Sample)
			{
				if (Offset >= Bytes.size())
				{
					OutError = "Terrain World coverage sample count is invalid.";
					return false;
				}
				const uint8 Count = std::to_integer<uint8>(Bytes[Offset++]);
				uint8 Previous = 0;
				if (Count == 0 || Count > TerrainWorldMaximumActiveLayers)
				{
					OutError = "Terrain World coverage sample count is invalid.";
					return false;
				}
				uint32 WeightSum = 0;
				for (uint8 Index = 0; Index < Count; ++Index)
				{
					if (Bytes.size() - Offset < 2)
					{
						OutError = "Terrain World coverage sample is truncated.";
						return false;
					}
					const uint8 PaletteIndex = std::to_integer<uint8>(Bytes[Offset++]);
					const uint8 Weight = std::to_integer<uint8>(Bytes[Offset++]);
					if (PaletteIndex >= PaletteCount || Weight == 0
						|| (Index && PaletteIndex <= Previous))
					{
						OutError = "Terrain World coverage sample is invalid.";
						return false;
					}
					Previous = PaletteIndex;
					WeightSum += Weight;
				}
				if (WeightSum != 255)
				{
					OutError = "Terrain World coverage sample weights do not sum to 255.";
					return false;
				}
			}
			if (Offset != Bytes.size())
			{
				OutError = "Terrain World coverage body has trailing bytes.";
				return false;
			}
			return true;
		}

		auto ValidateMetadata(std::span<const std::byte> Bytes, std::string& OutError) -> bool
		{
			FBinaryReader Reader(Bytes);
			int32 Minimum = 0, Maximum = 0;
			uint64 ErrorBits = 0;
			std::array<uint64, 6> BoundsBits{};
			uint8 Count = 0;
			if (!Reader.ReadI32(Minimum) || !Reader.ReadI32(Maximum)
				|| !Reader.ReadU64(ErrorBits)
				|| !std::ranges::all_of(BoundsBits,
					[&](uint64& Bits) { return Reader.ReadU64(Bits); })
				|| !Reader.ReadU8(Count)
				|| Minimum < -32768 || Maximum > 32767 || Minimum > Maximum || Count != 5)
			{
				OutError = "Terrain World metadata body is malformed.";
				return false;
			}
			static constexpr std::array<uint32, 5> Ceilings{
				16u * 1024u, 160u * 1024u, 320u * 1024u, 96u * 1024u, 160u * 1024u};
			for (uint8 Expected = 1; Expected <= 5; ++Expected)
			{
				uint8 Product = 0;
				uint32 Ceiling = 0;
				if (!Reader.ReadU8(Product) || !Reader.ReadU32(Ceiling)
					|| Product != Expected || Ceiling != Ceilings[Expected - 1])
				{
					OutError = "Terrain World metadata directory is invalid.";
					return false;
				}
			}
			const std::array<double, 6> Bounds{
				std::bit_cast<double>(BoundsBits[0]), std::bit_cast<double>(BoundsBits[1]),
				std::bit_cast<double>(BoundsBits[2]), std::bit_cast<double>(BoundsBits[3]),
				std::bit_cast<double>(BoundsBits[4]), std::bit_cast<double>(BoundsBits[5])};
			if (!Reader.IsAtEnd() || !std::isfinite(std::bit_cast<double>(ErrorBits))
				|| !std::ranges::all_of(Bounds, [](double Value) { return std::isfinite(Value); })
				|| Bounds[0] > Bounds[3] || Bounds[1] > Bounds[4] || Bounds[2] > Bounds[5])
			{
				OutError = "Terrain World metadata body has invalid error or trailing bytes.";
				return false;
			}
			return true;
		}

		auto ValidateBody(ETerrainTileProductClass ProductClass,
			std::span<const std::byte> Bytes, std::string& OutError) -> bool
		{
			if (Bytes.empty() || Bytes.size() > MaximumBodyBytes(ProductClass))
			{
				OutError = "Terrain World product body exceeds its byte bound.";
				return false;
			}
			switch (ProductClass)
			{
			case ETerrainTileProductClass::Metadata: return ValidateMetadata(Bytes, OutError);
			case ETerrainTileProductClass::Height: return ValidateFixedGrid(Bytes, 257, 2, OutError);
			case ETerrainTileProductClass::Coverage: return ValidateCoverage(Bytes, OutError);
			case ETerrainTileProductClass::Collision: return ValidateFixedGrid(Bytes, 129, 2, OutError);
			case ETerrainTileProductClass::Query: return ValidateFixedGrid(Bytes, 129, 7, OutError);
			}
			OutError = "Terrain World product class is invalid.";
			return false;
		}
	}

	auto TerrainFloorDiv(int64 Value, int64 Divisor, int64& OutResult) -> bool
	{
		if (Divisor == 0 || (Value == std::numeric_limits<int64>::min() && Divisor == -1)) return false;
		const int64 Quotient = Value / Divisor;
		const int64 Remainder = Value % Divisor;
		OutResult = Quotient - ((Remainder != 0 && ((Remainder < 0) != (Divisor < 0))) ? 1 : 0);
		return true;
	}

	auto TerrainFloorMod(int64 Value, int64 Divisor, int64& OutResult) -> bool
	{
		int64 Quotient = 0;
		if (!TerrainFloorDiv(Value, Divisor, Quotient)) return false;
		if (Divisor == -1) { OutResult = 0; return true; }
		OutResult = Value % Divisor;
		if (OutResult < 0) OutResult += std::abs(Divisor);
		return true;
	}

	auto ValidateTerrainWorldDefinition(const FTerrainWorldDefinition& Definition,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		uint64 CellsX = 0, CellsY = 0;
		if (!Definition.WorldId.IsValid() || !CheckedAxisCells(
			Definition.SampleExtent.Min.X, Definition.SampleExtent.Max.X, CellsX)
			|| !CheckedAxisCells(Definition.SampleExtent.Min.Y, Definition.SampleExtent.Max.Y, CellsY)
			|| Definition.TileSchemeVersion != TerrainWorldTileSchemeVersion
			|| !IsFinite(Definition.Coordinates.OriginX)
			|| !IsFinite(Definition.Coordinates.OriginY)
			|| !IsFinite(Definition.Coordinates.OriginZ)
			|| !IsFinite(Definition.Coordinates.HeightDatumMeters)
			|| !IsCanonicalPositive(Definition.Coordinates.SampleSpacingMeters)
			|| !Definition.BuildPolicyId.IsValid() || Definition.BuildPolicyVersion == 0
			|| Definition.ProductProfile < 1 || Definition.ProductProfile > 4
			|| Definition.PeakBuildBudgetBytes == 0
			|| Definition.PeakBuildBudgetBytes > (Definition.ProductProfile == 1
				? 2ull * 1024ull * 1024ull * 1024ull : 4ull * 1024ull * 1024ull * 1024ull)
			|| Definition.TargetPlatform == ECookTargetPlatform::Invalid
			|| Definition.TargetProfile == ECookTargetProfile::Invalid
			|| Definition.RegionTileDimension != 8)
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain World definition has an invalid identity, extent, coordinate, policy, target, or region value.",
				OutOutcome, OutError);
		if (Definition.Layers.size() > TerrainWorldMaximumLayers
			|| Definition.Sources.size() > TerrainWorldMaximumSources)
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain World definition exceeds a layer or source count ceiling.", OutOutcome, OutError);
		for (int64 Coordinate : {Definition.SampleExtent.Min.X, Definition.SampleExtent.Min.Y,
			Definition.SampleExtent.Max.X, Definition.SampleExtent.Max.Y})
			if (std::abs(static_cast<long double>(Coordinate)
				* Definition.Coordinates.SampleSpacingMeters) > MaximumLatticeContributionMeters)
				return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
					"Terrain World definition lattice contribution exceeds 2^40 meters.", OutOutcome, OutError);
		std::unordered_set<FGuid> LayerIds;
		std::unordered_set<std::string> LayerNames;
		for (const FTerrainLayerDefinition& Layer : Definition.Layers)
			if (!Layer.LayerId.IsValid() || Layer.DisplayName.empty()
				|| !LayerIds.insert(Layer.LayerId).second || !LayerNames.insert(Layer.DisplayName).second)
				return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
					"Terrain World layers require unique nonzero identities and display names.", OutOutcome, OutError);
		FGuid PreviousSource;
		for (const FTerrainCompositionSource& Source : Definition.Sources)
		{
			uint64 SourceCellsX = 0, SourceCellsY = 0;
			if (!Source.SourceId.IsValid() || Source.ContentHash.IsZero()
				|| !CheckedAxisCells(Source.AffectedSamples.Min.X, Source.AffectedSamples.Max.X, SourceCellsX)
				|| !CheckedAxisCells(Source.AffectedSamples.Min.Y, Source.AffectedSamples.Max.Y, SourceCellsY)
				|| Source.BlendOperation < static_cast<uint8>(ETerrainCompositionBlendOperation::Replace)
				|| Source.BlendOperation > static_cast<uint8>(ETerrainCompositionBlendOperation::Maximum)
				|| Source.Strength < 0 || Source.Strength > 255
				|| Source.AffectedProductMask == 0
				|| (Source.AffectedProductMask & ~(TerrainSourceAffectsHeight | TerrainSourceAffectsCoverage)) != 0
				|| (PreviousSource.IsValid() && !GuidLess(PreviousSource, Source.SourceId)))
				return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
					"Terrain World sources are invalid, duplicated, or not canonically ordered.", OutOutcome, OutError);
			PreviousSource = Source.SourceId;
		}
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto GetTerrainTileSampleExtent(const FTerrainTileKey& Tile,
		FTerrainSampleExtent& OutExtent, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		if (!Tile.WorldId.IsValid() || Tile.SchemeVersion != TerrainWorldTileSchemeVersion
			|| Tile.TileX > std::numeric_limits<int64>::max() / TerrainWorldTileCells
			|| Tile.TileX < std::numeric_limits<int64>::min() / TerrainWorldTileCells
			|| Tile.TileY > std::numeric_limits<int64>::max() / TerrainWorldTileCells
			|| Tile.TileY < std::numeric_limits<int64>::min() / TerrainWorldTileCells)
			return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
				"Terrain tile rectangle cannot be represented.", OutOutcome, OutError);
		const int64 MinX = Tile.TileX * TerrainWorldTileCells;
		const int64 MinY = Tile.TileY * TerrainWorldTileCells;
		if (MinX > std::numeric_limits<int64>::max() - TerrainWorldTileCells
			|| MinY > std::numeric_limits<int64>::max() - TerrainWorldTileCells)
			return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
				"Terrain tile rectangle cannot be represented.", OutOutcome, OutError);
		OutExtent = {{MinX, MinY}, {MinX + TerrainWorldTileCells, MinY + TerrainWorldTileCells}};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto ResolveTerrainSampleAddress(const FTerrainWorldId& WorldId,
		const FTerrainSampleExtent& Extent, FTerrainGlobalSample Sample,
		FTerrainTileAddress& OutAddress, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		uint64 CellsX = 0, CellsY = 0;
		if (!WorldId.IsValid() || !CheckedAxisCells(Extent.Min.X, Extent.Max.X, CellsX)
			|| !CheckedAxisCells(Extent.Min.Y, Extent.Max.Y, CellsY))
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain sample lookup extent is invalid.", OutOutcome, OutError);
		if (Sample.X < Extent.Min.X || Sample.X > Extent.Max.X
			|| Sample.Y < Extent.Min.Y || Sample.Y > Extent.Max.Y)
			return TerrainWorldFail(ETerrainWorldOutcome::Unavailable,
				"Terrain sample is outside the inclusive world extent.", OutOutcome, OutError);
		FTerrainGlobalSample Lookup = Sample;
		if (Lookup.X == Extent.Max.X) --Lookup.X;
		if (Lookup.Y == Extent.Max.Y) --Lookup.Y;
		int64 TileX = 0, TileY = 0, LocalX = 0, LocalY = 0;
		if (!TerrainFloorDiv(Lookup.X, 256, TileX) || !TerrainFloorDiv(Lookup.Y, 256, TileY)
			|| !TerrainFloorMod(Lookup.X, 256, LocalX) || !TerrainFloorMod(Lookup.Y, 256, LocalY))
			return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
				"Terrain sample address arithmetic overflowed.", OutOutcome, OutError);
		if (Sample.X == Extent.Max.X) ++LocalX;
		if (Sample.Y == Extent.Max.Y) ++LocalY;
		OutAddress = {{WorldId, TileX, TileY, TerrainWorldTileSchemeVersion},
			{static_cast<uint16>(LocalX), static_cast<uint16>(LocalY)}};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto TerrainSampleToWorldPosition(const FTerrainWorldCoordinates& Coordinates,
		FTerrainGlobalSample Sample, int16 Height, std::array<double, 3>& OutPosition,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		if (!IsFinite(Coordinates.OriginX) || !IsFinite(Coordinates.OriginY)
			|| !IsFinite(Coordinates.OriginZ) || !IsFinite(Coordinates.HeightDatumMeters)
			|| !IsCanonicalPositive(Coordinates.SampleSpacingMeters))
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain coordinate values are invalid.", OutOutcome, OutError);
		const long double ContributionX = static_cast<long double>(Sample.X) * Coordinates.SampleSpacingMeters;
		const long double ContributionY = static_cast<long double>(Sample.Y) * Coordinates.SampleSpacingMeters;
		if (std::abs(ContributionX) > MaximumLatticeContributionMeters
			|| std::abs(ContributionY) > MaximumLatticeContributionMeters)
			return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
				"Terrain lattice contribution exceeds 2^40 meters.", OutOutcome, OutError);
		OutPosition = {Coordinates.OriginX + static_cast<double>(ContributionX),
			Coordinates.OriginY + static_cast<double>(ContributionY),
			Coordinates.OriginZ + Coordinates.HeightDatumMeters + static_cast<double>(Height) * 0.25};
		if (!std::ranges::all_of(OutPosition, IsFinite))
			return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
				"Terrain world position is not finite.", OutOutcome, OutError);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto NormalizeTerrainHeightQuantum(int32 Height, int16& OutHeight,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		if (Height < -32768 || Height > 32767)
			return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
				"Terrain height is outside the schema-1 signed quantum envelope.",
				OutOutcome, OutError);
		OutHeight = static_cast<int16>(Height);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto ValidateTerrainNormalizedTileInput(const FTerrainTileRecipeInput& Input,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		uint64 CellsX = 0, CellsY = 0;
		FTerrainSampleExtent TileExtent;
		if (!Input.Tile.WorldId.IsValid() || Input.Tile.SchemeVersion != TerrainWorldTileSchemeVersion
			|| Input.ProductSchemaVersion != TerrainWorldSchemaVersion
			|| !Input.CompositionPolicyId.IsValid() || Input.CompositionPolicyVersion == 0
			|| Input.BuilderVersion == 0 || Input.TargetPlatform == ECookTargetPlatform::Invalid
			|| Input.TargetProfile == ECookTargetProfile::Invalid
			|| !IsFinite(Input.Coordinates.OriginX) || !IsFinite(Input.Coordinates.OriginY)
			|| !IsFinite(Input.Coordinates.OriginZ)
			|| !IsFinite(Input.Coordinates.HeightDatumMeters)
			|| !IsCanonicalPositive(Input.Coordinates.SampleSpacingMeters)
			|| !CheckedAxisCells(Input.WorldExtent.Min.X, Input.WorldExtent.Max.X, CellsX)
			|| !CheckedAxisCells(Input.WorldExtent.Min.Y, Input.WorldExtent.Max.Y, CellsY)
			|| !GetTerrainTileSampleExtent(Input.Tile, TileExtent, OutOutcome, OutError))
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Normalized Terrain tile identity, schema, policy, target, or extent is invalid.", OutOutcome, OutError);
		for (int64 Coordinate : {Input.WorldExtent.Min.X, Input.WorldExtent.Min.Y,
			Input.WorldExtent.Max.X, Input.WorldExtent.Max.Y})
			if (std::abs(static_cast<long double>(Coordinate)
				* Input.Coordinates.SampleSpacingMeters) > MaximumLatticeContributionMeters)
				return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
					"Normalized Terrain tile lattice contribution exceeds 2^40 meters.", OutOutcome, OutError);
		if (TileExtent.Max.X < Input.WorldExtent.Min.X || TileExtent.Min.X > Input.WorldExtent.Max.X
			|| TileExtent.Max.Y < Input.WorldExtent.Min.Y || TileExtent.Min.Y > Input.WorldExtent.Max.Y)
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Normalized Terrain tile does not intersect its world.", OutOutcome, OutError);
		if ((Input.HeightHalo.empty() || Input.CoverageHalo.empty())
			&& (TileExtent.Min.X > Input.WorldExtent.Min.X || TileExtent.Max.X < Input.WorldExtent.Max.X
				|| TileExtent.Min.Y > Input.WorldExtent.Min.Y || TileExtent.Max.Y < Input.WorldExtent.Max.Y))
			return TerrainWorldFail(ETerrainWorldOutcome::MissingDependency,
				"Normalized Terrain tile requires a one-sample halo away from every world edge.",
				OutOutcome, OutError);
		if (Input.Sources.size() > TerrainWorldMaximumTileSources
			|| Input.LayerIds.size() > TerrainWorldMaximumTileLayers
			|| Input.Heights.size() != TerrainWorldSampleCount
			|| Input.Coverage.size() != TerrainWorldSampleCount
			|| (!Input.HeightHalo.empty() && Input.HeightHalo.size() != 259u * 259u)
			|| (!Input.CoverageHalo.empty() && Input.CoverageHalo.size() != 259u * 259u))
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"Normalized Terrain tile exceeds a count bound or has an invalid sample array.", OutOutcome, OutError);
		FGuid PreviousSource;
		for (const FTerrainCompositionSource& Source : Input.Sources)
		{
			if (!Source.SourceId.IsValid() || Source.ContentHash.IsZero()
				|| (PreviousSource.IsValid() && !GuidLess(PreviousSource, Source.SourceId))
				|| Source.AffectedProductMask == 0
				|| (Source.AffectedProductMask & ~(TerrainSourceAffectsHeight | TerrainSourceAffectsCoverage)) != 0)
				return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
					"Normalized Terrain sources are invalid or not canonically ordered.", OutOutcome, OutError);
			PreviousSource = Source.SourceId;
		}
		if (!std::ranges::is_sorted(Input.LayerIds, GuidLess)
			|| std::adjacent_find(Input.LayerIds.begin(), Input.LayerIds.end()) != Input.LayerIds.end()
			|| std::ranges::any_of(Input.LayerIds, [](const FGuid& Id) { return !Id.IsValid(); }))
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Normalized Terrain layers are invalid, duplicated, or not canonically ordered.", OutOutcome, OutError);
		auto ValidateCoverageSamples = [&](std::span<const FTerrainCoverageSample> Samples) -> bool
		{
			for (const FTerrainCoverageSample& Sample : Samples)
			{
				uint32 Sum = 0;
				if (Sample.LayerCount == 0 || Sample.LayerCount > TerrainWorldMaximumActiveLayers)
					return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
						"Terrain coverage active layer count is invalid.", OutOutcome, OutError);
				FGuid Previous;
				for (uint8 Index = 0; Index < Sample.LayerCount; ++Index)
				{
					const auto& Weight = Sample.Layers[Index];
					if (Weight.Weight == 0
						|| std::ranges::find(Input.LayerIds, Weight.LayerId) == Input.LayerIds.end()
						|| (Previous.IsValid() && !GuidLess(Previous, Weight.LayerId)))
						return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
							"Terrain coverage weights are invalid or not canonically ordered.", OutOutcome, OutError);
					Previous = Weight.LayerId;
					Sum += Weight.Weight;
				}
				if (Sum != 255)
					return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
						"Terrain coverage weights must sum exactly to 255.", OutOutcome, OutError);
			}
			return true;
		};
		if (!ValidateCoverageSamples(Input.Coverage)
			|| (!Input.CoverageHalo.empty() && !ValidateCoverageSamples(Input.CoverageHalo))) return false;
		for (size_t Index = 0; Index < Input.Neighbors.size(); ++Index)
		{
			const FTerrainNeighborEvidence& Neighbor = Input.Neighbors[Index];
			if (!Neighbor.bPresent) continue;
			static constexpr std::array<std::pair<int64, int64>, 8> Offsets{{
				{-1,-1}, {0,-1}, {1,-1}, {-1,0}, {1,0}, {-1,1}, {0,1}, {1,1}}};
			int64 ExpectedX = 0, ExpectedY = 0;
			if (!CheckedAdd(Input.Tile.TileX, Offsets[Index].first, ExpectedX)
				|| !CheckedAdd(Input.Tile.TileY, Offsets[Index].second, ExpectedY))
				return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
					"Terrain neighbor coordinate overflowed.", OutOutcome, OutError);
			if (Neighbor.Tile.WorldId != Input.Tile.WorldId
				|| Neighbor.Tile.SchemeVersion != Input.Tile.SchemeVersion
				|| Neighbor.Tile.TileX != ExpectedX || Neighbor.Tile.TileY != ExpectedY
				|| Neighbor.HeightEdgeHash.IsZero() || Neighbor.CoverageEdgeHash.IsZero())
				return TerrainWorldFail(ETerrainWorldOutcome::BorderMismatch,
					"Terrain neighbor evidence is missing, unordered, or identifies the wrong neighbor.", OutOutcome, OutError);
			FBinaryWriter HeightWriter;
			FBinaryWriter CoverageWriter;
			const int64 DeltaX = Offsets[Index].first;
			const int64 DeltaY = Offsets[Index].second;
			if (DeltaX != 0 && DeltaY != 0)
				AppendBorderSample(HeightWriter, CoverageWriter, Input,
					DeltaX < 0 ? 0 : 256, DeltaY < 0 ? 0 : 256);
			else if (DeltaX != 0)
				for (uint32 Y = 0; Y <= 256; ++Y)
					AppendBorderSample(HeightWriter, CoverageWriter, Input,
						DeltaX < 0 ? 0 : 256, Y);
			else
				for (uint32 X = 0; X <= 256; ++X)
					AppendBorderSample(HeightWriter, CoverageWriter, Input,
						X, DeltaY < 0 ? 0 : 256);
			if (FXxHash128::HashBuffer(HeightWriter.GetBytes()) != Neighbor.HeightEdgeHash
				|| FXxHash128::HashBuffer(CoverageWriter.GetBytes()) != Neighbor.CoverageEdgeHash)
				return TerrainWorldFail(ETerrainWorldOutcome::BorderMismatch,
					"Terrain neighbor evidence does not match the tile's canonical border.", OutOutcome, OutError);
		}
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto EncodeTerrainTileProduct(ETerrainTileProductClass ProductClass,
		const FTerrainTileKey& Tile, const FGuid& GenerationId,
		std::span<const FXxHash128> Dependencies, std::span<const std::byte> Body,
		FByteArray& OutBytes, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (!IsValidProductClass(ProductClass) || !Tile.WorldId.IsValid()
			|| Tile.SchemeVersion != TerrainWorldTileSchemeVersion || !GenerationId.IsValid())
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain tile product identity is invalid.", OutOutcome, OutError);
		if (Dependencies.size() > TerrainWorldMaximumDependencies)
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile product dependency count exceeds its ceiling.", OutOutcome, OutError);
		const uint64 TotalBytes = HeaderBytes + Dependencies.size() * 16ull + Body.size();
		if (TotalBytes > ProductCeiling(ProductClass))
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile product exceeds its class byte ceiling.", OutOutcome, OutError);
		if (!ValidateTerrainWorldProductBody(ProductClass, Body, OutError))
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain tile product body is structurally invalid: " + OutError,
				OutOutcome, OutError);
		const FXxHash128 BodyHash = FXxHash128::HashBuffer(Body);
		FBinaryWriter Writer;
		Writer.WriteU32(ProductEnvelopeMagic);
		Writer.WriteU16(TerrainWorldSchemaVersion);
		Writer.WriteU16(static_cast<uint16>(ProductClass));
		Writer.WriteU32(RequiredFlags);
		Writer.WriteU32(0);
		WriteTileKey(Writer, Tile);
		Writer.WriteU16(0);
		WriteGuid(Writer, GenerationId);
		Writer.WriteU64(Body.size());
		Writer.WriteU64(Body.size());
		WriteHash(Writer, BodyHash);
		Writer.WriteU32(static_cast<uint32>(Dependencies.size()));
		Writer.WriteU32(0);
		for (const FXxHash128& Dependency : Dependencies) WriteHash(Writer, Dependency);
		Writer.WriteBytes(Body);
		if (Writer.GetBytes().size() != TotalBytes)
			return TerrainWorldFail(ETerrainWorldOutcome::PublicationFailed,
				"Terrain tile product header size is inconsistent.", OutOutcome, OutError);
		OutBytes = Writer.TakeBytes();
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto DecodeTerrainTileProduct(std::span<const std::byte> Bytes,
		ETerrainTileProductClass ExpectedClass, FTerrainTileProduct& OutProduct,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!IsValidProductClass(ExpectedClass))
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Expected Terrain tile product class is invalid.", OutOutcome, OutError);
		if (Bytes.size() < 8)
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header is truncated.", OutOutcome, OutError);
		uint32 Magic = 0;
		uint16 Version = 0, EncodedClass = 0;
		if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, Version)
			|| !ReadLittleEndianAt(Bytes, 6, EncodedClass))
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header is truncated.", OutOutcome, OutError);
		if (Magic != ProductEnvelopeMagic)
			return TerrainWorldFail(ETerrainWorldOutcome::UnsupportedLegacySchema,
				std::format("Terrain tile product magic {:#x} does not match the unified TWPD envelope; legacy Terrain values are never decoded.",
					Magic), OutOutcome, OutError);
		if (Version != TerrainWorldSchemaVersion)
			return TerrainWorldFail(ETerrainWorldOutcome::Incompatible,
				"Terrain tile product schema version is incompatible.", OutOutcome, OutError);
		if (EncodedClass != static_cast<uint16>(ExpectedClass))
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product class does not match the requested product.", OutOutcome, OutError);
		if (Bytes.size() > ProductCeiling(ExpectedClass) || Bytes.size() < HeaderBytes)
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile product byte count is outside its class bound.", OutOutcome, OutError);
		FBinaryReader Reader(Bytes);
		uint32 HeaderMagic = 0, Required = 0, Optional = 0, DependencyCount = 0, Reserved32 = 0;
		uint16 HeaderVersion = 0, HeaderClass = 0, TileReserved = 0;
		FTerrainTileProduct Candidate;
		uint64 LogicalBytes = 0, StoredBytes = 0;
		if (!Reader.ReadU32(HeaderMagic) || !Reader.ReadU16(HeaderVersion)
			|| !Reader.ReadU16(HeaderClass) || !Reader.ReadU32(Required)
			|| !Reader.ReadU32(Optional) || !ReadTileKey(Reader, Candidate.Tile)
			|| !Reader.ReadU16(TileReserved) || !ReadGuid(Reader, Candidate.GenerationId)
			|| !Reader.ReadU64(LogicalBytes) || !Reader.ReadU64(StoredBytes)
			|| !ReadHash(Reader, Candidate.BodyHash) || !Reader.ReadU32(DependencyCount)
			|| !Reader.ReadU32(Reserved32))
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header is truncated.", OutOutcome, OutError);
		if (HeaderMagic != Magic || HeaderVersion != Version || HeaderClass != EncodedClass
			|| Required != RequiredFlags || Optional != 0 || TileReserved != 0 || Reserved32 != 0
			|| !Candidate.Tile.WorldId.IsValid() || Candidate.Tile.SchemeVersion != TerrainWorldTileSchemeVersion
			|| !Candidate.GenerationId.IsValid() || DependencyCount > TerrainWorldMaximumDependencies
			|| LogicalBytes != StoredBytes || StoredBytes != Reader.GetRemainingBytes() - DependencyCount * 16ull)
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header fields are invalid.", OutOutcome, OutError);
		Candidate.Dependencies.resize(DependencyCount);
		for (FXxHash128& Dependency : Candidate.Dependencies)
			if (!ReadHash(Reader, Dependency) || Dependency.IsZero())
				return TerrainWorldFail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile product dependency is missing or invalid.", OutOutcome, OutError);
		std::span<const std::byte> Body;
		if (!Reader.ReadRegion(Body, StoredBytes, ProductCeiling(ExpectedClass)) || !Reader.IsAtEnd())
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product body is truncated or has trailing bytes.", OutOutcome, OutError);
		if (FXxHash128::HashBuffer(Body) != Candidate.BodyHash)
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product body checksum is invalid.", OutOutcome, OutError);
		if (!ValidateTerrainWorldProductBody(ExpectedClass, Body, OutError))
			return TerrainWorldFail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product body is structurally invalid: " + OutError,
				OutOutcome, OutError);
		Candidate.ProductClass = ExpectedClass;
		Candidate.Bytes.assign(Bytes.begin(), Bytes.end());
		OutProduct = std::move(Candidate);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto FTerrainTileGenerationPublisher::BeginRequest() -> uint64
	{
		std::lock_guard Lock(Mutex);
		return ++LatestRequestId;
	}

	auto FTerrainTileGenerationPublisher::Publish(uint64 RequestId,
		FTerrainTileGeneration Candidate, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		if (!ValidateGeneration(Candidate, OutOutcome, OutError)) return false;
		std::lock_guard Lock(Mutex);
		if (RequestId == 0 || RequestId != LatestRequestId)
			return TerrainWorldFail(ETerrainWorldOutcome::Superseded,
				"Terrain tile generation publication was superseded.", OutOutcome, OutError);
		Current = std::make_shared<const FTerrainTileGeneration>(std::move(Candidate));
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto FTerrainTileGenerationPublisher::GetCurrent() const
		-> std::shared_ptr<const FTerrainTileGeneration>
	{
		std::lock_guard Lock(Mutex);
		return Current;
	}

	auto FTerrainTileGenerationPublisher::Retire() -> void
	{
		std::lock_guard Lock(Mutex);
		++LatestRequestId;
		Current.reset();
	}

	auto ValidateTerrainWorldProductBody(ETerrainTileProductClass ProductClass,
		std::span<const std::byte> Bytes, std::string& OutError) -> bool
	{
		return ValidateBody(ProductClass, Bytes, OutError);
	}

	auto GetTerrainWorldProductBodyMaximumBytes(ETerrainTileProductClass ProductClass) -> uint64
	{
		return MaximumBodyBytes(ProductClass);
	}
}
