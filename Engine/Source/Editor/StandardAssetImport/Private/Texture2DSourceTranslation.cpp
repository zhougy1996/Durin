#include "Texture2DSourceTranslation.h"
#include "EncodedSourceSnapshot.h"
#include "Texture2DBuildAdapter.h"
#include "Texture2DPropertyEditing.h"
#include "Texture2DPostLoad.h"
#include "Texture2DSourceRelocation.h"

#include "AssetMutation.h"
#include "Hash/XxHash.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "Asset/MountedSource.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture/TextureBuilder.h"

namespace Durin::Asset::Import::Standard
{
	namespace
	{
		constexpr std::string_view DefaultTextureSourceRoot = "Textures";

		auto FindOwningMount(std::string_view VirtualPath)
			-> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Extension,
			std::string_view RequestedSourcePath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format(
					"Texture asset {} is not beneath a registered package mount.",
					AssetPath.ToString());
				return false;
			}
			std::filesystem::path StoredPath;
			if (RequestedSourcePath.empty())
				StoredPath = std::filesystem::path(DefaultTextureSourceRoot)
					/ (std::string(AssetPath.GetAssetName()) + std::string(Extension));
			else if (RequestedSourcePath.starts_with('/'))
			{
				const PathUtilities::FSourcePathResult Requested =
					PathUtilities::ResolveSourcePath(
						RequestedSourcePath, PathUtilities::EPathExistence::AllowMissing);
				if (!Requested)
				{
					OutError = Requested.Message;
					return false;
				}
				if (Requested.Mount != Mount)
				{
					OutError = "Texture source relocation must remain within the asset's owning mount.";
					return false;
				}
				StoredPath = Requested.RelativePath;
			}
			else
				StoredPath = std::filesystem::path(RequestedSourcePath);
			StoredPath = StoredPath.lexically_normal();
			const std::string RelativeString = StoredPath.generic_string();
			if (StoredPath.empty() || StoredPath == "." || StoredPath.is_absolute()
				|| RelativeString == ".." || RelativeString.starts_with("../"))
			{
				OutError = std::format(
					"Texture source path '{}' must be a normalized mount-relative path.",
					RequestedSourcePath);
				return false;
			}
			OutStoredPath = Mount->VirtualRoot + RelativeString;
			std::string RequestedExtension = StoredPath.extension().generic_string();
			std::string InputExtension(Extension);
			std::ranges::transform(RequestedExtension, RequestedExtension.begin(), [](char Value) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Value)));
			});
			std::ranges::transform(InputExtension, InputExtension.begin(), [](char Value) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Value)));
			});
			if (RequestedExtension != InputExtension)
			{
				OutError = std::format(
					"Texture source destination extension '{}' must match the input extension '{}'.",
					StoredPath.extension().generic_string(), Extension);
				return false;
			}
			return true;
		}

		auto SubmitTexture2DFromMountedSource(
			DTexture2D& Texture,
			const FMountedSourceFile& Source,
			const Asset::Build::FTexture2DBuildSettings& Settings,
			std::string& OutError,
			Asset::Build::ETexture2DBuildPriority Priority,
			Asset::Build::FTexture2DAuthoringCompletion Completion = {}) -> bool
		{
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(Source, Snapshot, OutError)) return false;
			FTextureSourceData SourceData;
			if (!TranslateTexture2DSource(Snapshot.GetBytes(), SourceData, OutError)) return false;
			return Asset::Build::SubmitTexture2DBuild(Texture, {
				.SourceData = std::move(SourceData),
				.SourceContentHashLow = Snapshot.ContentHash.HashLow,
				.SourceContentHashHigh = Snapshot.ContentHash.HashHigh,
				.SourcePath = Snapshot.SourcePath,
				.Settings = Settings,
				.DecoderId = "DurinImage",
				.DecoderVersion = 1,
				.SourceFileSize = Snapshot.FileSize,
				.SourceLastWriteTime = Snapshot.LastWriteTime,
				.Priority = Priority}, OutError, std::move(Completion));
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
		std::span<const uint8> EncodedBytes,
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

	auto BuildTexture2DCandidateFromSource(
		DTexture2D& Texture,
		std::span<const uint8> EncodedBytes,
		const FSourcePath& SourcePath,
		const FTexture2DImportSettings& Settings,
		std::string& OutError,
		int64 SourceLastWriteTime) -> bool
	{
		auto Bytes = std::make_shared<const std::vector<uint8>>(
			EncodedBytes.begin(), EncodedBytes.end());
		FEncodedSourceSnapshot Source{
			.SourcePath = SourcePath,
			.Bytes = std::move(Bytes),
			.FileSize = EncodedBytes.size(),
			.LastWriteTime = SourceLastWriteTime};
		Source.ContentHash = FXxHash128::HashBuffer(Source.GetBytes());
		return BuildTexture2DCandidateFromSnapshot(Texture, Source, Settings, OutError);
	}

	auto BuildTexture2DCandidateFromSnapshot(
		DTexture2D& Texture,
		const FEncodedSourceSnapshot& Source,
		const FTexture2DImportSettings& Settings,
		std::string& OutError) -> bool
	{
		FTextureSourceData SourceData;
		if (!TranslateTexture2DSource(Source.GetBytes(), SourceData, OutError)) return false;
		Asset::Build::FTexture2DBuildProduct Product;
		if (!Asset::Build::BuildTexture2D({
			.SourceData = std::move(SourceData),
			.SourceContentHashLow = Source.ContentHash.HashLow,
			.SourceContentHashHigh = Source.ContentHash.HashHigh,
			.Settings = {
				.Usage = Settings.Usage,
				.CompressionQuality = Settings.CompressionQuality,
				.AlphaMipMode = Settings.AlphaMipMode,
				.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
				.MaxResolution = Settings.MaxResolution,
				.bSRGB = Settings.bSRGB}}, Product, OutError)) return false;
		return Asset::Build::PublishTexture2DProduct(Texture, std::move(Product), {
			.SourcePath = Source.SourcePath,
			.DecoderId = "DurinImage",
			.DecoderVersion = 1,
			.SourceFileSize = Source.FileSize,
			.SourceLastWriteTime = Source.LastWriteTime}, OutError);
	}

	auto ImportTexture2DAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings,
		bool bEngineAuthoringContext) -> FTexture2DImportResult
	{
		if (!RegisterTexture2DPropertyEditing()
			|| !RegisterTexture2DSourceRelocation())
			return {false, "Texture2D editor authoring policy is unavailable.", nullptr};
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return {false, "Source file does not exist.", nullptr};
		if (!IsTexture2DSourceExtension(Input.extension().generic_string()))
			return {false, "Unsupported texture source format.", nullptr};
		if (Settings.Usage != ETextureUsage::Color
			&& Settings.Usage != ETextureUsage::Normal
			&& Settings.Usage != ETextureUsage::DataMask)
			return {false, "Texture usage preset is invalid.", nullptr};
		if (Settings.CompressionQuality != ETextureCompressionQuality::Low
			&& Settings.CompressionQuality != ETextureCompressionQuality::Normal
			&& Settings.CompressionQuality != ETextureCompressionQuality::High)
			return {false, "Texture compression quality is invalid.", nullptr};
		if (Settings.AlphaMipMode != ETextureAlphaMipMode::Average
			&& Settings.AlphaMipMode != ETextureAlphaMipMode::PreserveCoverage)
			return {false, "Texture alpha mip mode is invalid.", nullptr};
		if (!std::isfinite(Settings.AlphaCoverageThreshold)
			|| Settings.AlphaCoverageThreshold <= 0.0f
			|| Settings.AlphaCoverageThreshold >= 1.0f)
			return {false,
				"Texture alpha coverage threshold must be greater than zero and less than one.",
				nullptr};

		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedAssetPath)
			|| Asset::FindResidentPackage(ParsedAssetPath))
			return {false,
				std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedAssetPath,
			Input.extension().generic_string(),
			Settings.SourceDestination,
			StoredSourcePath,
			Error)) return {false, std::move(Error), nullptr};
		FScopedMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(
			Input,
			ParsedAssetPath.ToString(),
			StoredSourcePath,
			MountedSource,
			Error,
			bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe))
			return {false, std::move(Error), nullptr};

		FEncodedSourceSnapshot Snapshot;
		if (!CaptureEncodedSource(MountedSource, Snapshot, Error))
		{
			return {false, std::move(Error), nullptr};
		}

		DTexture2D* Texture = nullptr;
		const Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult)
		{
			return {false, CreateResult.Message, nullptr};
		}
		if (!BuildTexture2DCandidateFromSnapshot(
			*Texture,
			Snapshot,
			Settings,
			Error))
		{
			Asset::UnloadPackage(Texture->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return {false, std::move(Error), nullptr};
		}
		const Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			Asset::UnloadPackage(Texture->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return {false, SaveResult.Message, nullptr};
		}
		MountedSource.Commit();
		return {true, {}, Texture};
	}

	auto MakeTexture2DBuildSettings(const DTexture2D& Texture)
		-> Asset::Build::FTexture2DBuildSettings
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
		const Asset::Build::FTexture2DBuildSettings& Settings,
		std::string& OutError,
		Asset::Build::ETexture2DBuildPriority Priority,
		Asset::Build::FTexture2DAuthoringCompletion Completion) -> bool
	{
		if (!Texture.GetPackage() || !Texture.GetSourceImportData().HasSource())
		{
			OutError = "Only packaged Texture2D assets with mounted provenance can rebuild.";
			return false;
		}
		const FTextureSourceDiagnostic Source = Texture.InspectSource();
		if (Source.Status == ETextureSourceStatus::Missing
			|| Source.Status == ETextureSourceStatus::Invalid
			|| Source.PhysicalPath.empty())
		{
			OutError = Source.Message.empty()
				? "The mounted Texture2D source is unavailable." : Source.Message;
			return false;
		}
		FMountedSourceFile MountedSource{
			.SourcePath = Texture.GetSourceImportData().Source.SourcePath,
			.PhysicalPath = Source.PhysicalPath};
		return SubmitTexture2DFromMountedSource(
			Texture, MountedSource, Settings, OutError, Priority, std::move(Completion));
	}

	auto ReimportTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError) -> bool
	{
		const FTextureSourceDiagnostic Source = Texture.InspectSource();
		if (!FilePath.empty())
		{
			std::error_code EquivalentError;
			const std::filesystem::path Requested =
				std::filesystem::absolute(FilePath).lexically_normal();
			if (Source.PhysicalPath.empty()
				|| !std::filesystem::equivalent(Source.PhysicalPath, Requested, EquivalentError)
				|| EquivalentError)
			{
				OutError =
					"Reimport is read-only and must use the persisted mounted source. "
					"Use ChangeSourceReference or IngestAndChangeSource first.";
				return false;
			}
		}
		return RebuildTexture2DFromCurrentSource(
			Texture, MakeTexture2DBuildSettings(Texture), OutError);
	}

	auto ChangeTexture2DSourceReference(
		DTexture2D& Texture,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FScopedMountedSourceFile Source;
		if (!ResolveMountedSourceReference(
			Texture.GetPackage()->GetPackagePath(), SourceVirtualPath, Source, OutError))
			return false;
		return SubmitTexture2DFromMountedSource(
			Texture,
			Source,
			MakeTexture2DBuildSettings(Texture),
			OutError,
			Asset::Build::ETexture2DBuildPriority::Interactive);
	}

	auto IngestAndChangeTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FScopedMountedSourceFile Source;
		if (!PrepareMountedSourceFile(
			FilePath,
			Texture.GetPackage()->GetPackagePath(),
			TargetSourceVirtualPath,
			Source,
			OutError)) return false;
		const bool bSubmitted = SubmitTexture2DFromMountedSource(
			Texture,
			Source,
			MakeTexture2DBuildSettings(Texture),
			OutError,
			Asset::Build::ETexture2DBuildPriority::Interactive);
		if (bSubmitted) Source.Commit();
		return bSubmitted;
	}

	auto RepairTexture2DSourcePath(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError) -> bool
	{
		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(
				std::filesystem::absolute(FilePath).lexically_normal());
		if (!Classified)
		{
			OutError =
				"Source repair requires a mounted source reference. "
				"Use IngestAndChangeSource with an explicit destination for external files.";
			return false;
		}
		return ChangeTexture2DSourceReference(
			Texture, Classified.NormalizedVirtualPath, OutError);
	}

	auto ChangeTexture2DSourceLocation(
		DTexture2D& Texture,
		std::string_view SourceDestination,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		if (SourceDestination.empty())
		{
			OutError = "Choose a source destination inside the asset's mount.";
			return false;
		}
		const FTextureSourceDiagnostic Current = Texture.InspectSource();
		if (Current.PhysicalPath.empty()
			|| !std::filesystem::is_regular_file(Current.PhysicalPath))
		{
			OutError = Current.Message.empty()
				? "Texture source is missing. Reimport it before changing its location."
				: Current.Message;
			return false;
		}
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(
			Texture.GetPackage()->GetPackagePath(), AssetPath, &OutError)) return false;
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			AssetPath,
			std::filesystem::path(Current.PhysicalPath).extension().generic_string(),
			SourceDestination,
			StoredSourcePath,
			OutError)) return false;
		FMountedSourceRelocation Relocation;
		if (!PrepareMountedSourceRelocation(
			Texture.GetPackage()->GetPackagePath(),
			Texture.GetSourceImportData().Source.SourcePath.Path,
			StoredSourcePath,
			Relocation,
			OutError)) return false;
		FMountedSourceFile Source{
			.SourcePath = Relocation.DestinationSourcePath,
			.PhysicalPath = Relocation.DestinationPhysicalPath,
			.Disposition = ESourceFileDisposition::IngestedExternal,
			.bCreatedFile = true};
		const bool bSubmitted = SubmitTexture2DFromMountedSource(
			Texture,
			Source,
			MakeTexture2DBuildSettings(Texture),
			OutError,
			Asset::Build::ETexture2DBuildPriority::Interactive);
		if (!bSubmitted) RollbackMountedSourceRelocation(Relocation);
		return bSubmitted;
	}

	auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage, std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidUsage(Usage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}
		if (Texture.GetUsage() == Usage) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.Usage = Usage;
		Settings.bSRGB = Asset::Build::TextureBuilder::GetDefaultSRGB(Usage);
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool
	{
		if (Texture.IsSRGB() == bSRGB) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.bSRGB = bSRGB;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool
	{
		if (Texture.GetMaxResolution() == MaxResolution) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.MaxResolution = MaxResolution;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality,
		std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidCompressionQuality(Quality))
		{
			OutError = "Texture compression quality is invalid.";
			return false;
		}
		if (Texture.GetCompressionQuality() == Quality) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.CompressionQuality = Quality;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode, std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidAlphaMipMode(Mode))
		{
			OutError = "Texture alpha mip mode is invalid.";
			return false;
		}
		if (Texture.GetAlphaMipMode() == Mode) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaMipMode = Mode;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}

	auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold, std::string& OutError) -> bool
	{
		if (!Asset::Build::TextureBuilder::IsValidAlphaCoverageThreshold(Threshold))
		{
			OutError = "Texture alpha coverage threshold must be greater than zero and less than one.";
			return false;
		}
		if (Texture.GetAlphaCoverageThreshold() == Threshold) return true;
		Asset::Build::FTexture2DBuildSettings Settings = MakeTexture2DBuildSettings(Texture);
		Settings.AlphaCoverageThreshold = Threshold;
		return RebuildTexture2DFromCurrentSource(Texture, Settings, OutError);
	}
}
