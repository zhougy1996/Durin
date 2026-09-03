#include "Texture/VolumeTexture.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "DObject/DurinPropertyTypes.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureBuildProvider.h"
#include "Texture/VolumeTextureRenderResource.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ToTextureSourceFormat(EVolumeTextureFormat Format)
			-> ETextureSourceFormat
		{
			switch (Format)
			{
			case EVolumeTextureFormat::R8_UNORM: return ETextureSourceFormat::R8_UNORM;
			case EVolumeTextureFormat::RG8_UNORM: return ETextureSourceFormat::RG8_UNORM;
			case EVolumeTextureFormat::RGBA8_UNORM: return ETextureSourceFormat::RGBA8;
			case EVolumeTextureFormat::R16_FLOAT: return ETextureSourceFormat::R16_FLOAT;
			case EVolumeTextureFormat::RGBA16_FLOAT: return ETextureSourceFormat::RGBA16_FLOAT;
			default: return ETextureSourceFormat::Invalid;
			}
		}

		auto ToVolumeTextureFormat(ETextureSourceFormat Format)
			-> std::optional<EVolumeTextureFormat>
		{
			switch (Format)
			{
			case ETextureSourceFormat::R8_UNORM: return EVolumeTextureFormat::R8_UNORM;
			case ETextureSourceFormat::RG8_UNORM: return EVolumeTextureFormat::RG8_UNORM;
			case ETextureSourceFormat::RGBA8: return EVolumeTextureFormat::RGBA8_UNORM;
			case ETextureSourceFormat::R16_FLOAT: return EVolumeTextureFormat::R16_FLOAT;
			case ETextureSourceFormat::RGBA16_FLOAT: return EVolumeTextureFormat::RGBA16_FLOAT;
			default: return std::nullopt;
			}
		}

		auto MakeVolumeTextureSource(const FVolumeTextureSourceData& Input)
			-> FTextureSource
		{
			if (!Input.IsValid()) return {};
			FTextureSource Result{
				.Payload = Input.Voxels,
				.Width = Input.Width,
				.Height = Input.Height,
				.Depth = Input.Depth,
				.NumSlices = 1,
				.SourceChannelCount = 0,
				.Format = ToTextureSourceFormat(Input.Format),
				.Kind = ETextureSourceKind::Volume};
			return Result.IsValid() ? std::move(Result) : FTextureSource{};
		}

		auto MakeVolumeTextureBuildInput(const FTextureSource& Source)
			-> FVolumeTextureSourceData
		{
			FVolumeTextureSourceData Result;
			const std::optional<EVolumeTextureFormat> Format =
				ToVolumeTextureFormat(Source.Format);
			if (!Source.IsValid() || Source.Kind != ETextureSourceKind::Volume
				|| !Format) return Result;
			Result.Width = Source.Width;
			Result.Height = Source.Height;
			Result.Depth = Source.Depth;
			Result.Format = *Format;
			Result.Voxels = Source.Payload;
			return Result.IsValid() ? std::move(Result) : FVolumeTextureSourceData{};
		}

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
			&& Voxels.GetPayloadSize() == Texels * Info.BytesPerBlock;
	}

	auto FVolumeTextureSourceData::SetVoxelBytes(std::span<const std::byte> Bytes) -> bool
	{
		return Voxels.UpdatePayload(Bytes);
	}

	auto FVolumeTextureSourceData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(PayloadSchemaVersion);
		Builder.UpdateValue(Width);
		Builder.UpdateValue(Height);
		Builder.UpdateValue(Depth);
		Builder.UpdateValue(Format);
		Builder.UpdateValue(Voxels.GetPayloadId());
		return Builder.Finalize();
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

	auto DVolumeTexture::CreateBuildInput() const -> FVolumeTextureSourceData
	{
		return MakeVolumeTextureBuildInput(GetSource());
	}

	auto DVolumeTexture::SetPlatformData(
		std::unique_ptr<FVolumeTexturePlatformData> Data,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Data || !Data->IsValid())
		{
			OutError = "VolumeTexture platform data must be complete and valid.";
			return false;
		}
		PlatformData = std::move(Data);
		OutError.clear();
		return true;
	}

	auto DVolumeTexture::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"VolumeTexture cooked platform data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* Value = &GetMutableCookedPlatformData();
		if (Ar.IsSaving())
		{
			if (!PlatformData || !PlatformData->IsValid())
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"VolumeTexture cooked platform data is unavailable.");
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
		auto Field = EnterArchiveField(Ar, {FName("Durin::DVolumeTexture"),
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		Value->Serialize(Ar, {.Alignment = TexturePayloadAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

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
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked volume texture '{}': required PlatformData field is missing.",
					GetObjectPath());
				return false;
			}
			PlatformData.reset();
			OutError.clear();
			return true;
		}
		FVolumeTextureSourceData BuildInput =
			MakeVolumeTextureBuildInput(GetSource());
		if (!BuildInput.IsValid())
		{
			OutError = "VolumeTexture source data is missing or invalid.";
			return false;
		}
		return BuildVolumeTextureSynchronously(*this, {
			.SourceData = BuildInput,
			.Settings = BuildSettings}, {
			.bMarkPackageDirty = false,
			.bSourceDecoderInvoked = false}, OutError);
	}

	auto DVolumeTexture::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			OutError = std::format("Cooked volume texture '{}': {}", GetObjectPath(), Message);
			return false;
		};
		FBulkData& CookedData = GetMutableCookedPlatformData();
		std::span<const std::byte> Bytes;
		if (!CookedData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		auto Candidate = std::make_unique<FVolumeTexturePlatformData>();
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload);
		Candidate->Serialize(Ar, {.TargetPlatform = ECookTargetPlatform::Win64,
			.TargetProfile = ECookTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
		{
			CookedData.UnlockReadOnly();
			return FailCooked(std::string(Ar.GetError()));
		}
		if (!CookedData.UnlockReadOnly(&OutError)) return FailCooked(OutError);
		if (!Candidate->IsValid())
			return FailCooked("platform data is invalid.");
		if (!SetPlatformData(std::move(Candidate), OutError))
			return FailCooked(OutError);
		UpdateResource();
		OutError.clear();
		return true;
	}

	auto DVolumeTexture::ContributeToCook(FCookContext& Context,
		std::string_view VirtualPackagePath, std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
		{
			OutError = "Volume textures support only the Win64 game cook target.";
			return false;
		}
		if (!PlatformData || !PlatformData->IsValid())
		{
			if (!PostLoad(OutError)) return false;
		}
		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DVolumeTexture::SetSourceData(
		const FVolumeTextureSourceData& Value, std::string& OutError) -> bool
	{
		CheckGameThread();
		FTextureSource Candidate = MakeVolumeTextureSource(Value);
		if (!Candidate.IsValid())
		{
			OutError = "VolumeTexture source data is invalid.";
			return false;
		}
		return SetSource(std::move(Candidate), OutError);
	}

	auto DVolumeTexture::SetBuildSettings(
		FVolumeTextureBuildSettings Value, std::string& OutError) -> bool
	{
		CheckGameThread();
		if (ToPixelFormat(Value.OutputFormat) == EPixelFormat::Unknown
			|| Value.MipFilter != EVolumeTextureMipFilter::Box)
		{
			OutError = "VolumeTexture build settings are invalid.";
			return false;
		}
		BuildSettings = Value;
		OutError.clear();
		return true;
	}

}
