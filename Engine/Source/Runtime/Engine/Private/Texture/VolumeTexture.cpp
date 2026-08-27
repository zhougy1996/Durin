#include "Texture/VolumeTexture.h"

#include "DObject/Package.h"

#include "AssetCook.h"
#include "DObject/DurinPropertyTypes.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureRenderResource.h"
#include "Texture/VolumeTexturePostLoad.h"

namespace Durin
{
	namespace
	{
		auto ToPixelFormat(EVolumeTextureFormat Format) -> EPixelFormat
		{
			switch (Format)
			{
			case EVolumeTextureFormat::R8_UNORM: return EPixelFormat::R8_UNORM;
			case EVolumeTextureFormat::RG8_UNORM: return EPixelFormat::RG8_UNORM;
			case EVolumeTextureFormat::RGBA8_UNORM: return EPixelFormat::RGBA8_UNORM;
			case EVolumeTextureFormat::R16_FLOAT: return EPixelFormat::R16_FLOAT;
			case EVolumeTextureFormat::RGBA16_FLOAT: return EPixelFormat::RGBA16_FLOAT;
			default: return EPixelFormat::Unknown;
			}
		}
	}

	auto FVolumeTextureSourceData::IsValid() const -> bool
	{
		if (PayloadSchemaVersion != VolumeTextureSourcePayloadSchemaVersion
			|| Width == 0 || Height == 0 || Depth == 0
			|| Width > MaximumVolumeTextureDimension
			|| Height > MaximumVolumeTextureDimension
			|| Depth > MaximumVolumeTextureDimension)
			return false;
		const EPixelFormat PixelFormat = ToPixelFormat(Format);
		const FPixelFormatInfo& Info = GetPixelFormatInfo(PixelFormat);
		if (Info.BlockSize != 1 || Info.BytesPerBlock == 0) return false;
		const uint64 Texels = static_cast<uint64>(Width) * Height * Depth;
		return Texels <= MaximumTexturePayloadBytes / Info.BytesPerBlock
			&& Voxels.GetBulkData().GetDescriptor().PayloadId == VolumeTextureSourcePayloadId
			&& GetVoxelBytes().size() == Texels * Info.BytesPerBlock;
	}

	auto FVolumeTextureSourceData::SetVoxelBytes(std::span<const std::byte> Bytes) -> bool
	{
		return Voxels.ReplaceBytes(VolumeTextureSourcePayloadId, Bytes);
	}

	auto FVolumeTextureMipData::IsValid(EPixelFormat PixelFormat) const -> bool
	{
		if (Width == 0 || Height == 0 || Depth == 0) return false;
		const FPixelFormatLayout Slice = GetPixelFormatLayout(PixelFormat, Width, Height);
		if (Slice.RowPitch != RowPitch || Slice.DataSize != DepthPitch
			|| Depth > std::numeric_limits<uint64>::max() / DepthPitch)
			return false;
		return Voxels.size() == static_cast<uint64>(DepthPitch) * Depth;
	}

	auto FVolumeTexturePlatformData::IsValid() const -> bool
	{
		if (Mips.empty()) return false;
		ETextureStablePixelFormat Stable;
		switch (PixelFormat)
		{
		case EPixelFormat::R8_UNORM: Stable = ETextureStablePixelFormat::R8_UNORM; break;
		case EPixelFormat::RG8_UNORM: Stable = ETextureStablePixelFormat::RG8_UNORM; break;
		case EPixelFormat::RGBA8_UNORM: Stable = ETextureStablePixelFormat::RGBA8_UNORM; break;
		case EPixelFormat::R16_FLOAT: Stable = ETextureStablePixelFormat::R16_FLOAT; break;
		case EPixelFormat::RGBA16_FLOAT: Stable = ETextureStablePixelFormat::RGBA16_FLOAT; break;
		default: return false;
		}
		(void)Stable;
		for (size_t MipIndex = 0; MipIndex < Mips.size(); ++MipIndex)
		{
			const FVolumeTextureMipData& Mip = Mips[MipIndex];
			if (!Mip.IsValid(PixelFormat)) return false;
			if (MipIndex > 0)
			{
				const FVolumeTextureMipData& Previous = Mips[MipIndex - 1];
				if (Mip.Width != std::max(1u, Previous.Width / 2)
					|| Mip.Height != std::max(1u, Previous.Height / 2)
					|| Mip.Depth != std::max(1u, Previous.Depth / 2)) return false;
			}
		}
		return Mips.size() <= MaximumTextureMipCount
			&& Mips.front().Width <= MaximumVolumeTextureDimension
			&& Mips.front().Height <= MaximumVolumeTextureDimension
			&& Mips.front().Depth <= MaximumVolumeTextureDimension
			&& Mips.back().Width == 1 && Mips.back().Height == 1
			&& Mips.back().Depth == 1;
	}

	DVolumeTexture::DVolumeTexture(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	DVolumeTexture::~DVolumeTexture() = default;

	auto DVolumeTexture::CreateRenderResourceCandidate(
		FTextureReference* TextureReference, uint64 Revision,
		const std::shared_ptr<FTextureResourceCompletion>& Completion)
		-> std::unique_ptr<FTextureAssetResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FVolumeTextureResource>(TextureReference,
			std::make_shared<const FVolumeTexturePlatformData>(*PlatformData),
			Revision, Completion);
	}

	auto DVolumeTexture::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return LoadCookedPlatformData(OutError);
		return InvokeVolumeTextureUncookedPostLoadHandler(*this, OutError);
	}

	auto DVolumeTexture::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			BuildStatus = ETextureBuildStatus::BuildFailure;
			LastBuildError = std::format("Cooked volume texture '{}': {}", GetObjectPath(), Message);
			OutError = LastBuildError;
			return false;
		};
		if (CookedPayload.PayloadId != VolumeTexturePrimaryCookedPayloadId
			|| CookedPayload.PayloadSchemaVersion != TexturePayloadSchemaVersion
			|| CookedPayload.CompressionMethod != static_cast<uint32>(
				Asset::ECookedPayloadCompression::None))
			return FailCooked("required TXPL descriptor is missing or incompatible.");
		if (!GetPackage()) return FailCooked("package companion path is unavailable.");
		Asset::FCookedPackagePayload Loaded;
		if (!Asset::LoadCookedPackagePayload(Asset::GetAssetRuntimeConfiguration(),
				GetPackage()->GetPackagePath(), CookedPayload,
				Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game,
				Loaded, &OutError))
			return FailCooked(OutError);
		auto Candidate = std::make_unique<FVolumeTexturePlatformData>();
		FCanonicalMemoryReader Ar(Loaded.Payload, EArchivePurpose::CookedPayload);
		Candidate->Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
			return FailCooked(std::string(Ar.GetError()));
		if (!Candidate->IsValid())
			return FailCooked("platform data is invalid.");
		PlatformData = std::move(Candidate);
		DerivedDataKey.clear();
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DVolumeTexture::AddToCook(Asset::FCookContext& Context,
		std::string_view VirtualPackagePath, std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = "Volume textures support only the Win64 game cook target.";
			return false;
		}
		if (!PlatformData || !PlatformData->IsValid())
		{
			if (!PostLoad(OutError)) return false;
		}
		std::vector<std::byte> PayloadBytes;
		FCanonicalMemoryWriter Ar(PayloadBytes, EArchivePurpose::CookedPayload);
		PlatformData->Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Ar.HasError())
		{
			OutError = Ar.GetFailure()->Message;
			return false;
		}
		Asset::FCookedBulkPayload Bulk{.PayloadId = VolumeTexturePrimaryCookedPayloadId,
			.Flags = 1, .PayloadSchemaVersion = TexturePayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = TexturePayloadAlignment, .Bytes = std::move(PayloadBytes)};
		const Asset::FAssetPackageSerializationOptions CookPackageOptions =
			Context.MakePackageSerializationOptions();
		return Context.AddPackage(std::string(VirtualPackagePath), {std::move(Bulk)},
			[this, CookPackageOptions](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<std::byte>& OutPackageBytes, std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != VolumeTexturePrimaryCookedPayloadId)
				{
					if (Error) *Error = "Volume texture cook did not produce its descriptor.";
					return false;
				}
				FProperty* DescriptorProperty = GetClass()->FindPropertyByName("CookedPayload");
				if (!DescriptorProperty)
				{
					if (Error) *Error = "VolumeTexture CookedPayload reflection is unavailable.";
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
				Asset::FAssetPackageSerializationOptions Options = CookPackageOptions;
				Options.SaveOverrides = std::move(Overrides);
				const Asset::FAssetResult Result = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, Options);
				if (!Result && Error) *Error = Result.Message;
				return static_cast<bool>(Result);
			}, &OutError);
	}

	auto DVolumeTexture::PublishBuiltData(FVolumeTextureSourceData InSourceData,
		FVolumeTextureBuildSettings InBuildSettings,
		std::unique_ptr<FVolumeTexturePlatformData> InPlatformData,
		std::string InDerivedDataKey, std::string InPersistenceDiagnostic,
		std::string& OutError) -> bool
	{
		if (!InSourceData.IsValid() || !InPlatformData || !InPlatformData->IsValid()
			|| InDerivedDataKey.empty()
			|| ToPixelFormat(InBuildSettings.OutputFormat) != InPlatformData->PixelFormat
			|| InSourceData.Format != InBuildSettings.OutputFormat
			|| InBuildSettings.MipFilter != EVolumeTextureMipFilter::Box)
		{
			OutError = "Volume texture publication requires compatible validated source, settings, payload, and key.";
			return false;
		}
		SourceData = std::move(InSourceData);
		BuildSettings = InBuildSettings;
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		DerivedDataDiagnostic = {
			.Status = ETextureDerivedDataStatus::Rebuilt,
			.Key = DerivedDataKey,
			.Message = InPersistenceDiagnostic.empty()
				? "Built VolumeTexture from canonical voxels."
				: std::format("Built VolumeTexture from canonical voxels; DDC persistence was best effort: {}",
					InPersistenceDiagnostic),
			.bSourceDecoderInvoked = false};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DVolumeTexture::PublishDerivedDataLoad(
		std::unique_ptr<FVolumeTexturePlatformData> InPlatformData,
		std::string InDerivedDataKey, std::string& OutError) -> bool
	{
		if (!InPlatformData || !InPlatformData->IsValid() || InDerivedDataKey.empty())
		{
			OutError = "Volume texture DDC publication requires valid platform data and key.";
			return false;
		}
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		DerivedDataDiagnostic = {.Status = ETextureDerivedDataStatus::Hit,
			.Key = DerivedDataKey,
			.Message = "Loaded VolumeTexture platform data from DDC."};
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		QueueRenderResourceBuild();
		OutError.clear();
		return true;
	}

	auto DVolumeTexture::PublishAssetImportData(
		AssetImport::DAssetImportData& Value, std::string& OutError) -> bool
	{
		if (Value.GetOuter() != this)
		{
			OutError = "VolumeTexture import data must be an owned inner object.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		AssetImportData = &Value;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DVolumeTexture::ExchangeBuiltState(DVolumeTexture& Other) noexcept -> void
	{
		if (&Other == this) return;
		std::swap(SourceData, Other.SourceData);
		std::swap(BuildSettings, Other.BuildSettings);
		std::swap(CookedPayload, Other.CookedPayload);
		std::swap(PlatformData, Other.PlatformData);
		std::swap(DerivedDataKey, Other.DerivedDataKey);
		std::swap(DerivedDataDiagnostic, Other.DerivedDataDiagnostic);
		std::swap(BuildStatus, Other.BuildStatus);
		std::swap(LastBuildError, Other.LastBuildError);
		auto RefreshRenderResource = [](DVolumeTexture& Texture) {
			if (Texture.PlatformData && Texture.PlatformData->IsValid())
				Texture.QueueRenderResourceBuild();
			else
				Texture.InvalidateRenderResource();
		};
		RefreshRenderResource(*this);
		RefreshRenderResource(Other);
		MarkPackageDirty();
		Other.MarkPackageDirty();
	}

	auto DVolumeTexture::RefreshBuildStatus() -> void
	{
		const auto& Completion = GetRenderCompletion();
		if (Completion->GetFailedRevision() != GetBuildRevision()) return;
		BuildStatus = Completion->GetFailureReason() == ETextureRenderFailure::UnsupportedFormat
			? ETextureBuildStatus::UnsupportedFormat : ETextureBuildStatus::UploadFailure;
		LastBuildError = BuildStatus == ETextureBuildStatus::UnsupportedFormat
			? "The current RHI does not support this volume texture format and usage."
			: "GPU volume texture creation or upload failed.";
	}
}
