#include "Texture/Texture2D.h"

#include "Asset/MountedSource.h"
#include "AssetCook.h"
#include "DObject/DurinPropertyTypes.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Serialization/Archive.h"
#include "DynamicRHI.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureDerivedData.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TextureSourceChannelCount = 4;
		constexpr std::string_view TextureDecoderId = "DurinImage";
		constexpr uint32 TextureDecoderVersion = 1;

		auto IsValidUsage(ETextureUsage Usage) -> bool
		{
			return Usage == ETextureUsage::Color || Usage == ETextureUsage::Normal
				|| Usage == ETextureUsage::DataMask;
		}

		auto IsValidCompressionQuality(ETextureCompressionQuality Quality) -> bool
		{
			return Quality == ETextureCompressionQuality::Low
				|| Quality == ETextureCompressionQuality::Normal
				|| Quality == ETextureCompressionQuality::High;
		}

		auto IsValidAlphaMipMode(ETextureAlphaMipMode Mode) -> bool
		{
			return Mode == ETextureAlphaMipMode::Average
				|| Mode == ETextureAlphaMipMode::PreserveCoverage;
		}

		auto IsValidAlphaCoverageThreshold(float Threshold) -> bool
		{
			return std::isfinite(Threshold) && Threshold > 0.0f && Threshold < 1.0f;
		}

		auto ResolveTextureSource(
			const DTexture2D& Texture,
			Asset::FMountedSourceResolution& OutResolution,
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
				return Asset::ResolveMountedSourceReference(
					Texture.GetPackage()->GetPackagePath(),
					Provenance.Source.SourcePath.Path,
					Asset::EMountedSourceExistencePolicy::AllowMissing,
					OutResolution, OutError);
			}
			OutError = "Texture asset has no normalized mounted-source provenance.";
			return false;
		}

		auto HashTextureSource(
			const std::filesystem::path& Path,
			FXxHash128& OutHash,
			std::string& OutError) -> bool
		{
			std::vector<std::byte> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path))
			{
				OutError = std::format("Failed to read texture source file: {}", Path.generic_string());
				return false;
			}
			OutHash = FXxHash128::HashBuffer(Bytes);
			OutError.clear();
			return true;
		}

	} // namespace

	auto FTextureSourceData::IsValid() const -> bool
	{
		return Format == ETextureSourceFormat::RGBA8
			&& Width > 0
			&& Height > 0
			&& static_cast<uint64>(Width) * Height * TextureSourceChannelCount == Pixels.size();
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
		Asset::FMountedSourceResolution Resolution;
		std::string Error;
		if (!ResolveTextureSource(*this, Resolution, Error))
			return {ETextureSourceStatus::Invalid, {}, std::move(Error)};
		if (!Resolution.bExists)
		{
			return {
				ETextureSourceStatus::Missing,
				Resolution.PhysicalPath.generic_string(),
				std::format(
					"Texture source is missing: {}. Use source-path repair to select its replacement.",
					SourceImportData.Source.SourcePath.Path)};
		}
		FXxHash128 CurrentHash;
		if (!HashTextureSource(Resolution.PhysicalPath, CurrentHash, Error))
			return {
				ETextureSourceStatus::Invalid,
				Resolution.PhysicalPath.generic_string(),
				std::move(Error)};
		if (SourceImportData.Source.HasContentHash()
			&& (CurrentHash.HashLow
					!= SourceImportData.Source.SourceContentHashLow
				|| CurrentHash.HashHigh
					!= SourceImportData.Source.SourceContentHashHigh))
		{
			return {
				ETextureSourceStatus::Changed,
				Resolution.PhysicalPath.generic_string(),
				"The mounted source bytes changed since this asset was last imported. "
				"Reimport updates this asset from the persisted source."};
		}
		return {ETextureSourceStatus::Available, Resolution.PhysicalPath.generic_string(), {}};
	}

	auto DTexture2D::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return LoadCookedPlatformData(OutError);
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

		const Asset::FAssetRuntimeConfiguration& LoadContext =
			Asset::GetAssetRuntimeConfiguration();
		if (!GetPackage())
			return FailCooked("package companion path could not be resolved.");
		Asset::FCookedPackagePayload LoadedPayload;
		if (!Asset::LoadCookedPackagePayload(
			LoadContext,
			GetPackage()->GetPackagePath(),
			CookedPayload,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game,
			LoadedPayload,
			&OutError))
		{
			return FailCooked(OutError);
		}
		const std::span<const std::byte> Bytes = LoadedPayload.Payload;

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
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"Texture2D '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}

		if (!PlatformData && !PostLoad(OutError))
		{
			OutError = std::format("Failed to cook Texture2D '{}': {}", GetObjectPath(), OutError);
			return false;
		}
		if (!PlatformData)
		{
			OutError = std::format("Failed to cook Texture2D '{}': platform data is unavailable.",
				GetObjectPath());
			return false;
		}

		std::vector<std::byte> PayloadBytes;
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

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = Texture2DPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = TexturePayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = TexturePayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		const Asset::FAssetPackageSerializationOptions CookPackageOptions =
			Context.MakePackageSerializationOptions();

		return Context.AddPackage(
			std::string(VirtualPackagePath),
			{std::move(BulkPayload)},
			[this, CookPackageOptions](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<std::byte>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != Texture2DPrimaryCookedPayloadId)
				{
					if (Error) *Error = "Texture2D cook did not produce its required descriptor.";
					return false;
				}

				FProperty* DescriptorProperty = GetClass()->FindPropertyByName("CookedPayload");
				if (!DescriptorProperty)
				{
					if (Error) *Error = "Texture2D CookedPayload reflection is unavailable.";
					return false;
				}
				auto Overrides = std::make_shared<Asset::FObjectSaveOverrides>();
				std::string OverrideError;
				if (!Overrides->AddPropertyValue(
					*this, *DescriptorProperty, Descriptors.front(), &OverrideError))
				{
					if (Error) *Error = OverrideError;
					return false;
				}
				Asset::FAssetPackageSerializationOptions SerializationOptions = CookPackageOptions;
				SerializationOptions.SaveOverrides = std::move(Overrides);
				const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, SerializationOptions);
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
			|| !IsValidUsage(State.Usage)
			|| !IsValidCompressionQuality(State.CompressionQuality)
			|| !IsValidAlphaMipMode(State.AlphaMipMode)
			|| !IsValidAlphaCoverageThreshold(State.AlphaCoverageThreshold))
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

	auto DTexture2D::PublishInterchangeProvenance(
		std::vector<std::byte> Provenance) -> void
	{
		static constexpr char Hex[] = "0123456789abcdef";
		InterchangeProvenance.resize(Provenance.size() * 2);
		for (size_t Index = 0; Index < Provenance.size(); ++Index)
		{
			const uint8 Value = static_cast<uint8>(Provenance[Index]);
			InterchangeProvenance[Index * 2] = Hex[Value >> 4];
			InterchangeProvenance[Index * 2 + 1] = Hex[Value & 0x0f];
		}
		MarkPackageDirty();
	}

	auto DTexture2D::ExchangeImportedState(DTexture2D& Other) -> void
	{
		if (&Other == this) return;
		std::swap(SourceImportData, Other.SourceImportData);
		std::swap(InterchangeProvenance, Other.InterchangeProvenance);
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
		if (PlatformData && PlatformData->IsValid()) QueueRenderResourceBuild();
		else InvalidateRenderResource();
		if (Other.PlatformData && Other.PlatformData->IsValid())
			Other.QueueRenderResourceBuild();
		else Other.InvalidateRenderResource();
		MarkPackageDirty();
		Other.MarkPackageDirty();
	}

}
