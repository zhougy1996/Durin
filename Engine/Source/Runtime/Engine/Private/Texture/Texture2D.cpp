#include "Texture/Texture2D.h"
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
	namespace
	{
		constexpr uint32 TextureSourceChannelCount = 4;

		auto MakeTexture2DBuildInput(const FTextureSource& Source)
			-> FTexture2DImportedData
		{
			FTexture2DImportedData Result;
			if (!Source.IsValid() || Source.GetKind() != ETextureSourceKind::Texture2D)
				return Result;
			const FTextureSourceSnapshot Snapshot = Source.CreateSnapshotBlocking(0);
			if (!Snapshot.IsValid()) return Result;
			std::vector<FTextureSourceData> CapturedMips;
			for (uint32 MipIndex = 0; MipIndex < Snapshot.Layers[0].NumMips; ++MipIndex)
			{
				const Image::FImageView View = Snapshot.GetMipImage(0, 0, MipIndex);
				if (!View.IsValid()) return {};
				const auto& Info = View.GetInfo();
				FTextureSourceData Mip{.Pixels = FByteArray(
					View.GetPixels().begin(), View.GetPixels().end()),
					.Width = Info.Width, .Height = Info.Height,
					.SourceChannelCount = Source.GetSourceChannelCount(),
					.Format = ETextureSourceFormat::RGBA8,
					.bHasTransparency = Source.HasTransparency()};
				CapturedMips.push_back(std::move(Mip));
			}
			if (CapturedMips.empty()
				|| !Result.Pixels.UpdatePayload(CapturedMips[0].Pixels)) return {};
			if (CapturedMips.size() > 1) Result.SuppliedMips = std::move(CapturedMips);
			Result.Width = Source.GetWidth();
			Result.Height = Source.GetHeight();
			Result.SourceChannelCount = Source.GetSourceChannelCount();
			Result.Format = ETextureSourceFormat::RGBA8;
			Result.bHasTransparency = Source.HasTransparency();
			Result.CanonicalSourceIdentity = Source.GetIdentity();
			return Result;
		}
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

	auto FTexture2DImportedData::IsValid() const -> bool
	{
		const uint64 ExpectedByteCount = static_cast<uint64>(Width)
			* Height * ::Durin::TextureSourceChannelCount;
		if (!(SchemaVersion == Texture2DImportedDataSchemaVersion
			&& Format == ETextureSourceFormat::RGBA8
			&& Width > 0 && Height > 0 && Width <= 16384 && Height <= 16384
			&& SourceChannelCount > 0 && SourceChannelCount <= TextureSourceChannelCount
			&& ExpectedByteCount == Pixels.GetPayloadSize()
			&& ExpectedByteCount <= MaximumTexture2DImportedPixelBytes)) return false;
		for (size_t Index = 0; Index < SuppliedMips.size(); ++Index)
		{
			const FTextureSourceData& Mip = SuppliedMips[Index];
			if (!Mip.IsValid() || Mip.Width != std::max(1u, Width >> std::min<size_t>(Index, 31))
				|| Mip.Height != std::max(1u, Height >> std::min<size_t>(Index, 31))) return false;
		}
		return SuppliedMips.empty() || (SuppliedMips[0].Width == Width
			&& SuppliedMips[0].Height == Height && SuppliedMips[0].Pixels.size() == ExpectedByteCount);
	}

	FTexture2DImportedData::FTexture2DImportedData(
		const FTextureSourceData& Source)
	{
		SetSourceData(Source);
	}

	FTexture2DImportedData::FTexture2DImportedData(
		FTextureSourceData&& Source)
	{
		SetSourceData(Source);
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
		FTextureSource Canonical;
		const FTextureSourceBlock Block{.Width = Width, .Height = Height};
		const FTextureSourceLayer Layer{.Format = Format};
		if (!Canonical.InitLayered(ETextureSourceKind::Texture2D,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Unknown, Source.Pixels,
			SourceChannelCount, bHasTransparency ? 1 : 0,
			ETextureSourceCompression::Raw)) return false;
		CanonicalSourceIdentity = Canonical.GetIdentity();
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
		if (!CanonicalSourceIdentity.IsZero()) return CanonicalSourceIdentity;
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

	auto DTexture2D::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		TexturePrivate::SerializeCookedPlatformData(Ar, GetMutableCookedPlatformData(),
			PlatformData.get(), FName("Durin::DTexture2D"), "Texture2D");
	}

	auto DTexture2D::GetImportedDataIdentity() const -> FXxHash128
	{
		return MakeTexture2DBuildInput(GetSource()).GetIdentity();
	}

	auto DTexture2D::CreateBuildInput() const -> FTexture2DImportedData
	{
		return MakeTexture2DBuildInput(GetSource());
	}

	auto DTexture2D::SetPlatformData(
		std::unique_ptr<FTexturePlatformData> Data,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Data || !Data->IsValid())
		{
			OutError = "Texture2D platform data must be complete and valid.";
			return false;
		}
		PlatformData = std::move(Data);
		OutError.clear();
		return true;
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
		BindTextureSourceOwner();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked Texture2D '{}': required PlatformData field is missing.",
					GetObjectPath());
				return false;
			}
			PlatformData.reset();
			OutError.clear();
			return true;
		}
		if (GetSource().GetSchemaVersion() != TextureSourceSchemaVersion)
		{
			FTextureSource Migrated = GetSource();
			if (!Migrated.MigrateLegacy()
				|| !SetSource(std::move(Migrated), OutError)) return false;
		}
		if (!GetSource().IsValid())
		{
			OutError = "Texture2D source data is missing or invalid.";
		}
		else if (BuildTexture2DSynchronously(*this, {
			.ImportedData = MakeTexture2DBuildInput(GetSource()),
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
		return false;
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

	auto DTexture2D::SetSourceData(
		const FTexture2DImportedData& Value, std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Value.IsValid())
		{
			OutError = "Texture2D source data could not be captured.";
			return false;
		}
		const FPackageResourceReadResult Read = Value.Pixels.GetPayload().Wait();
		if (!Read)
		{
			OutError = "Texture2D source payload could not be read.";
			return false;
		}
		FTextureSource NewSource;
		FByteArray Decoded(Read.Buffer.GetBytes().begin(), Read.Buffer.GetBytes().end());
		if (!Value.SuppliedMips.empty())
		{
			Decoded.clear();
			for (const FTextureSourceData& Mip : Value.SuppliedMips)
				Decoded.insert(Decoded.end(), Mip.Pixels.begin(), Mip.Pixels.end());
		}
		const FTextureSourceBlock Block{.Width = Value.Width, .Height = Value.Height};
		const FTextureSourceLayer Layer{.Format = ETextureSourceFormat::RGBA8,
			.NumMips = static_cast<uint32>(std::max<size_t>(1, Value.SuppliedMips.size()))};
		if (!NewSource.InitLayeredImpl(ETextureSourceKind::Texture2D,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Unknown, Decoded,
			Value.SourceChannelCount, Value.bHasTransparency ? 1 : 0,
			ETextureSourceCompression::Raw))
		{
			OutError = "Texture2D source data could not be initialized.";
			return false;
		}
		return SetSource(std::move(NewSource), OutError);
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
		FByteArray Bytes;
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
			&& MakeTexture2DBuildInput(ProposedSource).IsValid()
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
		AdvanceAuthoredGeneration();
		OutError.clear();
		return true;
	}

}
