#include "Texture/Texture2D.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "DObject/DurinPropertyTypes.h"
#include "Hash/XxHash.h"
#include "Serialization/Archive.h"
#include "DynamicRHI.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/TextureDerivedData.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TextureSourceChannelCount = 4;
	} // namespace

	auto IsValidTextureUsage(ETextureUsage Usage) -> bool
	{
		return Usage == ETextureUsage::Color || Usage == ETextureUsage::Normal
			|| Usage == ETextureUsage::DataMask;
	}

	auto GetDefaultTextureSRGB(ETextureUsage Usage) -> bool
	{
		return Usage == ETextureUsage::Color;
	}

	auto IsValidTextureCompressionQuality(
		ETextureCompressionQuality Quality) -> bool
	{
		return Quality == ETextureCompressionQuality::Low
			|| Quality == ETextureCompressionQuality::Normal
			|| Quality == ETextureCompressionQuality::High;
	}

	auto IsValidTextureAlphaMipMode(ETextureAlphaMipMode Mode) -> bool
	{
		return Mode == ETextureAlphaMipMode::Average
			|| Mode == ETextureAlphaMipMode::PreserveCoverage;
	}

	auto IsValidTextureAlphaCoverageThreshold(float Threshold) -> bool
	{
		return std::isfinite(Threshold) && Threshold > 0.0f && Threshold < 1.0f;
	}

	auto FTextureSourceData::IsValid() const -> bool
	{
		return Format == ETextureSourceFormat::RGBA8
			&& Width > 0
			&& Height > 0
			&& Width <= 16384 && Height <= 16384
			&& static_cast<uint64>(Width) * Height * TextureSourceChannelCount == Pixels.size()
			&& Pixels.size() <= MaximumTexture2DImportedPixelBytes;
	}

	auto FTextureSourceData::GetImportedDataIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FEditorBulkData PayloadIdentity;
		if (!PayloadIdentity.UpdatePayload(Pixels)) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(Texture2DImportedDataSchemaVersion);
		Builder.UpdateValue(Width);
		Builder.UpdateValue(Height);
		Builder.UpdateValue(SourceChannelCount);
		Builder.UpdateValue(static_cast<uint8>(Format));
		Builder.UpdateValue(bHasTransparency);
		Builder.UpdateValue(PayloadIdentity.GetPayloadId());
		return Builder.Finalize();
	}

	auto FTexture2DImportedData::IsValid() const -> bool
	{
		const uint64 ExpectedByteCount = static_cast<uint64>(Width)
			* Height * ::Durin::TextureSourceChannelCount;
		return SchemaVersion == Texture2DImportedDataSchemaVersion
			&& Format == ETextureSourceFormat::RGBA8
			&& Width > 0 && Height > 0 && Width <= 16384 && Height <= 16384
			&& SourceChannelCount > 0 && SourceChannelCount <= TextureSourceChannelCount
			&& ExpectedByteCount == Pixels.GetPayloadSize()
			&& ExpectedByteCount <= MaximumTexture2DImportedPixelBytes;
	}

	auto FTexture2DImportedData::SetSourceData(
		const FTextureSourceData& Source) -> bool
	{
		if (!Source.IsValid()
			|| !Pixels.UpdatePayload(Source.Pixels))
			return false;
		Width = Source.Width;
		Height = Source.Height;
		SourceChannelCount = Source.SourceChannelCount;
		Format = Source.Format;
		bHasTransparency = Source.bHasTransparency;
		SchemaVersion = Texture2DImportedDataSchemaVersion;
		return IsValid();
	}

	auto FTexture2DImportedData::ToSourceData() const -> FTextureSourceData
	{
		const FSharedByteBuffer Payload = Pixels.GetPayload().Wait().Buffer;
		const std::span<const std::byte> Bytes = Payload.GetBytes();
		return {
			.Pixels = FByteArray(Bytes.begin(), Bytes.end()),
			.Width = Width,
			.Height = Height,
			.SourceChannelCount = SourceChannelCount,
			.Format = Format,
			.bHasTransparency = bHasTransparency};
	}

	auto FTexture2DImportedData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(Width);
		Builder.UpdateValue(Height);
		Builder.UpdateValue(SourceChannelCount);
		Builder.UpdateValue(static_cast<uint8>(Format));
		Builder.UpdateValue(bHasTransparency);
		Builder.UpdateValue(Pixels.GetPayloadId());
		return Builder.Finalize();
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

	auto DTexture2D::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Texture2D cooked platform data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* Value = &CookedPlatformData;
		if (Ar.IsSaving())
		{
			if (!PlatformData || !PlatformData->IsValid())
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Texture2D cooked platform data is unavailable.");
				return;
			}
			FByteArray Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload);
			PlatformData->Serialize(Writer, {
				.TargetPlatform = ECookTargetPlatform::Win64,
				.TargetProfile = ECookTargetProfile::Game});
			std::string Error;
			if (Writer.HasError() || !FBulkData::TryCreateDetached(Bytes, Projection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error.empty()
					? std::string(Writer.GetError()) : std::move(Error));
				return;
			}
			Value = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DTexture2D"),
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		Value->Serialize(Ar, {.Alignment = TexturePayloadAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
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

	auto DTexture2D::GetPlatformData() const -> const FTexturePlatformData*
	{
		if (!PlatformData && GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedPlatformData.GetMetadata().LogicalSize != 0)
		{
			std::string Error;
			const_cast<DTexture2D*>(this)->LoadCookedPlatformData(Error);
		}
		return PlatformData.get();
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

	auto DTexture2D::PostLoad(std::string& OutError) -> bool
	{
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedPlatformData.GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked Texture2D '{}': required PlatformData field is missing.",
					GetObjectPath());
				return false;
			}
			SourceData.reset();
			PlatformData.reset();
			DerivedDataKey.clear();
			bLoadedFromDerivedDataCache = false;
			DerivedDataDiagnostic = {
				.Status = ETextureDerivedDataStatus::CookedLoaded,
				.Message = std::format(
					"Loaded cooked Texture2D metadata for '{}'.", GetObjectPath())};
			BuildStatus = ETextureBuildStatus::Ready;
			LastBuildError.clear();
			OutError.clear();
			return true;
		}
		BuildStatus = ETextureBuildStatus::Unbuilt;
		LastBuildError.clear();
		DerivedDataKey.clear();
		DerivedDataDiagnostic = {};
		bLoadedFromDerivedDataCache = false;
		if (!ImportedData.IsValid())
		{
			OutError = "Texture2D canonical imported data is missing or invalid.";
		}
		else if (BuildTexture2DSynchronously(*this, {
			.SourceData = ImportedData.ToSourceData(),
			.Settings = {
				.Usage = Usage,
				.CompressionQuality = CompressionQuality,
				.AlphaMipMode = AlphaMipMode,
				.AlphaCoverageThreshold = AlphaCoverageThreshold,
				.MaxResolution = MaxResolution,
				.bSRGB = bSRGB}}, {
			.bMarkPackageDirty = false,
			.bReportLoadMutation = false,
			.bSourceDecoderInvoked = false}, OutError)) return true;
		if (BuildStatus == ETextureBuildStatus::Unbuilt)
			PublishUncookedLoadFailure(
				ETextureDerivedDataStatus::Incompatible,
				ETextureBuildStatus::BuildFailure,
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

		std::span<const std::byte> Bytes;
		if (!CookedPlatformData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);

		auto CandidatePlatformData = std::make_unique<FTexturePlatformData>();
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		CandidatePlatformData->Serialize(PayloadAr, {
			.TargetPlatform = ECookTargetPlatform::Win64,
			.TargetProfile = ECookTargetProfile::Game});
		if (PayloadAr.HasError() || !RequireArchiveEnd(PayloadAr))
		{
			CookedPlatformData.UnlockReadOnly();
			return FailCooked(std::string(PayloadAr.GetError()));
		}
		if (!CookedPlatformData.UnlockReadOnly(&OutError)) return FailCooked(OutError);

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

	auto DTexture2D::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
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

		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
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
			OutError = "Texture2D imported state must be applied on the game thread.";
			return false;
		}
		if (!State.SourceData || !State.SourceData->IsValid()
			|| !State.PlatformData || !State.PlatformData->IsValid()
			|| State.DerivedDataKey.empty()
			|| !IsValidTextureUsage(State.Usage)
			|| !IsValidTextureCompressionQuality(State.CompressionQuality)
			|| !IsValidTextureAlphaMipMode(State.AlphaMipMode)
			|| !IsValidTextureAlphaCoverageThreshold(State.AlphaCoverageThreshold))
		{
			OutError = "Texture2D imported state is incomplete or invalid.";
			return false;
		}

		FTexture2DImportedData ImportedCandidate;
		if (!ImportedCandidate.SetSourceData(*State.SourceData))
		{
			OutError = "Texture2D canonical imported data could not be captured.";
			return false;
		}

		SourceWidth = State.SourceData->Width;
		SourceHeight = State.SourceData->Height;
		SourceChannelCount = State.SourceData->SourceChannelCount;
		bSourceHasTransparency = State.SourceData->bHasTransparency;
		ImportedData = std::move(ImportedCandidate);
		SourceData = std::move(State.SourceData);
		PlatformData = std::move(State.PlatformData);
		DerivedDataKey = std::move(State.DerivedDataKey);
		Usage = State.Usage;
		bSRGB = State.bSRGB;
		MaxResolution = State.MaxResolution;
		CompressionQuality = State.CompressionQuality;
		AlphaMipMode = State.AlphaMipMode;
		AlphaCoverageThreshold = State.AlphaCoverageThreshold;
		bLoadedFromDerivedDataCache = State.bLoadedFromDerivedDataCache;
		DerivedDataDiagnostic = {
			.Status = State.bLoadedFromDerivedDataCache
				? ETextureDerivedDataStatus::Hit
				: ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = State.BuildDiagnostic.empty()
				? (State.bLoadedFromDerivedDataCache
					? "Applied cached Texture2D build result."
					: "Applied normalized Texture2D build result.")
				: std::format(
					"Applied {} Texture2D build result; DDC persistence was best effort: {}",
					State.bLoadedFromDerivedDataCache ? "cached" : "normalized",
					State.BuildDiagnostic),
			.bSourceDecoderInvoked = State.bSourceDecoderInvoked};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		if (State.bMarkPackageDirty) MarkPackageDirty();
		if (State.bReportLoadMutation)
		{
			ReportAssetLoadMutation(
				this,
				"Engine.Texture2D.SourceIdentity",
				"Texture source identity metadata was reconciled by an uncooked post-load build.");
		}
		OutError.clear();
		return true;
	}

	auto DTexture2D::PublishDerivedDataLoad(
		std::unique_ptr<FTexturePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		std::string& OutError) -> bool
	{
		if (!IsInGameThread() || !InPlatformData || !InPlatformData->IsValid()
			|| InDerivedDataKey.empty())
		{
			OutError = "Texture2D derived-data load result is incomplete or invalid.";
			return false;
		}
		SourceData.reset();
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		bLoadedFromDerivedDataCache = true;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Hit,
			.Key = DerivedDataKey,
			.Message = std::format("Texture2D DDC hit for key {}.", DerivedDataKey)};
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

	auto DTexture2D::PublishAssetImportData(
		DAssetImportData& Value, std::string& OutError) -> bool
	{
		if (Value.GetOuter() != this)
		{
			OutError = "Texture2D import data must be an owned inner object.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		AssetImportData = &Value;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DTexture2D::ExchangeImportedState(DTexture2D& Other) -> void
	{
		if (&Other == this) return;
		std::swap(SourceWidth, Other.SourceWidth);
		std::swap(SourceHeight, Other.SourceHeight);
		std::swap(SourceChannelCount, Other.SourceChannelCount);
		std::swap(bSourceHasTransparency, Other.bSourceHasTransparency);
		std::swap(ImportedData, Other.ImportedData);
		std::swap(Usage, Other.Usage);
		std::swap(bSRGB, Other.bSRGB);
		std::swap(MaxResolution, Other.MaxResolution);
		std::swap(CompressionQuality, Other.CompressionQuality);
		std::swap(AlphaMipMode, Other.AlphaMipMode);
		std::swap(AlphaCoverageThreshold, Other.AlphaCoverageThreshold);
		std::swap(CookedPlatformData, Other.CookedPlatformData);
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
