#include "VolumeTextureSourceTranslation.h"

#include "AssetAuthoring.h"
#include "Image/ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "ImportService.h"

namespace Durin::Asset::Forge
{
	namespace
	{
		auto Lowercase(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Value;
		}

		auto MakeSourceFile(const FVolumeTextureCapturedSource& Source) -> FTextureSourceFile
		{
			return {.SourcePath = Source.SourcePath,
				.SourceContentHashLow = Source.ContentHash.HashLow,
				.SourceContentHashHigh = Source.ContentHash.HashHigh};
		}

		auto AppendPixel(const Image::FDecodedImage& Image, size_t Pixel,
			EVolumeTextureSourceChannels Channels, std::vector<std::byte>& OutVoxels) -> void
		{
			if (Channels == EVolumeTextureSourceChannels::RGBA)
			{
				for (size_t Channel = 0; Channel < 4; ++Channel)
					OutVoxels.push_back(static_cast<std::byte>(Image.Pixels[Pixel + Channel]));
				return;
			}
			uint8 Value = 0;
			switch (Channels)
			{
			case EVolumeTextureSourceChannels::Red: Value = std::to_integer<uint8>(Image.Pixels[Pixel]); break;
			case EVolumeTextureSourceChannels::Green: Value = std::to_integer<uint8>(Image.Pixels[Pixel + 1]); break;
			case EVolumeTextureSourceChannels::Blue: Value = std::to_integer<uint8>(Image.Pixels[Pixel + 2]); break;
			case EVolumeTextureSourceChannels::Alpha: Value = std::to_integer<uint8>(Image.Pixels[Pixel + 3]); break;
			case EVolumeTextureSourceChannels::Luminance:
				Value = static_cast<uint8>((54u * std::to_integer<uint8>(Image.Pixels[Pixel])
					+ 183u * std::to_integer<uint8>(Image.Pixels[Pixel + 1])
					+ 19u * std::to_integer<uint8>(Image.Pixels[Pixel + 2]) + 128u) >> 8u);
				break;
			case EVolumeTextureSourceChannels::RGBA: break;
			}
			OutVoxels.push_back(static_cast<std::byte>(Value));
		}

		auto FindOwningMount(std::string_view VirtualPath) -> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeDefaultSourceDestination(const FAssetPath& AssetPath,
			std::string_view FileName, std::string& OutPath, std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = "Volume texture asset is not beneath a registered package mount.";
				return false;
			}
			OutPath = Mount->VirtualRoot + "Sources/VolumeTextures/"
				+ std::string(FileName);
			return true;
		}

		auto IsPowerOfTwo(uint32 Value) -> bool
		{
			return Value != 0 && (Value & (Value - 1)) == 0;
		}

		auto SuggestAtlasChannels(const Image::FDecodedImage& Image)
			-> EVolumeTextureSourceChannels
		{
			if (Image.SourceChannelCount <= 2)
				return EVolumeTextureSourceChannels::Red;

			bool bRgbEqual = true;
			bool bRgbWhite = true;
			bool bAlphaVaries = false;
			const size_t PixelCount = Image.Pixels.size() / 4;
			const size_t SampleCount = std::min<size_t>(PixelCount, 65536);
			for (size_t Sample = 0; Sample < SampleCount; ++Sample)
			{
				const size_t PixelIndex = SampleCount == PixelCount
					? Sample : Sample * PixelCount / SampleCount;
				const std::byte* Pixel = Image.Pixels.data() + PixelIndex * 4;
				bRgbEqual &= Pixel[0] == Pixel[1] && Pixel[1] == Pixel[2];
				bRgbWhite &= Pixel[0] == std::byte{255} && Pixel[1] == std::byte{255} && Pixel[2] == std::byte{255};
				bAlphaVaries |= Pixel[3] != std::byte{255};
			}
			if (Image.SourceChannelCount == 4 && bAlphaVaries && bRgbWhite)
				return EVolumeTextureSourceChannels::Alpha;
			return bRgbEqual
				? EVolumeTextureSourceChannels::Red
				: EVolumeTextureSourceChannels::RGBA;
		}

		auto SuggestCubicAtlasLayouts(const Image::FDecodedImage& Image,
			EVolumeTextureSourceChannels Channels)
			-> std::vector<FVolumeTextureImportSettings>
		{
			struct FCandidate
			{
				FVolumeTextureImportSettings Settings;
				double Utilization = 0.0;
				bool bPowerOfTwo = false;
				double Score = 0.0;
			};
			std::vector<FCandidate> Candidates;
			const uint32 MaximumSlice = std::min({Image.Width, Image.Height,
				MaximumVolumeTextureDimension});
			for (uint32 Slice = 1; Slice <= MaximumSlice; ++Slice)
			{
				if (Image.Width % Slice != 0 || Image.Height % Slice != 0) continue;
				const uint32 TilesX = Image.Width / Slice;
				const uint32 TilesY = Image.Height / Slice;
				const uint64 CellCount = static_cast<uint64>(TilesX) * TilesY;
				if (CellCount < Slice) continue;
				FVolumeTextureImportSettings Settings{
					.Channels = Channels,
					.SliceWidth = Slice,
					.SliceHeight = Slice,
					.Depth = Slice,
					.TilesX = TilesX,
					.TilesY = TilesY};
				if (!Settings.IsValid()) continue;
				const double Utilization =
					static_cast<double>(Slice) / static_cast<double>(CellCount);
				const bool bPowerOfTwo = IsPowerOfTwo(Slice);
				Candidates.push_back({Settings, Utilization, bPowerOfTwo,
					Utilization + (bPowerOfTwo ? 0.15 : 0.0)});
			}
			std::ranges::sort(Candidates, [](const FCandidate& A, const FCandidate& B) {
				if (A.Score != B.Score) return A.Score > B.Score;
				return A.Settings.SliceWidth > B.Settings.SliceWidth;
			});
			std::vector<FVolumeTextureImportSettings> Result;
			for (const FCandidate& Candidate : Candidates | std::views::take(3))
				Result.push_back(Candidate.Settings);
			return Result;
		}

		template<typename TMountedSource>
		auto ReadCaptured(const TMountedSource& Source,
			std::vector<std::byte>& OutBytes, FVolumeTextureCapturedSource& Out,
			std::string& OutError) -> bool
		{
			if (!FFileHelper::LoadFileToArray(OutBytes, Source.PhysicalPath))
			{
				OutError = std::format("Failed to read volume texture source '{}'.",
					Source.PhysicalPath.generic_string());
				return false;
			}
			Out = {.SourcePath = Source.SourcePath,
				.ContentHash = FXxHash128::HashBuffer(OutBytes), .Bytes = OutBytes};
			return true;
		}

		auto MakeImportSettings(const FVolumeTextureSourceImportData& Source)
			-> FVolumeTextureImportSettings
		{
			return {.ImportFormat = Source.ImportFormat, .Channels = Source.Channels,
				.SliceWidth = Source.SliceWidth, .SliceHeight = Source.SliceHeight,
				.Depth = Source.Depth, .TilesX = Source.TilesX, .TilesY = Source.TilesY};
		}
	}

	auto FVolumeTextureImportSettings::IsValid(std::string* OutError) const -> bool
	{
		auto Fail = [&](std::string Message) {
			if (OutError) *OutError = std::move(Message);
			return false;
		};
		if (ImportFormat != EVolumeTextureImportFormat::PngRowMajorAtlas)
			return Fail("Only PNG row-major atlas volume import is supported.");
		if (SliceWidth == 0 || SliceHeight == 0 || Depth == 0
			|| TilesX == 0 || TilesY == 0
			|| SliceWidth > MaximumVolumeTextureDimension
			|| SliceHeight > MaximumVolumeTextureDimension
			|| Depth > MaximumVolumeTextureDimension)
			return Fail(std::format("Slice dimensions and depth must be between 1 and {}.",
				MaximumVolumeTextureDimension));
		const uint64 CellCount = static_cast<uint64>(TilesX) * TilesY;
		if (CellCount < Depth)
			return Fail("Atlas tiles do not provide enough row-major cells for depth.");
		const uint64 AtlasWidth = static_cast<uint64>(SliceWidth) * TilesX;
		const uint64 AtlasHeight = static_cast<uint64>(SliceHeight) * TilesY;
		if (AtlasWidth > std::numeric_limits<uint32>::max()
			|| AtlasHeight > std::numeric_limits<uint32>::max()
			|| AtlasWidth > std::numeric_limits<uint64>::max() / AtlasHeight
			|| AtlasWidth * AtlasHeight > MaximumTexturePayloadBytes / 4u)
			return Fail("Decoded RGBA atlas exceeds the 2 GiB image budget.");
		const uint64 BytesPerVoxel = GetOutputFormat() == EVolumeTextureFormat::RGBA8_UNORM ? 4u : 1u;
		const uint64 VoxelCount = static_cast<uint64>(SliceWidth) * SliceHeight * Depth;
		if (VoxelCount > MaximumTexturePayloadBytes / BytesPerVoxel)
			return Fail("Decoded volume texture exceeds the 2 GiB source payload limit.");
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectVolumeTextureAtlasSource(
		std::string_view FilePath) -> FVolumeTextureAtlasInspection
	{
		Image::FDecodedImage Image;
		std::string Error;
		if (!Image::DecodeImageFromFile(FilePath, Image, Error))
			return {.Message = std::format(
				"Failed to inspect the volume atlas: {}", Error)};

		FVolumeTextureAtlasInspection Result;
		Result.bSucceeded = true;
		Result.AtlasWidth = Image.Width;
		Result.AtlasHeight = Image.Height;
		Result.SourceChannelCount = Image.SourceChannelCount;
		Result.SuggestedChannels = SuggestAtlasChannels(Image);
		Result.SuggestedLayouts = SuggestCubicAtlasLayouts(
			Image, Result.SuggestedChannels);
		if (Result.SuggestedLayouts.empty())
		{
			Result.Message = "No cubic atlas layout could be inferred from the PNG dimensions.";
			return Result;
		}

		const FVolumeTextureImportSettings& Best = Result.SuggestedLayouts.front();
		const double BestUtilization = static_cast<double>(Best.Depth)
			/ (static_cast<double>(Best.TilesX) * Best.TilesY);
		bool bClearlyBetter = Result.SuggestedLayouts.size() == 1;
		if (!bClearlyBetter)
		{
			const FVolumeTextureImportSettings& Second = Result.SuggestedLayouts[1];
			const double SecondUtilization = static_cast<double>(Second.Depth)
				/ (static_cast<double>(Second.TilesX) * Second.TilesY);
			bClearlyBetter = BestUtilization - SecondUtilization >= 0.2;
		}
		Result.bHasConfidentLayout = IsPowerOfTwo(Best.SliceWidth)
			&& BestUtilization >= 0.75 && bClearlyBetter;
		Result.Message = Result.bHasConfidentLayout
			? "A high-confidence cubic layout was inferred from the PNG dimensions."
			: "Several cubic layouts fit the PNG dimensions; review the suggested layouts.";
		return Result;
	}

	auto TranslateVolumeTextureAtlasSource(const FVolumeTextureCapturedSource& Source,
		const FVolumeTextureImportSettings& Settings,
		FVolumeTextureSourceData& OutSourceData, std::string& OutError) -> bool
	{
		OutSourceData = {};
		if (!Settings.IsValid(&OutError)) return false;
		constexpr std::array<std::byte, 8> PngSignature = {
			std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
			std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
		if (Source.Bytes.size() < PngSignature.size()
			|| !std::ranges::equal(PngSignature, Source.Bytes.first(PngSignature.size())))
		{
			OutError = "Volume texture atlas is not PNG data.";
			return false;
		}
		const uint64 ExpectedWidth = static_cast<uint64>(Settings.SliceWidth) * Settings.TilesX;
		const uint64 ExpectedHeight = static_cast<uint64>(Settings.SliceHeight) * Settings.TilesY;
		Image::FDecodedImage Image;
		if (!Image::DecodeImageFromMemory(Source.Bytes, Image, OutError,
			{.MaximumDecodedPixels = ExpectedWidth * ExpectedHeight}))
		{
			OutError = std::format("Failed to decode volume atlas '{}': {}",
				Source.SourcePath.Path, OutError);
			return false;
		}
		if (Image.Width != ExpectedWidth || Image.Height != ExpectedHeight)
		{
			OutError = std::format("Volume atlas is {}x{}; expected {}x{} from slice size and tiles.",
				Image.Width, Image.Height, ExpectedWidth, ExpectedHeight);
			return false;
		}
		const uint32 BytesPerVoxel = Settings.GetOutputFormat()
			== EVolumeTextureFormat::RGBA8_UNORM ? 4u : 1u;
		const uint64 TotalBytes = static_cast<uint64>(Settings.SliceWidth)
			* Settings.SliceHeight * Settings.Depth * BytesPerVoxel;
		std::vector<std::byte> Voxels;
		Voxels.reserve(static_cast<size_t>(TotalBytes));
		for (uint32 Z = 0; Z < Settings.Depth; ++Z)
		{
			const uint32 TileX = Z % Settings.TilesX;
			const uint32 TileY = Z / Settings.TilesX;
			for (uint32 Y = 0; Y < Settings.SliceHeight; ++Y)
				for (uint32 X = 0; X < Settings.SliceWidth; ++X)
				{
					const size_t Pixel = (static_cast<size_t>(TileY * Settings.SliceHeight + Y)
						* Image.Width + TileX * Settings.SliceWidth + X) * 4;
					AppendPixel(Image, Pixel, Settings.Channels, Voxels);
				}
		}
		FVolumeTextureSourceData Candidate{
			.Width = Settings.SliceWidth, .Height = Settings.SliceHeight,
			.Depth = Settings.Depth, .Format = Settings.GetOutputFormat()};
		if (!Candidate.SetVoxelBytes(Voxels))
		{
			OutError = "Volume source payload could not be published as authored bulk data.";
			return false;
		}
		if (!Candidate.IsValid())
		{
			OutError = "Decoded volume texture source failed normalized layout validation.";
			return false;
		}
		OutSourceData = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto BuildVolumeTextureCandidate(DVolumeTexture& Texture,
		const FVolumeTextureCapturedSource& Source,
		const FVolumeTextureImportSettings& Settings, std::string& OutError) -> bool
	{
		FVolumeTextureSourceData SourceData;
		if (!TranslateVolumeTextureAtlasSource(Source, Settings, SourceData, OutError)) return false;
		const FVolumeTextureBuildSettings BuildSettings{.OutputFormat = SourceData.Format};
		Asset::Build::FVolumeTextureBuildProduct Product;
		if (!Asset::Build::BuildVolumeTexture(std::move(SourceData), BuildSettings, Product, OutError)
			|| !Asset::Build::PublishVolumeTextureProduct(Texture, std::move(Product), OutError))
			return false;
		FVolumeTextureSourceImportData Provenance{
			.Source = MakeSourceFile(Source), .SourceFile = Source.SourcePath.Path,
			.ImportFormat = Settings.ImportFormat, .Channels = Settings.Channels,
			.SliceWidth = Settings.SliceWidth, .SliceHeight = Settings.SliceHeight,
			.Depth = Settings.Depth, .TilesX = Settings.TilesX, .TilesY = Settings.TilesY,
			.DecoderId = std::string(VolumeTextureSourceProviderId),
			.DecoderVersion = VolumeTextureSourceProviderVersion};
		return Texture.PublishSourceImportData(std::move(Provenance), OutError);
	}

	auto ImportVolumeTextureAsset(std::string_view FilePath,
		std::string_view AssetPath, const FVolumeTextureImportSettings& Settings,
		bool bEngineAuthoringContext,
		const FVolumeTextureAuthoringOptions& AuthoringOptions)
		-> FVolumeTextureImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return {false, "Volume texture source image does not exist.", nullptr};
		if (Lowercase(Input.extension().generic_string()) != ".png")
			return {false, "Volume texture atlas source must use PNG encoding.", nullptr};
		std::string Error;
		if (!Settings.IsValid(&Error)) return {false, std::move(Error), nullptr};
		FAssetPath ParsedAssetPath;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedAssetPath) || Asset::FindResidentPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};
		std::string SourceDestination = Settings.SourceDestination;
		if (SourceDestination.empty()
			&& !MakeDefaultSourceDestination(ParsedAssetPath,
				Input.filename().generic_string(), SourceDestination, Error))
			return {false, std::move(Error), nullptr};
		const Asset::EMountedSourceMutationContext MutationContext = bEngineAuthoringContext
			? Asset::EMountedSourceMutationContext::EngineAuthoring
			: Asset::EMountedSourceMutationContext::DependencySafe;
		Asset::FScopedMountedSourceFile MountedSource;
		if (!Asset::PrepareMountedSourceFile(Input, ParsedAssetPath.ToString(),
			SourceDestination, MountedSource, Error, MutationContext))
			return {false, std::move(Error), nullptr};
		FInterchangeImportRequest Request;
		if (!MakeVolumeTextureInterchangeRequest(MountedSource.SourcePath, ParsedAssetPath,
			Settings, EInterchangeImportMode::Import,
			{.OwnerId = std::format("VolumeTexture.Import:{}", ParsedAssetPath.ToString())},
			{}, Request, Error, AuthoringOptions)) return {false, std::move(Error), nullptr};
		const FInterchangeImportResult Imported = GetImportService().RunInterchangeImportInline(
			std::move(Request), std::format("Import VolumeTexture {}", ParsedAssetPath.GetAssetName()));
		if (Imported.Outcome.State != EImportOperationState::Succeeded)
			return {false, Imported.Outcome.Diagnostic, nullptr};
		DObject* Object = nullptr;
		(void)Asset::LoadAsset(ParsedAssetPath, Object);
		auto* Texture = Cast<DVolumeTexture>(Object);
		if (!Texture) return {false, "VolumeTexture Interchange published no asset.", nullptr};
		MountedSource.Commit();
		return {true, {}, Texture};
	}

	auto SubmitVolumeTextureInterchangeImport(std::string_view FilePath,
		const FAssetPath& Destination, const FVolumeTextureImportSettings& Settings,
		bool bEngineAuthoringContext, FInterchangeImportCompletion Completion,
		std::string& OutError,
		const FVolumeTextureAuthoringOptions& AuthoringOptions)
		-> FInterchangeImportHandle
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)
			|| Lowercase(Input.extension().generic_string()) != ".png"
			|| !Settings.IsValid(&OutError))
		{
			if (OutError.empty()) OutError = "VolumeTexture source is unavailable or invalid.";
			return {};
		}
		std::string SourceDestination = Settings.SourceDestination;
		if (SourceDestination.empty() && !MakeDefaultSourceDestination(Destination,
			Input.filename().generic_string(), SourceDestination, OutError)) return {};
		auto Mounted = std::make_shared<FScopedMountedSourceFile>();
		if (!PrepareMountedSourceFile(Input, Destination.ToString(), SourceDestination,
			*Mounted, OutError, bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe)) return {};
		FInterchangeImportRequest Request;
		if (!MakeVolumeTextureInterchangeRequest(Mounted->SourcePath, Destination, Settings,
			EInterchangeImportMode::Import,
			{.OwnerId = std::format("VolumeTexture.Import:{}", Destination.ToString()),
				.ConflictIdentities = {Destination.ToString()}}, {}, Request, OutError,
			AuthoringOptions)) return {};
		OutError.clear();
		return GetImportService().SubmitInterchangeImport(std::move(Request),
			std::format("Import VolumeTexture {}", Destination.GetAssetName()),
			[Mounted, Completion = std::move(Completion)](const FInterchangeImportResult& Result) {
				if (Result.Outcome.State == EImportOperationState::Succeeded) Mounted->Commit();
				if (Completion) Completion(Result);
			});
	}

	auto RepairVolumeTextureSource(DVolumeTexture& Texture,
		std::string_view SourcePath, std::string& OutError,
		const FVolumeTextureAuthoringOptions& AuthoringOptions) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged volume textures can retain source provenance.";
			return false;
		}
		Asset::FMountedSourceResolution MountedSource;
		if (!Asset::ResolveMountedSourceReference(Texture.GetPackage()->GetPackagePath(),
			SourcePath, Asset::EMountedSourceExistencePolicy::RequireFile,
			MountedSource, OutError)) return false;
		FAssetPath Destination;
		if (!FAssetPath::TryCreate(Texture.GetPackage()->GetPackagePath(), Destination, &OutError))
			return false;
		std::optional<FInterchangeProvenance> Existing;
		FInterchangeProvenance Persisted;
		std::string ProvenanceError;
		if (InspectVolumeTextureInterchangeProvenance(Texture, Persisted, ProvenanceError))
			Existing = std::move(Persisted);
		FInterchangeImportRequest Request;
		if (!MakeVolumeTextureInterchangeRequest(MountedSource.SourcePath, Destination,
			MakeImportSettings(Texture.GetSourceImportData()), EInterchangeImportMode::Repair,
			{.OwnerId = std::format("VolumeTexture.Repair:{}", Destination.ToString())},
			std::move(Existing), Request, OutError, AuthoringOptions)) return false;
		const FInterchangeImportResult Result = GetImportService().RunInterchangeImportInline(
			std::move(Request), std::format("Repair VolumeTexture {}", Destination.GetAssetName()));
		if (Result.Outcome.State == EImportOperationState::Succeeded) return true;
		OutError = Result.Outcome.Diagnostic;
		return false;
	}
}
