#include "Terrain/TerrainWorldBuildFunctions.h"

#include "Serialization/BinaryFormat.h"

namespace Durin::Asset::Private
{
	namespace
	{
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

		class FTerrainWorldBuildFunction final : public IBuildFunction
		{
		public:
			explicit FTerrainWorldBuildFunction(ETerrainTileProductClass InClass)
				: ProductClass(InClass) {}

			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.Version = TerrainWorldBuilderVersion,
					.CacheBucket = std::format("TerrainWorld/{}/Objects",
					GetTerrainWorldBuildValueName(ProductClass)),
					.ExpectedValueName = std::string(GetTerrainWorldBuildValueName(ProductClass)),
					.MaximumValueBytes = MaximumBodyBytes(ProductClass)};
			}

			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				if (Definition.GetTargetFact("TerrainProductClass")
						!= std::optional<std::string_view>(std::to_string(static_cast<uint8>(ProductClass)))
					|| Value.GetName() != GetTerrainWorldBuildValueName(ProductClass))
				{
					OutError = "Terrain World product definition or value name is incompatible.";
					return false;
				}
				if (!ValidateBody(ProductClass, Value.GetBytes(), OutError)) return false;
				if (const FBuildValue* Expected = Definition.GetInput(TerrainWorldProductInputName);
					Expected && !std::ranges::equal(Expected->GetBytes(), Value.GetBytes()))
				{
					OutError = "Terrain World cached product body does not match its normalized build input.";
					return false;
				}
				return true;
			}

			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(TerrainWorldProductInputName);
				if (!Input || !ValidateBody(ProductClass, Input->GetBytes(), OutError)) return false;
				if (Context.IsCanceled())
				{
					OutError = "Terrain World product build was cancelled.";
					return false;
				}
				OutValue = FBuildValue::FromOwned(std::string(GetTerrainWorldBuildValueName(ProductClass)),
					FByteArray(Input->GetBytes().begin(), Input->GetBytes().end()));
				return true;
			}

		private:
			ETerrainTileProductClass ProductClass;
		};
	}

	auto GetTerrainWorldBuildFunctionName(ETerrainTileProductClass ProductClass)
		-> FBuildFunctionName
	{
		return FBuildFunctionName::FromString(std::format(
			"Durin.GeometryBuild.TerrainWorld.{}",
			GetTerrainWorldBuildValueName(ProductClass)));
	}

	auto ValidateTerrainWorldProductBody(ETerrainTileProductClass ProductClass,
		std::span<const std::byte> Bytes, std::string& OutError) -> bool
	{
		return ValidateBody(ProductClass, Bytes, OutError);
	}

	auto GetTerrainWorldBuildValueName(ETerrainTileProductClass ProductClass)
		-> std::string_view
	{
		switch (ProductClass)
		{
		case ETerrainTileProductClass::Metadata: return "TerrainWorldMetadata";
		case ETerrainTileProductClass::Height: return "TerrainWorldHeight";
		case ETerrainTileProductClass::Coverage: return "TerrainWorldCoverage";
		case ETerrainTileProductClass::Collision: return "TerrainWorldCollision";
		case ETerrainTileProductClass::Query: return "TerrainWorldQuery";
		}
		return "TerrainWorldInvalid";
	}

	auto CreateTerrainWorldBuildFunction(ETerrainTileProductClass ProductClass)
		-> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FTerrainWorldBuildFunction>(ProductClass);
	}
}
