#include "AssetForge/Builtins/VolumeTextureImport.h"
#include "AssetForge/Builtins/VolumeTextureImportData.h"
#include "AssetForge/Builtins/VolumeTextureFactory.h"

#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "DObject/DObjectGlobals.h"
#include "EncodedSourceSnapshot.h"
#include "Image/ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/StringHelper.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureBuildProvider.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	namespace
	{
		auto ResolveOwningPackagePhysicalPath(const DVolumeTexture& Texture,
			std::filesystem::path& OutPath, std::string& OutError) -> bool
		{
			if (!Texture.GetPackage())
			{
				OutError = "VolumeTexture source capture requires an owning package.";
				return false;
			}
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(Texture.GetPackage()->GetPackagePath(),
					EMountPathExistence::AllowMissing);
			if (!Resolved) { OutError = Resolved.Message; return false; }
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return true;
		}

		auto AppendPixel(const Image::FDecodedImage& Image, size_t Pixel,
			EVolumeTextureSourceChannels Channels, FByteArray& OutVoxels) -> void
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

		auto CaptureVolumeSource(std::string Filename,
			const std::filesystem::path& PhysicalPath,
			FEncodedSourceSnapshot& OutSnapshot,
			FVolumeTextureCapturedSource& Out,
			std::string& OutError) -> bool
		{
			if (!CaptureEncodedSource(Filename, PhysicalPath,
				OutSnapshot, OutError, MaximumTexturePayloadBytes)) return false;
			Out = {.Filename = std::move(Filename),
				.ContentHash = OutSnapshot.ContentHash,
				.Bytes = OutSnapshot.GetBytes()};
			return true;
		}

		auto MakeImportSettings(const FVolumeTextureImportDataState& State)
			-> FVolumeTextureImportSettings
		{
			return {.ImportFormat = EVolumeTextureImportFormat::PngRowMajorAtlas,
				.Channels = State.Channels,
				.SliceWidth = State.SliceWidth, .SliceHeight = State.SliceHeight,
				.Depth = State.Depth, .TilesX = State.TilesX, .TilesY = State.TilesY};
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
				Source.Filename, OutError);
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
		FByteArray Voxels;
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

	namespace
	{
		auto PublishDirectVolumeImportData(DVolumeTexture& Texture,
			std::string Filename, ESourceHintBase HintBase,
			const std::filesystem::path& PhysicalPath,
			const FEncodedSourceSnapshot& Snapshot,
			const FVolumeTextureImportSettings& Settings,
			std::string& OutError) -> bool
		{
			FVolumeTextureImportDataState State;
			State.SourceData.Sources.push_back({
				.Role = "source",
				.DisplayLabel = PhysicalPath.filename().generic_string(),
				.Hint = std::move(Filename),
				.HintBase = HintBase,
				.ContentHashLow = Snapshot.ContentHash.HashLow,
				.ContentHashHigh = Snapshot.ContentHash.HashHigh,
				.ByteCount = Snapshot.FileSize});
			State.Channels = Settings.Channels;
			State.SliceWidth = Settings.SliceWidth;
			State.SliceHeight = Settings.SliceHeight;
			State.Depth = Settings.Depth;
			State.TilesX = Settings.TilesX;
			State.TilesY = Settings.TilesY;
			auto* Data = dynamic_cast<DVolumeTextureImportData*>(
				Texture.GetAssetImportData());
			if (!Data) Data = NewObject<DVolumeTextureImportData>(
				&Texture, "AssetImportData");
			if (!Data || !Data->SetState(std::move(State), OutError)
				|| !Texture.SetAssetImportData(*Data, OutError)) return false;
			Texture.MarkPackageDirty();
			return true;
		}

		auto RebuildVolumeFromFilename(DVolumeTexture& Texture,
			std::string Filename, ESourceHintBase HintBase,
			const FVolumeTextureImportSettings& Settings,
			std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> bool
		{
			std::filesystem::path OwningPackagePath;
			if (!ResolveOwningPackagePhysicalPath(Texture, OwningPackagePath, OutError))
				return false;
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath) PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				if (!ResolveSourceHint(HintBase, Filename,
					OwningPackagePath.generic_string(), PhysicalPathText, OutError)) return false;
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath)
				|| StringUtils::FoldAscii(PhysicalPath.extension().generic_string()) != ".png")
			{
				OutError = "VolumeTexture source must be an existing PNG file.";
				return false;
			}
			if (SelectedPhysicalPath && !MakeSourceHint(
				PhysicalPath.generic_string(), OwningPackagePath.generic_string(),
				HintBase, Filename, OutError)) return false;
			FEncodedSourceSnapshot Snapshot;
			FVolumeTextureCapturedSource Captured;
			if (!CaptureVolumeSource(Filename, PhysicalPath, Snapshot, Captured, OutError))
				return false;
			FVolumeTextureSourceData SourceData;
			if (!TranslateVolumeTextureAtlasSource(
				Captured, Settings, SourceData, OutError)) return false;
			if (!BuildVolumeTextureSynchronously(Texture, {
				.SourceData = SourceData,
				.Settings = {.OutputFormat = Settings.GetOutputFormat()}}, {}, OutError)
				|| !PublishDirectVolumeImportData(Texture, std::move(Filename), HintBase,
					PhysicalPath,
					Snapshot, Settings, OutError)) return false;
			if (!SaveOptions) return true;
			DPackage* Package = Texture.GetPackage();
			const FAssetResult Saved = SavePackagesAtomically(
				std::span<DPackage* const>(&Package, 1), *SaveOptions);
			if (Saved) return true;
			OutError = Saved.Message;
			return false;
		}
	}

	DVolumeTextureFactory::DVolumeTextureFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DVolumeTexture::StaticClass();
		Formats = {"png"};
	}

	auto DVolumeTextureFactory::FactoryCreateFromFile(
		DClass* InClass,
		DObject* InParent,
		FName InName,
		EObjectFlags Flags,
		std::string_view Filename,
		DObject*,
		FFactoryDiagnostics* Diagnostics) const -> DObject*
	{
		auto Failed = [&](std::string Message) -> DObject* {
			if (Diagnostics) Diagnostics->Report(Message);
			return nullptr;
		};
		if (InClass != DVolumeTexture::StaticClass())
			return Failed("Volume texture factory requires the exact VolumeTexture class.");
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Failed("Volume texture factory requires an asset package parent.");
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Failed("Volume texture source image does not exist.");
		if (StringUtils::FoldAscii(Input.extension().generic_string()) != ".png")
			return Failed("Volume texture atlas source must use PNG encoding.");
		std::string Error;
		if (!Settings.IsValid(&Error)) return Failed(std::move(Error));
		auto* Texture = NewObject<DVolumeTexture>(
			InClass, Package, InName, Flags);
		if (!Texture)
			return Failed("Volume texture object could not be created.");
		if (!RebuildVolumeFromFilename(
			*Texture, Input.generic_string(), ESourceHintBase::AssetRelative,
			Settings, Error, nullptr, Input)) return Failed(std::move(Error));
		return Texture;
	}

	auto DVolumeTextureFactory::GetReimportCapabilities(
		const DObject& Object) const -> FReimportCapabilities
	{
		const auto* Texture = Cast<DVolumeTexture>(&Object);
		const auto* Data = Texture ? dynamic_cast<const DVolumeTextureImportData*>(
			Texture->GetAssetImportData()) : nullptr;
		if (!Texture || !Texture->GetPackage() || !Data)
			return {.Diagnostic = "VolumeTexture has no current family import data."};
		const FVolumeTextureImportDataState State = Data->GetVolumeTextureState();
		const FSourceFile* Source = State.SourceData.FindByRole("source");
		const bool bHasSource = Source && !Source->Hint.empty();
		return {.bCanReimport = bHasSource, .bCanReimportFromFile = true,
			.Diagnostic = bHasSource ? std::string{}
				: "VolumeTexture has no source hint to reimport."};
	}

	auto DVolumeTextureFactory::Reimport(
		DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Texture = Cast<DVolumeTexture>(&Object);
		const auto* Data = Texture ? dynamic_cast<const DVolumeTextureImportData*>(
			Texture->GetAssetImportData()) : nullptr;
		const FVolumeTextureImportDataState State = Data
			? Data->GetVolumeTextureState() : FVolumeTextureImportDataState{};
		const FSourceFile* Source = Data ? State.SourceData.FindByRole("source") : nullptr;
		if (!Texture || !Data || !Source || Source->Hint.empty())
		{
			if (Completion) Completion({EReimportStatus::MissingSource,
				"VolumeTexture has no source hint to reimport."});
			return;
		}
		std::string Error;
		const bool bSucceeded = RebuildVolumeFromFilename(*Texture,
			Source->Hint, Source->HintBase, MakeImportSettings(State), Error, nullptr);
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto DVolumeTextureFactory::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, FReimportCompletion Completion) const
		-> void
	{
		auto* Texture = Cast<DVolumeTexture>(&Object);
		const auto* Data = Texture ? dynamic_cast<const DVolumeTextureImportData*>(
			Texture->GetAssetImportData()) : nullptr;
		if (!Texture || !Data || Filenames.size() != 1 || Filenames.front().empty())
		{
			if (Completion) Completion({EReimportStatus::SourceOrBuildFailure,
				"VolumeTexture reimport requires one source file and current import data."});
			return;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(Filenames.front()).lexically_normal();
		std::string Error;
		const bool bSucceeded = RebuildVolumeFromFilename(*Texture, {},
			ESourceHintBase::AssetRelative,
			MakeImportSettings(Data->GetVolumeTextureState()), Error, nullptr, Requested);
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto ReimportVolumeTexture(DVolumeTexture& Texture, std::string& OutError,
		const FAssetBundleSaveOptions& SaveOptions) -> bool
	{
		const auto* ImportData = dynamic_cast<const DVolumeTextureImportData*>(
			Texture.GetAssetImportData());
		if (!ImportData)
		{
			OutError = "VolumeTexture has no current family import data.";
			return false;
		}
		const FVolumeTextureImportDataState State =
			ImportData->GetVolumeTextureState();
		const FSourceFile* Source =
			State.SourceData.FindByRole("source");
		if (!Source)
		{
			OutError = "VolumeTexture has no source filename to reimport.";
			return false;
		}
		return RebuildVolumeFromFilename(Texture, Source->Hint, Source->HintBase,
			MakeImportSettings(State), OutError, &SaveOptions);
	}

	auto ReimportVolumeTextureFromFile(DVolumeTexture& Texture,
		std::string_view FilePath, std::string& OutError,
		const FAssetBundleSaveOptions& SaveOptions) -> bool
	{
		const auto* ImportData = dynamic_cast<const DVolumeTextureImportData*>(
			Texture.GetAssetImportData());
		if (!ImportData)
		{
			OutError = "VolumeTexture has no current family import parameters.";
			return false;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			OutError = "The selected VolumeTexture source file does not exist.";
			return false;
		}
		return RebuildVolumeFromFilename(Texture, {},
			ESourceHintBase::AssetRelative,
			MakeImportSettings(ImportData->GetVolumeTextureState()), OutError,
			&SaveOptions, Requested);
	}
}
