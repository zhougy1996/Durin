#include "Texture/TextureCube.h"

#include "Asset/MountedSource.h"
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
		constexpr std::string_view TextureDecoderId = "DurinImage";
		constexpr uint32 TextureDecoderVersion = 1;

		auto FaceToIndex(ETextureCubeFace Face) -> size_t
		{
			const size_t Index = static_cast<size_t>(Face);
			check(Index < TextureCubeFaceCount);
			return Index;
		}

		auto ResolveCubeSource(const DTextureCube& Texture, ETextureCubeFace Face) -> std::filesystem::path
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces
				&& Provenance.GetFace(Face).HasSource())
			{
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						Provenance.GetFace(Face).SourcePath.Path,
						PathUtilities::EPathExistence::RequireFile);
				if (Resolved) return Resolved.PhysicalPath;
			}
			return {};
		}

		auto ResolvePanoramaSource(const DTextureCube& Texture) -> std::filesystem::path
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama
				&& Provenance.Panorama.HasSource())
			{
				const PathUtilities::FSourcePathResult Resolved =
					PathUtilities::ResolveSourcePath(
						Provenance.Panorama.SourcePath.Path,
						PathUtilities::EPathExistence::RequireFile);
				if (Resolved) return Resolved.PhysicalPath;
			}
			return {};
		}

		auto ValidateCubeProvenance(const DTextureCube& Texture, std::string& OutError) -> bool
		{
			const FTextureCubeSourceImportData& Provenance = Texture.GetSourceImportData();
			if (!Provenance.HasSource()) return true;
			if (!Texture.GetPackage())
			{
				OutError = "TextureCube source cannot be validated without an owning package.";
				return false;
			}
			auto ValidateSourcePath = [&](std::string_view SourcePath) -> bool {
				Asset::FMountedSourceResolution Resolution;
				return Asset::ResolveMountedSourceReference(
					Texture.GetPackage()->GetPackagePath(), SourcePath,
					Asset::EMountedSourceExistencePolicy::AllowMissing,
					Resolution, OutError);
			};
			if (Provenance.DecoderId != TextureDecoderId
				|| Provenance.DecoderVersion != TextureDecoderVersion
				|| Provenance.ProjectionVersion != TextureCubeProjectionVersion)
			{
				OutError = "TextureCube source decoder or projection version is unsupported.";
				return false;
			}
			if (Provenance.SourceLayout != Texture.GetSourceLayout())
			{
				OutError = "TextureCube source provenance layout does not match its authored layout.";
				return false;
			}
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				if (Provenance.Panorama.HasSource())
				{
					OutError = "Six-face TextureCube provenance contains an inactive panorama source.";
					return false;
				}
				for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
				{
					const FTextureSourceFile& Source =
						Provenance.GetFace(static_cast<ETextureCubeFace>(FaceIndex));
					if (!Source.HasSource() || !Source.HasContentHash())
					{
						OutError = "TextureCube face source provenance is incomplete.";
						return false;
					}
					if (!ValidateSourcePath(Source.SourcePath.Path)) return false;
				}
				return true;
			}
			if (Provenance.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			{
				if (!Provenance.Panorama.HasSource() || !Provenance.Panorama.HasContentHash())
				{
					OutError = "TextureCube panorama source provenance is incomplete.";
					return false;
				}
				if (!ValidateSourcePath(Provenance.Panorama.SourcePath.Path)) return false;
				for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
					if (Provenance.GetFace(static_cast<ETextureCubeFace>(FaceIndex)).HasSource())
					{
						OutError = "Panorama TextureCube provenance contains inactive face sources.";
						return false;
					}
				return true;
			}
			OutError = "TextureCube source provenance layout is invalid.";
			return false;
		}


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

	auto FTextureCubeSourceImportData::GetFace(ETextureCubeFace Face) const -> const FTextureSourceFile&
	{
		switch (Face)
		{
		case ETextureCubeFace::PositiveX: return PositiveX;
		case ETextureCubeFace::NegativeX: return NegativeX;
		case ETextureCubeFace::PositiveY: return PositiveY;
		case ETextureCubeFace::NegativeY: return NegativeY;
		case ETextureCubeFace::PositiveZ: return PositiveZ;
		case ETextureCubeFace::NegativeZ: return NegativeZ;
		}
		checkf(false, "Invalid cube face");
		return PositiveX;
	}

	auto FTextureCubeSourceImportData::GetMutableFace(ETextureCubeFace Face) -> FTextureSourceFile&
	{
		return const_cast<FTextureSourceFile&>(std::as_const(*this).GetFace(Face));
	}

	auto FTextureCubeSourceImportData::HasSource() const -> bool
	{
		if (SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			return Panorama.HasSource();
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			if (GetFace(static_cast<ETextureCubeFace>(FaceIndex)).HasSource()) return true;
		return false;
	}

	auto FTextureCubeSourceData::IsValid() const -> bool
	{
		std::string Error;
		return ValidateCubeSourceData(*this, Error);
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

	auto DTextureCube::GetSourceFile(ETextureCubeFace Face) const -> const std::string&
	{
		if (SourceImportData.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			const FTextureSourceFile& Source = SourceImportData.GetFace(Face);
			if (Source.HasSource()) return Source.SourcePath.Path;
		}
		static const std::string EmptySource;
		return EmptySource;
	}

	auto DTextureCube::ResolvePanoramaSource() const -> std::filesystem::path
	{
		return Durin::ResolvePanoramaSource(*this);
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
		std::swap(SourceImportData, Other.SourceImportData);
		std::swap(ImportProvenance, Other.ImportProvenance);
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

	auto DTextureCube::PublishImportProvenance(
		std::vector<std::byte> Provenance) -> void
	{
		static constexpr char Hex[] = "0123456789abcdef";
		ImportProvenance.resize(Provenance.size() * 2);
		for (size_t Index = 0; Index < Provenance.size(); ++Index)
		{
			const uint8 Value = std::to_integer<uint8>(Provenance[Index]);
			ImportProvenance[Index * 2] = Hex[Value >> 4];
			ImportProvenance[Index * 2 + 1] = Hex[Value & 0x0f];
		}
	}

	auto DTextureCube::PublishAuthoringCandidate(
		ETextureCubeSourceLayout InSourceLayout,
		FTextureCubeSourceImportData InSourceImportData,
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
		SourceLayout = InSourceLayout;
		SourceImportData = std::move(InSourceImportData);
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
