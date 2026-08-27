#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/Texture2DImportData.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Asset/AssetOperations.h"
#include "Asset/SourceHint.h"
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
		auto ResolveOwningPackagePhysicalPath(
			std::string_view PackagePath,
			std::filesystem::path& OutPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FAssetPathResult Resolved =
				PathUtilities::ResolveAssetPath(
					PackagePath, PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			OutError.clear();
			return true;
		}

		auto PublishTexture2DImportData(
			DTexture2D& Texture,
			std::string Filename,
			ESourceHintBase HintBase,
			std::string DisplayLabel,
			FXxHash128 ContentHash,
			uint64 ByteCount,
			std::string& OutError) -> bool
		{
			FTexture2DImportDataState State;
			State.SourceData.Sources.push_back({
				.Role = "source",
				.DisplayLabel = std::move(DisplayLabel),
				.Hint = std::move(Filename),
				.HintBase = HintBase,
				.ContentHashLow = ContentHash.HashLow,
				.ContentHashHigh = ContentHash.HashHigh,
				.ByteCount = ByteCount});
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
			ESourceHintBase HintBase,
			const Asset::FTexture2DBuildSettings& Settings,
			std::string& OutError,
			Asset::ETexture2DCompilationPriority Priority,
			Asset::FTexture2DCompilationCompletion Completion,
			bool bPublishImportData,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> bool
		{
			if (!Texture.GetPackage())
			{
				OutError = "Texture2D source capture requires an owning package.";
				return false;
			}
			std::filesystem::path OwningPackagePath;
			if (!ResolveOwningPackagePhysicalPath(
					Texture.GetPackage()->GetPackagePath(), OwningPackagePath, OutError))
				return false;
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath)
				PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				const bool bResolved = ResolveSourceHint(
					HintBase, Filename, OwningPackagePath.generic_string(),
					PhysicalPathText, OutError);
				if (!bResolved) return false;
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath))
			{
				OutError = std::format("Texture2D source file is missing: {}.", Filename);
				return false;
			}
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(
				PhysicalPath.generic_string(), PhysicalPath, Snapshot, OutError,
				64ull * 1'024ull * 1'024ull)) return false;
			if (SelectedPhysicalPath
				&& !MakeSourceHint(
					PhysicalPath.generic_string(), OwningPackagePath.generic_string(),
					HintBase, Filename, OutError)) return false;
			FTextureSourceData SourceData;
			if (!TranslateTexture2DSource(
				Snapshot.GetBytes(), SourceData, OutError)) return false;
			const FXxHash128 ContentHash = Snapshot.ContentHash;
			const uint64 ByteCount = Snapshot.FileSize;
			const std::string DisplayLabel = PhysicalPath.filename().generic_string();
			return Asset::SubmitTexture2DCompilation(Texture, {
				.Build = {
					.SourceData = std::move(SourceData),
					.SourceContentHashLow = ContentHash.HashLow,
					.SourceContentHashHigh = ContentHash.HashHigh,
					.Settings = Settings},
				.Publication = {
					.bMarkPackageDirty = bPublishImportData,
					.bReportLoadMutation = !bPublishImportData},
				.Priority = Priority}, OutError,
				[&Texture, Filename, HintBase, DisplayLabel,
					ContentHash, ByteCount,
					bPublishImportData,
					Completion = std::move(Completion)](
						Asset::FTexture2DCompilationResult Result) mutable {
					if (Result.Succeeded() && bPublishImportData)
					{
						std::string Error;
						if (!PublishTexture2DImportData(Texture, std::move(Filename), HintBase,
							DisplayLabel, ContentHash, ByteCount, Error))
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
		const FTexture2DImportSettings& Settings) -> FTexture2DImportResult
	{
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
		FEncodedSourceSnapshot Snapshot;
		if (!CaptureEncodedSource(
			Input.generic_string(), Input, Snapshot, Error,
			64ull * 1'024ull * 1'024ull)) return Failed(std::move(Error));
		std::filesystem::path OwningPackagePath;
		if (!ResolveOwningPackagePhysicalPath(
				ParsedAssetPath.GetView(), OwningPackagePath, Error))
			return Failed(std::move(Error));
		std::string Filename;
		ESourceHintBase HintBase;
		if (!MakeSourceHint(
			Input.generic_string(), OwningPackagePath.generic_string(),
			HintBase, Filename, Error))
			return Failed(std::move(Error));
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
		if (!Asset::PublishTexture2DProduct(*Texture, std::move(Product), {}, Error))
		{
			Abandon();
			return Failed(std::move(Error));
		}
		auto* ImportData = NewObject<DTexture2DImportData>(Texture, "AssetImportData");
		FTexture2DImportDataState ImportState;
		ImportState.SourceData.Sources.push_back({
			.Role = "source",
			.DisplayLabel = Input.filename().generic_string(),
			.Hint = Filename,
			.HintBase = HintBase,
			.ContentHashLow = Snapshot.ContentHash.HashLow,
			.ContentHashHigh = Snapshot.ContentHash.HashHigh,
			.ByteCount = Snapshot.FileSize});
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

	auto RebuildTexture2DFromImportedData(
		DTexture2D& Texture,
		const Asset::FTexture2DBuildSettings& Settings,
		std::string& OutError,
		Asset::ETexture2DCompilationPriority Priority,
		Asset::FTexture2DCompilationCompletion Completion) -> bool
	{
		if (!Texture.GetPackage() || !Texture.GetImportedData().IsValid())
		{
			OutError = "Only packaged Texture2D assets with canonical imported pixels can rebuild.";
			return false;
		}
		const FXxHash128 Identity = Texture.GetImportedDataIdentity();
		return Asset::SubmitTexture2DCompilation(Texture, {
			.Build = {
				.SourceData = Texture.GetImportedData().ToSourceData(),
				.SourceContentHashLow = Identity.HashLow,
				.SourceContentHashHigh = Identity.HashHigh,
				.Settings = Settings},
			.Publication = {
				.bMarkPackageDirty = true,
				.bReportLoadMutation = false,
				.bSourceDecoderInvoked = false},
			.Priority = Priority}, OutError, std::move(Completion));
	}

	auto ReimportTexture2D(
		DTexture2D& Texture,
		std::string& OutError,
		Asset::FTexture2DCompilationCompletion Completion) -> bool
	{
		const DAssetImportData* ImportData = Texture.GetAssetImportData();
		const FSourceFile* Source = ImportData
			? ImportData->GetSourceData().FindByRole("source") : nullptr;
		if (!Source || Source->Hint.empty())
		{
			OutError = "Texture2D has no source hint to reimport.";
			return false;
		}
		return SubmitTexture2DFromFilename(
			Texture, Source->Hint, Source->HintBase,
			MakeTexture2DBuildSettings(Texture),
			OutError, Asset::ETexture2DCompilationPriority::Interactive,
			std::move(Completion), true);
	}

	auto ReimportTexture2DFromFile(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError,
		Asset::FTexture2DCompilationCompletion Completion) -> bool
	{
		if (FilePath.empty())
		{
			OutError = "A Texture2D source file must be selected.";
			return false;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			OutError = "The selected Texture2D source file does not exist.";
			return false;
		}
		return SubmitTexture2DFromFilename(
			Texture, {}, ESourceHintBase::AssetRelative,
			MakeTexture2DBuildSettings(Texture), OutError,
			Asset::ETexture2DCompilationPriority::Interactive,
			std::move(Completion), true, Requested);
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
		return RebuildTexture2DFromImportedData(Texture, Settings, OutError);
	}

	auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool
	{
		if (Texture.IsSRGB() == bSRGB) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.bSRGB = bSRGB;
		return RebuildTexture2DFromImportedData(Texture, Settings, OutError);
	}

	auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool
	{
		if (Texture.GetMaxResolution() == MaxResolution) return true;
		Asset::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.MaxResolution = MaxResolution;
		return RebuildTexture2DFromImportedData(Texture, Settings, OutError);
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
		return RebuildTexture2DFromImportedData(Texture, Settings, OutError);
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
		return RebuildTexture2DFromImportedData(Texture, Settings, OutError);
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
		return RebuildTexture2DFromImportedData(Texture, Settings, OutError);
	}
}
