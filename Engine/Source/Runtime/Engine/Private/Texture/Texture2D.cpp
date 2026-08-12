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
#include "Serialization/Archive.h"
#include "DynamicRHI.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/Texture2DBuildCoordinator.h"
#include "Texture/TextureBuild.h"
#include "Texture/TextureDerivedData.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		constexpr uint64 TextureDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureDerivedDataCleanupDeleteLimit = 16;
		constexpr std::string_view DefaultTextureSourceRoot = "Textures";
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
				OutError = std::format("Texture asset {} is not beneath a registered package mount.",
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
				OutError = std::format("Texture source path '{}' must be a normalized mount-relative path.",
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
			OutError = "Texture asset has no normalized mounted-source provenance.";
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
			auto Candidate = std::make_unique<FTexturePlatformData>();
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			Candidate->Serialize(Ar, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				OutStatus = Ar.GetFailure()
					&& Ar.GetFailure()->Code == EArchiveFailureCode::UnsupportedVersion
					? ETextureDerivedDataStatus::Incompatible
					: ETextureDerivedDataStatus::Corrupt;
				OutMessage = Ar.GetError();
				return false;
			}
			OutPlatformData = std::move(Candidate);
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
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTexturePlatformData&>(PlatformData).Serialize(Ar, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (Ar.HasError())
			{
				OutError = Ar.GetError();
				return false;
			}
			if (!GetTextureObjectStore().Write(Key, Bytes, &OutError)) return false;
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
	{}

	DTexture2D::~DTexture2D() = default;

	auto DTexture2D::Serialize(FArchive& Ar) -> void
	{
		Super::Serialize(Ar);
	}

	auto DTexture2D::BeginDestroy() -> void
	{
		if (FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator())
		{
			if (ActiveBuildRequestId != 0) Coordinator->Cancel(ActiveBuildRequestId);
		}
		++BuildRequestGeneration;
		ActiveBuildRequestId = 0;
		Super::BeginDestroy();
	}

	auto DTexture2D::GetBuildReadinessDiagnostic() const -> FTexture2DBuildDiagnostic
	{
		FTexture2DBuildDiagnostic Diagnostic;
		const uint64 RequestId = ActiveBuildRequestId != 0
			? ActiveBuildRequestId : LastBuildRequestId;
		if (RequestId != 0)
		{
			if (FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator())
				Diagnostic = Coordinator->GetDiagnostic(RequestId);
		}
		if (ActiveBuildRequestId == 0
			&& BuildStatus == ETextureBuildStatus::Ready
			&& GetRenderResourceState() == ERenderResourceState::Ready)
		{
			Diagnostic.Phase = ETexture2DBuildPhase::Ready;
		}
		else if (ActiveBuildRequestId == 0
			&& BuildStatus == ETextureBuildStatus::Ready
			&& Diagnostic.Phase != ETexture2DBuildPhase::Failed
			&& Diagnostic.Phase != ETexture2DBuildPhase::Cancelled)
		{
			Diagnostic.Phase = ETexture2DBuildPhase::UploadPending;
		}
		return Diagnostic;
	}

	auto DTexture2D::CancelPendingBuild() -> bool
	{
		if (ActiveBuildRequestId == 0) return false;
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		return Coordinator && Coordinator->Cancel(ActiveBuildRequestId);
	}

	auto DTexture2D::WaitForPendingBuild(double TimeoutSeconds) -> bool
	{
		if (ActiveBuildRequestId == 0) return true;
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		if (!Coordinator) return false;
		const uint64 RequestId = ActiveBuildRequestId;
		if (!Coordinator->WaitForRequest(RequestId, TimeoutSeconds)) return false;
		Coordinator->PumpCompletions(std::numeric_limits<uint32>::max());
		return ActiveBuildRequestId == 0 && BuildStatus == ETextureBuildStatus::Ready;
	}

	auto DTexture2D::SubmitAsyncBuild(
		std::vector<uint8> EncodedSource,
		const FSourcePath& SourcePath,
		const FTexture2DImportSettings& Settings,
		ETexture2DBuildPriority Priority,
		bool bMarkDirtyOnCommit,
		bool bReportLoadMutationOnCommit,
		uint64 SourceFileSizeSnapshot,
		int64 SourceLastWriteTimeSnapshot,
		std::string& OutError) -> bool
	{
		InitializeTexture2DBuildCoordinator();
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		if (!Coordinator)
		{
			OutError = "The Texture2D build coordinator is unavailable.";
			return false;
		}
		if (EncodedSource.empty() || SourcePath.IsEmpty())
		{
			OutError = "An asynchronous Texture2D build requires source bytes and mounted provenance.";
			return false;
		}
		if (!TextureBuild::IsValidUsage(Settings.Usage)
			|| !TextureBuild::IsValidCompressionQuality(Settings.CompressionQuality)
			|| !TextureBuild::IsValidAlphaMipMode(Settings.AlphaMipMode)
			|| !TextureBuild::IsValidAlphaCoverageThreshold(Settings.AlphaCoverageThreshold))
		{
			OutError = "Texture2D asynchronous build settings are invalid.";
			return false;
		}

		if (ActiveBuildRequestId != 0) Coordinator->Cancel(ActiveBuildRequestId);
		const uint64 Generation = ++BuildRequestGeneration;
		const std::string AssetIdentity = GetObjectPath();
		const FXxHash128 SourceHash = FXxHash128::HashBuffer(EncodedSource);
		FTextureSourceData NormalizedSource;
		if (!TextureBuild::DecodeRGBA8(EncodedSource, NormalizedSource, OutError))
		{
			return false;
		}
		const bool bResolvedSRGB = Settings.bSRGB.value_or(
			TextureBuild::GetDefaultSRGB(Settings.Usage));
		FTexture2DBuildRequest Request{
			.AssetIdentity = AssetIdentity,
			.SourcePath = SourcePath,
			.SourceData = std::move(NormalizedSource),
			.SourceHash = SourceHash,
			.Settings = {
				.Usage = Settings.Usage,
				.bSRGB = bResolvedSRGB,
				.MaxResolution = Settings.MaxResolution,
				.CompressionQuality = Settings.CompressionQuality,
				.AlphaMipMode = Settings.AlphaMipMode,
				.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold},
			.Generation = Generation,
			.EstimatedWidth = SourceWidth,
			.EstimatedHeight = SourceHeight,
			.Priority = Priority};
		const TWeakObjectPtr<DTexture2D> WeakThis(this);
		const uint64 RequestId = Coordinator->Submit(
			std::move(Request),
			[WeakThis](FTexture2DBuildResult&& Result) {
				if (DTexture2D* Texture = WeakThis.Get())
					Texture->ApplyAsyncBuildResult(std::move(Result));
			});
		if (RequestId == 0)
		{
			OutError = "The Texture2D build coordinator rejected the request.";
			return false;
		}

		ActiveBuildRequestId = RequestId;
		LastBuildRequestId = RequestId;
		PendingBuildAssetIdentity = AssetIdentity;
		PendingBuildSourcePath = SourcePath;
		PendingBuildSettings = Settings;
		PendingBuildSettings.bSRGB = bResolvedSRGB;
		PendingBuildSourceFileSize = SourceFileSizeSnapshot;
		PendingBuildSourceLastWriteTime = SourceLastWriteTimeSnapshot;
		bPendingBuildMarksDirty = bMarkDirtyOnCommit;
		bPendingBuildReportsLoadMutation = bReportLoadMutationOnCommit;
		OutError.clear();
		return true;
	}

	auto DTexture2D::ApplyAsyncBuildResult(FTexture2DBuildResult&& Result) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (Result.RequestId != ActiveBuildRequestId
			|| Result.Generation != BuildRequestGeneration
			|| Result.AssetIdentity != PendingBuildAssetIdentity
			|| GetObjectPath() != PendingBuildAssetIdentity
			|| Result.SourcePath != PendingBuildSourcePath)
		{
			return;
		}
		ActiveBuildRequestId = 0;
		if (Result.Phase == ETexture2DBuildPhase::Cancelled)
		{
			return;
		}
		if (Result.Phase != ETexture2DBuildPhase::UploadPending
			|| !Result.SourceData || !Result.SourceData->IsValid()
			|| !Result.PlatformData || !Result.PlatformData->IsValid())
		{
			LastBuildError = Result.Error.empty()
				? "The asynchronous Texture2D build failed." : std::move(Result.Error);
			if (!PlatformData)
				BuildStatus = ETextureBuildStatus::BuildFailure;
			return;
		}
		const FTexture2DBuildSettingsSnapshot ExpectedSettings{
			.Usage = PendingBuildSettings.Usage,
			.bSRGB = PendingBuildSettings.bSRGB.value_or(
				TextureBuild::GetDefaultSRGB(PendingBuildSettings.Usage)),
			.MaxResolution = PendingBuildSettings.MaxResolution,
			.CompressionQuality = PendingBuildSettings.CompressionQuality,
			.AlphaMipMode = PendingBuildSettings.AlphaMipMode,
			.AlphaCoverageThreshold = PendingBuildSettings.AlphaCoverageThreshold};
		if (Result.Settings != ExpectedSettings) return;

		SourceImportData = {
			.Source = {
				.SourcePath = Result.SourcePath,
				.SourceContentHashLow = Result.SourceHash.HashLow,
				.SourceContentHashHigh = Result.SourceHash.HashHigh},
			.DecoderId = std::string(TextureDecoderId),
			.DecoderVersion = TextureDecoderVersion};
		SourceContentHash = Result.SourceHash.ToString();
		SourceFileSize = PendingBuildSourceFileSize;
		SourceLastWriteTime = PendingBuildSourceLastWriteTime;
		SourceWidth = Result.SourceData->Width;
		SourceHeight = Result.SourceData->Height;
		SourceChannelCount = Result.SourceData->SourceChannelCount;
		bSourceHasTransparency = Result.SourceData->bHasTransparency;
		Usage = Result.Settings.Usage;
		bSRGB = Result.Settings.bSRGB;
		MaxResolution = Result.Settings.MaxResolution;
		CompressionQuality = Result.Settings.CompressionQuality;
		AlphaMipMode = Result.Settings.AlphaMipMode;
		AlphaCoverageThreshold = Result.Settings.AlphaCoverageThreshold;
		SourceData = std::move(Result.SourceData);
		PlatformData = std::move(Result.PlatformData);
		DerivedDataKey = std::move(Result.DerivedDataKey);
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = std::format(
				"Asynchronously rebuilt Texture2D and stored DDC key {}.", DerivedDataKey),
			.bSourceDecoderInvoked = true};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		std::filesystem::path CommittedSourcePath;
		std::string ResolveError;
		if (ResolveTextureSource(*this, CommittedSourcePath, ResolveError))
		{
			Asset::StoreSourceFingerprint(CommittedSourcePath, {
				.FileSize = SourceFileSize,
				.LastWriteTimeTicks = SourceLastWriteTime,
				.ContentHash = SourceContentHash});
		}
		QueueRenderResourceBuild();
		if (bPendingBuildMarksDirty) MarkPackageDirty();
		if (bPendingBuildReportsLoadMutation)
		{
			Asset::ReportAssetLoadMutation(
				this,
				"Engine.Texture2D.SourceIdentity",
				"Texture source identity metadata was reconciled by an asynchronous load build.");
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
		InvalidateRenderResource();
	}

	auto DTexture2D::CreateRenderResourceCandidate(
		FTextureReference* TextureReference,
		uint64 Revision,
		const std::shared_ptr<FTextureResourceCompletion>& Completion)
		-> std::unique_ptr<FTextureAssetResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTexture2DResource>(
			TextureReference,
			std::make_shared<const FTexturePlatformData>(*PlatformData),
			Revision,
			Completion);
	}

	auto DTexture2D::InspectSource() const -> FTextureSourceDiagnostic
	{
		if (!SourceImportData.HasSource())
			return {};
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

	auto DTexture2D::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedPlatformData(OutError);
		if (Asset::IsAssetMigrationLoad())
		{
			OutError.clear();
			return true;
		}

		BuildStatus = ETextureBuildStatus::Unbuilt;
		LastBuildError.clear();
		DerivedDataKey.clear();
		DerivedDataDiagnostic = {};
		bLoadedFromDerivedDataCache = false;
		if (InvokeTexture2DUncookedPostLoadHandler(*this, OutError)) return true;
		if (BuildStatus == ETextureBuildStatus::Unbuilt)
			PublishUncookedLoadFailure(
				ETextureDerivedDataStatus::Incompatible,
				ETextureBuildStatus::MissingSource,
				OutError);
		return false;
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

		auto CandidatePlatformData = std::make_unique<FTexturePlatformData>();
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		CandidatePlatformData->Serialize(PayloadAr, {
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr))
		{
			return FailCooked(std::string(PayloadAr.GetError()));
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
		auto ValidatedPlatformData = std::make_unique<FTexturePlatformData>();
		const Asset::FDerivedDataObjectReadResult Read =
			GetTextureObjectStore().Read(ExpectedKey, PayloadBytes);
		FCanonicalMemoryReader DdcAr(PayloadBytes, EArchivePurpose::DerivedDataPayload);
		if (Read)
		{
			ValidatedPlatformData->Serialize(DdcAr, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (!DdcAr.HasError()) RequireArchiveEnd(DdcAr);
		}
		if (!Read || DdcAr.HasError())
		{
			if (!PlatformData && !PostLoad(OutError))
			{
				OutError = std::format("Failed to cook Texture2D '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			if (!PlatformData)
			{
				OutError = std::format("Failed to cook Texture2D '{}': {}", GetObjectPath(), OutError);
				return false;
			}
			PayloadBytes.clear();
			FCanonicalMemoryWriter CookAr(PayloadBytes, EArchivePurpose::CookedPayload);
			PlatformData->Serialize(CookAr, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (CookAr.HasError())
			{
				OutError = std::format(
					"Failed to cook Texture2D '{}': {}", GetObjectPath(), CookAr.GetError());
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
						return Name != FName("SourceImportData")
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
		return Super::PreEditChangeProperty(Proposal, OutError);
	}

	auto DTexture2D::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
	}

	auto DTexture2D::RefreshBuildStatus() -> void
	{
		const std::shared_ptr<FTextureResourceCompletion>& Completion =
			GetRenderCompletion();
		if (Completion->GetFailedRevision() == GetBuildRevision())
		{
			if (BuildStatus == ETextureBuildStatus::Ready)
			{
				if (Completion->GetFailureReason()
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

	auto DTexture2D::PublishImportedState(
		FTexture2DImportedState State,
		std::string& OutError) -> bool
	{
		if (!IsInGameThread())
		{
			OutError = "Texture2D imported state must be published on the game thread.";
			return false;
		}
		if (!State.SourceImportData.HasSource()
			|| !State.SourceImportData.Source.HasContentHash()
			|| State.SourceContentHash.empty()
			|| !State.SourceData || !State.SourceData->IsValid()
			|| !State.PlatformData || !State.PlatformData->IsValid()
			|| State.DerivedDataKey.empty()
			|| !TextureBuild::IsValidUsage(State.Usage)
			|| !TextureBuild::IsValidCompressionQuality(State.CompressionQuality)
			|| !TextureBuild::IsValidAlphaMipMode(State.AlphaMipMode)
			|| !TextureBuild::IsValidAlphaCoverageThreshold(State.AlphaCoverageThreshold))
		{
			OutError = "Texture2D imported state is incomplete or invalid.";
			return false;
		}

		SourceWidth = State.SourceData->Width;
		SourceHeight = State.SourceData->Height;
		SourceChannelCount = State.SourceData->SourceChannelCount;
		bSourceHasTransparency = State.SourceData->bHasTransparency;
		SourceImportData = std::move(State.SourceImportData);
		SourceContentHash = std::move(State.SourceContentHash);
		SourceFileSize = State.SourceFileSize;
		SourceLastWriteTime = State.SourceLastWriteTime;
		SourceData = std::move(State.SourceData);
		PlatformData = std::move(State.PlatformData);
		DerivedDataKey = std::move(State.DerivedDataKey);
		Usage = State.Usage;
		bSRGB = State.bSRGB;
		MaxResolution = State.MaxResolution;
		CompressionQuality = State.CompressionQuality;
		AlphaMipMode = State.AlphaMipMode;
		AlphaCoverageThreshold = State.AlphaCoverageThreshold;
		bLoadedFromDerivedDataCache = false;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = "Published normalized Texture2D build product.",
			.bSourceDecoderInvoked = true};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		if (State.bMarkPackageDirty) MarkPackageDirty();
		if (State.bReportLoadMutation)
		{
			Asset::ReportAssetLoadMutation(
				this,
				"Engine.Texture2D.SourceIdentity",
				"Texture source identity metadata was reconciled by an authoring load build.");
		}
		OutError.clear();
		return true;
	}

	auto DTexture2D::PublishDerivedDataLoad(
		std::unique_ptr<FTexturePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		bool bSourceAvailable,
		std::string& OutError) -> bool
	{
		if (!IsInGameThread() || !InPlatformData || !InPlatformData->IsValid()
			|| InDerivedDataKey.empty())
		{
			OutError = "Texture2D derived-data load publication is incomplete or invalid.";
			return false;
		}
		SourceData.reset();
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
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

	auto DTexture2D::PublishUncookedLoadFailure(
		ETextureDerivedDataStatus DerivedDataStatus,
		ETextureBuildStatus InBuildStatus,
		std::string Message,
		std::string InDerivedDataKey) -> bool
	{
		DerivedDataKey = std::move(InDerivedDataKey);
		DerivedDataDiagnostic = {
			.Status = DerivedDataStatus,
			.Key = DerivedDataKey,
			.Message = Message};
		BuildStatus = InBuildStatus;
		LastBuildError = std::move(Message);
		return false;
	}

	auto DTexture2D::PublishSourceFingerprint(
		uint64 FileSize, int64 LastWriteTime) -> void
	{
		SourceFileSize = FileSize;
		SourceLastWriteTime = LastWriteTime;
	}

	auto DTexture2D::ExchangeImportedState(DTexture2D& Other) -> void
	{
		if (&Other == this) return;
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

}
