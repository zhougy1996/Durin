#include "Texture/TextureBuildOperations.h"

#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "Hash/XxHash.h"
#include "ImageDecoder.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/TextureDerivedDataWriter.h"

namespace Durin
{
	struct FTextureBuildOperations
	{
		static auto BuildFromEncodedBytes(
			DTexture2D& Texture,
			std::span<const uint8> EncodedBytes,
			const FSourcePath& SourcePath,
			const FTexture2DImportSettings& Settings,
			std::string& OutError) -> bool;

		static auto UpdateSourceFingerprint(
			DTexture2D& Texture,
			const std::filesystem::path& PhysicalFilePath) -> void;
	};
}

namespace Durin::AssetBuild
{
	namespace
	{
		constexpr std::string_view TextureDecoderId = "DurinImage";
		constexpr uint32 TextureDecoderVersion = 1;
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

		auto StoreTexture2DDerivedData(
			std::string_view Key,
			const FTexturePlatformData& PlatformData,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!TextureDerivedDataWriter::EncodeTexture2DPayload(
				PlatformData,
				Asset::ECookTargetPlatform::Win64,
				Asset::ECookTargetProfile::Game,
				Bytes,
				OutError)) return false;
			return Asset::FDerivedDataObjectStore(
				"Textures/Objects", MaximumTexturePayloadBytes).Write(Key, Bytes, &OutError);
		}
	}

	auto BuildTexture2DFromEncodedBytes(
		DTexture2D& Texture,
		std::span<const uint8> EncodedBytes,
		const FSourcePath& SourcePath,
		const FTexture2DImportSettings& Settings,
		std::string& OutError) -> bool
	{
		return FTextureBuildOperations::BuildFromEncodedBytes(
			Texture, EncodedBytes, SourcePath, Settings, OutError);
	}

	auto ImportTexture2DAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings,
		bool bEngineAuthoringContext) -> FTexture2DImportResult
	{
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return {false, "Source file does not exist.", nullptr};
		if (!Asset::IsSupportedImageExtension(Input.extension().generic_string()))
			return {false, "Unsupported texture source format.", nullptr};
		if (!TextureBuilder::IsValidUsage(Settings.Usage))
			return {false, "Texture usage preset is invalid.", nullptr};
		if (!TextureBuilder::IsValidCompressionQuality(Settings.CompressionQuality))
			return {false, "Texture compression quality is invalid.", nullptr};
		if (!TextureBuilder::IsValidAlphaMipMode(Settings.AlphaMipMode))
			return {false, "Texture alpha mip mode is invalid.", nullptr};
		if (!TextureBuilder::IsValidAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
			return {false,
				"Texture alpha coverage threshold must be greater than zero and less than one.",
				nullptr};

		FAssetPath ParsedAssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::GetAssetRegistry().FindAssetExact(ParsedAssetPath)
			|| Asset::FindLoadedPackage(ParsedAssetPath))
			return {false,
				std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedAssetPath,
			Input.extension().generic_string(),
			Settings.SourceDestination,
			StoredSourcePath,
			Error)) return {false, std::move(Error), nullptr};
		FMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(
			Input,
			ParsedAssetPath.ToString(),
			StoredSourcePath,
			MountedSource,
			Error,
			bEngineAuthoringContext)) return {false, std::move(Error), nullptr};

		std::vector<uint8> EncodedBytes;
		if (!FFileHelper::LoadFileToArray(
			EncodedBytes, MountedSource.PhysicalPath.generic_string()))
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, "Failed to read the mounted texture source.", nullptr};
		}

		DTexture2D* Texture = nullptr;
		const Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult)
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, CreateResult.Message, nullptr};
		}
		if (!BuildTexture2DFromEncodedBytes(
			*Texture, EncodedBytes, MountedSource.SourcePath, Settings, Error))
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(Error), nullptr};
		}
		FTextureBuildOperations::UpdateSourceFingerprint(
			*Texture, MountedSource.PhysicalPath);
		const Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		CommitMountedSourceFile(MountedSource);
		return {true, {}, Texture};
	}
}

namespace Durin
{
	auto FTextureBuildOperations::BuildFromEncodedBytes(
		DTexture2D& Texture,
		std::span<const uint8> EncodedBytes,
		const FSourcePath& SourcePath,
		const FTexture2DImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (!Texture.GetPackage() || EncodedBytes.empty())
		{
			OutError = "Encoded Texture2D candidates require a package and non-empty source bytes.";
			return false;
		}
		if (!AssetBuild::TextureBuilder::IsValidUsage(Settings.Usage)
			|| !AssetBuild::TextureBuilder::IsValidCompressionQuality(Settings.CompressionQuality)
			|| !AssetBuild::TextureBuilder::IsValidAlphaMipMode(Settings.AlphaMipMode)
			|| !AssetBuild::TextureBuilder::IsValidAlphaCoverageThreshold(
				Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D candidate build settings are invalid.";
			return false;
		}
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				SourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Dependency =
			PathUtilities::CheckMountDependency(
				Texture.GetPackage()->GetPackagePath(), Resolved.NormalizedVirtualPath);
		if (!Dependency)
		{
			OutError = Dependency.Message;
			return false;
		}

		FTextureSourceData CandidateSourceData;
		if (!AssetBuild::TextureBuilder::DecodeRGBA8(
			EncodedBytes, CandidateSourceData, OutError))
		{
			Texture.BuildStatus = ETextureBuildStatus::DecodeFailure;
			Texture.LastBuildError = OutError;
			return false;
		}
		const bool bCandidateSRGB =
			Settings.bSRGB.value_or(AssetBuild::TextureBuilder::GetDefaultSRGB(Settings.Usage));
		auto CandidatePlatformData = std::make_unique<FTexturePlatformData>();
		if (!AssetBuild::TextureBuilder::BuildMipChain(
			CandidateSourceData,
			Settings.Usage,
			bCandidateSRGB,
			*CandidatePlatformData,
			OutError,
			Settings.MaxResolution,
			Settings.CompressionQuality,
			Settings.AlphaMipMode,
			Settings.AlphaCoverageThreshold))
		{
			Texture.BuildStatus = ETextureBuildStatus::BuildFailure;
			Texture.LastBuildError = OutError;
			return false;
		}

		const FXxHash128 SourceHash = FXxHash128::HashBuffer(EncodedBytes);
		const std::string NewKey =
			AssetBuild::TextureDerivedDataWriter::BuildTexture2DDerivedDataKey({
			.SourceContentHash = SourceHash,
			.Usage = Settings.Usage,
			.bSRGB = bCandidateSRGB,
			.CompressionQuality = Settings.CompressionQuality,
			.AlphaMipMode = Settings.AlphaMipMode,
			.MaximumResolution = Settings.MaxResolution,
			.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (!AssetBuild::StoreTexture2DDerivedData(
			NewKey, *CandidatePlatformData, OutError))
		{
			Texture.BuildStatus = ETextureBuildStatus::BuildFailure;
			Texture.LastBuildError = OutError;
			return false;
		}

		Texture.Usage = Settings.Usage;
		Texture.bSRGB = bCandidateSRGB;
		Texture.MaxResolution = Settings.MaxResolution;
		Texture.CompressionQuality = Settings.CompressionQuality;
		Texture.AlphaMipMode = Settings.AlphaMipMode;
		Texture.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold;
		Texture.SourceImportData = {
			.Source = {
				.SourcePath = {.Path = Resolved.NormalizedVirtualPath},
				.SourceContentHashLow = SourceHash.HashLow,
				.SourceContentHashHigh = SourceHash.HashHigh},
			.DecoderId = std::string(AssetBuild::TextureDecoderId),
			.DecoderVersion = AssetBuild::TextureDecoderVersion};
		Texture.SourceContentHash = SourceHash.ToString();
		Texture.SourceFileSize = EncodedBytes.size();
		Texture.SourceLastWriteTime = 0;
		Texture.SourceWidth = CandidateSourceData.Width;
		Texture.SourceHeight = CandidateSourceData.Height;
		Texture.SourceChannelCount = CandidateSourceData.SourceChannelCount;
		Texture.bSourceHasTransparency = CandidateSourceData.bHasTransparency;
		Texture.SourceData = std::make_unique<FTextureSourceData>(std::move(CandidateSourceData));
		Texture.PlatformData = std::move(CandidatePlatformData);
		Texture.DerivedDataKey = NewKey;
		Texture.bLoadedFromDerivedDataCache = false;
		Texture.DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = Texture.DerivedDataKey,
			.Message = "Built Texture2D candidate from encoded source bytes.",
			.bSourceDecoderInvoked = true};
		Texture.BuildStatus = ETextureBuildStatus::Ready;
		Texture.LastBuildError.clear();
		Texture.QueueRenderResourceBuild();
		Texture.MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto FTextureBuildOperations::UpdateSourceFingerprint(
		DTexture2D& Texture,
		const std::filesystem::path& PhysicalFilePath) -> void
	{
		Texture.SourceFileSize = 0;
		Texture.SourceLastWriteTime = 0;
		std::error_code Error;
		const uint64 FileSize = std::filesystem::file_size(PhysicalFilePath, Error);
		if (Error) return;
		const std::filesystem::file_time_type LastWriteTime =
			std::filesystem::last_write_time(PhysicalFilePath, Error);
		if (Error) return;
		Texture.SourceFileSize = FileSize;
		Texture.SourceLastWriteTime =
			DerivedDataCache::FileTimeToStableTicks(LastWriteTime);
	}
}
