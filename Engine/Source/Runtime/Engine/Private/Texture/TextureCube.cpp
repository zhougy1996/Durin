#include "Texture/TextureCube.h"
#include "Logging/LogMacros.h"
#include "Texture/TextureCookedData.h"

#include "DObject/Package.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DynamicRHI.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Texture/TextureCubeBuildProvider.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureDerivedData.h"
#include "Threading/RunnableThread.h"
#include "Texture/TexturePlatformCache.h"
#include "Texture/TextureDerivedDataCache.h"
#include "Texture/TextureDerivedDataKey.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

		struct FCubeCacheResult final : FTexturePlatformCacheResult
		{
			std::unique_ptr<FTextureCubePlatformData> Data;
			auto Apply(DTexture& Texture) -> void override
			{
				Cast<DTextureCube>(&Texture)->SetPlatformData(std::move(Data));
				Texture.UpdateResource();
			}
		};
		struct FCubeCacheInput final : FTexturePlatformCacheInput
		{
			explicit FCubeCacheInput(const DTextureCube& Texture)
				: Width(Texture.GetOriginalSourceWidth()), Height(Texture.GetOriginalSourceHeight()),
				FaceDimension(Texture.GetPanoramaFaceDimension()), Exposure(Texture.GetPanoramaExposureEV()),
				bSRGB(Texture.IsSRGB()), Output(Texture.GetOutput())
			{
				Source = Texture.GetSource().CopyTornOff();
				EstimatedBytes = Source.GetDecodedPayloadSize() * 4
					+ static_cast<uint64>(FaceDimension) * FaceDimension * 6 * 32;
			}
			uint32 Width, Height, FaceDimension;
			float Exposure;
			bool bSRGB;
			ETextureCubeOutput Output;
			auto Build() const -> std::unique_ptr<FTexturePlatformCacheResult> override;
		};

		auto MakeTextureCubeBuildRequest(const FCubeCacheInput& Input,
			FTextureCubeBuildRequest& OutRequest, std::string& OutError) -> bool
		{
			const FTextureSource& Source = Input.Source;
			if (Source.GetKind() == ETextureSourceKind::TextureCube)
			{
				FTextureCubeDecodedFaces Faces = ReadTextureCubeFaces(Source);
				if (!Faces.IsValid()) return false;
				OutRequest.Input = FTextureCubeFacesBuildInput{
					.DecodedFaces = std::move(Faces),
					.SourceIdentity = Source.GetIdentity(),
					.SourceLayout = ETextureCubeSourceLayout::SixFaces,
					.OriginalSourceWidth = Input.Width,
					.OriginalSourceHeight = Input.Height,
					.Settings = {.bSRGB = Input.bSRGB}};
				return true;
			}
			if (Source.GetKind() != ETextureSourceKind::LongLatCube) return false;
			const FTextureSource::FMipData Mips = Source.GetMipData();
			const Image::FImageView View = Mips.GetMipImage(0, 0, 0);
			if (!View.IsValid())
			{
				OutError = "TextureCube panorama source payload could not be loaded.";
				return false;
			}
			const auto& Info = View.GetInfo();
			FTextureCubePanoramaBuildInput Panorama;
			Panorama.Settings = {.FaceDimension = Input.FaceDimension,
				.ExposureEV = Input.Exposure, .Output = Input.Output};
			if (Info.Format == Image::ERawImageFormat::RGBA32F)
			{
				FTextureCubePanoramaFloatImage Image;
				Image.Width = Info.Width;
				Image.Height = Info.Height;
				const size_t PixelCount = static_cast<size_t>(Info.Width) * Info.Height;
				Image.Pixels.resize(PixelCount * 3);
				for (size_t Index = 0; Index < PixelCount; ++Index)
					std::memcpy(Image.Pixels.data() + Index * 3,
						View.GetPixels().data() + Index * 4 * sizeof(float),
						3 * sizeof(float));
				Panorama.Image = std::move(Image);
			}
			else if (Info.Format == Image::ERawImageFormat::RGBA8)
			{
				FTextureCubePanoramaImage Image;
				Image.Width = Info.Width;
				Image.Height = Info.Height;
				Image.SourceChannelCount = Source.GetSourceChannelCount();
				Image.bHasTransparency = Source.HasTransparency();
				Image.Pixels.assign(View.GetPixels().begin(), View.GetPixels().end());
				Panorama.Image = std::move(Image);
			}
			else
			{
				OutError = "TextureCube panorama source format is unsupported by the projection recipe.";
				return false;
			}
			OutRequest.Input = std::move(Panorama);
			return true;
		}

		auto FCubeCacheInput::Build() const -> std::unique_ptr<FTexturePlatformCacheResult>
		{
			auto Result = std::make_unique<FCubeCacheResult>();
#if DURIN_WITH_EDITOR
			// Existing canonical six-face and HDR panorama identities can query DDC
			// before reading pixels. Noncanonical panorama input still normalizes on the worker.
			const bool bHDR = Source.GetKind() == ETextureSourceKind::LongLatCube
				&& Output == ETextureCubeOutput::HDR && Source.GetSourceChannelCount() == 4
				&& !Source.HasTransparency() && Source.GetFormat() == ETextureSourceFormat::RGBA32_FLOAT;
			if (Source.GetKind() == ETextureSourceKind::TextureCube || bHDR)
			{
				FModularFeatureRegistry::Get().InvokeSingle<ITextureCubeBuildProvider>([&](ITextureCubeBuildProvider& Provider) {
					const auto Descriptor = Provider.GetDescriptor();
					if (!Descriptor.IsValid()) return false;
					const auto Hash = Source.GetIdentity();
					std::string Error;
					const auto Key = BuildTextureCubeDerivedDataKey({
						.SourceLayout = bHDR ? ETextureCubeBuildSourceLayout::EquirectangularPanorama : ETextureCubeBuildSourceLayout::SixFaces,
						.FaceContentHashes = {Hash, Hash, Hash, Hash, Hash, Hash},
						.PanoramaContentHash = bHDR ? Hash : FXxHash128{},
						.FaceDimension = bHDR ? FaceDimension : 0,
						.ExposureEV = bHDR && Exposure != 0.0f ? Exposure : 0.0f,
						.bSRGB = bSRGB, .BuilderVersion = Descriptor.BuilderVersion,
						.ProjectionVersion = Descriptor.ProjectionVersion,
						.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game}, Error);
					if (!Key.IsValid()) return false;
					auto Data = std::make_unique<FTextureCubePlatformData>();
					TextureDerivedDataCache::FOperationDiagnostic Diagnostic;
					if (TextureDerivedDataCache::Load(Key, ECookTargetPlatform::Win64, ECookTargetProfile::Game,
						*Data, Diagnostic) != TextureDerivedDataCache::ELoadResult::Hit) return false;
					Result->Data = std::move(Data);
					return true;
				});
				if (Result->Data) return Result;
			}
#endif
			FTextureCubeBuildRequest Request;
			if (!MakeTextureCubeBuildRequest(*this, Request, Result->Error))
			{
				if (Result->Error.empty()) Result->Error = "Invalid cube source payload.";
				return Result;
			}
			auto Built = InvokeTextureCubeBuildProvider(Request);
			if (Built) Result->Data = std::move(Built.Value->Product.PlatformData);
			else Result->Error = Built.Outcome.Diagnostic.empty() ? "Cube platform build failed." : Built.Outcome.Diagnostic;
			return Result;
		}

		auto ValidateCubeSourceData(const FTextureCubeDecodedFaces& SourceData, std::string& OutError) -> bool
		{
			if ((SourceData.TransparencyMask & ~0x3fu) != 0
				|| SourceData.SourceChannelCounts[0] == 0 || SourceData.SourceChannelCounts[0] > 4)
			{
				OutError = "Cube face import metadata is invalid.";
				return false;
			}
			const Image::FImage& Reference = SourceData.Faces[0];
		if (Reference.GetPixels().size() > (512ull * 1024ull * 1024ull) / TextureCubeFaceCount)
		{
			OutError = "Cube face pixels exceed the build input limit.";
			return false;
		}
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				const Image::FImage& Face = SourceData.Faces[FaceIndex];
				if (!Face.IsValid() || Face.GetInfo().Format != Image::ERawImageFormat::RGBA8
					|| Face.GetInfo().Depth != 1 || Face.GetInfo().SliceCount != 1
					|| Face.GetInfo().Width > 16384 || Face.GetInfo().Height > 16384
					|| Face.GetInfo().GammaSpace != Image::EImageGammaSpace::Unknown)
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[FaceIndex]);
					return false;
				}
				if (Face.GetInfo().Width != Face.GetInfo().Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[FaceIndex], Face.GetInfo().Width, Face.GetInfo().Height);
					return false;
				}
				if (Face.GetInfo().Width != Reference.GetInfo().Width || Face.GetInfo().Height != Reference.GetInfo().Height)
				{
					OutError = std::format("{} face dimensions {}x{} do not match PositiveX {}x{}; all faces must be identical.",
						FaceNames[FaceIndex], Face.GetInfo().Width, Face.GetInfo().Height, Reference.GetInfo().Width, Reference.GetInfo().Height);
					return false;
				}
				if (SourceData.SourceChannelCounts[FaceIndex] != SourceData.SourceChannelCounts[0])
				{
					OutError = std::format("{} face source channel count {} does not match PositiveX {}; all faces must use an identical source format.",
						FaceNames[FaceIndex], SourceData.SourceChannelCounts[FaceIndex], SourceData.SourceChannelCounts[0]);
					return false;
				}
			}
			return true;
		}

	}

	auto FTextureCubeDecodedFaces::IsValid() const -> bool
	{
		std::string Error;
		return ValidateCubeSourceData(*this, Error);
	}

	auto ReadTextureCubeFaces(const FTextureSource& Source) -> FTextureCubeDecodedFaces
	{
		FTextureCubeDecodedFaces Result;
		if (!Source.IsValid() || Source.GetKind() != ETextureSourceKind::TextureCube)
			return Result;
		const FTextureSource::FMipData Mips = Source.GetMipData();
		const Image::FImageView View = Mips.GetMipImage(0, 0, 0);
		if (!View.IsValid() || View.GetInfo().Format != Image::ERawImageFormat::RGBA8
			|| View.GetInfo().SliceCount != TextureCubeFaceCount) return {};
		const uint64 FaceBytes = static_cast<uint64>(Source.GetWidth()) * Source.GetHeight() * 4;
		if (Mips.GetData().GetSize() != FaceBytes * TextureCubeFaceCount) return {};
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			auto ImageResult1 = Image::FImage::TryCreate({.Width = Source.GetWidth(), .Height = Source.GetHeight(),
				.Format = Image::ERawImageFormat::RGBA8}, View.GetBuffer().MakeView(Index * FaceBytes, FaceBytes));
			if (!ImageResult1) return {};
			Result.Faces[Index] = std::move(*ImageResult1);
		}
		Result.SourceChannelCounts.fill(Source.GetSourceChannelCount());
		Result.TransparencyMask = Source.GetTransparencyMask();
		return Result.IsValid() ? std::move(Result) : FTextureCubeDecodedFaces{};
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
		TexturePrivate::SerializeCookedPlatformData(Ar, GetMutableCookedPlatformData(),
			PlatformData.get(), FName("Durin::DTextureCube"), "TextureCube");
	}

	auto DTextureCube::GetBuiltFaceDimension() const -> uint32
	{
		if (PlatformData && !PlatformData->Faces[0].Mips.empty())
			return PlatformData->Faces[0].Mips[0].Width;
		return 0;
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

	auto DTextureCube::SetPlatformData(
		std::unique_ptr<FTextureCubePlatformData> Data) -> void
	{
		CheckGameThread();
		InvalidateRenderResource();
		PlatformData = std::move(Data);
	}

	auto DTextureCube::CreateRenderResourceCandidate(
		FTextureReference* TextureReference)
		-> std::unique_ptr<FTextureResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTextureCubeResource>(
			TextureReference,
			PlatformData);
	}

	auto DTextureCube::RebuildPlatformData() -> bool
	{
		std::string Error;
		FTextureCubeBuildRequest Request;
		if (!MakeTextureCubeBuildRequest(FCubeCacheInput(*this), Request, Error))
		{
			DURIN_ERROR("RebuildPlatformData '{}': {}", GetObjectPath(), Error);
			return false;
		}
		const auto Result = BuildTextureCubeSynchronously(*this, Request,
			{.bSourceDecoderInvoked = false, .bPreserveSource = true});
		if (!Result) DURIN_ERROR("RebuildPlatformData '{}': {}", GetObjectPath(), Result.Diagnostic);
		return static_cast<bool>(Result);
	}

	auto DTextureCube::BuildPlatformDataForLoad() -> void
	{
		if (!SubmitTexturePlatformCache(*this, std::make_shared<FCubeCacheInput>(*this)))
			DURIN_ERROR("PostLoad '{}': TextureCube cache admission failed.", GetObjectPath());
	}

	auto DTextureCube::LoadCookedPlatformData() -> bool
	{
		return TexturePrivate::LoadCookedPlatformData<FTextureCubePlatformData>(
			*this, GetMutableCookedPlatformData(), "TextureCube");
	}

	auto PrepareTextureCubeSource(
		const FTextureCubeDecodedFaces& Value) -> std::optional<FTextureSource>
	{
		if (!Value.IsValid())
		{
			DURIN_WARN("TextureCube source data is invalid.");
			return std::nullopt;
		}
		FByteBuffer Bytes;
		Bytes.reserve(Value.Faces[0].GetPixels().size() * TextureCubeFaceCount);
		for (const auto& Face : Value.Faces)
			Bytes.insert(Bytes.end(), Face.GetPixels().begin(), Face.GetPixels().end());
		FTextureSource NewSource;
		const FTextureSourceBlock Block{.Width = Value.Faces[0].GetInfo().Width,
			.Height = Value.Faces[0].GetInfo().Width, .NumSlices = TextureCubeFaceCount};
		const FTextureSourceLayer Layer{.Format = ETextureSourceFormat::RGBA8};
		if (!NewSource.InitLayered(ETextureSourceKind::TextureCube,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Unknown, Bytes,
			Value.SourceChannelCounts[0], Value.TransparencyMask,
			ETextureSourceCompression::Zstd))
		{
			DURIN_WARN("TextureCube source data could not be initialized.");
			return std::nullopt;
		}
		return NewSource;
	}

	auto PrepareTextureCubePanoramaSource(Image::FImageView Value,
		uint8 SourceChannelCount, uint8 TransparencyMask) -> std::optional<FTextureSource>
	{
		FTextureSource NewSource;
		if (!NewSource.InitLongLatCube(Value, SourceChannelCount,
			TransparencyMask, ETextureSourceCompression::Zstd))
		{
			DURIN_WARN("TextureCube panorama source data could not be initialized.");
			return std::nullopt;
		}
		return NewSource;
	}

	auto DTextureCube::SetBuildSettings(
		ETextureCubeSourceLayout InSourceLayout,
		uint32 InPanoramaFaceDimension,
		float InPanoramaExposureEV,
		uint32 InOriginalSourceWidth,
		uint32 InOriginalSourceHeight,
		bool bInSRGB, ETextureCubeOutput InOutput) -> void
	{
		CheckGameThread();
		SourceLayout = InSourceLayout;
		PanoramaFaceDimension = InPanoramaFaceDimension;
		PanoramaExposureEV = InPanoramaExposureEV;
		OriginalSourceWidth = InOriginalSourceWidth;
		OriginalSourceHeight = InOriginalSourceHeight;
		bSRGB = bInSRGB;
		Output = InOutput;
		InvalidateAuthoredBuild();
	}
}
