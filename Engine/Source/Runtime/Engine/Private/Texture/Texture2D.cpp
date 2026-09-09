#include "Texture/Texture2D.h"
#include "Logging/LogMacros.h"
#include "Texture/TextureCookedData.h"

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

	auto DTexture2D::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		TexturePrivate::SerializeCookedPlatformData(Ar, GetMutableCookedPlatformData(),
			PlatformData.get(), FName("Durin::DTexture2D"), "Texture2D");
	}

	auto DTexture2D::CreateBuildRequest(const FTexture2DBuildSettings& Settings) const
		-> FTexture2DBuildRequest
	{
		return MakeTexture2DBuildRequest(GetSource(), Settings);
	}

	auto DTexture2D::SetPlatformData(
		std::unique_ptr<FTexturePlatformData> Data) -> void
	{
		CheckGameThread();
		PlatformData = std::move(Data);
	}

	auto DTexture2D::CreateRenderResourceCandidate(
		FTextureReference* TextureReference)
		-> std::unique_ptr<FTextureResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTexture2DResource>(
			TextureReference,
			std::make_shared<const FTexturePlatformData>(*PlatformData));
	}

	auto DTexture2D::PostLoad() -> void
	{
		std::string Error;
		BindTextureSourceOwner();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
			{
				Error = std::format(
					"Cooked Texture2D '{}': required PlatformData field is missing.",
					GetObjectPath());
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
			PlatformData.reset();
			return;
		}
		if (GetSource().GetSchemaVersion() != TextureSourceSchemaVersion)
		{
			FTextureSource Migrated = GetSource();
			if (!Migrated.MigrateLegacy())
			{
				Error = "Texture source migration failed.";
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
			if (!SetSource(std::move(Migrated), Error))
			{
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
		}
		if (!GetSource().IsValid())
		{
			Error = "Texture2D source data is missing or invalid.";
		}
		else if (BuildTexture2DSynchronously(*this, CreateBuildRequest({
				.Usage = Usage,
				.CompressionQuality = CompressionQuality,
				.AlphaMipMode = AlphaMipMode,
				.AlphaCoverageThreshold = AlphaCoverageThreshold,
				.MaxResolution = MaxResolution,
				.bSRGB = bSRGB}), {
			.bMarkPackageDirty = false,
			.bReportLoadMutation = false,
			.bSourceDecoderInvoked = false}, Error)) return;
		DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
	}

	auto DTexture2D::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		return TexturePrivate::LoadCookedPlatformData<FTexturePlatformData>(
			*this, GetMutableCookedPlatformData(), "Texture2D", OutError);
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

		if (!HasPlatformData()) PostLoad();
		if (!HasPlatformData())
		{
			OutError = std::format("Failed to cook Texture2D '{}': platform data is unavailable.",
				GetObjectPath());
			return false;
		}

		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DTexture2D::SetSourceMipChain(std::span<const Image::FImageView> Mips,
		uint8 SourceChannelCount, uint8 TransparencyMask,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (Mips.empty())
		{
			OutError = "Texture2D source mip chain is empty.";
			return false;
		}
		const Image::FImageInfo Base = Mips[0].GetInfo();
		FByteBuffer Bytes;
		for (size_t Index = 0; Index < Mips.size(); ++Index)
		{
			const auto& Info = Mips[Index].GetInfo();
			if (!Mips[Index].IsValid() || Info.Format != Base.Format
				|| Info.GammaSpace != Base.GammaSpace || Info.Depth != 1
				|| Info.SliceCount != 1
				|| Info.Width != std::max(1u, Base.Width >> std::min<size_t>(Index, 31))
				|| Info.Height != std::max(1u, Base.Height >> std::min<size_t>(Index, 31)))
			{
				OutError = "Texture2D supplied mip chain is non-canonical.";
				return false;
			}
			Bytes.insert(Bytes.end(), Mips[Index].GetPixels().begin(),
				Mips[Index].GetPixels().end());
		}
		FTextureSource NewSource;
		const FTextureSourceBlock Block{.Width = Base.Width, .Height = Base.Height};
		const FTextureSourceLayer Layer{.Format = ETextureSourceFormat::RGBA8,
			.NumMips = static_cast<uint32>(Mips.size())};
		if (Base.Format != Image::ERawImageFormat::RGBA8
			|| !NewSource.InitLayeredImpl(ETextureSourceKind::Texture2D,
				std::span(&Block, 1), std::span(&Layer, 1),
				Base.GammaSpace == Image::EImageGammaSpace::SRGB
					? ETextureSourceGammaSpace::SRGB : ETextureSourceGammaSpace::Linear,
				Bytes, SourceChannelCount, TransparencyMask,
				ETextureSourceCompression::Raw))
		{
			OutError = "Texture2D supplied mip chain could not be initialized.";
			return false;
		}
		return SetSource(std::move(NewSource), OutError);
	}

	auto DTexture2D::ValidateSettingsAfterImportOrEdit(
		const FTextureSource& ProposedSource) const -> bool
	{
		const auto Blocks = ProposedSource.GetBlocks();
		const auto Layers = ProposedSource.GetLayers();
		return ProposedSource.GetKind() == ETextureSourceKind::Texture2D
			&& Blocks.size() == 1 && Layers.size() == 1
			&& Blocks[0].Depth == 1 && Blocks[0].NumSlices == 1
			&& Layers[0].Format == ETextureSourceFormat::RGBA8
			&& Layers[0].NumMips >= 1
			&& !MakeTexture2DBuildRequest(ProposedSource).SourceMips.empty()
			&& IsValidTextureUsage(Usage)
			&& IsValidTextureCompressionQuality(CompressionQuality)
			&& IsValidTextureAlphaMipMode(AlphaMipMode)
			&& IsValidTextureAlphaCoverageThreshold(AlphaCoverageThreshold);
	}

	auto DTexture2D::SetBuildSettings(ETextureUsage InUsage, bool bInSRGB,
		uint32 InMaxResolution, ETextureCompressionQuality InCompressionQuality,
		ETextureAlphaMipMode InAlphaMipMode, float InAlphaCoverageThreshold,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!IsValidTextureUsage(InUsage)
			|| !IsValidTextureCompressionQuality(InCompressionQuality)
			|| !IsValidTextureAlphaMipMode(InAlphaMipMode)
			|| !IsValidTextureAlphaCoverageThreshold(InAlphaCoverageThreshold))
		{
			OutError = "Texture2D build settings are invalid.";
			return false;
		}
		Usage = InUsage;
		bSRGB = bInSRGB;
		MaxResolution = InMaxResolution;
		CompressionQuality = InCompressionQuality;
		AlphaMipMode = InAlphaMipMode;
		AlphaCoverageThreshold = InAlphaCoverageThreshold;
		InvalidateAuthoredBuild();
		OutError.clear();
		return true;
	}

}
