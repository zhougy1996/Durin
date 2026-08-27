#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/Texture2DImportData.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Texture2DPostLoad.h"
#include "Asset/AssetOperations.h"
#include "Asset/SourceFilename.h"
#include "Asset.h"
#include "DObject/DObjectGlobals.h"
#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureBuilder.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		auto PublishTexture2DImportData(
			DTexture2D& Texture,
			std::string Filename,
			std::string DisplayLabel,
			FXxHash128 ContentHash,
			uint64 ByteCount,
			int64 LastWriteTime,
			std::string& OutError) -> bool
		{
			FTexture2DImportDataState State;
			State.SourceData.Sources.push_back({
				.StableIdentity = "root", .Role = "source",
				.DisplayLabel = std::move(DisplayLabel),
				.Filename = std::move(Filename),
				.ContentHashLow = ContentHash.HashLow,
				.ContentHashHigh = ContentHash.HashHigh,
				.ByteCount = ByteCount, .LastWriteTime = LastWriteTime});
			State.DecoderId = "DurinImage";
			State.DecoderVersion = 1;
			auto* ImportData = dynamic_cast<DTexture2DImportData*>(
				Texture.GetAssetImportData());
			if (!ImportData)
				ImportData = NewObject<DTexture2DImportData>(
					&Texture, "Texture2DImportData");
			return ImportData && ImportData->SetState(std::move(State), OutError)
				&& Texture.PublishAssetImportData(*ImportData, OutError);
		}

		auto SubmitTexture2DFromFilename(
			DTexture2D& Texture,
			std::string Filename,
			const Asset::FTexture2DBuildSettings& Settings,
			std::string& OutError,
			Asset::ETexture2DCompilationPriority Priority,
			Asset::FTexture2DCompilationCompletion Completion,
			bool bPublishImportData) -> bool
		{
			std::string PhysicalPathText;
			if (!AssetImport::ResolveSourceFilename(
				Filename, PhysicalPathText, OutError)) return false;
			const std::filesystem::path PhysicalPath(PhysicalPathText);
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				OutError = std::format("Texture2D source file is missing: {}.", Filename);
				return false;
			}
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(
				{.Path = Filename}, PhysicalPath, Snapshot, OutError,
				64ull * 1'024ull * 1'024ull)) return false;
			FTextureSourceData SourceData;
			if (!TranslateTexture2DSource(
				Snapshot.GetBytes(), SourceData, OutError)) return false;
			const FXxHash128 ContentHash = Snapshot.ContentHash;
			const uint64 ByteCount = Snapshot.FileSize;
			const int64 LastWriteTime = Snapshot.LastWriteTime;
			const std::string DisplayLabel = PhysicalPath.filename().generic_string();
			return Asset::SubmitTexture2DCompilation(Texture, {
				.Build = {
					.SourceData = std::move(SourceData),
					.SourceContentHashLow = ContentHash.HashLow,
					.SourceContentHashHigh = ContentHash.HashHigh,
					.Settings = Settings},
				.Publication = {
					.SourceFilename = Filename,
					.DecoderId = "DurinImage", .DecoderVersion = 1,
					.SourceFileSize = ByteCount,
					.SourceLastWriteTime = LastWriteTime,
					.bMarkPackageDirty = bPublishImportData,
					.bReportLoadMutation = !bPublishImportData},
				.Priority = Priority}, OutError,
				[&Texture, Filename = std::move(Filename), DisplayLabel,
					ContentHash, ByteCount, LastWriteTime,
					bPublishImportData,
					Completion = std::move(Completion)](
						Asset::FTexture2DCompilationResult Result) mutable {
					if (Result.Succeeded() && bPublishImportData)
					{
						std::string Error;
						if (!PublishTexture2DImportData(Texture, std::move(Filename),
							DisplayLabel, ContentHash, ByteCount, LastWriteTime, Error))
						{
							Result.Status = Asset::ETexture2DCompilationStatus::Failed;
							Result.Diagnostic = std::move(Error);
						}
						else if (const Asset::FAssetResult Saved =
							Asset::SavePackage(Texture.GetPackage()); !Saved)
						{
							Result.Status = Asset::ETexture2DCompilationStatus::Failed;
							Result.Diagnostic = Saved.Message;
						}
					}
					if (Completion) Completion(std::move(Result));
				});
		}
	}

	auto IsTexture2DSourceExtension(std::string_view Extension) -> bool
	{
		std::string Lowercase(Extension);
		std::ranges::transform(Lowercase, Lowercase.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg"
			|| Lowercase == ".bmp" || Lowercase == ".tga";
	}

	auto TranslateTexture2DSource(
		std::span<const std::byte> EncodedBytes,
		FTextureSourceData& OutSourceData,
		std::string& OutError) -> bool
	{
		Image::FDecodedImage DecodedImage;
		if (!Image::DecodeImageFromMemory(
			EncodedBytes,
			DecodedImage,
			OutError,
			{.MaximumDecodedPixels = 16384ull * 16384ull})) return false;
		if (DecodedImage.Width > 16384 || DecodedImage.Height > 16384)
		{
			OutError = std::format("Texture dimensions {}x{} exceed the 16384 pixel limit.",
				DecodedImage.Width, DecodedImage.Height);
			return false;
		}

		OutSourceData = {
			.Pixels = std::move(DecodedImage.Pixels),
			.Width = DecodedImage.Width,
			.Height = DecodedImage.Height,
			.SourceChannelCount = DecodedImage.SourceChannelCount,
			.Format = ETextureSourceFormat::RGBA8,
			.bHasTransparency = DecodedImage.bHasTransparency};
		if (OutSourceData.IsValid()) return true;
		OutSourceData = {};
		OutError = "Decoded texture source data is invalid.";
		return false;
	}

	auto ImportTexture2DAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings,
		bool bAllowEngineContentWrite) -> FTexture2DImportResult
	{
		(void)bAllowEngineContentWrite;
		auto Failed = [](std::string Message, DTexture2D* Asset = nullptr) {
			return FTexture2DImportResult{false, std::move(Message), Asset};
		};
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Failed("Source file does not exist.");
		if (!IsTexture2DSourceExtension(Input.extension().generic_string()))
			return Failed("Unsupported texture source format.");
		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return Failed(std::move(Error));
		if (Asset::FindAssetExact(ParsedAssetPath)
			|| Asset::FindResidentPackage(ParsedAssetPath))
			return Failed(std::format(
				"Asset {} already exists.", ParsedAssetPath.ToString()));
		std::string Filename;
		if (!AssetImport::MakeSourceFilename(
			Input.generic_string(), Filename, Error)) return Failed(std::move(Error));
		FEncodedSourceSnapshot Snapshot;
		if (!CaptureEncodedSource(
			{.Path = Filename}, Input, Snapshot, Error,
			64ull * 1'024ull * 1'024ull)) return Failed(std::move(Error));
		FTextureSourceData SourceData;
		if (!TranslateTexture2DSource(
			Snapshot.GetBytes(), SourceData, Error)) return Failed(std::move(Error));
		Asset::FTexture2DBuildProduct Product;
		if (!Asset::BuildTexture2D({
			.SourceData = std::move(SourceData),
			.SourceContentHashLow = Snapshot.ContentHash.HashLow,
			.SourceContentHashHigh = Snapshot.ContentHash.HashHigh,
			.Settings = {
				.Usage = Settings.Usage,
				.CompressionQuality = Settings.CompressionQuality,
				.AlphaMipMode = Settings.AlphaMipMode,
				.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
				.MaxResolution = Settings.MaxResolution,
				.bSRGB = Settings.bSRGB}}, Product, Error))
			return Failed(std::move(Error));

		DTexture2D* Texture = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!Created || !Texture)
			return Failed(Created.Message.empty()
				? "Texture2D destination could not be created." : Created.Message);
		auto Abandon = [&] {
			(void)Asset::UnloadPackage(
				ParsedAssetPath, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		};
		if (!Asset::PublishTexture2DProduct(*Texture, std::move(Product), {
			.SourceFilename = Filename,
			.DecoderId = "DurinImage",
			.DecoderVersion = 1,
			.SourceFileSize = Snapshot.FileSize,
			.SourceLastWriteTime = Snapshot.LastWriteTime}, Error))
		{
			Abandon();
			return Failed(std::move(Error));
		}
		auto* ImportData = NewObject<DTexture2DImportData>(Texture, "AssetImportData");
		FTexture2DImportDataState ImportState;
		ImportState.SourceData.Sources.push_back({
			.StableIdentity = "root",
			.Role = "source",
			.DisplayLabel = Input.filename().generic_string(),
			.Filename = Filename,
			.ContentHashLow = Snapshot.ContentHash.HashLow,
			.ContentHashHigh = Snapshot.ContentHash.HashHigh,
			.ByteCount = Snapshot.FileSize,
			.LastWriteTime = Snapshot.LastWriteTime});
		ImportState.DecoderId = "DurinImage";
		ImportState.DecoderVersion = 1;
		if (!ImportData || !ImportData->SetState(std::move(ImportState), Error)
			|| !Texture->PublishAssetImportData(*ImportData, Error))
		{
			Abandon();
			return Failed(Error.empty()
				? "Texture2D import metadata could not be published." : std::move(Error));
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Texture->GetPackage());
		if (!Saved)
			return Failed(Saved.Message.empty()
				? "Texture2D was published but its package could not be saved."
				: Saved.Message, Texture);
		return {true, {}, Texture};
	}

	auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
		-> Asset::FTexture2DBuildSettings
	{
		return {
			.Usage = Texture.GetUsage(),
			.CompressionQuality = Texture.GetCompressionQuality(),
			.AlphaMipMode = Texture.GetAlphaMipMode(),
			.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
			.MaxResolution = Texture.GetMaxResolution(),
			.bSRGB = Texture.IsSRGB()};
	}

	auto RebuildTexture2DFromCurrentSource(
		DTexture2D& Texture,
		const Asset::FTexture2DBuildSettings& Settings,
		std::string& OutError,
		Asset::ETexture2DCompilationPriority Priority,
		Asset::FTexture2DCompilationCompletion Completion) -> bool
	{
		if (!Texture.GetPackage() || Texture.GetSourceFile().empty())
		{
			OutError = "Only packaged Texture2D assets with a source filename can rebuild.";
			return false;
		}
		const FTextureSourceDiagnostic Source = Texture.InspectSource();
		if (Source.Status == ETextureSourceStatus::Missing
			|| Source.Status == ETextureSourceStatus::Invalid
			|| Source.PhysicalPath.empty())
		{
			OutError = Source.Message.empty()
				? "The Texture2D source file is unavailable." : Source.Message;
			return false;
		}
		return SubmitTexture2DFromFilename(
			Texture, Texture.GetSourceFile(), Settings, OutError,
			Priority, std::move(Completion), true);
	}

	auto RecoverTexture2DDerivedData(
		DTexture2D& Texture,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage() || Texture.GetSourceFile().empty())
		{
			OutError = "Only packaged Texture2D assets with a source filename can recover.";
			return false;
		}
		return SubmitTexture2DFromFilename(
			Texture, Texture.GetSourceFile(), MakeTexture2DBuildSettings(Texture),
			OutError, Asset::ETexture2DCompilationPriority::Background, {}, false);
	}

	auto ReimportTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError,
		Asset::FTexture2DCompilationCompletion Completion) -> bool
	{
		std::string Filename = Texture.GetSourceFile();
		if (!FilePath.empty())
		{
			const std::filesystem::path Requested =
				std::filesystem::absolute(FilePath).lexically_normal();
			if (!std::filesystem::is_regular_file(Requested))
			{
				OutError = "The selected Texture2D source file does not exist.";
				return false;
			}
			if (!AssetImport::MakeSourceFilename(
				Requested.generic_string(), Filename, OutError)) return false;
		}
		if (Filename.empty())
		{
			OutError = "Texture2D has no source filename to reimport.";
			return false;
		}
		return SubmitTexture2DFromFilename(
			Texture, std::move(Filename), MakeTexture2DBuildSettings(Texture),
			OutError, Asset::ETexture2DCompilationPriority::Interactive,
			std::move(Completion), true);
	}

	auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage, std::string& OutError) -> bool
	{
		if (!Asset::TextureBuilder::IsValidUsage(Usage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}
		if (Texture.GetUsage() == Usage) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.Usage = Usage;
		Settings.bSRGB = Asset::TextureBuilder::GetDefaultSRGB(Usage);
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool
	{
		if (Texture.IsSRGB() == bSRGB) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.bSRGB = bSRGB;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool
	{
		if (Texture.GetMaxResolution() == MaxResolution) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.MaxResolution = MaxResolution;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality,
		std::string& OutError) -> bool
	{
		if (!Asset::TextureBuilder::IsValidCompressionQuality(Quality))
		{
			OutError = "Texture compression quality is invalid.";
			return false;
		}
		if (Texture.GetCompressionQuality() == Quality) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.CompressionQuality = Quality;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode, std::string& OutError) -> bool
	{
		if (!Asset::TextureBuilder::IsValidAlphaMipMode(Mode))
		{
			OutError = "Texture alpha mip mode is invalid.";
			return false;
		}
		if (Texture.GetAlphaMipMode() == Mode) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaMipMode = Mode;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold, std::string& OutError) -> bool
	{
		if (!Asset::TextureBuilder::IsValidAlphaCoverageThreshold(Threshold))
		{
			OutError = "Texture alpha coverage threshold must be greater than zero and less than one.";
			return false;
		}
		if (Texture.GetAlphaCoverageThreshold() == Threshold) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaCoverageThreshold = Threshold;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}
}
