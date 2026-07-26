#include "Texture/Texture2D.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "Hash/XxHash.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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

		auto ResolveMountedFile(std::string_view VirtualPath) -> std::filesystem::path
		{
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				if (VirtualPath.starts_with(Mount.VirtualRoot))
				{
					return (std::filesystem::path(Mount.PhysicalPath) / std::string(VirtualPath.substr(Mount.VirtualRoot.size()))).lexically_normal();
				}
			}
			return std::filesystem::path(VirtualPath).lexically_normal();
		}

		auto ResolveTextureSource(std::string_view SourceFile, const std::filesystem::path& PackageFile) -> std::filesystem::path
		{
			const std::filesystem::path StoredPath(SourceFile);
			if (!StoredPath.is_absolute() && !SourceFile.starts_with('/'))
			{
				return (PackageFile.parent_path() / StoredPath).lexically_normal();
			}

			const std::filesystem::path LegacyPath = ResolveMountedFile(SourceFile);
			if (std::filesystem::is_regular_file(LegacyPath)) return LegacyPath;
			return (PackageFile.parent_path() / StoredPath.filename()).lexically_normal();
		}

		auto ResolveTextureSource(const DTexture2D& Texture) -> std::filesystem::path
		{
			return ResolveTextureSource(Texture.GetSourceFile(),
				ResolveMountedFile(Texture.GetPackage()->GetPackagePath()));
		}

		auto MakeTextureDerivedDataKey(const DTexture2D& Texture, std::string& OutKey, std::string& OutError) -> bool
		{
			if (!IsCanonicalTextureHash(Texture.GetSourceContentHash()))
			{
				OutError = "Texture source content hash is missing or invalid.";
				return false;
			}
			OutKey = BuildTexture2DDerivedDataKey({
				.SourceContentHash = FXxHash128::FromString(Texture.GetSourceContentHash()),
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
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			if (!EncodeTexture2DPayload(
				PlatformData, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game,
				Bytes, OutError)
				|| !GetTextureObjectStore().Write(Key, Bytes, &OutError)) return false;
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
		, RenderResource(std::make_shared<FTexture2DRenderResource>())
	{
		static const bool RegisteredAssetContributors = [] {
			Asset::RegisterAssetMoveContributor(DTexture2D::StaticClass(), [](DObject* Object, const FAssetPath& OldPath, const FAssetPath& NewPath, Asset::FAssetMoveContribution& Out) -> Asset::FAssetResult {
				auto* Texture = Cast<DTexture2D>(Object);
				if (!Texture || Texture->SourceFile.empty()) return {};
				const std::string Original = Texture->SourceFile;
				const std::filesystem::path OldPackage = ResolveMountedFile(OldPath.ToString());
				const std::filesystem::path NewPackage = ResolveMountedFile(NewPath.ToString());
				const std::filesystem::path SourceName(Original);
				const std::filesystem::path OldSource = SourceName.is_absolute() ? SourceName : OldPackage.parent_path() / SourceName;
				const std::string NewFileName = OldPath.GetAssetName() == NewPath.GetAssetName()
					? SourceName.filename().generic_string()
					: std::string(NewPath.GetAssetName()) + SourceName.extension().generic_string();
				const std::filesystem::path NewSource = NewPackage.parent_path() / NewFileName;
				if (OldSource.lexically_normal() != NewSource.lexically_normal()) Out.Files.emplace_back(OldSource.lexically_normal(), NewSource.lexically_normal());
				if (NewFileName != Original)
				{
					Out.Apply = [Texture, NewFileName] { Texture->SourceFile = NewFileName; };
					Out.Rollback = [Texture, Original] { Texture->SourceFile = Original; };
				}
				return {};
			});
			Asset::RegisterAssetDeleteContributor(DTexture2D::StaticClass(), [](const Asset::FAssetData& Data,
				const Asset::FAssetPackageInspection& Inspection, Asset::FAssetDeleteContribution& Out) -> Asset::FAssetResult {
				const Asset::FAssetPackageField* SourceField = Inspection.FindField("SourceFile");
				std::string SourceFile;
				if (!SourceField || !SourceField->TryReadString(SourceFile) || SourceFile.empty()) return {};
				Out.Files.push_back(ResolveTextureSource(SourceFile, Data.PhysicalPath));
				return {};
			});
			return true;
		}();
		(void)RegisteredAssetContributors;
	}

	DTexture2D::~DTexture2D()
	{
		const uint64 ReleaseRevision = ++BuildRevision;
		if (GDynamicRHI != nullptr)
		{
			RenderResource->QueueRelease(ReleaseRevision);
		}
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
		const uint64 ReleaseRevision = ++BuildRevision;
		if (GDynamicRHI != nullptr) RenderResource->QueueRelease(ReleaseRevision);
	}

	auto DTexture2D::QueueRenderResourceBuild() -> void
	{
		check(PlatformData && PlatformData->IsValid());
		const uint64 Revision = ++BuildRevision;
		if (GDynamicRHI == nullptr) return;
		// The immutable value snapshot decouples queued uploads from subsequent imports/rebuilds.
		RenderResource->QueueBuild(std::make_shared<const FTexturePlatformData>(*PlatformData), Revision);
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

		SourceContentHash = FXxHash128::HashBuffer(SourceBytes).ToString();
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
		if (SourceFile.empty())
		{
			OutError = "Texture asset has no source file.";
			return false;
		}
		const std::filesystem::path PhysicalPath = ResolveTextureSource(*this);
		if (!std::filesystem::is_regular_file(PhysicalPath))
		{
			OutError = std::format("Texture source file does not exist: {}", SourceFile);
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

	auto DTexture2D::PostLoad(std::string& OutError) -> bool
	{
		BuildStatus = ETextureBuildStatus::Unbuilt;
		LastBuildError.clear();
		DerivedDataKey.clear();
		DerivedDataDiagnostic = {};
		bLoadedFromDerivedDataCache = false;
		if (SourceFile.empty())
		{
			OutError = "Texture asset has no source file.";
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
			DerivedDataDiagnostic.Message = OutError;
			BuildStatus = ETextureBuildStatus::MissingSource;
			LastBuildError = OutError;
			return false;
		}
		const std::filesystem::path PhysicalPath = ResolveTextureSource(*this);
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
		if (IsCanonicalTextureHash(SourceContentHash)
			&& (!bSourceAvailable || bSourceFingerprintMatches))
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
					SourceFile, CacheMessage);
				OutError = DerivedDataDiagnostic.Message;
				BuildStatus = ETextureBuildStatus::MissingSource;
				LastBuildError = OutError;
				return false;
			}
		}
		else if (!bSourceAvailable)
		{
			OutError = std::format("Texture source file does not exist: {}", SourceFile);
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::SourceUnavailable;
			DerivedDataDiagnostic.Message = OutError;
			BuildStatus = ETextureBuildStatus::MissingSource;
			LastBuildError = OutError;
			return false;
		}
		const bool bMetadataChanged = !IsCanonicalTextureHash(SourceContentHash)
			|| !bSourceFingerprintMatches;
		if (!BuildSourceData(PhysicalPath.generic_string(), OutError)) return false;
		if (bMetadataChanged) MarkPackageDirty();
		return true;
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
		if (!RenderResource) return;
		if (RenderResource->GetFailedRevision() == BuildRevision)
		{
			if (BuildStatus == ETextureBuildStatus::Ready)
			{
				if (RenderResource->GetFailureReason() == ETextureRenderFailure::UnsupportedFormat)
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
		const std::string SourceFileName = std::string(ParsedAssetPath.GetAssetName()) + Extension;
		const std::filesystem::path Destination = std::filesystem::path(ResolveMountedFile(ParsedAssetPath.ToString())).replace_extension(Extension);
		if (std::filesystem::exists(Destination)) return {false, std::format("Imported source already exists: {}", Destination.generic_string()), nullptr};

		DTexture2D* Texture = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Texture);
		if (!CreateResult) return {false, CreateResult.Message, nullptr};
		Texture->Usage = Settings.Usage;
		Texture->bSRGB = Settings.bSRGB.value_or(TextureBuild::GetDefaultSRGB(Settings.Usage));
		Texture->MaxResolution = Settings.MaxResolution;
		Texture->CompressionQuality = Settings.CompressionQuality;
		Texture->AlphaMipMode = Settings.AlphaMipMode;
		Texture->AlphaCoverageThreshold = Settings.AlphaCoverageThreshold;
		std::string BuildError;
		if (!Texture->BuildSourceData(Input.generic_string(), BuildError))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(BuildError), nullptr};
		}

		std::error_code ErrorCode;
		std::filesystem::create_directories(Destination.parent_path(), ErrorCode);
		if (ErrorCode || !std::filesystem::copy_file(Input, Destination, std::filesystem::copy_options::none, ErrorCode))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::format("Failed to copy source file to {}: {}", Destination.generic_string(), ErrorCode.message()), nullptr};
		}
		Texture->SourceFile = SourceFileName;
		Texture->UpdateSourceFingerprint(Destination);
		Asset::FAssetResult SaveResult = Asset::SavePackage(Texture->GetPackage());
		if (!SaveResult)
		{
			std::filesystem::remove(Destination, ErrorCode);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		return {true, {}, Texture};
	}
}
