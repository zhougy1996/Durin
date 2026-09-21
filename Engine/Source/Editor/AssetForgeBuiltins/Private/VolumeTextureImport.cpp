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
	auto FVolumeTextureFactoryError::Format() const -> std::string
	{
		if (const auto* Settings = std::get_if<FVolumeTextureImportSettingsError>(&Cause))
			return FormatVolumeTextureImportSettingsError(*Settings);
		return FormatVolumeTextureRebuildError(std::get<FVolumeTextureRebuildError>(Cause));
	}

	auto FormatVolumeTextureRebuildError(const FVolumeTextureRebuildError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EVolumeTextureRebuildError::None: return {};
		case EVolumeTextureRebuildError::Package: return "VolumeTexture source capture requires an owning package.";
		case EVolumeTextureRebuildError::Mount: return "VolumeTexture owning package path could not be resolved: " + Error.ObjectPath;
		case EVolumeTextureRebuildError::SourceHint:
			return Error.SourceHintCause ? FormatSourceHintError(*Error.SourceHintCause) : "VolumeTexture source hint is invalid.";
		case EVolumeTextureRebuildError::SourceFile: return "VolumeTexture source must be an existing PNG file: " + Error.Filename;
		case EVolumeTextureRebuildError::Capture:
			return Error.CaptureCause ? FormatEncodedSourceError(*Error.CaptureCause) : "VolumeTexture source capture failed.";
		case EVolumeTextureRebuildError::Translation:
			return Error.TranslationCause ? FormatVolumeTextureTranslationError(*Error.TranslationCause) : "VolumeTexture source translation failed.";
		case EVolumeTextureRebuildError::Build:
			return Error.BuildCause ? Error.BuildCause->Diagnostic : "VolumeTexture build failed.";
		case EVolumeTextureRebuildError::ImportValidation:
			return Error.ImportCause ? FormatAssetImportDataError(*Error.ImportCause) : "VolumeTexture import data is invalid.";
		case EVolumeTextureRebuildError::ImportAllocation: return "Could not allocate asset import data.";
		case EVolumeTextureRebuildError::Save: return Error.SaveCause ? Error.SaveCause->Message : "VolumeTexture save failed.";
		case EVolumeTextureRebuildError::ImportData: return "VolumeTexture has no current family import data.";
		case EVolumeTextureRebuildError::MissingSource: return "VolumeTexture has no source filename to reimport.";
		}
		return {};
	}

	namespace
	{
		auto ResolveOwningPackagePhysicalPath(const DVolumeTexture& Texture,
			std::filesystem::path& OutPath) -> FVolumeTextureRebuildResult
		{
			if (!Texture.GetPackage()) return {.Error = {.Code = EVolumeTextureRebuildError::Package,
				.ObjectPath = Texture.GetObjectPath()}};
			const auto Resolved = FMountPaths::ResolveAssetPath(Texture.GetPackage()->GetPackagePath(), EMountPathExistence::AllowMissing);
			if (!Resolved) return {.Error = {.Code = EVolumeTextureRebuildError::Mount,
				.ObjectPath = Texture.GetObjectPath(), .MountCause = Resolved.Error}};
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return {};
		}

		auto AppendPixel(const Image::FDecodedImage& Image, size_t Pixel,
			EVolumeTextureSourceChannels Channels, FByteBuffer& OutVoxels) -> void
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
				if (!Settings.Validate()) continue;
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
			FVolumeTextureCapturedSource& Out) -> std::expected<void, FEncodedSourceError>
		{
			auto Captured = CaptureEncodedSource(Filename, PhysicalPath, MaximumTexturePayloadBytes);
			if (!Captured) return std::unexpected(std::move(Captured.error()));
			OutSnapshot = std::move(*Captured);
			Out = {.Filename = std::move(Filename),
				.ContentHash = OutSnapshot.ContentHash,
				.Bytes = OutSnapshot.GetBytes()};
			return {};
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

	auto FormatVolumeTextureImportSettingsError(const FVolumeTextureImportSettingsError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EVolumeTextureImportSettingsError::None: return {};
		case EVolumeTextureImportSettingsError::ImportFormat:
			return "Only PNG row-major atlas volume import is supported.";
		case EVolumeTextureImportSettingsError::Dimensions:
			return std::format("Slice dimensions and depth must be between 1 and {}.", MaximumVolumeTextureDimension);
		case EVolumeTextureImportSettingsError::CellCount:
			return "Atlas tiles do not provide enough row-major cells for depth.";
		case EVolumeTextureImportSettingsError::AtlasBudget:
			return "Decoded RGBA atlas exceeds the 2 GiB image budget.";
		case EVolumeTextureImportSettingsError::VolumeBudget:
			return "Decoded volume texture exceeds the 2 GiB source payload limit.";
		}
		return {};
	}

	auto FVolumeTextureImportSettings::Validate() const -> FVolumeTextureImportSettingsResult
	{
		auto Fail = [&](EVolumeTextureImportSettingsError Code) -> FVolumeTextureImportSettingsResult {
			return {.Error = {.Code = Code, .Settings = *this}};
		};
		if (ImportFormat != EVolumeTextureImportFormat::PngRowMajorAtlas)
			return Fail(EVolumeTextureImportSettingsError::ImportFormat);
		if (SliceWidth == 0 || SliceHeight == 0 || Depth == 0
			|| TilesX == 0 || TilesY == 0
			|| SliceWidth > MaximumVolumeTextureDimension
			|| SliceHeight > MaximumVolumeTextureDimension
			|| Depth > MaximumVolumeTextureDimension)
			return Fail(EVolumeTextureImportSettingsError::Dimensions);
		const uint64 CellCount = static_cast<uint64>(TilesX) * TilesY;
		if (CellCount < Depth)
			return Fail(EVolumeTextureImportSettingsError::CellCount);
		const uint64 AtlasWidth = static_cast<uint64>(SliceWidth) * TilesX;
		const uint64 AtlasHeight = static_cast<uint64>(SliceHeight) * TilesY;
		if (AtlasWidth > std::numeric_limits<uint32>::max()
			|| AtlasHeight > std::numeric_limits<uint32>::max()
			|| AtlasWidth > std::numeric_limits<uint64>::max() / AtlasHeight
			|| AtlasWidth * AtlasHeight > MaximumTexturePayloadBytes / 4u)
			return Fail(EVolumeTextureImportSettingsError::AtlasBudget);
		const uint64 BytesPerVoxel = GetOutputFormat() == EVolumeTextureFormat::RGBA8_UNORM ? 4u : 1u;
		const uint64 VoxelCount = static_cast<uint64>(SliceWidth) * SliceHeight * Depth;
		if (VoxelCount > MaximumTexturePayloadBytes / BytesPerVoxel)
			return Fail(EVolumeTextureImportSettingsError::VolumeBudget);
		return {};
	}

	auto FormatVolumeTextureAtlasInspection(const FVolumeTextureAtlasInspection& Inspection) -> std::string
	{
		if (!Inspection)
			return std::format("Failed to inspect the volume atlas: {}", Image::FormatImageDecodeError(Inspection.Error));
		if (Inspection.SuggestedLayouts.empty())
			return "No cubic atlas layout could be inferred from the PNG dimensions.";
		return Inspection.bHasConfidentLayout
			? "A high-confidence cubic layout was inferred from the PNG dimensions."
			: "Several cubic layouts fit the PNG dimensions; review the suggested layouts.";
	}

	auto InspectVolumeTextureAtlasSource(
		std::string_view FilePath) -> FVolumeTextureAtlasInspection
	{
		auto DecodeResult = Image::DecodeImageFromFile(FilePath);
		if (!DecodeResult)
			return {.Error = DecodeResult.error()};
		auto Image = std::move(*DecodeResult);

		FVolumeTextureAtlasInspection Result;
		Result.AtlasWidth = Image.Width;
		Result.AtlasHeight = Image.Height;
		Result.SourceChannelCount = Image.SourceChannelCount;
		Result.SuggestedChannels = SuggestAtlasChannels(Image);
		Result.SuggestedLayouts = SuggestCubicAtlasLayouts(
			Image, Result.SuggestedChannels);
		if (Result.SuggestedLayouts.empty()) return Result;

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
		return Result;
	}

	auto FormatVolumeTextureTranslationError(const FVolumeTextureTranslationError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EVolumeTextureTranslationError::None: return {};
		case EVolumeTextureTranslationError::Settings:
			return Error.SettingsCause ? FormatVolumeTextureImportSettingsError(*Error.SettingsCause) : "Invalid volume import settings.";
		case EVolumeTextureTranslationError::Signature: return "Volume texture atlas is not PNG data.";
		case EVolumeTextureTranslationError::Decode:
			return std::format("Failed to decode volume atlas '{}': {}", Error.Filename,
				Error.DecodeCause ? Image::FormatImageDecodeError(*Error.DecodeCause) : "Image decode failed.");
		case EVolumeTextureTranslationError::Dimensions:
			return std::format("Volume atlas is {}x{}; expected {}x{} from slice size and tiles.",
				Error.ActualWidth, Error.ActualHeight, Error.ExpectedWidth, Error.ExpectedHeight);
		case EVolumeTextureTranslationError::Publication:
			return "Volume source payload could not be published as authored bulk data.";
		case EVolumeTextureTranslationError::Layout:
			return "Decoded volume texture source failed normalized layout validation.";
		}
		return {};
	}

	auto TranslateVolumeTextureAtlasSource(const FVolumeTextureCapturedSource& Source,
		const FVolumeTextureImportSettings& Settings,
		FVolumeTextureSourceData& OutSourceData) -> FVolumeTextureTranslationResult
	{
		OutSourceData = {};
		if (const auto Validation = Settings.Validate(); !Validation)
			return {.Error = {.Code = EVolumeTextureTranslationError::Settings,
				.Filename = Source.Filename, .SettingsCause = Validation.Error}};
		constexpr std::array<std::byte, 8> PngSignature = {
			std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
			std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
		if (Source.Bytes.size() < PngSignature.size()
			|| !std::ranges::equal(PngSignature, Source.Bytes.first(PngSignature.size())))
		{
			return {.Error = {.Code = EVolumeTextureTranslationError::Signature, .Filename = Source.Filename}};
		}
		const uint64 ExpectedWidth = static_cast<uint64>(Settings.SliceWidth) * Settings.TilesX;
		const uint64 ExpectedHeight = static_cast<uint64>(Settings.SliceHeight) * Settings.TilesY;
		auto DecodeResult = Image::DecodeImageFromMemory(Source.Bytes, {.MaximumDecodedPixels = ExpectedWidth * ExpectedHeight});
		if (!DecodeResult)
		{
			return {.Error = {.Code = EVolumeTextureTranslationError::Decode, .Filename = Source.Filename,
				.ExpectedWidth = ExpectedWidth, .ExpectedHeight = ExpectedHeight, .DecodeCause = DecodeResult.error()}};
		}
		auto Image = std::move(*DecodeResult);
		if (Image.Width != ExpectedWidth || Image.Height != ExpectedHeight)
		{
			return {.Error = {.Code = EVolumeTextureTranslationError::Dimensions, .Filename = Source.Filename,
				.ExpectedWidth = ExpectedWidth, .ExpectedHeight = ExpectedHeight,
				.ActualWidth = Image.Width, .ActualHeight = Image.Height}};
		}
		const uint32 BytesPerVoxel = Settings.GetOutputFormat()
			== EVolumeTextureFormat::RGBA8_UNORM ? 4u : 1u;
		const uint64 TotalBytes = static_cast<uint64>(Settings.SliceWidth)
			* Settings.SliceHeight * Settings.Depth * BytesPerVoxel;
		FByteBuffer Voxels;
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
			return {.Error = {.Code = EVolumeTextureTranslationError::Publication, .Filename = Source.Filename}};
		}
		if (!Candidate.IsValid())
		{
			return {.Error = {.Code = EVolumeTextureTranslationError::Layout, .Filename = Source.Filename}};
		}
		OutSourceData = std::move(Candidate);
		return {};
	}

	namespace
	{
		auto PublishDirectVolumeImportData(DVolumeTexture& Texture,
			std::string Filename, ESourceHintBase HintBase,
			const std::filesystem::path& PhysicalPath,
			const FEncodedSourceSnapshot& Snapshot,
			const FVolumeTextureImportSettings& Settings) -> FVolumeTextureRebuildResult
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
			State.SourceData.Normalize();
			if (const auto Validation = State.Validate(); !Validation)
				return {.Error = {.Code = EVolumeTextureRebuildError::ImportValidation, .ObjectPath = Texture.GetObjectPath(),
					.ImportCause = std::make_shared<FAssetImportDataError>(Validation.Error)}};
			if (!Data) return {.Error = {.Code = EVolumeTextureRebuildError::ImportAllocation, .ObjectPath = Texture.GetObjectPath()}};
			Data->SetState(std::move(State));
			Texture.SetAssetImportData(*Data);
			Texture.MarkPackageDirty();
			return {};
		}

		auto RebuildVolumeFromFilename(DVolumeTexture& Texture,
			std::string Filename, ESourceHintBase HintBase,
			const FVolumeTextureImportSettings& Settings,
			const FAssetBundleSaveOptions* SaveOptions,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> FVolumeTextureRebuildResult
		{
			std::filesystem::path OwningPackagePath;
			if (const auto Resolved = ResolveOwningPackagePhysicalPath(Texture, OwningPackagePath); !Resolved) return Resolved;
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath) PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				if (const auto Resolved = ResolveSourceHint(HintBase, Filename,
					OwningPackagePath.generic_string(), PhysicalPathText); !Resolved)
					return {.Error = {.Code = EVolumeTextureRebuildError::SourceHint, .ObjectPath = Texture.GetObjectPath(),
					.Filename = Filename, .SourceHintCause = Resolved.Error}};
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath)
				|| StringUtils::FoldAscii(PhysicalPath.extension().generic_string()) != ".png")
			{
				return {.Error = {.Code = EVolumeTextureRebuildError::SourceFile, .ObjectPath = Texture.GetObjectPath(),
					.Filename = PhysicalPath.generic_string()}};
			}
			if (SelectedPhysicalPath)
			{
				if (const auto Hint = MakeSourceHint(
					PhysicalPath.generic_string(), OwningPackagePath.generic_string(),
					HintBase, Filename); !Hint)
					return {.Error = {.Code = EVolumeTextureRebuildError::SourceHint, .ObjectPath = Texture.GetObjectPath(),
					.Filename = Filename, .SourceHintCause = Hint.Error}};
			}
			FEncodedSourceSnapshot Snapshot;
			FVolumeTextureCapturedSource Captured;
			if (const auto Capture = CaptureVolumeSource(Filename, PhysicalPath, Snapshot, Captured); !Capture)
				return {.Error = {.Code = EVolumeTextureRebuildError::Capture, .ObjectPath = Texture.GetObjectPath(),
					.Filename = Filename, .CaptureCause = std::make_shared<FEncodedSourceError>(Capture.error())}};
			FVolumeTextureSourceData SourceData;
			if (const auto Translated = TranslateVolumeTextureAtlasSource(
				Captured, Settings, SourceData); !Translated)
				return {.Error = {.Code = EVolumeTextureRebuildError::Translation, .ObjectPath = Texture.GetObjectPath(),
					.Filename = Filename, .TranslationCause = Translated.Error}};
			auto BuildResult = BuildVolumeTextureSynchronously(Texture, {.SourceData = SourceData, .Settings = {.OutputFormat = Settings.GetOutputFormat()}}, {});
			if (!BuildResult) return {.Error = {.Code = EVolumeTextureRebuildError::Build,
				.ObjectPath = Texture.GetObjectPath(), .Filename = Filename, .BuildCause = std::move(BuildResult)}};
			if (const auto Published = PublishDirectVolumeImportData(Texture, std::move(Filename), HintBase, PhysicalPath, Snapshot, Settings); !Published) return Published;
			if (!SaveOptions) return {};
			DPackage* Package = Texture.GetPackage();
			const FAssetWriteResult Saved = SavePackages(
				std::span<DPackage* const>(&Package, 1), *SaveOptions).Result;
			if (Saved) return {};
			return {.Error = {.Code = EVolumeTextureRebuildError::Save, .ObjectPath = Texture.GetObjectPath(),
				.SaveCause = std::make_shared<FAssetWriteResult>(Saved)}};
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
		auto Failed = [&](auto Error) -> DObject* {
			if (Diagnostics) Diagnostics->ReportDomainFailure(std::make_shared<FVolumeTextureFactoryError>(std::move(Error)));
			return nullptr;
		};
		auto Reject = [&](EFactoryError Code) -> DObject* {
			if (Diagnostics) Diagnostics->ReportFailure({
				.Code = Code,
				.ExpectedClass = DVolumeTexture::StaticClass()->GetName(),
				.RequestedClass = InClass ? InClass->GetName() : std::string{},
				.Filename = std::string(Filename)});
			return nullptr;
		};
		if (InClass != DVolumeTexture::StaticClass())
			return Reject(EFactoryError::ExactClass);
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Reject(EFactoryError::AssetPackageParent);
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Reject(EFactoryError::SourceMissing);
		if (StringUtils::FoldAscii(Input.extension().generic_string()) != ".png")
			return Reject(EFactoryError::SourceFormat);
		if (const auto Validation = Settings.Validate(); !Validation)
			return Failed(Validation.Error);
		auto* Texture = NewObject<DVolumeTexture>(
			InClass, Package, InName, Flags);
		if (!Texture)
			return Reject(EFactoryError::ObjectCreation);
		if (const auto Rebuilt = RebuildVolumeFromFilename(
			*Texture, Input.generic_string(), ESourceHintBase::AssetRelative,
			Settings, nullptr, Input); !Rebuilt) return Failed(Rebuilt.Error);
		return Texture;
	}

	auto DVolumeTextureFactory::QueryReimportActions(std::string_view AssetClassName) const
		-> FReimportActions
	{
		if (AssetClassName != DVolumeTexture::StaticClass()->GetQualifiedName().ToString())
			return {};
		return {.bSupportsReimport = true, .bSupportsReimportFromFile = true};
	}

	auto DVolumeTextureFactory::GetSourceFileDialogs(const DObject& Object) const
		-> std::vector<FReimportSourceFileDialog>
	{
		if (!Cast<DVolumeTexture>(&Object)) return {};
		return {{"Reimport VolumeTexture Atlas From File", "PNG", "*.png"}};
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
		const auto Rebuilt = RebuildVolumeFromFilename(*Texture,
			Source->Hint, Source->HintBase, MakeImportSettings(State), nullptr);
		if (Completion) Completion(Rebuilt
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, FormatVolumeTextureRebuildError(Rebuilt.Error),
				std::make_shared<FVolumeTextureFactoryError>(Rebuilt.Error)});
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
		const auto Rebuilt = RebuildVolumeFromFilename(*Texture, {},
			ESourceHintBase::AssetRelative,
			MakeImportSettings(Data->GetVolumeTextureState()), nullptr, Requested);
		if (Completion) Completion(Rebuilt
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, FormatVolumeTextureRebuildError(Rebuilt.Error),
				std::make_shared<FVolumeTextureFactoryError>(Rebuilt.Error)});
	}

	auto ReimportVolumeTexture(DVolumeTexture& Texture,
		const FAssetBundleSaveOptions& SaveOptions) -> FVolumeTextureRebuildResult
	{
		const auto* ImportData = dynamic_cast<const DVolumeTextureImportData*>(
			Texture.GetAssetImportData());
		if (!ImportData)
		{
			return {.Error = {.Code = EVolumeTextureRebuildError::ImportData, .ObjectPath = Texture.GetObjectPath()}};
		}
		const FVolumeTextureImportDataState State =
			ImportData->GetVolumeTextureState();
		const FSourceFile* Source =
			State.SourceData.FindByRole("source");
		if (!Source)
		{
			return {.Error = {.Code = EVolumeTextureRebuildError::MissingSource, .ObjectPath = Texture.GetObjectPath()}};
		}
		return RebuildVolumeFromFilename(Texture, Source->Hint, Source->HintBase,
			MakeImportSettings(State), &SaveOptions);
	}

	auto ReimportVolumeTextureFromFile(DVolumeTexture& Texture,
		std::string_view FilePath,
		const FAssetBundleSaveOptions& SaveOptions) -> FVolumeTextureRebuildResult
	{
		const auto* ImportData = dynamic_cast<const DVolumeTextureImportData*>(
			Texture.GetAssetImportData());
		if (!ImportData)
		{
			return {.Error = {.Code = EVolumeTextureRebuildError::ImportData, .ObjectPath = Texture.GetObjectPath()}};
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			return {.Error = {.Code = EVolumeTextureRebuildError::SourceFile, .ObjectPath = Texture.GetObjectPath(), .Filename = std::string(FilePath)}};
		}
		return RebuildVolumeFromFilename(Texture, {},
			ESourceHintBase::AssetRelative,
			MakeImportSettings(ImportData->GetVolumeTextureState()),
			&SaveOptions, Requested);
	}
}
