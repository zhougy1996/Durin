#pragma once

#include "BuiltinImportProvenance.h"
#include "BuiltinImportSchema.h"
#include "BuiltinSingleAssetImport.h"
#include "ImageFamilyImports.h"

#include "Asset/SourcePath.h"
#include "AssetForge/ImportService.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetForge/Builtins/VolumeTextureImport.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/VolumeTextureBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		inline constexpr std::string_view CubeTranslatorId = "Durin.TextureCube";
		inline constexpr std::string_view CubePlanningPassId = "Durin.TextureCube.Default";
		inline constexpr std::string_view CubeBuilderId = "Durin.TextureCube.Builder";
		inline constexpr std::string_view CubeTranslatorSchema = "Durin.TextureCube.TranslatorSettings";
		inline constexpr std::string_view CubePlanSchema = "Durin.TextureCube.Plan";
		inline constexpr std::string_view CubePanoramaSchema = "Durin.TextureCube.Panorama";
		inline constexpr std::string_view ImageNodeSchema = "Durin.Image.RGBA8";
		inline constexpr std::string_view ImageTranslatorId = "Durin.Image";
		inline constexpr std::string_view EmptyImageSettingsSchema = "Durin.Image.TranslatorSettings";
		inline constexpr std::string_view VolumePlanningPassId = "Durin.VolumeTexture.Atlas";
		inline constexpr std::string_view VolumeBuilderId = "Durin.VolumeTexture.Builder";
		inline constexpr std::string_view VolumePlanSchema = "Durin.VolumeTexture.Plan";
		inline constexpr std::string_view TerrainTranslatorId = "Durin.TerrainHeightmap";
		inline constexpr std::string_view TerrainPlanningPassId = "Durin.TerrainHeightmap.Default";
		inline constexpr std::string_view TerrainBuilderId = "Durin.TerrainHeightmap.Builder";
		inline constexpr std::string_view TerrainTranslatorSchema = "Durin.TerrainHeightmap.TranslatorSettings";
		inline constexpr std::string_view TerrainPlanSchema = "Durin.TerrainHeightmap.Plan";
		inline constexpr std::string_view TerrainNodeSchema = "Durin.TerrainHeightmap.Samples";

		auto EmptyPayload(std::string_view Schema) -> FSchemaPayload
		{
			return MakeSchemaPayload(std::string(Schema), 1, {});
		}

		auto ValidatePayload(const FSchemaPayload& Value,
			std::string_view Schema, std::string& OutError) -> bool
		{
			return ValidateSchemaPayload(Value, Schema, 1, OutError);
		}

		struct FDecodedImage
		{
			FTextureSourceData Source;
			FSourcePath Path;
			FXxHash128 Hash{};
			uint64 FileSize = 0;
			int64 LastWriteTime = 0;
		};

		auto EncodeImage(const FDecodedImage& Image) -> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Image.Source.Width);
			AppendValue(Bytes, Image.Source.Height);
			AppendValue(Bytes, Image.Source.SourceChannelCount);
			AppendValue(Bytes, Image.Source.Format);
			AppendValue(Bytes, Image.Source.bHasTransparency);
			AppendValue(Bytes, Image.Hash.HashLow);
			AppendValue(Bytes, Image.Hash.HashHigh);
			AppendValue(Bytes, Image.FileSize);
			AppendValue(Bytes, Image.LastWriteTime);
			AppendString(Bytes, Image.Path.Path);
			AppendValue(Bytes, static_cast<uint64>(Image.Source.Pixels.size()));
			Bytes.insert(Bytes.end(), Image.Source.Pixels.begin(), Image.Source.Pixels.end());
			return MakeSchemaPayload(std::string(ImageNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeImage(const FSchemaPayload& Value,
			FDecodedImage& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, ImageNodeSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			uint64 PixelBytes = 0;
			if (!ReadValue(Bytes, Out.Source.Width) || !ReadValue(Bytes, Out.Source.Height)
				|| !ReadValue(Bytes, Out.Source.SourceChannelCount)
				|| !ReadValue(Bytes, Out.Source.Format)
				|| !ReadValue(Bytes, Out.Source.bHasTransparency)
				|| !ReadValue(Bytes, Out.Hash.HashLow) || !ReadValue(Bytes, Out.Hash.HashHigh)
				|| !ReadValue(Bytes, Out.FileSize) || !ReadValue(Bytes, Out.LastWriteTime)
				|| !ReadString(Bytes, Out.Path.Path) || !ReadValue(Bytes, PixelBytes)
				|| PixelBytes != Bytes.size())
			{
				OutError = "Normalized image payload is malformed.";
				return false;
			}
			Out.Source.Pixels.assign(Bytes.begin(), Bytes.end());
			if (!Out.Source.IsValid() || Out.Path.IsEmpty())
			{
				OutError = "Normalized image payload contains invalid data.";
				return false;
			}
			OutError.clear();
			return true;
		}

		struct FCubePlan
		{
			FAssetPath Destination;
			EImportOutputPolicy Policy = EImportOutputPolicy::Create;
			ETextureCubeSourceLayout Layout = ETextureCubeSourceLayout::SixFaces;
			FTextureCubeImportSettings FaceSettings;
			FTextureCubePanoramaImportSettings PanoramaSettings;
		};

		auto EncodeCubeTranslatorSettings(ETextureCubeSourceLayout Layout)
			-> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Layout);
			return MakeSchemaPayload(std::string(CubeTranslatorSchema), 1, std::move(Bytes));
		}

		auto DecodeCubeTranslatorSettings(const FSchemaPayload& Value,
			ETextureCubeSourceLayout& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, CubeTranslatorSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			if (!ReadValue(Bytes, Out) || !Bytes.empty()
				|| (Out != ETextureCubeSourceLayout::SixFaces
					&& Out != ETextureCubeSourceLayout::EquirectangularPanorama))
			{
				OutError = "TextureCube translator settings are malformed.";
				return false;
			}
			return true;
		}

		auto EncodeCubePlan(const FCubePlan& Plan) -> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			AppendValue(Bytes, Plan.Policy);
			AppendValue(Bytes, Plan.Layout);
			AppendValue(Bytes, Plan.FaceSettings.bSRGB);
			AppendValue(Bytes, Plan.PanoramaSettings.FaceDimension);
			AppendValue(Bytes, Plan.PanoramaSettings.ExposureEV);
			return MakeSchemaPayload(std::string(CubePlanSchema), 1, std::move(Bytes));
		}

		auto DecodeCubePlan(const FSchemaPayload& Value,
			FCubePlan& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, CubePlanSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination) || !ReadValue(Bytes, Out.Policy)
				|| !ReadValue(Bytes, Out.Layout) || !ReadValue(Bytes, Out.FaceSettings.bSRGB)
				|| !ReadValue(Bytes, Out.PanoramaSettings.FaceDimension)
				|| !ReadValue(Bytes, Out.PanoramaSettings.ExposureEV) || !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, Out.Destination, &OutError)
				|| !std::isfinite(Out.PanoramaSettings.ExposureEV))
			{
				if (OutError.empty()) OutError = "TextureCube plan is malformed.";
				return false;
			}
			return true;
		}

		auto EncodePanorama(const FTextureCubePanoramaSourceData& Source,
			const FSourceSnapshotEntry& Entry) -> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, static_cast<uint8>(Source.index()));
			AppendValue(Bytes, Entry.ContentHash.HashLow);
			AppendValue(Bytes, Entry.ContentHash.HashHigh);
			AppendValue(Bytes, Entry.ByteCount);
			AppendValue(Bytes, Entry.LastWriteTime);
			AppendString(Bytes, Entry.SourcePath.Path);
			std::visit([&](const auto& Image) {
				AppendValue(Bytes, Image.Width);
				AppendValue(Bytes, Image.Height);
				if constexpr (std::is_same_v<std::decay_t<decltype(Image)>,
					Asset::TextureCubeBuilder::FTexturePanoramaImage>)
				{
					AppendValue(Bytes, Image.SourceChannelCount);
					AppendValue(Bytes, Image.bHasTransparency);
					AppendTrivialVector(Bytes, std::span(Image.Pixels));
				}
				else AppendTrivialVector(Bytes, std::span(Image.Pixels));
			}, Source);
			return MakeSchemaPayload(std::string(CubePanoramaSchema), 1, std::move(Bytes));
		}

		struct FDecodedPanorama
		{
			FTextureCubePanoramaSourceData Source;
			FSourcePath Path;
			FXxHash128 Hash{};
			uint64 FileSize = 0;
			int64 LastWriteTime = 0;
		};

		auto DecodePanorama(const FSchemaPayload& Value,
			FDecodedPanorama& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, CubePanoramaSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			uint8 Kind = 0;
			if (!ReadValue(Bytes, Kind) || Kind > 1 || !ReadValue(Bytes, Out.Hash.HashLow)
				|| !ReadValue(Bytes, Out.Hash.HashHigh) || !ReadValue(Bytes, Out.FileSize)
				|| !ReadValue(Bytes, Out.LastWriteTime) || !ReadString(Bytes, Out.Path.Path))
				return false;
			if (Kind == 0)
			{
				Asset::TextureCubeBuilder::FTexturePanoramaImage Image;
				if (!ReadValue(Bytes, Image.Width) || !ReadValue(Bytes, Image.Height)
					|| !ReadValue(Bytes, Image.SourceChannelCount)
					|| !ReadValue(Bytes, Image.bHasTransparency)
					|| !ReadTrivialVector(Bytes, Image.Pixels,
						Asset::TextureCubeBuilder::MaximumPanoramaPixels * 4)) return false;
				Out.Source = std::move(Image);
			}
			else
			{
				Asset::TextureCubeBuilder::FTexturePanoramaFloatImage Image;
				if (!ReadValue(Bytes, Image.Width) || !ReadValue(Bytes, Image.Height)
					|| !ReadTrivialVector(Bytes, Image.Pixels,
						Asset::TextureCubeBuilder::MaximumPanoramaPixels * 4)) return false;
				Out.Source = std::move(Image);
			}
			if (!Bytes.empty() || Out.Path.IsEmpty())
			{
				OutError = "TextureCube panorama payload is malformed.";
				return false;
			}
			return true;
		}

		struct FVolumePlan
		{
			FAssetPath Destination;
			EImportOutputPolicy Policy = EImportOutputPolicy::Create;
			FVolumeTextureImportSettings Settings;
		};

		auto EncodeVolumePlan(const FVolumePlan& Plan) -> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			AppendValue(Bytes, Plan.Policy);
			AppendValue(Bytes, Plan.Settings.ImportFormat);
			AppendValue(Bytes, Plan.Settings.Channels);
			AppendValue(Bytes, Plan.Settings.SliceWidth);
			AppendValue(Bytes, Plan.Settings.SliceHeight);
			AppendValue(Bytes, Plan.Settings.Depth);
			AppendValue(Bytes, Plan.Settings.TilesX);
			AppendValue(Bytes, Plan.Settings.TilesY);
			return MakeSchemaPayload(std::string(VolumePlanSchema), 1, std::move(Bytes));
		}

		auto DecodeVolumePlan(const FSchemaPayload& Value,
			FVolumePlan& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, VolumePlanSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination) || !ReadValue(Bytes, Out.Policy)
				|| !ReadValue(Bytes, Out.Settings.ImportFormat)
				|| !ReadValue(Bytes, Out.Settings.Channels)
				|| !ReadValue(Bytes, Out.Settings.SliceWidth)
				|| !ReadValue(Bytes, Out.Settings.SliceHeight)
				|| !ReadValue(Bytes, Out.Settings.Depth) || !ReadValue(Bytes, Out.Settings.TilesX)
				|| !ReadValue(Bytes, Out.Settings.TilesY) || !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, Out.Destination, &OutError)
				|| !Out.Settings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "VolumeTexture plan is malformed.";
				return false;
			}
			return true;
		}

		struct FTerrainPlan
		{
			FAssetPath Destination;
			EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		};

		auto EncodeTerrainPlan(const FTerrainPlan& Plan) -> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			AppendValue(Bytes, Plan.Policy);
			return MakeSchemaPayload(std::string(TerrainPlanSchema), 1, std::move(Bytes));
		}

		auto DecodeTerrainPlan(const FSchemaPayload& Value,
			FTerrainPlan& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, TerrainPlanSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination) || !ReadValue(Bytes, Out.Policy) || !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, Out.Destination, &OutError))
			{
				if (OutError.empty()) OutError = "Terrain heightmap plan is malformed.";
				return false;
			}
			return true;
		}

		struct FDecodedTerrain
		{
			FTerrainHeightmapSourceData Source;
			FSourcePath Path;
			FXxHash128 Hash{};
			uint64 FileSize = 0;
			int64 LastWriteTime = 0;
		};

		auto EncodeTerrain(const FDecodedTerrain& Terrain) -> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Terrain.Source.Width);
			AppendValue(Bytes, Terrain.Source.Height);
			AppendString(Bytes, Terrain.Source.DecoderId);
			AppendValue(Bytes, Terrain.Source.DecoderVersion);
			AppendValue(Bytes, Terrain.Source.SourceFormat);
			AppendValue(Bytes, Terrain.Source.SourceProfileVersion);
			AppendString(Bytes, Terrain.Path.Path);
			AppendValue(Bytes, Terrain.Hash.HashLow);
			AppendValue(Bytes, Terrain.Hash.HashHigh);
			AppendValue(Bytes, Terrain.FileSize);
			AppendValue(Bytes, Terrain.LastWriteTime);
			AppendTrivialVector(Bytes, std::span(Terrain.Source.Samples));
			return MakeSchemaPayload(std::string(TerrainNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeTerrain(const FSchemaPayload& Value,
			FDecodedTerrain& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, TerrainNodeSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			if (!ReadValue(Bytes, Out.Source.Width) || !ReadValue(Bytes, Out.Source.Height)
				|| !ReadString(Bytes, Out.Source.DecoderId)
				|| !ReadValue(Bytes, Out.Source.DecoderVersion)
				|| !ReadValue(Bytes, Out.Source.SourceFormat)
				|| !ReadValue(Bytes, Out.Source.SourceProfileVersion)
				|| !ReadString(Bytes, Out.Path.Path) || !ReadValue(Bytes, Out.Hash.HashLow)
				|| !ReadValue(Bytes, Out.Hash.HashHigh) || !ReadValue(Bytes, Out.FileSize)
				|| !ReadValue(Bytes, Out.LastWriteTime)
				|| !ReadTrivialVector(Bytes, Out.Source.Samples, MaximumTerrainHeightmapSamples)
				|| !Bytes.empty() || !Out.Source.IsValid())
			{
				OutError = "Terrain heightmap payload is malformed.";
				return false;
			}
			return true;
		}

		auto AddFailure(std::vector<FImportDiagnostic>& Diagnostics,
			std::string Identity, std::string Phase, std::string Message) -> void
		{
			Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
				.Category = EImportDiagnosticCategory::CandidateFailure,
				.Identity = std::move(Identity), .Phase = std::move(Phase),
				.Message = std::move(Message)});
		}

		auto ApplyProvenanceBytes(DObject& Object,
			const FImportProvenance& Provenance,
			std::vector<FImportDiagnostic>& Diagnostics) -> bool
		{
			std::vector<std::byte> Bytes;
			std::string Error;
			if (!SerializeImportProvenance(Provenance, Bytes, Error))
			{
				AddFailure(Diagnostics, "Durin.ImageFamily.ProvenanceFailed", "Publication", Error);
				return false;
			}
			if (auto* Cube = Cast<DTextureCube>(&Object)) Cube->PublishImportProvenance(std::move(Bytes));
			else if (auto* Volume = Cast<DVolumeTexture>(&Object)) Volume->PublishImportProvenance(std::move(Bytes));
			else if (auto* Terrain = Cast<DTerrainHeightmap>(&Object)) Terrain->PublishImportProvenance(std::move(Bytes));
			else return false;
			return true;
		}

		auto PolicyFor(EImportMode Mode) -> EImportOutputPolicy
		{
			return Mode == EImportMode::Import || Mode == EImportMode::Preview
				? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		}
	}

}
