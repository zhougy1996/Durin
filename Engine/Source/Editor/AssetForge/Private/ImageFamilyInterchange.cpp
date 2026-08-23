#include "ImageFamilyInterchange.h"

#include "AssetAuthoring.h"
#include "ImportService.h"
#include "TerrainHeightmapSourceTranslation.h"
#include "Texture2DSourceTranslation.h"
#include "TextureCubeSourceTranslation.h"
#include "VolumeTextureSourceTranslation.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Texture/TextureCubeBuildOperations.h"
#include "Texture/VolumeTextureBuildOperations.h"

namespace Durin::Asset::Forge
{
	namespace
	{
		inline constexpr std::string_view CubeTranslatorId = "Durin.TextureCube";
		inline constexpr std::string_view CubePipelineId = "Durin.TextureCube.Default";
		inline constexpr std::string_view CubeFactoryId = "Durin.TextureCube.Factory";
		inline constexpr std::string_view CubeTranslatorSchema = "Durin.TextureCube.TranslatorSettings";
		inline constexpr std::string_view CubePlanSchema = "Durin.TextureCube.Plan";
		inline constexpr std::string_view CubePanoramaSchema = "Durin.TextureCube.Panorama";
		inline constexpr std::string_view ImageNodeSchema = "Durin.Image.RGBA8";
		inline constexpr std::string_view ImageTranslatorId = "Durin.Image";
		inline constexpr std::string_view EmptyImageSettingsSchema = "Durin.Image.TranslatorSettings";
		inline constexpr std::string_view VolumePipelineId = "Durin.VolumeTexture.Atlas";
		inline constexpr std::string_view VolumeFactoryId = "Durin.VolumeTexture.Factory";
		inline constexpr std::string_view VolumePlanSchema = "Durin.VolumeTexture.Plan";
		inline constexpr std::string_view TerrainTranslatorId = "Durin.TerrainHeightmap";
		inline constexpr std::string_view TerrainPipelineId = "Durin.TerrainHeightmap.Default";
		inline constexpr std::string_view TerrainFactoryId = "Durin.TerrainHeightmap.Factory";
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

		auto Payload(std::string Schema, std::vector<std::byte> Bytes) -> FInterchangePayload
		{
			FInterchangePayload Result{
				.SchemaId = std::move(Schema), .SchemaVersion = 1, .Bytes = std::move(Bytes)};
			std::string Error;
			(void)Result.Finalize(Error);
			return Result;
		}

		auto EmptyPayload(std::string_view Schema) -> FInterchangePayload
		{
			return Payload(std::string(Schema), {});
		}

		auto ValidatePayload(const FInterchangePayload& Value,
			std::string_view Schema, std::string& OutError) -> bool
		{
			if (Value.SchemaId != Schema || Value.SchemaVersion != 1
				|| Value.ContentHash != FXxHash128::HashBuffer(Value.Bytes))
			{
				OutError = std::format("Interchange payload '{}' is invalid.", Schema);
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

		auto EncodeImage(const FDecodedImage& Image) -> FInterchangePayload
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

		auto DecodeImage(const FInterchangePayload& Value,
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
			-> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			Append(Bytes, Layout);
			return Payload(std::string(CubeTranslatorSchema), std::move(Bytes));
		}

		auto DecodeCubeTranslatorSettings(const FInterchangePayload& Value,
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

		auto EncodeCubePlan(const FCubePlan& Plan) -> FInterchangePayload
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

		auto DecodeCubePlan(const FInterchangePayload& Value,
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
			const FSourceSnapshotEntry& Entry) -> FInterchangePayload
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

		auto DecodePanorama(const FInterchangePayload& Value,
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

		auto EncodeVolumePlan(const FVolumePlan& Plan) -> FInterchangePayload
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

		auto DecodeVolumePlan(const FInterchangePayload& Value,
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

		auto EncodeTerrainPlan(const FTerrainPlan& Plan) -> FInterchangePayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			Append(Bytes, Plan.Policy);
			return Payload(std::string(TerrainPlanSchema), std::move(Bytes));
		}

		auto DecodeTerrainPlan(const FInterchangePayload& Value,
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

		auto EncodeTerrain(const FDecodedTerrain& Terrain) -> FInterchangePayload
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

		auto DecodeTerrain(const FInterchangePayload& Value,
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
			const FInterchangeProvenance& Provenance,
			std::vector<FImportDiagnostic>& Diagnostics) -> bool
		{
			std::vector<std::byte> Bytes;
			std::string Error;
			if (!SerializeInterchangeProvenance(Provenance, Bytes, Error))
			{
				AddFailure(Diagnostics, "Durin.ImageFamily.ProvenanceFailed", "Publication", Error);
				return false;
			}
			if (auto* Cube = Cast<DTextureCube>(&Object)) Cube->PublishInterchangeProvenance(std::move(Bytes));
			else if (auto* Volume = Cast<DVolumeTexture>(&Object)) Volume->PublishInterchangeProvenance(std::move(Bytes));
			else if (auto* Terrain = Cast<DTerrainHeightmap>(&Object)) Terrain->PublishInterchangeProvenance(std::move(Bytes));
			else return false;
			return true;
		}

		class FCubeTranslator final : public IInterchangeTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				return IsTextureCubePanoramaSourceExtension(Source.Extension);
			}
			auto DiscoverDependencies(std::span<const FSourceSnapshotEntry>,
				FDependencyRequestSink&, std::vector<FImportDiagnostic>&) const -> bool override { return true; }
			auto Translate(const FSourceSnapshot& Snapshot, const FInterchangePayload& Settings,
				FTranslatedAssetGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				ETextureCubeSourceLayout Layout;
				std::string Error;
				if (!DecodeCubeTranslatorSettings(Settings, Layout, Error))
				{
					AddFailure(Diagnostics, "Durin.TextureCube.SettingsInvalid", "Translation", Error);
					return false;
				}
				if (Layout == ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
					FTextureCubePanoramaSourceData Panorama;
					if (!Root || !TranslateTextureCubePanoramaSource(Root->GetBytes(),
						std::filesystem::path(Root->SourcePath.Path).extension().generic_string(), Panorama, Error))
					{
						AddFailure(Diagnostics, "Durin.TextureCube.DecodeFailed", "Translation",
							std::format("Panorama decode failed: {}", Error));
						return false;
					}
					return Builder.AddNode({.StableIdentity = "panorama",
						.NodeKind = "Durin.TextureCube.Panorama",
						.Payload = EncodePanorama(Panorama, *Root),
						.SourceIdentities = {"root"}});
				}
				for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					const std::string SourceId = Index == 0 ? "root" : std::format("face:{}", Index);
					const FSourceSnapshotEntry* Entry = Snapshot.FindSource(SourceId);
					FDecodedImage Image;
					if (!Entry || !TranslateTexture2DSource(Entry->GetBytes(), Image.Source, Error))
					{
						AddFailure(Diagnostics, "Durin.TextureCube.FaceDecodeFailed", "Translation", Error);
						return false;
					}
					Image.Path = Entry->SourcePath;
					Image.Hash = Entry->ContentHash;
					Image.FileSize = Entry->ByteCount;
					Image.LastWriteTime = Entry->LastWriteTime;
					if (!Builder.AddNode({.StableIdentity = std::format("image:{}", Index),
						.NodeKind = "Durin.Image.RGBA8", .Payload = EncodeImage(Image),
						.SourceIdentities = {SourceId}})) return false;
				}
				return true;
			}
		};

		class FCubePipeline final : public IInterchangePipeline
		{
		public:
			auto Execute(const FTranslatedAssetGraph& Graph, const FImportFactoryGraph*,
				const FInterchangePayload& Settings, FImportFactoryGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				FCubePlan Plan;
				std::string Error;
				if (!DecodeCubePlan(Settings, Plan, Error)
					|| (Plan.Layout == ETextureCubeSourceLayout::SixFaces
						? !Graph.FindNode("image:0") : !Graph.FindNode("panorama")))
				{
					AddFailure(Diagnostics, "Durin.TextureCube.PlanInvalid", "Pipeline", Error);
					return false;
				}
				std::vector<std::string> References;
				if (Plan.Layout == ETextureCubeSourceLayout::SixFaces)
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
						References.push_back(std::format("image:{}", Index));
				else References.push_back("panorama");
				return Builder.AddNode({.StableIdentity = "texture-cube",
					.FactoryId = std::string(CubeFactoryId), .FactoryContractVersion = 1,
					.OutputClassName = "Durin::DTextureCube", .Destination = Plan.Destination,
					.Policy = Plan.Policy, .Settings = Settings,
					.TranslatedNodeReferences = std::move(References)});
			}
		};

		class FCubeProduct final : public IInterchangeFactoryProduct
		{
		public:
			Asset::Build::FTextureCubeBuildProduct Product;
			Asset::Build::FTextureCubePublicationContext Publication;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				auto Result = std::make_unique<FCubeProduct>();
				Result->Product.SourceLayout = Product.SourceLayout;
				Result->Product.SourceData = Product.SourceData;
				if (Product.PlatformData)
					Result->Product.PlatformData =
						std::make_unique<FTextureCubePlatformData>(*Product.PlatformData);
				Result->Product.DerivedDataKey = Product.DerivedDataKey;
				Result->Product.SourceWidth = Product.SourceWidth;
				Result->Product.SourceHeight = Product.SourceHeight;
				Result->Product.PanoramaFaceDimension = Product.PanoramaFaceDimension;
				Result->Product.PanoramaExposureEV = Product.PanoramaExposureEV;
				Result->Product.bSRGB = Product.bSRGB;
				Result->Publication = Publication;
				return Result;
			}
		};

		class FCubeFactory final : public IInterchangeFactory
		{
		public:
			auto BuildDetachedProduct(const FImportFactoryNode& Node,
				const FTranslatedAssetGraph& Graph, IImportProgressReporter*,
				const std::function<bool()>& Canceled,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				if (Canceled()) return {};
				FCubePlan Plan;
				std::string Error;
				if (!DecodeCubePlan(Node.Settings, Plan, Error)) return {};
				auto Result = std::make_unique<FCubeProduct>();
				bool Built = false;
				if (Plan.Layout == ETextureCubeSourceLayout::SixFaces)
				{
					FTextureCubeSourceData Source;
					std::array<FXxHash128, TextureCubeFaceCount> Hashes;
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
					{
						const auto* ImageNode = Graph.FindNode(std::format("image:{}", Index));
						FDecodedImage Image;
						if (!ImageNode || !DecodeImage(ImageNode->Payload, Image, Error)) return {};
						Source.Faces[Index] = std::move(Image.Source);
						Hashes[Index] = Image.Hash;
						Result->Publication.FaceHashes[Index] = Image.Hash;
						Result->Publication.FacePaths[Index] = std::move(Image.Path);
					}
					Built = Asset::Build::BuildTextureCubeFaces(std::move(Source), Hashes,
						Plan.FaceSettings, Result->Product, Error);
				}
				else
				{
					const auto* PanoramaNode = Graph.FindNode("panorama");
					FDecodedPanorama Panorama;
					if (!PanoramaNode || !DecodePanorama(PanoramaNode->Payload, Panorama, Error)) return {};
					Result->Publication.PanoramaHash = Panorama.Hash;
					Result->Publication.PanoramaPath = std::move(Panorama.Path);
					Built = std::visit([&](auto&& Source) {
						return Asset::Build::BuildTextureCubePanorama(std::move(Source), Panorama.Hash,
							Plan.PanoramaSettings, Result->Product, Error);
					}, std::move(Panorama.Source));
				}
				if (!Built)
				{
					AddFailure(Diagnostics, "Durin.TextureCube.BuildFailed", "ProductBuild", Error);
					return {};
				}
				return Result;
			}

			auto MaterializeCandidate(const FImportFactoryNode& Node,
				std::unique_ptr<IInterchangeFactoryProduct> Product,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Typed = dynamic_cast<FCubeProduct*>(Product.get());
				FAssetPath Path = Node.Destination;
				if (Node.Policy != EImportOutputPolicy::Create && !MakeCandidatePath(Node.Destination, Path)) return {};
				DTextureCube* AssetObject = nullptr;
				if (!Typed || !Asset::CreateAsset(Path, AssetObject)) return {};
				auto Result = std::make_unique<FCandidate>(AssetObject, Node.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::Build::PublishTextureCubeProduct(*AssetObject,
					std::move(Typed->Product), Typed->Publication, Error))
				{
					AddFailure(Diagnostics, "Durin.TextureCube.MaterializeFailed", "Materialization", Error);
					Result->Abandon();
					return {};
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* A = Cast<DTextureCube>(&Target);
				auto* B = Cast<DTextureCube>(Candidate.GetAsset());
				return A && B ? std::make_unique<TExchange<DTextureCube>>(*A, *B) : nullptr;
			}
			auto ApplyProvenance(DObject& Object, const FInterchangeProvenance& Provenance,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				return ApplyProvenanceBytes(Object, Provenance, Diagnostics);
			}
		};

		class FVolumePipeline final : public IInterchangePipeline
		{
		public:
			auto Execute(const FTranslatedAssetGraph& Graph, const FImportFactoryGraph*,
				const FInterchangePayload& Settings, FImportFactoryGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				FVolumePlan Plan;
				std::string Error;
				if (!Graph.FindNode("image") || !DecodeVolumePlan(Settings, Plan, Error))
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.PlanInvalid", "Pipeline", Error);
					return false;
				}
				return Builder.AddNode({.StableIdentity = "volume-texture",
					.FactoryId = std::string(VolumeFactoryId), .FactoryContractVersion = 1,
					.OutputClassName = "Durin::DVolumeTexture", .Destination = Plan.Destination,
					.Policy = Plan.Policy, .Settings = Settings,
					.TranslatedNodeReferences = {"image"}});
			}
		};

		class FVolumeProduct final : public IInterchangeFactoryProduct
		{
		public:
			Asset::Build::FVolumeTextureBuildProduct Product;
			FVolumeTextureSourceImportData Provenance;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				auto Result = std::make_unique<FVolumeProduct>();
				Result->Product.SourceData = Product.SourceData;
				Result->Product.Settings = Product.Settings;
				if (Product.PlatformData)
					Result->Product.PlatformData =
						std::make_unique<FVolumeTexturePlatformData>(*Product.PlatformData);
				Result->Product.DerivedDataKey = Product.DerivedDataKey;
				Result->Product.bCacheHit = Product.bCacheHit;
				Result->Provenance = Provenance;
				return Result;
			}
		};

		class FVolumeFactory final : public IInterchangeFactory
		{
		public:
			auto BuildDetachedProduct(const FImportFactoryNode& Node,
				const FTranslatedAssetGraph& Graph, IImportProgressReporter*,
				const std::function<bool()>& Canceled,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				if (Canceled()) return {};
				const auto* ImageNode = Graph.FindNode("image");
				FDecodedImage Image;
				FVolumePlan Plan;
				std::string Error;
				if (!ImageNode || !DecodeImage(ImageNode->Payload, Image, Error)
					|| !DecodeVolumePlan(Node.Settings, Plan, Error)) return {};
				const uint64 ExpectedWidth = static_cast<uint64>(Plan.Settings.SliceWidth) * Plan.Settings.TilesX;
				const uint64 ExpectedHeight = static_cast<uint64>(Plan.Settings.SliceHeight) * Plan.Settings.TilesY;
				if (Image.Source.Width != ExpectedWidth || Image.Source.Height != ExpectedHeight)
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.LayoutMismatch", "ProductBuild",
						std::format("Volume atlas is {}x{}; expected {}x{}.", Image.Source.Width,
							Image.Source.Height, ExpectedWidth, ExpectedHeight));
					return {};
				}
				const uint32 BytesPerVoxel = Plan.Settings.GetOutputFormat() == EVolumeTextureFormat::RGBA8_UNORM ? 4 : 1;
				std::vector<std::byte> Voxels;
				Voxels.reserve(static_cast<size_t>(Plan.Settings.SliceWidth) * Plan.Settings.SliceHeight
					* Plan.Settings.Depth * BytesPerVoxel);
				auto AppendPixel = [&](size_t Pixel) {
					if (Plan.Settings.Channels == EVolumeTextureSourceChannels::RGBA)
					{
						Voxels.insert(Voxels.end(), Image.Source.Pixels.begin() + Pixel,
							Image.Source.Pixels.begin() + Pixel + 4);
						return;
					}
					uint8 Value = 0;
					switch (Plan.Settings.Channels)
					{
					case EVolumeTextureSourceChannels::Red: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel]); break;
					case EVolumeTextureSourceChannels::Green: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel + 1]); break;
					case EVolumeTextureSourceChannels::Blue: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel + 2]); break;
					case EVolumeTextureSourceChannels::Alpha: Value = std::to_integer<uint8>(Image.Source.Pixels[Pixel + 3]); break;
					case EVolumeTextureSourceChannels::Luminance:
						Value = static_cast<uint8>((54 * std::to_integer<uint8>(Image.Source.Pixels[Pixel])
							+ 183 * std::to_integer<uint8>(Image.Source.Pixels[Pixel + 1])
							+ 19 * std::to_integer<uint8>(Image.Source.Pixels[Pixel + 2]) + 128) >> 8); break;
					case EVolumeTextureSourceChannels::RGBA: break;
					}
					Voxels.push_back(static_cast<std::byte>(Value));
				};
				for (uint32 Z = 0; Z < Plan.Settings.Depth; ++Z)
					for (uint32 Y = 0; Y < Plan.Settings.SliceHeight; ++Y)
						for (uint32 X = 0; X < Plan.Settings.SliceWidth; ++X)
						{
							const uint32 TileX = Z % Plan.Settings.TilesX;
							const uint32 TileY = Z / Plan.Settings.TilesX;
							const size_t Pixel = (static_cast<size_t>(TileY * Plan.Settings.SliceHeight + Y)
								* Image.Source.Width + TileX * Plan.Settings.SliceWidth + X) * 4;
							AppendPixel(Pixel);
						}
				FVolumeTextureSourceData Source{.Width = Plan.Settings.SliceWidth,
					.Height = Plan.Settings.SliceHeight, .Depth = Plan.Settings.Depth,
					.Format = Plan.Settings.GetOutputFormat()};
				if (!Source.SetVoxelBytes(Voxels)) return {};
				auto Result = std::make_unique<FVolumeProduct>();
				if (!Asset::Build::BuildVolumeTexture(std::move(Source),
					{.OutputFormat = Plan.Settings.GetOutputFormat()}, Result->Product, Error))
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.BuildFailed", "ProductBuild", Error);
					return {};
				}
				Result->Provenance = {.Source = {.SourcePath = Image.Path,
					.SourceContentHashLow = Image.Hash.HashLow, .SourceContentHashHigh = Image.Hash.HashHigh},
					.SourceFile = Image.Path.Path, .ImportFormat = Plan.Settings.ImportFormat,
					.Channels = Plan.Settings.Channels, .SliceWidth = Plan.Settings.SliceWidth,
					.SliceHeight = Plan.Settings.SliceHeight, .Depth = Plan.Settings.Depth,
					.TilesX = Plan.Settings.TilesX, .TilesY = Plan.Settings.TilesY,
					.DecoderId = std::string(VolumeTextureSourceProviderId),
					.DecoderVersion = VolumeTextureSourceProviderVersion};
				return Result;
			}
			auto MaterializeCandidate(const FImportFactoryNode& Node,
				std::unique_ptr<IInterchangeFactoryProduct> Product,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Typed = dynamic_cast<FVolumeProduct*>(Product.get());
				FAssetPath Path = Node.Destination;
				if (Node.Policy != EImportOutputPolicy::Create && !MakeCandidatePath(Node.Destination, Path)) return {};
				DVolumeTexture* AssetObject = nullptr;
				if (!Typed || !Asset::CreateAsset(Path, AssetObject)) return {};
				auto Result = std::make_unique<FCandidate>(AssetObject, Node.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::Build::PublishVolumeTextureProduct(*AssetObject,
					std::move(Typed->Product), Error)
					|| !AssetObject->PublishSourceImportData(std::move(Typed->Provenance), Error))
				{
					AddFailure(Diagnostics, "Durin.VolumeTexture.MaterializeFailed", "Materialization", Error);
					Result->Abandon();
					return {};
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* A = Cast<DVolumeTexture>(&Target);
				auto* B = Cast<DVolumeTexture>(Candidate.GetAsset());
				return A && B ? std::make_unique<TExchange<DVolumeTexture>>(*A, *B) : nullptr;
			}
			auto ApplyProvenance(DObject& Object, const FInterchangeProvenance& Provenance,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				return ApplyProvenanceBytes(Object, Provenance, Diagnostics);
			}
		};

		class FTerrainTranslator final : public IInterchangeTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				return IsTerrainHeightmapSourceExtension(Source.Extension);
			}
			auto DiscoverDependencies(std::span<const FSourceSnapshotEntry>, FDependencyRequestSink&,
				std::vector<FImportDiagnostic>&) const -> bool override { return true; }
			auto Translate(const FSourceSnapshot& Snapshot, const FInterchangePayload& Settings,
				FTranslatedAssetGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				std::string Error;
				if (!ValidatePayload(Settings, TerrainTranslatorSchema, Error)) return false;
				const auto* Root = Snapshot.FindSource("root");
				FDecodedTerrain Terrain;
				if (!Root || !TranslateTerrainHeightmapSource(
					std::filesystem::path(Root->SourcePath.Path).extension().generic_string(),
					Root->GetBytes(), Terrain.Source, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.DecodeFailed", "Translation", Error);
					return false;
				}
				Terrain.Path = Root->SourcePath;
				Terrain.Hash = Root->ContentHash;
				Terrain.FileSize = Root->ByteCount;
				Terrain.LastWriteTime = Root->LastWriteTime;
				return Builder.AddNode({.StableIdentity = "heightmap",
					.NodeKind = "Durin.TerrainHeightmap.Samples",
					.Payload = EncodeTerrain(Terrain), .SourceIdentities = {"root"}});
			}
		};

		class FTerrainPipeline final : public IInterchangePipeline
		{
		public:
			auto Execute(const FTranslatedAssetGraph& Graph, const FImportFactoryGraph*,
				const FInterchangePayload& Settings, FImportFactoryGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				FTerrainPlan Plan;
				std::string Error;
				if (!Graph.FindNode("heightmap") || !DecodeTerrainPlan(Settings, Plan, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.PlanInvalid", "Pipeline", Error);
					return false;
				}
				return Builder.AddNode({.StableIdentity = "terrain-heightmap",
					.FactoryId = std::string(TerrainFactoryId), .FactoryContractVersion = 1,
					.OutputClassName = "Durin::DTerrainHeightmap", .Destination = Plan.Destination,
					.Policy = Plan.Policy, .Settings = Settings,
					.TranslatedNodeReferences = {"heightmap"}});
			}
		};

		class FTerrainProduct final : public IInterchangeFactoryProduct
		{
		public:
			Asset::Build::FTerrainHeightmapBuildProduct Product;
			Asset::Build::FTerrainHeightmapPublicationContext Publication;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				return std::make_unique<FTerrainProduct>(*this);
			}
		};

		class FTerrainFactory final : public IInterchangeFactory
		{
		public:
			auto BuildDetachedProduct(const FImportFactoryNode& Node,
				const FTranslatedAssetGraph& Graph, IImportProgressReporter*,
				const std::function<bool()>& Canceled,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IInterchangeFactoryProduct> override
			{
				if (Canceled()) return {};
				const auto* SourceNode = Graph.FindNode("heightmap");
				FDecodedTerrain Terrain;
				FTerrainPlan Plan;
				std::string Error;
				if (!SourceNode || !DecodeTerrain(SourceNode->Payload, Terrain, Error)
					|| !DecodeTerrainPlan(Node.Settings, Plan, Error)) return {};
				auto Result = std::make_unique<FTerrainProduct>();
				if (!Asset::Build::BuildTerrainHeightmap({.Samples = std::move(Terrain.Source.Samples),
					.Width = Terrain.Source.Width, .Height = Terrain.Source.Height,
					.SourceContentHashLow = Terrain.Hash.HashLow,
					.SourceContentHashHigh = Terrain.Hash.HashHigh,
					.DecoderId = Terrain.Source.DecoderId,
					.DecoderVersion = Terrain.Source.DecoderVersion,
					.SourceFormat = Terrain.Source.SourceFormat,
					.SourceProfileVersion = Terrain.Source.SourceProfileVersion,
					.ShouldCancel = Canceled}, Result->Product, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.BuildFailed", "ProductBuild", Error);
					return {};
				}
				Result->Publication = {.SourcePath = std::move(Terrain.Path),
					.DecoderId = std::move(Terrain.Source.DecoderId),
					.DecoderVersion = Terrain.Source.DecoderVersion,
					.SourceFormat = Terrain.Source.SourceFormat,
					.SourceProfileVersion = Terrain.Source.SourceProfileVersion,
					.SourceFileSize = Terrain.FileSize,
					.SourceLastWriteTime = Terrain.LastWriteTime};
				return Result;
			}
			auto MaterializeCandidate(const FImportFactoryNode& Node,
				std::unique_ptr<IInterchangeFactoryProduct> Product,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Typed = dynamic_cast<FTerrainProduct*>(Product.get());
				FAssetPath Path = Node.Destination;
				if (Node.Policy != EImportOutputPolicy::Create && !MakeCandidatePath(Node.Destination, Path)) return {};
				DTerrainHeightmap* AssetObject = nullptr;
				if (!Typed || !Asset::CreateAsset(Path, AssetObject)) return {};
				auto Result = std::make_unique<FCandidate>(AssetObject, Node.Policy == EImportOutputPolicy::Create);
				std::string Error;
				if (!Asset::Build::PublishTerrainHeightmapProduct(*AssetObject,
					std::move(Typed->Product), Typed->Publication, Error))
				{
					AddFailure(Diagnostics, "Durin.TerrainHeightmap.MaterializeFailed", "Materialization", Error);
					Result->Abandon();
					return {};
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* A = Cast<DTerrainHeightmap>(&Target);
				auto* B = Cast<DTerrainHeightmap>(Candidate.GetAsset());
				if (A && B) A->PrepareCandidateRevision(*B);
				return A && B ? std::make_unique<TExchange<DTerrainHeightmap>>(*A, *B) : nullptr;
			}
			auto ApplyProvenance(DObject& Object, const FInterchangeProvenance& Provenance,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				return ApplyProvenanceBytes(Object, Provenance, Diagnostics);
			}
		};

		auto DecodeStoredProvenance(std::string_view Hex,
			FInterchangeProvenance& Out, std::string& OutError) -> bool
		{
			if (Hex.empty() || (Hex.size() & 1) != 0)
			{
				OutError = "Interchange provenance encoding is malformed.";
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
			return DeserializeInterchangeProvenance(Bytes, Out, OutError);
		}

		auto PolicyFor(EInterchangeImportMode Mode) -> EImportOutputPolicy
		{
			return Mode == EInterchangeImportMode::Import || Mode == EInterchangeImportMode::Preview
				? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		}
	}

	auto MakeTextureCubeInterchangeRequest(std::span<const FSourcePath> MountedSources,
		ETextureCubeSourceLayout Layout, const FAssetPath& Destination,
		const FTextureCubeImportSettings& FaceSettings,
		const FTextureCubePanoramaImportSettings& PanoramaSettings,
		EInterchangeImportMode Mode, FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest, std::string& OutError) -> bool
	{
		const size_t Required = Layout == ETextureCubeSourceLayout::SixFaces ? TextureCubeFaceCount : 1;
		if (MountedSources.size() != Required || Destination.ToString().empty()
			|| std::ranges::any_of(MountedSources, [](const FSourcePath& Source) { return Source.IsEmpty(); }))
		{
			OutError = "TextureCube Interchange source set is invalid.";
			return false;
		}
		if (Owner.OwnerId.empty()) Owner.OwnerId = "TextureCube.Interchange";
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(Destination.ToString());
		const FCubePlan Plan{.Destination = Destination, .Policy = PolicyFor(Mode), .Layout = Layout,
			.FaceSettings = FaceSettings, .PanoramaSettings = PanoramaSettings};
		OutRequest = {.Mode = Mode, .RootSource = MountedSources.front(),
			.TranslatorId = std::string(CubeTranslatorId),
			.TranslatorSettings = EncodeCubeTranslatorSettings(Layout),
			.PipelineStack = {{.PipelineId = std::string(CubePipelineId),
				.ContractVersion = 1, .Settings = EncodeCubePlan(Plan)}},
			.Destination = Destination, .Owner = std::move(Owner),
			.ExistingProvenance = std::move(ExistingProvenance)};
		for (size_t Index = 1; Index < MountedSources.size(); ++Index)
			OutRequest.DeclaredSources.push_back({.StableIdentity = std::format("face:{}", Index),
				.Role = std::format("CubeFace{}", Index), .SourcePath = MountedSources[Index]});
		OutError.clear();
		return true;
	}

	auto MakeVolumeTextureInterchangeRequest(const FSourcePath& MountedSource,
		const FAssetPath& Destination, const FVolumeTextureImportSettings& Settings,
		EInterchangeImportMode Mode, FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest, std::string& OutError) -> bool
	{
		if (MountedSource.IsEmpty() || Destination.ToString().empty() || !Settings.IsValid(&OutError)) return false;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "VolumeTexture.Interchange";
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(Destination.ToString());
		OutRequest = {.Mode = Mode, .RootSource = MountedSource,
			.TranslatorId = std::string(ImageTranslatorId),
			.TranslatorSettings = EmptyPayload(EmptyImageSettingsSchema),
			.PipelineStack = {{.PipelineId = std::string(VolumePipelineId), .ContractVersion = 1,
				.Settings = EncodeVolumePlan({.Destination = Destination, .Policy = PolicyFor(Mode),
					.Settings = Settings})}}, .Destination = Destination, .Owner = std::move(Owner),
			.ExistingProvenance = std::move(ExistingProvenance)};
		OutError.clear();
		return true;
	}

	auto MakeTerrainHeightmapInterchangeRequest(const FSourcePath& MountedSource,
		const FAssetPath& Destination, EInterchangeImportMode Mode, FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest, std::string& OutError) -> bool
	{
		if (MountedSource.IsEmpty() || Destination.ToString().empty())
		{
			OutError = "Terrain heightmap Interchange request is invalid.";
			return false;
		}
		if (Owner.OwnerId.empty()) Owner.OwnerId = "TerrainHeightmap.Interchange";
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(Destination.ToString());
		OutRequest = {.Mode = Mode, .RootSource = MountedSource,
			.TranslatorId = std::string(TerrainTranslatorId),
			.TranslatorSettings = EmptyPayload(TerrainTranslatorSchema),
			.PipelineStack = {{.PipelineId = std::string(TerrainPipelineId), .ContractVersion = 1,
				.Settings = EncodeTerrainPlan({.Destination = Destination, .Policy = PolicyFor(Mode)})}},
			.Destination = Destination, .Owner = std::move(Owner),
			.ExistingProvenance = std::move(ExistingProvenance)};
		OutError.clear();
		return true;
	}

	auto InspectTextureCubeInterchangeProvenance(const DTextureCube& Texture,
		FInterchangeProvenance& Out, std::string& OutError) -> bool
	{
		if (!Texture.GetInterchangeProvenance().empty())
			return DecodeStoredProvenance(Texture.GetInterchangeProvenance(), Out, OutError);
		OutError = "TextureCube has no persisted Interchange provenance; reimport requires explicit repair.";
		return false;
	}

	auto InspectVolumeTextureInterchangeProvenance(const DVolumeTexture& Texture,
		FInterchangeProvenance& Out, std::string& OutError) -> bool
	{
		if (!Texture.GetInterchangeProvenance().empty())
			return DecodeStoredProvenance(Texture.GetInterchangeProvenance(), Out, OutError);
		OutError = "VolumeTexture has no persisted Interchange provenance; reimport requires explicit repair.";
		return false;
	}

	auto InspectTerrainHeightmapInterchangeProvenance(const DTerrainHeightmap& Heightmap,
		FInterchangeProvenance& Out, std::string& OutError) -> bool
	{
		if (!Heightmap.GetInterchangeProvenance().empty())
			return DecodeStoredProvenance(Heightmap.GetInterchangeProvenance(), Out, OutError);
		OutError = "Terrain heightmap has no persisted Interchange provenance; reimport requires explicit repair.";
		return false;
	}

	auto RegisterImageFamilyInterchange(FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FInterchangeRegistration>& OutRegistrations,
		std::string& OutError) -> bool
	{
		auto Add = [&](FInterchangeRegistration Registration) {
			if (!Registration) return false;
			OutRegistrations.push_back(std::move(Registration));
			return true;
		};
		if (!Add(Service.RegisterTranslatorScoped({.Descriptor = {
			.Identity = {.Id = std::string(CubeTranslatorId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(CubeTranslatorSchema), .SchemaVersion = 1}},
			.Extensions = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr"}, .Priority = 120,
			.TranslationThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FCubeTranslator>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterPipelineScoped({.Descriptor = {
			.Identity = {.Id = std::string(CubePipelineId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(CubePlanSchema), .SchemaVersion = 1}},
			.Priority = 100, .ExecutionThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FCubePipeline>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterFactoryScoped({.Descriptor = {
			.Identity = {.Id = std::string(CubeFactoryId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(CubePlanSchema), .SchemaVersion = 1}},
			.OutputClassName = "Durin::DTextureCube", .Priority = 100,
			.ProductBuildThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FCubeFactory>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterPipelineScoped({.Descriptor = {
			.Identity = {.Id = std::string(VolumePipelineId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(VolumePlanSchema), .SchemaVersion = 1}},
			.Priority = 100, .ExecutionThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FVolumePipeline>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterFactoryScoped({.Descriptor = {
			.Identity = {.Id = std::string(VolumeFactoryId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(VolumePlanSchema), .SchemaVersion = 1}},
			.OutputClassName = "Durin::DVolumeTexture", .Priority = 100,
			.ProductBuildThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FVolumeFactory>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterTranslatorScoped({.Descriptor = {
			.Identity = {.Id = std::string(TerrainTranslatorId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(TerrainTranslatorSchema), .SchemaVersion = 1}},
			.Extensions = {".png", ".raw", ".r16"}, .Priority = 120,
			.TranslationThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTerrainTranslator>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterPipelineScoped({.Descriptor = {
			.Identity = {.Id = std::string(TerrainPipelineId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(TerrainPlanSchema), .SchemaVersion = 1}},
			.Priority = 100, .ExecutionThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTerrainPipeline>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterFactoryScoped({.Descriptor = {
			.Identity = {.Id = std::string(TerrainFactoryId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(TerrainPlanSchema), .SchemaVersion = 1}},
			.OutputClassName = "Durin::DTerrainHeightmap", .Priority = 100,
			.ProductBuildThread = EInterchangeThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FTerrainFactory>()}, OwnerGate, OutError))) return false;
		OutError.clear();
		return true;
	}
}
