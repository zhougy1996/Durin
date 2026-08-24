#pragma once

#include "ImageFamilyImports.h"

#include "AssetAuthoring.h"
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

		template<typename T>
		auto Append(std::vector<std::byte>& Bytes, const T& Value) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* Begin = reinterpret_cast<const std::byte*>(&Value);
			Bytes.insert(Bytes.end(), Begin, Begin + sizeof(T));
		}

		template<typename T>
		auto Read(std::span<const std::byte>& Bytes, T& Out) -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			if (Bytes.size() < sizeof(T)) return false;
			std::memcpy(&Out, Bytes.data(), sizeof(T));
			Bytes = Bytes.subspan(sizeof(T));
			return true;
		}

		auto AppendString(std::vector<std::byte>& Bytes, std::string_view Value) -> void
		{
			Append(Bytes, static_cast<uint64>(Value.size()));
			Bytes.insert(Bytes.end(), reinterpret_cast<const std::byte*>(Value.data()),
				reinterpret_cast<const std::byte*>(Value.data() + Value.size()));
		}

		auto ReadString(std::span<const std::byte>& Bytes, std::string& Out) -> bool
		{
			uint64 Size = 0;
			if (!Read(Bytes, Size) || Size > Bytes.size() || Size > 1'048'576) return false;
			Out.assign(reinterpret_cast<const char*>(Bytes.data()), static_cast<size_t>(Size));
			Bytes = Bytes.subspan(static_cast<size_t>(Size));
			return true;
		}

		template<typename T>
		auto AppendVector(std::vector<std::byte>& Bytes, std::span<const T> Values) -> void
		{
			Append(Bytes, static_cast<uint64>(Values.size()));
			const auto Raw = std::as_bytes(Values);
			Bytes.insert(Bytes.end(), Raw.begin(), Raw.end());
		}

		template<typename T>
		auto ReadVector(std::span<const std::byte>& Bytes, std::vector<T>& Out,
			uint64 MaximumElements) -> bool
		{
			uint64 Count = 0;
			if (!Read(Bytes, Count) || Count > MaximumElements
				|| Count > Bytes.size() / sizeof(T)) return false;
			Out.resize(static_cast<size_t>(Count));
			const size_t ByteCount = static_cast<size_t>(Count) * sizeof(T);
			std::memcpy(Out.data(), Bytes.data(), ByteCount);
			Bytes = Bytes.subspan(ByteCount);
			return true;
		}

		auto Payload(std::string Schema, std::vector<std::byte> Bytes) -> FSchemaPayload
		{
			FSchemaPayload Result{
				.SchemaId = std::move(Schema), .SchemaVersion = 1, .Bytes = std::move(Bytes)};
			std::string Error;
			(void)Result.Finalize(Error);
			return Result;
		}

		auto EmptyPayload(std::string_view Schema) -> FSchemaPayload
		{
			return Payload(std::string(Schema), {});
		}

		auto ValidatePayload(const FSchemaPayload& Value,
			std::string_view Schema, std::string& OutError) -> bool
		{
			if (Value.SchemaId != Schema || Value.SchemaVersion != 1
				|| Value.ContentHash != FXxHash128::HashBuffer(Value.Bytes))
			{
				OutError = std::format("AssetForge payload '{}' is invalid.", Schema);
				return false;
			}
			return true;
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
			Append(Bytes, Image.Source.Width);
			Append(Bytes, Image.Source.Height);
			Append(Bytes, Image.Source.SourceChannelCount);
			Append(Bytes, Image.Source.Format);
			Append(Bytes, Image.Source.bHasTransparency);
			Append(Bytes, Image.Hash.HashLow);
			Append(Bytes, Image.Hash.HashHigh);
			Append(Bytes, Image.FileSize);
			Append(Bytes, Image.LastWriteTime);
			AppendString(Bytes, Image.Path.Path);
			Append(Bytes, static_cast<uint64>(Image.Source.Pixels.size()));
			Bytes.insert(Bytes.end(), Image.Source.Pixels.begin(), Image.Source.Pixels.end());
			return Payload(std::string(ImageNodeSchema), std::move(Bytes));
		}

		auto DecodeImage(const FSchemaPayload& Value,
			FDecodedImage& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, ImageNodeSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			uint64 PixelBytes = 0;
			if (!Read(Bytes, Out.Source.Width) || !Read(Bytes, Out.Source.Height)
				|| !Read(Bytes, Out.Source.SourceChannelCount)
				|| !Read(Bytes, Out.Source.Format)
				|| !Read(Bytes, Out.Source.bHasTransparency)
				|| !Read(Bytes, Out.Hash.HashLow) || !Read(Bytes, Out.Hash.HashHigh)
				|| !Read(Bytes, Out.FileSize) || !Read(Bytes, Out.LastWriteTime)
				|| !ReadString(Bytes, Out.Path.Path) || !Read(Bytes, PixelBytes)
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
			Append(Bytes, Layout);
			return Payload(std::string(CubeTranslatorSchema), std::move(Bytes));
		}

		auto DecodeCubeTranslatorSettings(const FSchemaPayload& Value,
			ETextureCubeSourceLayout& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, CubeTranslatorSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			if (!Read(Bytes, Out) || !Bytes.empty()
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
			Append(Bytes, Plan.Policy);
			Append(Bytes, Plan.Layout);
			Append(Bytes, Plan.FaceSettings.bSRGB);
			Append(Bytes, Plan.PanoramaSettings.FaceDimension);
			Append(Bytes, Plan.PanoramaSettings.ExposureEV);
			return Payload(std::string(CubePlanSchema), std::move(Bytes));
		}

		auto DecodeCubePlan(const FSchemaPayload& Value,
			FCubePlan& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, CubePlanSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination) || !Read(Bytes, Out.Policy)
				|| !Read(Bytes, Out.Layout) || !Read(Bytes, Out.FaceSettings.bSRGB)
				|| !Read(Bytes, Out.PanoramaSettings.FaceDimension)
				|| !Read(Bytes, Out.PanoramaSettings.ExposureEV) || !Bytes.empty()
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
			Append(Bytes, static_cast<uint8>(Source.index()));
			Append(Bytes, Entry.ContentHash.HashLow);
			Append(Bytes, Entry.ContentHash.HashHigh);
			Append(Bytes, Entry.ByteCount);
			Append(Bytes, Entry.LastWriteTime);
			AppendString(Bytes, Entry.SourcePath.Path);
			std::visit([&](const auto& Image) {
				Append(Bytes, Image.Width);
				Append(Bytes, Image.Height);
				if constexpr (std::is_same_v<std::decay_t<decltype(Image)>,
					Asset::Build::TextureCubeBuilder::FTexturePanoramaImage>)
				{
					Append(Bytes, Image.SourceChannelCount);
					Append(Bytes, Image.bHasTransparency);
					AppendVector(Bytes, std::span(Image.Pixels));
				}
				else AppendVector(Bytes, std::span(Image.Pixels));
			}, Source);
			return Payload(std::string(CubePanoramaSchema), std::move(Bytes));
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
			if (!Read(Bytes, Kind) || Kind > 1 || !Read(Bytes, Out.Hash.HashLow)
				|| !Read(Bytes, Out.Hash.HashHigh) || !Read(Bytes, Out.FileSize)
				|| !Read(Bytes, Out.LastWriteTime) || !ReadString(Bytes, Out.Path.Path))
				return false;
			if (Kind == 0)
			{
				Asset::Build::TextureCubeBuilder::FTexturePanoramaImage Image;
				if (!Read(Bytes, Image.Width) || !Read(Bytes, Image.Height)
					|| !Read(Bytes, Image.SourceChannelCount)
					|| !Read(Bytes, Image.bHasTransparency)
					|| !ReadVector(Bytes, Image.Pixels,
						Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels * 4)) return false;
				Out.Source = std::move(Image);
			}
			else
			{
				Asset::Build::TextureCubeBuilder::FTexturePanoramaFloatImage Image;
				if (!Read(Bytes, Image.Width) || !Read(Bytes, Image.Height)
					|| !ReadVector(Bytes, Image.Pixels,
						Asset::Build::TextureCubeBuilder::MaximumPanoramaPixels * 4)) return false;
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
			Append(Bytes, Plan.Policy);
			Append(Bytes, Plan.Settings.ImportFormat);
			Append(Bytes, Plan.Settings.Channels);
			Append(Bytes, Plan.Settings.SliceWidth);
			Append(Bytes, Plan.Settings.SliceHeight);
			Append(Bytes, Plan.Settings.Depth);
			Append(Bytes, Plan.Settings.TilesX);
			Append(Bytes, Plan.Settings.TilesY);
			return Payload(std::string(VolumePlanSchema), std::move(Bytes));
		}

		auto DecodeVolumePlan(const FSchemaPayload& Value,
			FVolumePlan& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, VolumePlanSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination) || !Read(Bytes, Out.Policy)
				|| !Read(Bytes, Out.Settings.ImportFormat)
				|| !Read(Bytes, Out.Settings.Channels)
				|| !Read(Bytes, Out.Settings.SliceWidth)
				|| !Read(Bytes, Out.Settings.SliceHeight)
				|| !Read(Bytes, Out.Settings.Depth) || !Read(Bytes, Out.Settings.TilesX)
				|| !Read(Bytes, Out.Settings.TilesY) || !Bytes.empty()
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
			Append(Bytes, Plan.Policy);
			return Payload(std::string(TerrainPlanSchema), std::move(Bytes));
		}

		auto DecodeTerrainPlan(const FSchemaPayload& Value,
			FTerrainPlan& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, TerrainPlanSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination) || !Read(Bytes, Out.Policy) || !Bytes.empty()
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
			Append(Bytes, Terrain.Source.Width);
			Append(Bytes, Terrain.Source.Height);
			AppendString(Bytes, Terrain.Source.DecoderId);
			Append(Bytes, Terrain.Source.DecoderVersion);
			Append(Bytes, Terrain.Source.SourceFormat);
			Append(Bytes, Terrain.Source.SourceProfileVersion);
			AppendString(Bytes, Terrain.Path.Path);
			Append(Bytes, Terrain.Hash.HashLow);
			Append(Bytes, Terrain.Hash.HashHigh);
			Append(Bytes, Terrain.FileSize);
			Append(Bytes, Terrain.LastWriteTime);
			AppendVector(Bytes, std::span(Terrain.Source.Samples));
			return Payload(std::string(TerrainNodeSchema), std::move(Bytes));
		}

		auto DecodeTerrain(const FSchemaPayload& Value,
			FDecodedTerrain& Out, std::string& OutError) -> bool
		{
			if (!ValidatePayload(Value, TerrainNodeSchema, OutError)) return false;
			std::span<const std::byte> Bytes(Value.Bytes);
			if (!Read(Bytes, Out.Source.Width) || !Read(Bytes, Out.Source.Height)
				|| !ReadString(Bytes, Out.Source.DecoderId)
				|| !Read(Bytes, Out.Source.DecoderVersion)
				|| !Read(Bytes, Out.Source.SourceFormat)
				|| !Read(Bytes, Out.Source.SourceProfileVersion)
				|| !ReadString(Bytes, Out.Path.Path) || !Read(Bytes, Out.Hash.HashLow)
				|| !Read(Bytes, Out.Hash.HashHigh) || !Read(Bytes, Out.FileSize)
				|| !Read(Bytes, Out.LastWriteTime)
				|| !ReadVector(Bytes, Out.Source.Samples, MaximumTerrainHeightmapSamples)
				|| !Bytes.empty() || !Out.Source.IsValid())
			{
				OutError = "Terrain heightmap payload is malformed.";
				return false;
			}
			return true;
		}

		auto MakeCandidatePath(const FAssetPath& Target, FAssetPath& Out) -> bool
		{
			for (uint32 Suffix = 1; Suffix != 0; ++Suffix)
				if (FAssetPath::TryCreate(std::format("{}_ImportCandidate_{}",
					Target.ToString(), Suffix), Out)
					&& !Asset::FindResidentPackage(Out) && !Asset::FindAssetExact(Out)) return true;
			return false;
		}

		class FCandidate final : public ISingleAssetCandidate
		{
		public:
			explicit FCandidate(DObject* InAsset, bool bInNew)
				: AssetObject(InAsset), Package(InAsset ? InAsset->GetPackage() : nullptr), bNew(bInNew) {}
			auto GetAsset() const -> DObject* override { return AssetObject; }
			auto GetPackage() const -> DPackage* override { return Package; }
			auto IsNewAsset() const -> bool override { return bNew; }
			auto GetAuthoredFingerprint() const -> std::string override
			{
				if (const auto* Cube = Cast<DTextureCube>(AssetObject)) return Cube->GetDerivedDataKey();
				if (const auto* Volume = Cast<DVolumeTexture>(AssetObject)) return Volume->GetDerivedDataKey();
				if (const auto* Terrain = Cast<DTerrainHeightmap>(AssetObject)) return Terrain->GetDerivedDataKey();
				return {};
			}
			auto Validate(std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				const bool Valid = AssetObject && Package &&
					((Cast<DTextureCube>(AssetObject) && Cast<DTextureCube>(AssetObject)->GetPlatformData())
					|| (Cast<DVolumeTexture>(AssetObject) && Cast<DVolumeTexture>(AssetObject)->GetPlatformData())
					|| (Cast<DTerrainHeightmap>(AssetObject) && Cast<DTerrainHeightmap>(AssetObject)->GetPayload()));
				if (!Valid) OutDiagnostics.push_back({
					.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::ValidationFailure,
					.Identity = "Durin.ImageFamily.CandidateInvalid",
					.Phase = "CandidateValidation",
					.Message = "Image-family candidate has no validated runtime data."});
				return Valid;
			}
			auto Abandon() noexcept -> void override
			{
				if (DPackage* Detached = DetachPackageForAbandon())
					(void)Asset::UnloadPackage(
						Detached, EAssetPackageUnloadPolicy::DiscardUnsaved);
			}
			auto DetachPackageForAbandon() noexcept -> DPackage* override
			{
				DPackage* Detached = Package;
				Package = nullptr;
				AssetObject = nullptr;
				return Detached;
			}
		private:
			DObject* AssetObject = nullptr;
			DPackage* Package = nullptr;
			bool bNew = false;
		};

		template<typename T>
		class TExchange final : public IPreparedImportedStateExchange
		{
		public:
			TExchange(T& InTarget, T& InCandidate) : Target(&InTarget), Candidate(&InCandidate) {}
			auto Commit() noexcept -> void override
			{
				if (!bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = true; }
			}
			auto Reverse() noexcept -> void override
			{
				if (bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = false; }
			}
			auto Finalize() noexcept -> void override { Target = nullptr; Candidate = nullptr; }
		private:
			T* Target;
			T* Candidate;
			bool bCommitted = false;
		};

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

		auto DecodeStoredProvenance(std::string_view Hex,
			FImportProvenance& Out, std::string& OutError) -> bool
		{
			if (Hex.empty() || (Hex.size() & 1) != 0)
			{
				OutError = "AssetForge provenance encoding is malformed.";
				return false;
			}
			auto Nibble = [](char Value) -> int {
				if (Value >= '0' && Value <= '9') return Value - '0';
				if (Value >= 'a' && Value <= 'f') return Value - 'a' + 10;
				return -1;
			};
			std::vector<std::byte> Bytes(Hex.size() / 2);
			for (size_t Index = 0; Index < Bytes.size(); ++Index)
			{
				const int High = Nibble(Hex[Index * 2]);
				const int Low = Nibble(Hex[Index * 2 + 1]);
				if (High < 0 || Low < 0) return false;
				Bytes[Index] = static_cast<std::byte>((High << 4) | Low);
			}
			return DeserializeImportProvenance(Bytes, Out, OutError);
		}

		auto PolicyFor(EImportMode Mode) -> EImportOutputPolicy
		{
			return Mode == EImportMode::Import || Mode == EImportMode::Preview
				? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		}
	}

}
