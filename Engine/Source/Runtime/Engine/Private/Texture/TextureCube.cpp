#include "Texture/TextureCube.h"

#include "DObject/Package.h"

#include "AssetCook.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DynamicRHI.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureCubePostLoad.h"
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
		std::vector<std::byte> Bytes;
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
				.Pixels = std::vector<std::byte>(Face.begin(), Face.end()),
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
		if (InvokeTextureCubeUncookedPostLoadHandler(*this, OutError)) return true;
		if (OutError.empty()) OutError = "TextureCube rebuild policy is unavailable.";
		return false;
	}

	auto DTextureCube::PostLoad(std::string& OutError) -> bool
	{
		if (Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return LoadCookedPlatformData(OutError);
		return InvokeTextureCubeUncookedPostLoadHandler(*this, OutError);
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
		if (CookedPayload.PayloadId != TextureCubePrimaryCookedPayloadId
			|| CookedPayload.LocationKind
				!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != TexturePayloadSchemaVersion
			|| CookedPayload.TargetPlatform != static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile != static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
			return FailCooked("required TXPL descriptor is missing or incompatible.");

		const Asset::FAssetRuntimeConfiguration& LoadContext =
			Asset::GetAssetRuntimeConfiguration();
		if (!GetPackage())
			return FailCooked("package companion path could not be resolved.");
		Asset::FCookedPackagePayload LoadedPayload;
		if (!Asset::LoadCookedPackagePayload(
			LoadContext, GetPackage()->GetPackagePath(), CookedPayload,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, LoadedPayload, &OutError))
			return FailCooked(OutError);
		const std::span<const std::byte> Bytes = LoadedPayload.Payload;
		auto CandidatePlatformData = std::make_unique<FTextureCubePlatformData>();
		FCanonicalMemoryReader PayloadAr(Bytes, EArchivePurpose::CookedPayload);
		CandidatePlatformData->Serialize(PayloadAr, {
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (PayloadAr.HasError()) return FailCooked(PayloadAr.GetFailure()->Message);

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

	auto DTextureCube::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
		{
			OutError = std::format(
				"TextureCube '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		std::vector<std::byte> PayloadBytes;
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
		FCanonicalMemoryWriter CookAr(PayloadBytes, EArchivePurpose::CookedPayload);
		PlatformData->Serialize(CookAr, {
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (CookAr.HasError())
		{
			OutError = std::format("Failed to cook TextureCube '{}': {}",
				GetObjectPath(), CookAr.GetFailure()->Message);
			return false;
		}

		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = TextureCubePrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = TexturePayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = TexturePayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		const Asset::FAssetPackageSerializationOptions CookPackageOptions =
			Context.MakePackageSerializationOptions();
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(BulkPayload)},
			[this, CookPackageOptions](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<std::byte>& OutPackageBytes, std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != TextureCubePrimaryCookedPayloadId)
				{
					if (Error) *Error = "TextureCube cook did not produce its required descriptor.";
					return false;
				}
				FProperty* DescriptorProperty = GetClass()->FindPropertyByName("CookedPayload");
				if (!DescriptorProperty)
				{
					if (Error) *Error = "TextureCube CookedPayload reflection is unavailable.";
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
				const Asset::FAssetResult Result =
					Asset::SerializeAssetPackageBytes(GetPackage(), OutPackageBytes, Options);
				if (!Result)
				{
					if (Error) *Error = Result.Message;
					return false;
				}
				return true;
			}, &OutError);
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

	auto DTextureCube::ExchangeImportedState(DTextureCube& Other) noexcept -> void
	{
		if (&Other == this) return;
		std::swap(SourceLayout, Other.SourceLayout);
		std::swap(ImportedData, Other.ImportedData);
		std::swap(PanoramaFaceDimension, Other.PanoramaFaceDimension);
		std::swap(PanoramaExposureEV, Other.PanoramaExposureEV);
		std::swap(OriginalSourceWidth, Other.OriginalSourceWidth);
		std::swap(OriginalSourceHeight, Other.OriginalSourceHeight);
		std::swap(bSRGB, Other.bSRGB);
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

	auto DTextureCube::PublishBuildProduct(
		ETextureCubeSourceLayout InSourceLayout,
		uint32 InPanoramaFaceDimension,
		float InPanoramaExposureEV,
		uint32 InOriginalSourceWidth,
		uint32 InOriginalSourceHeight,
		bool bInSRGB,
		std::unique_ptr<FTextureCubeSourceData> InSourceData,
		std::unique_ptr<FTextureCubePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		FTextureDerivedDataDiagnostic InDiagnostic) -> void
	{
		check(InPlatformData && InPlatformData->IsValid());
		if (InSourceData)
		{
			FTextureCubeImportedData Candidate;
			check(Candidate.SetSourceData(*InSourceData));
			ImportedData = std::move(Candidate);
		}
		check(ImportedData.IsValid());
		SourceLayout = InSourceLayout;
		PanoramaFaceDimension = InPanoramaFaceDimension;
		PanoramaExposureEV = InPanoramaExposureEV;
		OriginalSourceWidth = InOriginalSourceWidth;
		OriginalSourceHeight = InOriginalSourceHeight;
		bSRGB = bInSRGB;
		SourceData = std::move(InSourceData);
		PlatformData = std::move(InPlatformData);
		DerivedDataKey = std::move(InDerivedDataKey);
		DerivedDataDiagnostic = std::move(InDiagnostic);
		BuildStatus = ETextureBuildStatus::Ready;
		LastBuildError.clear();
		bLoadedFromDerivedDataCache =
			DerivedDataDiagnostic.Status == ETextureDerivedDataStatus::Hit;
		QueueRenderResourceBuild();
		MarkPackageDirty();
	}

	auto DTextureCube::PublishDerivedDataLoad(
		std::unique_ptr<FTextureCubePlatformData> InPlatformData,
		std::string InDerivedDataKey,
		std::string& OutError) -> bool
	{
		if (!InPlatformData || !InPlatformData->IsValid() || InDerivedDataKey.empty())
		{
			OutError = "TextureCube DDC publication requires valid platform data and key.";
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
