#include "Texture/Texture2D.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "SourceFingerprintCache.h"
#include "DerivedDataObjectStore.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "Hash/XxHash.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/SourcePath.h"
#include "DynamicRHI.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/TextureBuild.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	namespace
	{
		constexpr uint64 TextureDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureDerivedDataCleanupDeleteLimit = 16;
		constexpr std::string_view SourceAssetsRoot = "SourceAssets";
		constexpr std::string_view DefaultTextureSourceRoot = "SourceAssets/Textures";
		constexpr std::string_view TextureDecoderId = "DurinImage";
		constexpr uint32 TextureDecoderVersion = 1;

		auto GetTextureObjectStore() -> Asset::FDerivedDataObjectStore
		{
			return Asset::FDerivedDataObjectStore("Textures/Objects", MaximumTexturePayloadBytes);
		}

		auto IsCanonicalTextureHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}

		auto FindOwningMount(std::string_view VirtualPath) -> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto MakeCanonicalSourceLocation(
			const FAssetPath& AssetPath,
			std::string_view Extension,
			std::string_view RequestedSourcePath,
			std::filesystem::path& OutPhysicalPath,
			std::string& OutStoredPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.ToString());
			if (!Mount)
			{
				OutError = std::format("Texture asset {} is not beneath a registered content mount.",
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
				StoredPath = std::filesystem::path(SourceAssetsRoot) / Requested.RelativePath;
			}
			else
				StoredPath = std::filesystem::path(RequestedSourcePath);
			StoredPath = StoredPath.lexically_normal();
			const std::filesystem::path Relative = StoredPath.lexically_relative(SourceAssetsRoot);
			const std::string RelativeString = Relative.generic_string();
			if (Relative.empty() || Relative == "." || RelativeString == ".."
				|| RelativeString.starts_with("../"))
			{
				OutError = std::format(
					"Texture source path '{}' must be normalized beneath {}/.",
					RequestedSourcePath, SourceAssetsRoot);
				return false;
			}
			OutStoredPath = Mount->VirtualRoot + Relative.generic_string();
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
			const PathUtilities::FSourcePathResult Resolved =
				PathUtilities::ResolveSourcePath(
					OutStoredPath, PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
			{
				OutError = Resolved.Message;
				return false;
			}
			OutPhysicalPath = Resolved.PhysicalPath;
			return true;
		}

		auto ResolveTextureSource(
			const DTexture2D& Texture,
			std::filesystem::path& OutPath,
			std::string& OutError) -> bool
		{
			const FTexture2DSourceImportData& Provenance = Texture.GetSourceImportData();
			if (Provenance.HasSource())
			{
				if (Provenance.DecoderId != TextureDecoderId
					|| Provenance.DecoderVersion != TextureDecoderVersion)
				{
					OutError = std::format(
						"Texture source decoder {} version {} is unsupported.",
						Provenance.DecoderId, Provenance.DecoderVersion);
					return false;
				}
				if (!Texture.GetPackage())
				{
					OutError = "Texture source cannot be resolved without an owning package.";
					return false;
				}
				const PathUtilities::FMountPolicyResult Dependency =
					PathUtilities::CheckMountDependency(
						Texture.GetPackage()->GetPackagePath(),
						Provenance.Source.SourcePath.Path);
				if (!Dependency)
				{
					OutError = Dependency.Message;
					return false;
				}
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						Provenance.Source.SourcePath.Path,
						PathUtilities::EPathExistence::AllowMissing);
				if (!Resolved)
				{
					OutError = Resolved.Message;
					return false;
				}
				OutPath = Resolved.PhysicalPath;
				OutError.clear();
				return true;
			}
			OutError = "Texture asset has no normalized SourceAssets provenance.";
			return false;
		}

		auto HashTextureSource(
			const std::filesystem::path& Path,
			FXxHash128& OutHash,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutError = std::format("Failed to read texture source file: {}", Path.generic_string());
				return false;
			}
			OutHash = FXxHash128::HashBuffer(Bytes);
			OutError.clear();
			return true;
		}

		auto MakeTextureDerivedDataKey(const DTexture2D& Texture, std::string& OutKey, std::string& OutError) -> bool
		{
			FXxHash128 SourceHash;
			if (Texture.GetSourceImportData().Source.HasContentHash())
			{
				const FTextureSourceFile& Source = Texture.GetSourceImportData().Source;
				SourceHash.HashLow = Source.SourceContentHashLow;
				SourceHash.HashHigh = Source.SourceContentHashHigh;
			}
			else if (IsCanonicalTextureHash(Texture.GetSourceContentHash()))
			{
				SourceHash = FXxHash128::FromString(Texture.GetSourceContentHash());
			}
			else
			{
				OutError = "Texture source content hash is missing or invalid.";
				return false;
			}
			OutKey = BuildTexture2DDerivedDataKey({
				.SourceContentHash = SourceHash,
				.Usage = Texture.GetUsage(),
				.bSRGB = Texture.IsSRGB(),
				.CompressionQuality = Texture.GetCompressionQuality(),
				.AlphaMipMode = Texture.GetAlphaMipMode(),
				.MaximumResolution = Texture.GetMaxResolution(),
				.AlphaCoverageThreshold = Texture.GetAlphaCoverageThreshold(),
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			OutError.clear();
			return true;
		}

		auto LoadTextureDerivedData(std::string_view Key,
			std::unique_ptr<FTexturePlatformData>& OutPlatformData,
			ETextureDerivedDataStatus& OutStatus,
			std::string& OutMessage) -> bool
		{
			std::vector<uint8> Bytes;
			const Asset::FDerivedDataObjectReadResult Read = GetTextureObjectStore().Read(Key, Bytes);
			if (!Read)
			{
				OutStatus = Read.Status == Asset::EDerivedDataObjectReadStatus::Missing
					? ETextureDerivedDataStatus::Missing
					: ETextureDerivedDataStatus::Corrupt;
				OutMessage = Read.Message;
				return false;
			}
			std::string Error;
			if (!DecodeTexture2DPayload(
				Bytes, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game,
				OutPlatformData, Error))
			{
				OutStatus = Error.find("unsupported") != std::string::npos
					? ETextureDerivedDataStatus::Incompatible
					: ETextureDerivedDataStatus::Corrupt;
				OutMessage = std::move(Error);
				return false;
			}
			OutStatus = ETextureDerivedDataStatus::Hit;
			OutMessage.clear();
			return true;
		}

		auto StoreTextureDerivedData(
			std::string_view Key,
			const FTexturePlatformData& PlatformData,
			std::string& OutError,
			bool bRunCleanup = true) -> bool
		{
			std::vector<uint8> Bytes;
			if (!EncodeTexture2DPayload(
				PlatformData, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game,
				Bytes, OutError)
				|| !GetTextureObjectStore().Write(Key, Bytes, &OutError)) return false;
			if (!bRunCleanup) return true;
			const Asset::FDerivedDataObjectCleanupResult Cleanup = GetTextureObjectStore().CleanupToBudget(
				TextureDerivedDataBudgetBytes, TextureDerivedDataCleanupDeleteLimit);
			if (!Cleanup.Message.empty()) DURIN_WARN("Texture2D DDC cleanup: {}", Cleanup.Message);
			return true;
		}
	} // namespace

	auto FTextureSourceData::IsValid() const -> bool
	{
		return Format == ETextureSourceFormat::RGBA8
			&& Width > 0
			&& Height > 0
			&& static_cast<uint64>(Width) * Height * TextureBuild::ChannelCount == Pixels.size();
	}

	auto FTexture2DMipData::IsValid(EPixelFormat PixelFormat) const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown || Width == 0 || Height == 0) return false;
		const FPixelFormatLayout Layout = GetPixelFormatLayout(PixelFormat, Width, Height);
		return Layout.DataSize > 0 && RowPitch == Layout.RowPitch && Pixels.size() == Layout.DataSize;
	}

	auto FTexturePlatformData::IsValid() const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown || Mips.empty() || Mips.size() > std::numeric_limits<uint8>::max()) return false;
		for (size_t MipIndex = 0; MipIndex < Mips.size(); ++MipIndex)
		{
			if (!Mips[MipIndex].IsValid(PixelFormat)) return false;
			if (MipIndex > 0)
			{
				const FTexture2DMipData& Previous = Mips[MipIndex - 1];
				if (Mips[MipIndex].Width != std::max(Previous.Width / 2, 1u)
					|| Mips[MipIndex].Height != std::max(Previous.Height / 2, 1u)) return false;
			}
		}
		return true;
	}

	DTexture2D::DTexture2D(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, TextureReference(std::make_unique<FTextureReference>())
		, RenderCompletion(
			std::make_shared<FTextureResourceCompletion>())
	{
		static const bool RegisteredAssetContributors = [] {
			Asset::RegisterAssetMoveContributor(DTexture2D::StaticClass(), [](DObject*, const FAssetPath&, const FAssetPath&,
				Asset::FAssetMoveContribution&) -> Asset::FAssetResult {
				// Portable SourceAssets provenance is independent of package placement.
				return {};
			});
			Asset::RegisterAssetDeleteContributor(DTexture2D::StaticClass(), [](const Asset::FAssetData&,
				const Asset::FAssetPackageInspection&, Asset::FAssetDeleteContribution&) -> Asset::FAssetResult {
				// Portable SourceAssets may be shared and require a separate, explicit source operation.
				return {};
			});
			return true;
		}();
		(void)RegisteredAssetContributors;
	}

	DTexture2D::~DTexture2D()
	{
		RenderCompletion->BeginRequest(++BuildRevision);
		if (RenderResource)
		{
			RenderResource->PrepareForRelease(BuildRevision);
			FTexture2DResource* Resource = RenderResource.get();
			Resource->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(RenderResource)));
		}
		if (bTextureReferenceInitializationQueued)
		{
			FTextureReference* Reference = TextureReference.get();
			Reference->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(TextureReference)));
		}
	}

	auto DTexture2D::GetTextureReferenceRHI() const
		-> FRHITextureReferenceRef
	{
		return TextureReference
			? TextureReference->GetTextureReferenceRHI()
			: FRHITextureReferenceRef{};
	}

	auto DTexture2D::GetRenderResourceState() const
		-> ERenderResourceState
	{
		return RenderCompletion->GetResourceState();
	}

	auto DTexture2D::GetAppliedRenderRevision() const -> uint64
	{
		return RenderCompletion->GetAppliedRevision();
	}

	auto DTexture2D::InvalidatePlatformData() -> void
	{
		PlatformData.reset();
		// Preserve specific failure states set by callers (DecodeFailure, BuildFailure, etc.).
		// Only downgrade Ready to Unbuilt; a caller that wants a specific failure status
		// sets it after this call.
		if (BuildStatus == ETextureBuildStatus::Ready)
		{
			BuildStatus = ETextureBuildStatus::Unbuilt;
			LastBuildError.clear();
		}
		RenderCompletion->BeginRequest(++BuildRevision);
		if (RenderResource)
		{
			RenderResource->PrepareForRelease(BuildRevision);
			FTexture2DResource* Resource = RenderResource.get();
			Resource->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(RenderResource)));
		}
		else
		{
			RenderCompletion->MarkReleased(BuildRevision);
		}
	}

	auto DTexture2D::QueueRenderResourceBuild() -> void
	{
		check(PlatformData && PlatformData->IsValid());
		const uint64 Revision = ++BuildRevision;
		const std::string OwnerDiagnostic = GetPackage()
			? GetPackage()->GetPackagePath()
			: "<transient DTexture2D>";
		RenderCompletion->BeginRequest(Revision);
		if (GDynamicRHI == nullptr) return;
		if (!bTextureReferenceInitializationQueued)
		{
			TextureReference->SetLifetimeDiagnostic(OwnerDiagnostic);
			TextureReference->BeginInit_GameThread();
			bTextureReferenceInitializationQueued = true;
		}

		auto Candidate = std::make_unique<FTexture2DResource>(
			TextureReference.get(),
			std::make_shared<const FTexturePlatformData>(*PlatformData),
			Revision,
			RenderCompletion);
		Candidate->SetLifetimeDiagnostic(OwnerDiagnostic, Revision);
		FTexture2DResource* CandidateView = Candidate.get();
		std::unique_ptr<FTexture2DResource> Previous =
			std::move(RenderResource);
		RenderResource = std::move(Candidate);
		CandidateView->BeginInit_GameThread();
		if (Previous)
		{
			FTexture2DResource* PreviousView = Previous.get();
			PreviousView->BeginRelease_GameThread();
			BeginCleanupRenderResource(
				FDeferredRenderResourceCleanup(std::move(Previous)));
		}
	}

	auto DTexture2D::DecodeSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool
	{
		std::vector<uint8> SourceBytes;
		if (!FFileHelper::LoadFileToArray(SourceBytes, PhysicalFilePath))
		{
			OutError = std::format("Failed to read texture source file: {}", PhysicalFilePath);
			return false;
		}

		auto NewSourceData = std::make_unique<FTextureSourceData>();
		if (!TextureBuild::DecodeRGBA8(PhysicalFilePath, *NewSourceData, OutError))
		{
			return false;
		}

		const FXxHash128 SourceHash = FXxHash128::HashBuffer(SourceBytes);
		SourceContentHash = SourceHash.ToString();
		if (SourceImportData.HasSource())
		{
			SourceImportData.Source.SourceContentHashLow = SourceHash.HashLow;
			SourceImportData.Source.SourceContentHashHigh = SourceHash.HashHigh;
		}
		UpdateSourceFingerprint(std::filesystem::path(PhysicalFilePath));
		SourceWidth = NewSourceData->Width;
		SourceHeight = NewSourceData->Height;
		SourceChannelCount = NewSourceData->SourceChannelCount;
		bSourceHasTransparency = NewSourceData->bHasTransparency;
		SourceData = std::move(NewSourceData);
		return true;
	}

	auto DTexture2D::UpdateSourceFingerprint(const std::filesystem::path& PhysicalFilePath) -> void
	{
		SourceFileSize = 0;
		SourceLastWriteTime = 0;
		std::error_code Error;
		const uint64 FileSize = std::filesystem::file_size(PhysicalFilePath, Error);
		if (Error) return;
		const std::filesystem::file_time_type LastWriteTime = std::filesystem::last_write_time(PhysicalFilePath, Error);
		if (Error) return;
		SourceFileSize = FileSize;
		SourceLastWriteTime = DerivedDataCache::FileTimeToStableTicks(LastWriteTime);
	}

	auto DTexture2D::BuildSourceData(std::string_view PhysicalFilePath, std::string& OutError) -> bool
	{
		if (!DecodeSourceData(PhysicalFilePath, OutError))
		{
			BuildStatus = ETextureBuildStatus::DecodeFailure;
			LastBuildError = OutError;
			return false;
		}
		const bool bSucceeded = RebuildPlatformData(OutError);
		DerivedDataDiagnostic.bSourceDecoderInvoked = true;
		return bSucceeded;
	}

	auto DTexture2D::EnsureSourceData(std::string& OutError) -> bool
	{
		if (SourceData && SourceData->IsValid()) return true;
		if (!SourceImportData.HasSource())
		{
			OutError = SourceFile.empty()
				? "Texture asset has no source file."
				: "Legacy texture source metadata is unsupported. Reimport the asset to create normalized SourceAssets provenance.";
			return false;
		}
		std::filesystem::path PhysicalPath;
		if (!ResolveTextureSource(*this, PhysicalPath, OutError)) return false;
		if (!std::filesystem::is_regular_file(PhysicalPath))
		{
			OutError = std::format("Texture source file does not exist: {}", GetSourceFile());
			return false;
		}
		return DecodeSourceData(PhysicalPath.generic_string(), OutError);
	}

	auto DTexture2D::RebuildPlatformData(std::string& OutError) -> bool
	{
		if (!EnsureSourceData(OutError)) return false;
		std::unique_ptr<FTexturePlatformData> NewPlatformData;
		if (!BuildPlatformData(Usage, bSRGB, MaxResolution, CompressionQuality, AlphaMipMode,
			AlphaCoverageThreshold, NewPlatformData, OutError))
		{
			// Classify the failure: UnsupportedFormat when the pixel-format selector
			// returns Unknown; everything else is a general BuildFailure.
			const bool bHasTransparency = SourceData ? SourceData->bHasTransparency : false;
			if (TextureBuild::SelectPixelFormat(Usage, bSRGB, bHasTransparency) == EPixelFormat::Unknown)
				BuildStatus = ETextureBuildStatus::UnsupportedFormat;
			else
				BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			return false;
		}
		std::string NewKey;
		if (!MakeTextureDerivedDataKey(*this, NewKey, OutError))
		{
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			return false;
		}
		if (!StoreTextureDerivedData(NewKey, *NewPlatformData, OutError))
		{
			DerivedDataDiagnostic = {
				.Status = ETextureDerivedDataStatus::WriteFailure,
				.Key = NewKey,
				.Message = std::format("Texture2D DDC write failed for key {}: {}", NewKey, OutError)};
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = DerivedDataDiagnostic.Message;
			OutError = LastBuildError;
			return false;
		}
		PlatformData = std::move(NewPlatformData);
		DerivedDataKey = std::move(NewKey);
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = std::format("Rebuilt Texture2D and stored DDC key {}.", DerivedDataKey)};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		return true;
	}

	auto DTexture2D::BuildPlatformData(ETextureUsage InUsage, bool bInSRGB, uint32 InMaxResolution,
		ETextureCompressionQuality InCompressionQuality, ETextureAlphaMipMode InAlphaMipMode,
		float InAlphaCoverageThreshold,
		std::unique_ptr<FTexturePlatformData>& OutPlatformData, std::string& OutError) const -> bool
	{
		OutError.clear();
		OutPlatformData.reset();
		if (!SourceData || !SourceData->IsValid())
		{
			OutError = "Texture source data is unavailable or invalid.";
			return false;
		}
		auto NewPlatformData = std::make_unique<FTexturePlatformData>();
		if (!TextureBuild::BuildMipChain(*SourceData, InUsage, bInSRGB, *NewPlatformData, OutError,
			InMaxResolution, InCompressionQuality, InAlphaMipMode, InAlphaCoverageThreshold)) return false;
		OutPlatformData = std::move(NewPlatformData);
		return true;
	}

	auto DTexture2D::SetUsage(ETextureUsage InUsage, std::string& OutError) -> bool
	{
		OutError.clear();
		if (!TextureBuild::IsValidUsage(InUsage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}
		if (Usage == InUsage) return true;
		if (!EnsureSourceData(OutError)) return false;
		const ETextureUsage PreviousUsage = Usage;
		const bool bPreviousSRGB = bSRGB;
		Usage = InUsage;
		bSRGB = TextureBuild::GetDefaultSRGB(Usage);
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		Usage = PreviousUsage;
		bSRGB = bPreviousSRGB;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::SetSRGB(bool bInSRGB, std::string& OutError) -> bool
	{
		OutError.clear();
		if (bSRGB == bInSRGB) return true;
		if (!EnsureSourceData(OutError)) return false;
		const bool bPreviousSRGB = bSRGB;
		bSRGB = bInSRGB;
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		bSRGB = bPreviousSRGB;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::SetMaxResolution(uint32 InMaxResolution, std::string& OutError) -> bool
	{
		OutError.clear();
		if (MaxResolution == InMaxResolution) return true;
		if (!EnsureSourceData(OutError)) return false;
		const uint32 PreviousMaxResolution = MaxResolution;
		MaxResolution = InMaxResolution;
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		MaxResolution = PreviousMaxResolution;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::SetCompressionQuality(ETextureCompressionQuality InQuality, std::string& OutError) -> bool
	{
		OutError.clear();
		if (!TextureBuild::IsValidCompressionQuality(InQuality))
		{
			OutError = "Texture compression quality is invalid.";
			return false;
		}
		if (CompressionQuality == InQuality) return true;
		if (!EnsureSourceData(OutError)) return false;
		const ETextureCompressionQuality PreviousQuality = CompressionQuality;
		CompressionQuality = InQuality;
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		CompressionQuality = PreviousQuality;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::SetAlphaMipMode(ETextureAlphaMipMode InMode, std::string& OutError) -> bool
	{
		OutError.clear();
		if (!TextureBuild::IsValidAlphaMipMode(InMode))
		{
			OutError = "Texture alpha mip mode is invalid.";
			return false;
		}
		if (AlphaMipMode == InMode) return true;
		if (!EnsureSourceData(OutError)) return false;
		const ETextureAlphaMipMode PreviousMode = AlphaMipMode;
		AlphaMipMode = InMode;
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		AlphaMipMode = PreviousMode;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::SetAlphaCoverageThreshold(float InThreshold, std::string& OutError) -> bool
	{
		OutError.clear();
		if (!TextureBuild::IsValidAlphaCoverageThreshold(InThreshold))
		{
			OutError = "Texture alpha coverage threshold must be greater than zero and less than one.";
			return false;
		}
		if (AlphaCoverageThreshold == InThreshold) return true;
		if (!EnsureSourceData(OutError)) return false;
		const float PreviousThreshold = AlphaCoverageThreshold;
		AlphaCoverageThreshold = InThreshold;
		if (RebuildPlatformData(OutError))
		{
			MarkPackageDirty();
			return true;
		}
		AlphaCoverageThreshold = PreviousThreshold;
		std::string RestoreError;
		RebuildPlatformData(RestoreError);
		return false;
	}

	auto DTexture2D::InspectSource() const -> FTextureSourceDiagnostic
	{
		if (!SourceImportData.HasSource())
		{
			if (SourceFile.empty()) return {};
			return {
				ETextureSourceStatus::Invalid,
				{},
				"Legacy texture source metadata is unsupported. Reimport the asset to create normalized SourceAssets provenance."};
		}
		std::filesystem::path PhysicalPath;
		std::string Error;
		if (!ResolveTextureSource(*this, PhysicalPath, Error))
			return {ETextureSourceStatus::Invalid, {}, std::move(Error)};
		if (!std::filesystem::is_regular_file(PhysicalPath))
		{
			return {
				ETextureSourceStatus::Missing,
				PhysicalPath.generic_string(),
				std::format(
					"Texture source is missing: {}. Use source-path repair to select its replacement.",
					SourceImportData.Source.SourcePath.Path)};
		}
		FXxHash128 CurrentHash;
		if (!HashTextureSource(PhysicalPath, CurrentHash, Error))
			return {
				ETextureSourceStatus::Invalid,
				PhysicalPath.generic_string(),
				std::move(Error)};
		if (SourceImportData.Source.HasContentHash()
			&& (CurrentHash.HashLow
					!= SourceImportData.Source.SourceContentHashLow
				|| CurrentHash.HashHigh
					!= SourceImportData.Source.SourceContentHashHigh))
		{
			return {
				ETextureSourceStatus::Changed,
				PhysicalPath.generic_string(),
				"The mounted source bytes changed since this asset was last imported. "
				"Reimport updates this asset from the persisted source."};
		}
		return {ETextureSourceStatus::Available, PhysicalPath.generic_string(), {}};
	}

	auto DTexture2D::ReimportSource(std::string_view FilePath, std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FMountedSourceFile MountedSource;
		if (!ResolveMountedSourceReference(
			GetPackage()->GetPackagePath(),
			SourceImportData.Source.SourcePath.Path,
			MountedSource, OutError)) return false;
		const std::filesystem::path Input = MountedSource.PhysicalPath;
		if (!FilePath.empty())
		{
			std::error_code EquivalentError;
			const std::filesystem::path Requested =
				std::filesystem::absolute(FilePath).lexically_normal();
			if (!std::filesystem::equivalent(Input, Requested, EquivalentError)
				|| EquivalentError)
			{
				OutError =
					"Reimport is read-only and must use the persisted mounted source. "
					"Use ChangeSourceReference or IngestAndChangeSource first.";
				return false;
			}
		}
		if (!Asset::IsSupportedImageExtension(Input.extension().generic_string()))
		{
			OutError = "Unsupported texture source format.";
			return false;
		}

		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(GetPackage()->GetPackagePath(), AssetPath, &OutError)) return false;
		const std::filesystem::path Destination = MountedSource.PhysicalPath;
		std::string StoredPath = MountedSource.SourcePath.Path;

		FXxHash128 SourceHash;
		if (!HashTextureSource(Input, SourceHash, OutError)) return false;
		FTextureSourceData CandidateSourceData;
		if (!TextureBuild::DecodeRGBA8(Input.generic_string(), CandidateSourceData, OutError)) return false;
		auto CandidatePlatformData = std::make_unique<FTexturePlatformData>();
		if (!TextureBuild::BuildMipChain(
			CandidateSourceData, Usage, bSRGB, *CandidatePlatformData, OutError,
			MaxResolution, CompressionQuality, AlphaMipMode, AlphaCoverageThreshold)) return false;
		const std::string NewKey = BuildTexture2DDerivedDataKey({
			.SourceContentHash = SourceHash,
			.Usage = Usage,
			.bSRGB = bSRGB,
			.CompressionQuality = CompressionQuality,
			.AlphaMipMode = AlphaMipMode,
			.MaximumResolution = MaxResolution,
			.AlphaCoverageThreshold = AlphaCoverageThreshold,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (!StoreTextureDerivedData(NewKey, *CandidatePlatformData, OutError)) return false;

		SourceImportData = {
			.Source = {
				.SourcePath = {.Path = std::move(StoredPath)},
				.SourceContentHashLow = SourceHash.HashLow,
				.SourceContentHashHigh = SourceHash.HashHigh},
			.DecoderId = std::string(TextureDecoderId),
			.DecoderVersion = TextureDecoderVersion};
		SourceFile.clear();
		SourceContentHash = SourceHash.ToString();
		UpdateSourceFingerprint(Destination);
		SourceWidth = CandidateSourceData.Width;
		SourceHeight = CandidateSourceData.Height;
		SourceChannelCount = CandidateSourceData.SourceChannelCount;
		bSourceHasTransparency = CandidateSourceData.bHasTransparency;
		SourceData = std::make_unique<FTextureSourceData>(std::move(CandidateSourceData));
		PlatformData = std::move(CandidatePlatformData);
		DerivedDataKey = NewKey;
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = "Reimported Texture2D source and populated the DDC.",
			.bSourceDecoderInvoked = true};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DTexture2D::RepairSourcePath(std::string_view FilePath, std::string& OutError) -> bool
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
		return ChangeSourceReference(Classified.NormalizedVirtualPath, OutError);
	}

	auto DTexture2D::ChangeSourceReference(
		std::string_view SourceVirtualPath, std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FMountedSourceFile Source;
		if (!ResolveMountedSourceReference(
			GetPackage()->GetPackagePath(), SourceVirtualPath, Source, OutError))
			return false;
		const FTexture2DSourceImportData Previous = SourceImportData;
		SourceImportData.Source.SourcePath = Source.SourcePath;
		if (!ReimportSource(Source.PhysicalPath.generic_string(), OutError))
		{
			SourceImportData = Previous;
			return false;
		}
		return true;
	}

	auto DTexture2D::IngestAndChangeSource(
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		FMountedSourceFile Source;
		if (!PrepareMountedSourceFile(
			FilePath, GetPackage()->GetPackagePath(),
			TargetSourceVirtualPath, Source, OutError)) return false;
		const bool bChanged = ChangeSourceReference(Source.SourcePath.Path, OutError);
		if (bChanged)
			CommitMountedSourceFile(Source);
		else
			RollbackMountedSourceFile(Source);
		return bChanged;
	}

	auto DTexture2D::ChangeSourceLocation(
		std::string_view SourceDestination, std::string& OutError) -> bool
	{
		if (!GetPackage())
		{
			OutError = "Only packaged textures can retain source provenance.";
			return false;
		}
		if (SourceDestination.empty())
		{
			OutError = "Choose a source destination beneath SourceAssets.";
			return false;
		}

		std::filesystem::path CurrentSource;
		if (!ResolveTextureSource(*this, CurrentSource, OutError)) return false;
		if (!std::filesystem::is_regular_file(CurrentSource))
		{
			OutError = std::format(
				"Texture source is missing: {}. Reimport the source before changing its location.",
				CurrentSource.generic_string());
			return false;
		}

		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(GetPackage()->GetPackagePath(), AssetPath, &OutError))
			return false;
		std::filesystem::path Destination;
		std::string StoredPath;
		if (!MakeCanonicalSourceLocation(
			AssetPath, CurrentSource.extension().generic_string(), SourceDestination,
			Destination, StoredPath, OutError)) return false;
		if (Destination == CurrentSource)
		{
			OutError.clear();
			return true;
		}
		if (std::filesystem::exists(Destination))
		{
			OutError = std::format(
				"Texture source destination already exists: {}. Choose a new location to avoid overwriting another asset's source.",
				Destination.generic_string());
			return false;
		}

		std::error_code Error;
		std::filesystem::create_directories(Destination.parent_path(), Error);
		if (Error)
		{
			OutError = std::format(
				"Failed to create texture source directory {}: {}",
				Destination.parent_path().generic_string(), Error.message());
			return false;
		}
		if (!std::filesystem::copy_file(CurrentSource, Destination, Error))
		{
			OutError = std::format(
				"Failed to copy texture source to {}: {}",
				Destination.generic_string(), Error.message());
			return false;
		}

		SourceImportData.Source.SourcePath.Path = std::move(StoredPath);
		UpdateSourceFingerprint(Destination);
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DTexture2D::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedPlatformData(OutError);

		BuildStatus = ETextureBuildStatus::Unbuilt;
		LastBuildError.clear();
		DerivedDataKey.clear();
		DerivedDataDiagnostic = {};
		bLoadedFromDerivedDataCache = false;
		if (!SourceImportData.HasSource())
		{
			OutError = SourceFile.empty()
				? "Texture asset has no source file."
				: "Legacy texture source metadata is unsupported. Reimport the asset to create normalized SourceAssets provenance.";
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
			DerivedDataDiagnostic.Message = OutError;
			BuildStatus = ETextureBuildStatus::MissingSource;
			LastBuildError = OutError;
			return false;
		}
		std::filesystem::path PhysicalPath;
		if (!ResolveTextureSource(*this, PhysicalPath, OutError))
		{
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::Incompatible;
			DerivedDataDiagnostic.Message = OutError;
			BuildStatus = ETextureBuildStatus::MissingSource;
			LastBuildError = OutError;
			return false;
		}
		const bool bSourceAvailable = std::filesystem::is_regular_file(PhysicalPath);
		std::error_code FingerprintError;
		uint64 CurrentFileSize = 0;
		std::filesystem::file_time_type CurrentLastWriteTime;
		if (bSourceAvailable)
		{
			CurrentFileSize = std::filesystem::file_size(PhysicalPath, FingerprintError);
			CurrentLastWriteTime = std::filesystem::last_write_time(PhysicalPath, FingerprintError);
		}
		const bool bSourceFingerprintMatches = bSourceAvailable && !FingerprintError
			&& CurrentFileSize == SourceFileSize
			&& DerivedDataCache::FileTimeToStableTicks(CurrentLastWriteTime) == SourceLastWriteTime;
		const int64 CurrentLastWriteTimeTicks = bSourceAvailable && !FingerprintError
			? DerivedDataCache::FileTimeToStableTicks(CurrentLastWriteTime)
			: 0;
		const bool bHasPersistedIdentity = SourceImportData.Source.HasContentHash()
			|| IsCanonicalTextureHash(SourceContentHash);
		bool bSourceContentMatches = bSourceFingerprintMatches;
		if (bHasPersistedIdentity && bSourceAvailable && !FingerprintError && !bSourceFingerprintMatches)
		{
			std::string CurrentContentHash;
			if (!Asset::FindSourceFingerprint(
					PhysicalPath, CurrentFileSize, CurrentLastWriteTimeTicks, CurrentContentHash))
			{
				FXxHash128 CurrentHash;
				if (!HashTextureSource(PhysicalPath, CurrentHash, OutError)) return false;
				CurrentContentHash = CurrentHash.ToString();
				Asset::StoreSourceFingerprint(PhysicalPath, {
					.FileSize = CurrentFileSize,
					.LastWriteTimeTicks = CurrentLastWriteTimeTicks,
					.ContentHash = CurrentContentHash});
			}
			const std::string PersistedContentHash = SourceImportData.Source.HasContentHash()
				? FXxHash128{
					.HashLow = SourceImportData.Source.SourceContentHashLow,
					.HashHigh = SourceImportData.Source.SourceContentHashHigh}.ToString()
				: SourceContentHash;
			bSourceContentMatches = CurrentContentHash == PersistedContentHash;
			if (bSourceContentMatches)
			{
				SourceFileSize = CurrentFileSize;
				SourceLastWriteTime = CurrentLastWriteTimeTicks;
			}
		}
		if (bHasPersistedIdentity
			&& (!bSourceAvailable || bSourceContentMatches))
		{
			if (!MakeTextureDerivedDataKey(*this, DerivedDataKey, OutError))
			{
				DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::Incompatible;
				DerivedDataDiagnostic.Message = OutError;
				return false;
			}
			std::unique_ptr<FTexturePlatformData> CachedPlatformData;
			ETextureDerivedDataStatus CacheStatus = ETextureDerivedDataStatus::Missing;
			std::string CacheMessage;
			if (LoadTextureDerivedData(
				DerivedDataKey, CachedPlatformData, CacheStatus, CacheMessage))
			{
				SourceData.reset();
				PlatformData = std::move(CachedPlatformData);
				BuildStatus = ETextureBuildStatus::Ready;
				bLoadedFromDerivedDataCache = true;
				DerivedDataDiagnostic = {
					.Status = bSourceAvailable
						? ETextureDerivedDataStatus::Hit
						: ETextureDerivedDataStatus::SourceUnavailableCached,
					.Key = DerivedDataKey,
					.Message = bSourceAvailable
						? std::format("Texture2D DDC hit for key {}.", DerivedDataKey)
						: std::format(
							"Texture2D source is unavailable, but cached key {} loaded successfully.",
							DerivedDataKey)};
				QueueRenderResourceBuild();
				OutError.clear();
				return true;
			}
			DerivedDataDiagnostic = {
				.Status = CacheStatus,
				.Key = DerivedDataKey,
				.Message = std::format("Texture2D DDC miss for key {}: {}", DerivedDataKey, CacheMessage)};
			if (!bSourceAvailable)
			{
				DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
				DerivedDataDiagnostic.Message = std::format(
					"Texture source file does not exist: {}. Cached payload was unavailable: {}",
					GetSourceFile(), CacheMessage);
				OutError = DerivedDataDiagnostic.Message;
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				return false;
			}
		}
		else if (!bSourceAvailable)
		{
			OutError = std::format("Texture source file does not exist: {}", GetSourceFile());
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
			DerivedDataDiagnostic.Message = OutError;
			BuildStatus = ETextureBuildStatus::MissingSource;
			LastBuildError = OutError;
			return false;
		}
		const bool bMetadataChanged = !bHasPersistedIdentity
			|| !bSourceContentMatches;
		if (!BuildSourceData(PhysicalPath.generic_string(), OutError)) return false;
		Asset::StoreSourceFingerprint(PhysicalPath, {
			.FileSize = SourceFileSize,
			.LastWriteTimeTicks = SourceLastWriteTime,
			.ContentHash = SourceContentHash});
		if (bMetadataChanged)
		{
			MarkPackageDirty();
			Asset::ReportAssetLoadMutation(
				this,
				"Engine.Texture2D.SourceIdentity",
				"Texture source identity metadata was reconciled during load.");
		}
		return true;
	}

	auto DTexture2D::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::CookedFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Cooked Texture2D '{}': {}", GetObjectPath(), Message);
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = DerivedDataDiagnostic.Message;
			OutError = LastBuildError;
			return false;
		};

		if (CookedPayload.PayloadId != Texture2DPrimaryCookedPayloadId
			|| CookedPayload.LocationKind
				!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != TexturePayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
		{
			return FailCooked("required TXPL descriptor is missing or incompatible.");
		}

		const Asset::FPackageLoadContext& LoadContext = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage()
			|| !Asset::ResolveCookedPackagePath(
				LoadContext.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				LoadContext.CookRoot, PackagePath, CompanionPath, &OutError))
		{
			return FailCooked(
				OutError.empty() ? "package companion path could not be resolved." : OutError);
		}

		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game,
			Container,
			&OutError))
		{
			return FailCooked(OutError);
		}
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);

		std::unique_ptr<FTexturePlatformData> CandidatePlatformData;
		if (!DecodeTexture2DPayload(
			Bytes,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game,
			CandidatePlatformData,
			OutError))
		{
			return FailCooked(OutError);
		}

		SourceData.reset();
		PlatformData = std::move(CandidatePlatformData);
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::CookedLoaded,
			.Message = std::format("Loaded cooked Texture2D payload for '{}'.", GetObjectPath())};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DTexture2D::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticSourceMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"Texture2D '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}

		std::string ExpectedKey;
		if (!MakeTextureDerivedDataKey(*this, ExpectedKey, OutError))
		{
			OutError = std::format("Failed to cook Texture2D '{}': {}", GetObjectPath(), OutError);
			return false;
		}

		std::vector<uint8> PayloadBytes;
		std::unique_ptr<FTexturePlatformData> ValidatedPlatformData;
		const Asset::FDerivedDataObjectReadResult Read =
			GetTextureObjectStore().Read(ExpectedKey, PayloadBytes);
		if (!Read
			|| !DecodeTexture2DPayload(
				PayloadBytes,
				Asset::ECookTargetPlatform::Win64,
				Asset::ECookTargetProfile::Game,
				ValidatedPlatformData,
				OutError))
		{
			if (!PlatformData && !PostLoad(OutError))
			{
				OutError = std::format("Failed to cook Texture2D '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			if (!PlatformData
				|| !EncodeTexture2DPayload(
					*PlatformData,
					Asset::ECookTargetPlatform::Win64,
					Asset::ECookTargetProfile::Game,
					PayloadBytes,
					OutError))
			{
				OutError = std::format("Failed to cook Texture2D '{}': {}", GetObjectPath(), OutError);
				return false;
			}
		}

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = Texture2DPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = TexturePayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = TexturePayloadAlignment,
			.Bytes = std::move(PayloadBytes)};

		return Context.AddPackage(
			std::string(VirtualPackagePath),
			{std::move(BulkPayload)},
			[this, bRetainDiagnosticSourceMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != Texture2DPrimaryCookedPayloadId)
				{
					if (Error) *Error = "Texture2D cook did not produce its required descriptor.";
					return false;
				}

				const std::string SavedSourceFile = SourceFile;
				const FTexture2DSourceImportData SavedSourceImportData = SourceImportData;
				const std::string SavedSourceContentHash = SourceContentHash;
				const uint64 SavedSourceFileSize = SourceFileSize;
				const int64 SavedSourceLastWriteTime = SourceLastWriteTime;
				const uint32 SavedSourceWidth = SourceWidth;
				const uint32 SavedSourceHeight = SourceHeight;
				const uint8 SavedSourceChannelCount = SourceChannelCount;
				const bool bSavedSourceHasTransparency = bSourceHasTransparency;
				const Asset::FCookedPayloadDescriptor SavedCookedPayload = CookedPayload;
				CookedPayload = Descriptors.front();
				if (!bRetainDiagnosticSourceMetadata)
				{
					SourceFile.clear();
					SourceImportData = {};
					SourceContentHash.clear();
					SourceFileSize = 0;
					SourceLastWriteTime = 0;
					SourceWidth = 0;
					SourceHeight = 0;
					SourceChannelCount = 0;
					bSourceHasTransparency = false;
				}

				Asset::FAssetPackageSerializationOptions SerializationOptions;
				if (!bRetainDiagnosticSourceMetadata)
				{
					SerializationOptions.PropertyFilter = [this](
						const DObject* Object, const FProperty* Property) {
						if (Object != this) return true;
						const FName Name = Property->NamePrivate;
						return Name != FName("SourceFile")
							&& Name != FName("SourceImportData")
							&& Name != FName("SourceContentHash")
							&& Name != FName("SourceFileSize")
							&& Name != FName("SourceLastWriteTime")
							&& Name != FName("SourceWidth")
							&& Name != FName("SourceHeight")
							&& Name != FName("SourceChannelCount")
							&& Name != FName("bSourceHasTransparency");
					};
				}
				const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, SerializationOptions);
				SourceFile = SavedSourceFile;
				SourceImportData = SavedSourceImportData;
				SourceContentHash = SavedSourceContentHash;
				SourceFileSize = SavedSourceFileSize;
				SourceLastWriteTime = SavedSourceLastWriteTime;
				SourceWidth = SavedSourceWidth;
				SourceHeight = SavedSourceHeight;
				SourceChannelCount = SavedSourceChannelCount;
				bSourceHasTransparency = bSavedSourceHasTransparency;
				CookedPayload = SavedCookedPayload;
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			},
			&OutError);
	}

	auto DTexture2D::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || !Proposal.DraftRootProperty || !Proposal.DraftRootContainer) return true;

		ETextureUsage CandidateUsage = Usage;
		bool bCandidateSRGB = bSRGB;
		uint32 CandidateMaxResolution = MaxResolution;
		ETextureCompressionQuality CandidateCompressionQuality = CompressionQuality;
		ETextureAlphaMipMode CandidateAlphaMipMode = AlphaMipMode;
		float CandidateAlphaCoverageThreshold = AlphaCoverageThreshold;
		const FName PropertyName = Proposal.MemberProperty->NamePrivate;
		if (PropertyName == FName("Usage"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
			{
				OutError = "The texture usage metadata is unavailable.";
				return false;
			}
			CandidateUsage = static_cast<ETextureUsage>(static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
			bCandidateSRGB = TextureBuild::GetDefaultSRGB(CandidateUsage);
		}
		else if (PropertyName == FName("bSRGB"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Bool)
			{
				OutError = "The texture color-space metadata is unavailable.";
				return false;
			}
			bCandidateSRGB = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<bool>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		}
		else if (PropertyName == FName("MaxResolution"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::UInt32)
			{
				OutError = "The texture maximum-resolution metadata is unavailable.";
				return false;
			}
			CandidateMaxResolution = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<uint32>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		}
		else if (PropertyName == FName("CompressionQuality"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
			{
				OutError = "The texture compression-quality metadata is unavailable.";
				return false;
			}
			CandidateCompressionQuality = static_cast<ETextureCompressionQuality>(
				static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
					Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
		}
		else if (PropertyName == FName("AlphaMipMode"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Enum)
			{
				OutError = "The texture alpha-mip-mode metadata is unavailable.";
				return false;
			}
			CandidateAlphaMipMode = static_cast<ETextureAlphaMipMode>(
				static_cast<const FEnumProperty*>(Proposal.DraftRootProperty)->GetValueAsUInt64(
					Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
		}
		else if (PropertyName == FName("AlphaCoverageThreshold"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Float)
			{
				OutError = "The texture alpha-coverage-threshold metadata is unavailable.";
				return false;
			}
			CandidateAlphaCoverageThreshold = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		}
		else return true;

		if (!EnsureSourceData(OutError)) return false;
		std::unique_ptr<FTexturePlatformData> CandidatePlatformData;
		if (!BuildPlatformData(CandidateUsage, bCandidateSRGB, CandidateMaxResolution,
			CandidateCompressionQuality, CandidateAlphaMipMode, CandidateAlphaCoverageThreshold,
			CandidatePlatformData, OutError)) return false;
		PendingEditUsage = CandidateUsage;
		bPendingEditSRGB = bCandidateSRGB;
		PendingEditMaxResolution = CandidateMaxResolution;
		PendingEditCompressionQuality = CandidateCompressionQuality;
		PendingEditAlphaMipMode = CandidateAlphaMipMode;
		PendingEditAlphaCoverageThreshold = CandidateAlphaCoverageThreshold;
		PendingEditPlatformData = std::move(CandidatePlatformData);
		return true;
	}

	auto DTexture2D::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName PropertyName = Event.MemberProperty->NamePrivate;
		if (PropertyName != FName("Usage") && PropertyName != FName("bSRGB")
			&& PropertyName != FName("MaxResolution") && PropertyName != FName("CompressionQuality")
			&& PropertyName != FName("AlphaMipMode") && PropertyName != FName("AlphaCoverageThreshold")) return;
		if (Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit) return;
		if (!PendingEditPlatformData || PendingEditUsage != Usage
			|| PendingEditMaxResolution != MaxResolution
			|| PendingEditCompressionQuality != CompressionQuality
			|| PendingEditAlphaMipMode != AlphaMipMode
			|| PendingEditAlphaCoverageThreshold != AlphaCoverageThreshold) return;

		std::string NewKey;
		std::string Error;
		const bool bPreviousSRGB = bSRGB;
		bSRGB = bPendingEditSRGB;
		if (!MakeTextureDerivedDataKey(*this, NewKey, Error)
			|| !StoreTextureDerivedData(NewKey, *PendingEditPlatformData, Error))
		{
			bSRGB = bPreviousSRGB;
			DerivedDataDiagnostic = {
				.Status = ETextureDerivedDataStatus::WriteFailure,
				.Key = NewKey,
				.Message = std::format("Texture2D DDC write failed after property edit: {}", Error)};
			DURIN_ERROR("{}: {}", GetObjectPath(), DerivedDataDiagnostic.Message);
			PendingEditPlatformData.reset();
			return;
		}
		PlatformData = std::move(PendingEditPlatformData);
		DerivedDataKey = std::move(NewKey);
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = std::format("Rebuilt Texture2D and stored DDC key {}.", DerivedDataKey)};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
	}

	auto DTexture2D::RefreshBuildStatus() -> void
	{
		if (RenderCompletion->GetFailedRevision() == BuildRevision)
		{
			if (BuildStatus == ETextureBuildStatus::Ready)
			{
				if (RenderCompletion->GetFailureReason()
					== ETextureRenderFailure::UnsupportedFormat)
				{
					BuildStatus = ETextureBuildStatus::UnsupportedFormat;
					LastBuildError = "The current RHI does not support this texture format and usage.";
				}
				else
				{
					BuildStatus = ETextureBuildStatus::UploadFailure;
					LastBuildError = "GPU texture creation or upload failed.";
				}
			}
		}
	}

	auto DTexture2D::BuildFromEncodedBytes(
		std::span<const uint8> EncodedBytes,
		const FSourcePath& InSourcePath,
		const FTexture2DImportSettings& Settings,
		std::string& OutError) -> bool
	{
		return BuildFromEncodedBytes(
			EncodedBytes, InSourcePath, Settings, nullptr, OutError);
	}

	auto DTexture2D::ExchangeImportedState(DTexture2D& Other) -> void
	{
		if (&Other == this) return;
		std::swap(SourceFile, Other.SourceFile);
		std::swap(SourceImportData, Other.SourceImportData);
		std::swap(SourceContentHash, Other.SourceContentHash);
		std::swap(SourceFileSize, Other.SourceFileSize);
		std::swap(SourceLastWriteTime, Other.SourceLastWriteTime);
		std::swap(SourceWidth, Other.SourceWidth);
		std::swap(SourceHeight, Other.SourceHeight);
		std::swap(SourceChannelCount, Other.SourceChannelCount);
		std::swap(bSourceHasTransparency, Other.bSourceHasTransparency);
		std::swap(Usage, Other.Usage);
		std::swap(bSRGB, Other.bSRGB);
		std::swap(MaxResolution, Other.MaxResolution);
		std::swap(CompressionQuality, Other.CompressionQuality);
		std::swap(AlphaMipMode, Other.AlphaMipMode);
		std::swap(AlphaCoverageThreshold, Other.AlphaCoverageThreshold);
		std::swap(CookedPayload, Other.CookedPayload);
		std::swap(ImportOwner, Other.ImportOwner);
		std::swap(SourceData, Other.SourceData);
		std::swap(PlatformData, Other.PlatformData);
		std::swap(DerivedDataKey, Other.DerivedDataKey);
		std::swap(DerivedDataDiagnostic, Other.DerivedDataDiagnostic);
		std::swap(bLoadedFromDerivedDataCache, Other.bLoadedFromDerivedDataCache);
		std::swap(BuildStatus, Other.BuildStatus);
		std::swap(LastBuildError, Other.LastBuildError);
		QueueRenderResourceBuild();
		Other.QueueRenderResourceBuild();
		MarkPackageDirty();
		Other.MarkPackageDirty();
	}

	auto DTexture2D::SetImportOwner(const FAssetPath& InOwner) -> void
	{
		if (ImportOwner == InOwner) return;
		ImportOwner = InOwner;
		MarkPackageDirty();
	}

	auto DTexture2D::BuildFromEncodedBytes(
		std::span<const uint8> EncodedBytes,
		const FSourcePath& InSourcePath,
		const FTexture2DImportSettings& Settings,
		const FEncodedBuildHooks* Hooks,
		std::string& OutError) -> bool
	{
#if DURIN_WITH_EDITOR
		if (!GetPackage() || EncodedBytes.empty())
		{
			OutError = "Encoded Texture2D candidates require a package and non-empty source bytes.";
			return false;
		}
		if (!TextureBuild::IsValidUsage(Settings.Usage)
			|| !TextureBuild::IsValidCompressionQuality(Settings.CompressionQuality)
			|| !TextureBuild::IsValidAlphaMipMode(Settings.AlphaMipMode)
			|| !TextureBuild::IsValidAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D candidate build settings are invalid.";
			return false;
		}
		const PathUtilities::FSourcePathResult Resolved =
			PathUtilities::ResolveSourcePath(
				InSourcePath.Path, PathUtilities::EPathExistence::AllowMissing);
		if (!Resolved)
		{
			OutError = Resolved.Message;
			return false;
		}
		const PathUtilities::FMountPolicyResult Dependency =
			PathUtilities::CheckMountDependency(
				GetPackage()->GetPackagePath(), Resolved.NormalizedVirtualPath);
		if (!Dependency)
		{
			OutError = Dependency.Message;
			return false;
		}

		FTextureSourceData CandidateSourceData;
		if (Hooks && Hooks->BeforeDecode && Hooks->BeforeDecode(OutError))
		{
			BuildStatus = ETextureBuildStatus::DecodeFailure;
			LastBuildError = OutError;
			return false;
		}
		if (!TextureBuild::DecodeRGBA8(EncodedBytes, CandidateSourceData, OutError))
		{
			BuildStatus = ETextureBuildStatus::DecodeFailure;
			LastBuildError = OutError;
			return false;
		}
		const bool bCandidateSRGB =
			Settings.bSRGB.value_or(TextureBuild::GetDefaultSRGB(Settings.Usage));
		auto CandidatePlatformData = std::make_unique<FTexturePlatformData>();
		if (Hooks && Hooks->BeforeTextureBuild && Hooks->BeforeTextureBuild(OutError))
		{
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			return false;
		}
		if (!TextureBuild::BuildMipChain(
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
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			return false;
		}

		const FXxHash128 SourceHash = FXxHash128::HashBuffer(EncodedBytes);
		const std::string NewKey = BuildTexture2DDerivedDataKey({
			.SourceContentHash = SourceHash,
			.Usage = Settings.Usage,
			.bSRGB = bCandidateSRGB,
			.CompressionQuality = Settings.CompressionQuality,
			.AlphaMipMode = Settings.AlphaMipMode,
			.MaximumResolution = Settings.MaxResolution,
			.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Hooks && Hooks->BeforeDerivedDataPublication
			&& Hooks->BeforeDerivedDataPublication(OutError))
		{
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			return false;
		}
		if (!StoreTextureDerivedData(NewKey, *CandidatePlatformData, OutError, false))
		{
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = OutError;
			return false;
		}

		Usage = Settings.Usage;
		bSRGB = bCandidateSRGB;
		MaxResolution = Settings.MaxResolution;
		CompressionQuality = Settings.CompressionQuality;
		AlphaMipMode = Settings.AlphaMipMode;
		AlphaCoverageThreshold = Settings.AlphaCoverageThreshold;
		SourceImportData = {
			.Source = {
				.SourcePath = {.Path = Resolved.NormalizedVirtualPath},
				.SourceContentHashLow = SourceHash.HashLow,
				.SourceContentHashHigh = SourceHash.HashHigh},
			.DecoderId = std::string(TextureDecoderId),
			.DecoderVersion = TextureDecoderVersion};
		SourceFile.clear();
		SourceContentHash = SourceHash.ToString();
		SourceFileSize = EncodedBytes.size();
		SourceLastWriteTime = 0;
		SourceWidth = CandidateSourceData.Width;
		SourceHeight = CandidateSourceData.Height;
		SourceChannelCount = CandidateSourceData.SourceChannelCount;
		bSourceHasTransparency = CandidateSourceData.bHasTransparency;
		SourceData = std::make_unique<FTextureSourceData>(std::move(CandidateSourceData));
		PlatformData = std::move(CandidatePlatformData);
		DerivedDataKey = NewKey;
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = "Built Texture2D candidate from encoded source bytes.",
			.bSourceDecoderInvoked = true};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		MarkPackageDirty();
		OutError.clear();
		return true;
#else
		(void)EncodedBytes;
		(void)InSourcePath;
		(void)Settings;
		(void)Hooks;
		OutError = "Encoded Texture2D candidate builds are unavailable in runtime-only builds.";
		return false;
#endif
	}

	auto DTexture2D::ImportAsset(std::string_view FilePath, std::string_view AssetPath, const FTexture2DImportSettings& Settings) -> FTexture2DImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)) return {false, "Source file does not exist.", nullptr};
		if (!Asset::IsSupportedImageExtension(Input.extension().generic_string())) return {false, "Unsupported texture source format.", nullptr};
		if (!TextureBuild::IsValidUsage(Settings.Usage)) return {false, "Texture usage preset is invalid.", nullptr};
		if (!TextureBuild::IsValidCompressionQuality(Settings.CompressionQuality))
			return {false, "Texture compression quality is invalid.", nullptr};
		if (!TextureBuild::IsValidAlphaMipMode(Settings.AlphaMipMode))
			return {false, "Texture alpha mip mode is invalid.", nullptr};
		if (!TextureBuild::IsValidAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
			return {false, "Texture alpha coverage threshold must be greater than zero and less than one.", nullptr};

		FAssetPath ParsedAssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &PathError)) return {false, std::move(PathError), nullptr};
		if (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		const std::string Extension = Input.extension().generic_string();
		std::filesystem::path Destination;
		std::string StoredSourcePath;
		if (!MakeCanonicalSourceLocation(
			ParsedAssetPath, Extension, Settings.SourceDestination,
			Destination, StoredSourcePath, PathError))
			return {false, std::move(PathError), nullptr};
		FMountedSourceFile MountedSource;
		if (!PrepareMountedSourceFile(
			Input, ParsedAssetPath.ToString(), StoredSourcePath, MountedSource, PathError))
			return {false, std::move(PathError), nullptr};
		Destination = MountedSource.PhysicalPath;
		StoredSourcePath = MountedSource.SourcePath.Path;
		FXxHash128 SourceHash;
		if (!HashTextureSource(Destination, SourceHash, PathError))
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, std::move(PathError), nullptr};
		}

		DTexture2D* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult)
		{
			RollbackMountedSourceFile(MountedSource);
			return {false, CreateResult.Message, nullptr};
		}
		Texture->Usage = Settings.Usage;
		Texture->bSRGB = Settings.bSRGB.value_or(TextureBuild::GetDefaultSRGB(Settings.Usage));
		Texture->MaxResolution = Settings.MaxResolution;
		Texture->CompressionQuality = Settings.CompressionQuality;
		Texture->AlphaMipMode = Settings.AlphaMipMode;
		Texture->AlphaCoverageThreshold = Settings.AlphaCoverageThreshold;
		std::string BuildError;
		if (!Texture->BuildSourceData(Destination.generic_string(), BuildError))
		{
			RollbackMountedSourceFile(MountedSource);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(BuildError), nullptr};
		}

		Texture->SourceImportData = {
			.Source = {
				.SourcePath = {.Path = std::move(StoredSourcePath)},
				.SourceContentHashLow = SourceHash.HashLow,
				.SourceContentHashHigh = SourceHash.HashHigh},
			.DecoderId = std::string(TextureDecoderId),
			.DecoderVersion = TextureDecoderVersion};
		Texture->SourceFile.clear();
		Texture->UpdateSourceFingerprint(Destination);
		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
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
