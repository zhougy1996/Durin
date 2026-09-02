#include "Texture/TextureCube.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DynamicRHI.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Texture/TextureCubeBuildProvider.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureDerivedData.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		auto ValidateCubeSourceData(const FTextureCubeSourceData& SourceData, std::string& OutError) -> bool
		{
			const FTextureSourceData& Reference = SourceData.Faces[0];
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				const FTextureSourceData& Face = SourceData.Faces[FaceIndex];
				if (!Face.IsValid())
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[FaceIndex]);
					return false;
				}
				if (Face.Width != Face.Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[FaceIndex], Face.Width, Face.Height);
					return false;
				}
				if (Face.Width != Reference.Width || Face.Height != Reference.Height)
				{
					OutError = std::format("{} face dimensions {}x{} do not match PositiveX {}x{}; all faces must be identical.",
						FaceNames[FaceIndex], Face.Width, Face.Height, Reference.Width, Reference.Height);
					return false;
				}
				if (Face.SourceChannelCount != Reference.SourceChannelCount)
				{
					OutError = std::format("{} face source channel count {} does not match PositiveX {}; all faces must use an identical source format.",
						FaceNames[FaceIndex], Face.SourceChannelCount, Reference.SourceChannelCount);
					return false;
				}
			}
			return true;
		}

	}

	auto FTextureCubeSourceData::IsValid() const -> bool
	{
		std::string Error;
		return ValidateCubeSourceData(*this, Error);
	}

	auto FTextureCubeImportedData::IsValid() const -> bool
	{
		const uint64 ExpectedByteCount = static_cast<uint64>(FaceDimension)
			* FaceDimension * 4ull * TextureCubeFaceCount;
		return SchemaVersion == TextureCubeImportedDataSchemaVersion
			&& FaceDimension > 0 && FaceDimension <= 16384
			&& SourceChannelCount > 0 && SourceChannelCount <= 4
			&& ExpectedByteCount == Pixels.GetPayloadSize()
			&& ExpectedByteCount <= MaximumTextureCubeImportedPixelBytes
			&& (TransparencyMask & ~0x3fu) == 0;
	}

	auto FTextureCubeImportedData::SetSourceData(
		const FTextureCubeSourceData& Source) -> bool
	{
		if (!Source.IsValid()) return false;
		FByteArray Bytes;
		const uint64 TotalBytes = static_cast<uint64>(Source.Faces[0].Pixels.size())
			* TextureCubeFaceCount;
		if (TotalBytes > MaximumTextureCubeImportedPixelBytes) return false;
		Bytes.reserve(static_cast<size_t>(TotalBytes));
		uint8 NewTransparencyMask = 0;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const FTextureSourceData& Face = Source.Faces[Index];
			Bytes.insert(Bytes.end(), Face.Pixels.begin(), Face.Pixels.end());
			if (Face.bHasTransparency) NewTransparencyMask |= static_cast<uint8>(1u << Index);
		}
		if (!Pixels.UpdatePayload(Bytes)) return false;
		FaceDimension = Source.Faces[0].Width;
		SourceChannelCount = Source.Faces[0].SourceChannelCount;
		TransparencyMask = NewTransparencyMask;
		SchemaVersion = TextureCubeImportedDataSchemaVersion;
		return IsValid();
	}

	auto FTextureCubeImportedData::ToSourceData() const -> FTextureCubeSourceData
	{
		FTextureCubeSourceData Result;
		if (!IsValid()) return Result;
		const FSharedByteBuffer Payload = Pixels.GetPayload().Wait().Buffer;
		const std::span<const std::byte> Bytes = Payload.GetBytes();
		const size_t FaceBytes = static_cast<size_t>(FaceDimension) * FaceDimension * 4;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const auto Face = Bytes.subspan(Index * FaceBytes, FaceBytes);
			Result.Faces[Index] = {
				.Pixels = FByteArray(Face.begin(), Face.end()),
				.Width = FaceDimension,
				.Height = FaceDimension,
				.SourceChannelCount = SourceChannelCount,
				.Format = ETextureSourceFormat::RGBA8,
				.bHasTransparency = (TransparencyMask & (1u << Index)) != 0};
		}
		return Result;
	}

	auto FTextureCubeImportedData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(FaceDimension);
		Builder.UpdateValue(SourceChannelCount);
		Builder.UpdateValue(TransparencyMask);
		Builder.UpdateValue(Pixels.GetPayloadId());
		return Builder.Finalize();
	}

	auto FTextureCubePlatformData::IsValid() const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown) return false;
		const FTexturePlatformData& Reference = Faces[0];
		if (!Reference.IsValid() || Reference.PixelFormat != PixelFormat || Reference.Mips.front().Width != Reference.Mips.front().Height)
			return false;
		for (const FTexturePlatformData& Face : Faces)
		{
			if (!Face.IsValid() || Face.PixelFormat != PixelFormat || Face.Mips.size() != Reference.Mips.size()) return false;
			for (size_t MipIndex = 0; MipIndex < Face.Mips.size(); ++MipIndex)
			{
				if (Face.Mips[MipIndex].Width != Reference.Mips[MipIndex].Width
					|| Face.Mips[MipIndex].Height != Reference.Mips[MipIndex].Height
					|| Face.Mips[MipIndex].RowPitch != Reference.Mips[MipIndex].RowPitch) return false;
			}
		}
		return true;
	}

	DTextureCube::DTextureCube(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	DTextureCube::~DTextureCube() = default;

	auto DTextureCube::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"TextureCube cooked platform data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* Value = &CookedPlatformData;
		if (Ar.IsSaving())
		{
			if (!PlatformData || !PlatformData->IsValid())
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"TextureCube cooked platform data is unavailable.");
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
					? Writer.GetFailure()->Message : std::move(Error));
				return;
			}
			Value = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DTextureCube"),
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		Value->Serialize(Ar, {.Alignment = TexturePayloadAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DTextureCube::GetBuiltFaceDimension() const -> uint32
	{
		if (PlatformData && !PlatformData->Faces[0].Mips.empty())
			return PlatformData->Faces[0].Mips[0].Width;
		return SourceData && SourceData->IsValid() ? SourceData->Faces[0].Width : 0;
	}

	auto DTextureCube::GetBuiltMipCount() const -> uint32
	{
		return PlatformData && PlatformData->IsValid()
			? static_cast<uint32>(PlatformData->Faces[0].Mips.size()) : 0;
	}

	auto DTextureCube::GetBuiltPixelFormat() const -> EPixelFormat
	{
		return PlatformData && PlatformData->IsValid() ? PlatformData->PixelFormat : EPixelFormat::Unknown;
	}

	auto DTextureCube::InvalidatePlatformData() -> void
	{
		PlatformData.reset();
		InvalidateRenderResource();
	}

	auto DTextureCube::GetPlatformData() const -> const FTextureCubePlatformData*
	{
		if (!PlatformData && GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedPlatformData.GetMetadata().LogicalSize != 0)
		{
			std::string Error;
			const_cast<DTextureCube*>(this)->LoadCookedPlatformData(Error);
		}
		return PlatformData.get();
	}

	auto DTextureCube::CreateRenderResourceCandidate(
		FTextureReference* TextureReference,
		uint64 Revision,
		const std::shared_ptr<FTextureResourceCompletion>& Completion)
		-> std::unique_ptr<FTextureAssetResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTextureCubeResource>(
			TextureReference,
			std::make_shared<const FTextureCubePlatformData>(*PlatformData),
			Revision,
			Completion);
	}

	auto DTextureCube::RebuildPlatformData(std::string& OutError) -> bool
	{
		if (!ImportedData.IsValid())
		{
			OutError = "TextureCube canonical imported data is missing or invalid.";
			return false;
		}
		return BuildTextureCubeSynchronously(*this, {.Input = FTextureCubeFacesBuildInput{
			.ImportedData = ImportedData, .SourceLayout = SourceLayout,
			.OriginalSourceWidth = OriginalSourceWidth,
			.OriginalSourceHeight = OriginalSourceHeight,
			.PanoramaFaceDimension = PanoramaFaceDimension,
			.PanoramaExposureEV = PanoramaExposureEV,
			.Settings = {.bSRGB = bSRGB}}}, {}, OutError);
	}

	auto DTextureCube::PostLoad(std::string& OutError) -> bool
	{
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedPlatformData.GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked TextureCube '{}': required PlatformData field is missing.",
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
					"Loaded cooked TextureCube metadata for '{}'.", GetObjectPath())};
			BuildStatus = ETextureBuildStatus::Ready;
			LastBuildError.clear();
			OutError.clear();
			return true;
		}
		if (!ImportedData.IsValid())
		{
			OutError = "TextureCube canonical imported data is missing or invalid.";
			return false;
		}
		return BuildTextureCubeSynchronously(*this, {.Input = FTextureCubeFacesBuildInput{
			.ImportedData = ImportedData, .SourceLayout = SourceLayout,
			.OriginalSourceWidth = OriginalSourceWidth,
			.OriginalSourceHeight = OriginalSourceHeight,
			.PanoramaFaceDimension = PanoramaFaceDimension,
			.PanoramaExposureEV = PanoramaExposureEV,
			.Settings = {.bSRGB = bSRGB}}}, {
			.bMarkPackageDirty = false, .bSourceDecoderInvoked = false}, OutError);
	}

	auto DTextureCube::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			DerivedDataDiagnostic.Status = ETextureDerivedDataStatus::CookedFailure;
			DerivedDataDiagnostic.Message = std::format(
				"Cooked TextureCube '{}': {}", GetObjectPath(), Message);
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = DerivedDataDiagnostic.Message;
			OutError = LastBuildError;
			return false;
		};
		std::span<const std::byte> Bytes;
		if (!CookedPlatformData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		auto CandidatePlatformData = std::make_unique<FTextureCubePlatformData>();
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
			.Message = std::format("Loaded cooked TextureCube payload for '{}'.", GetObjectPath())};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DTextureCube::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
		{
			OutError = std::format(
				"TextureCube '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		if (!PlatformData && !PostLoad(OutError))
		{
			OutError = std::format("Failed to cook TextureCube '{}': {}", GetObjectPath(), OutError);
			return false;
		}
		if (!PlatformData)
		{
			OutError = std::format("Failed to cook TextureCube '{}': platform data is unavailable.",
				GetObjectPath());
			return false;
		}
		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DTextureCube::RefreshBuildStatus() -> void
	{
		const std::shared_ptr<FTextureResourceCompletion>& Completion =
			GetRenderCompletion();
		if (Completion->GetFailedRevision() == GetBuildRevision())
		{
			if (Completion->GetFailureReason()
				== ETextureRenderFailure::UnsupportedFormat)
			{
				BuildStatus = ETextureBuildStatus::UnsupportedFormat;
				LastBuildError = "The current RHI does not support this cube texture format and usage.";
			}
			else
			{
				BuildStatus = ETextureBuildStatus::UploadFailure;
				LastBuildError = "GPU cube texture creation or upload failed.";
			}
		}
	}

	auto DTextureCube::PublishAssetImportData(
		DAssetImportData& Value, std::string& OutError) -> bool
	{
		if (Value.GetOuter() != this)
		{
			OutError = "TextureCube import data must be an owned inner object.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		AssetImportData = &Value;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DTextureCube::ApplyBuildResult(
		FTextureCubeImportedData InImportedData,
		ETextureCubeSourceLayout InSourceLayout,
		uint32 InPanoramaFaceDimension,
		float InPanoramaExposureEV,
		uint32 InOriginalSourceWidth,
		uint32 InOriginalSourceHeight,
		bool bInSRGB,
		std::unique_ptr<FTextureCubePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		FTextureDerivedDataDiagnostic InDiagnostic,
		bool bMarkPackageDirty) -> void
	{
		check(InPlatformData && InPlatformData->IsValid());
		check(InImportedData.IsValid());
		ImportedData = std::move(InImportedData);
		SourceLayout = InSourceLayout;
		PanoramaFaceDimension = InPanoramaFaceDimension;
		PanoramaExposureEV = InPanoramaExposureEV;
		OriginalSourceWidth = InOriginalSourceWidth;
		OriginalSourceHeight = InOriginalSourceHeight;
		bSRGB = bInSRGB;
		SourceData = (InDiagnostic.Status == ETextureDerivedDataStatus::Rebuilt
			|| InDiagnostic.bSourceDecoderInvoked)
			? std::make_unique<FTextureCubeSourceData>(ImportedData.ToSourceData())
			: nullptr;
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		DerivedDataDiagnostic = std::move(InDiagnostic);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		bLoadedFromDerivedDataCache =
			DerivedDataDiagnostic.Status == ETextureDerivedDataStatus::Hit;
		QueueRenderResourceBuild();
		if (bMarkPackageDirty) MarkPackageDirty();
	}

	auto DTextureCube::PublishDerivedDataLoad(
		std::unique_ptr<FTextureCubePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		std::string& OutError) -> bool
	{
		if (!InPlatformData || !InPlatformData->IsValid() || InDerivedDataKey.empty())
		{
			OutError = "TextureCube DDC result application requires valid platform data and key.";
			return false;
		}
		SourceData.reset();
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		bLoadedFromDerivedDataCache = true;
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Hit,
			.Key = DerivedDataKey,
			.Message = "Loaded TextureCube platform data from DDC."};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}
}
