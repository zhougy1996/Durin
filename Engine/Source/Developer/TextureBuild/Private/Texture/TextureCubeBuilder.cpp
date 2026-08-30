#include "Texture/TextureCubeBuilder.h"

#include "Math/Color.h"
#include "RHIResources.h"

namespace Durin::Asset::TextureCubeBuilder
{
	namespace
	{
		constexpr uint32 LDRChannelCount = 4;
		constexpr uint32 HDRChannelCount = 3;
		constexpr double Pi = 3.1415926535897932384626433832795;

		struct FBilinearSample
		{
			std::array<size_t, 4> PixelIndices;
			std::array<double, 4> Weights;
		};

		auto WrapCoordinate(int64 Coordinate, uint32 Extent) -> uint32
		{
			const int64 SignedExtent = static_cast<int64>(Extent);
			const int64 Result = Coordinate % SignedExtent;
			return static_cast<uint32>(Result < 0 ? Result + SignedExtent : Result);
		}

		auto MakeBilinearSample(const FVector3& Direction, uint32 Width, uint32 Height) -> FBilinearSample
		{
			double U = 0.5 + std::atan2(Direction.y, Direction.x) / (2.0 * Pi);
			U -= std::floor(U);
			const double V = std::clamp(0.5 - std::asin(std::clamp(Direction.z, -1.0, 1.0)) / Pi, 0.0, 1.0);
			const double SourceX = U * Width - 0.5;
			const double SourceY = V * Height - 0.5;
			const int64 X0 = static_cast<int64>(std::floor(SourceX));
			const int64 Y0 = static_cast<int64>(std::floor(SourceY));
			const double FractionX = SourceX - std::floor(SourceX);
			const double FractionY = SourceY - std::floor(SourceY);
			const uint32 WrappedX0 = WrapCoordinate(X0, Width);
			const uint32 WrappedX1 = WrapCoordinate(X0 + 1, Width);
			const uint32 ClampedY0 = static_cast<uint32>(std::clamp<int64>(Y0, 0, Height - 1));
			const uint32 ClampedY1 = static_cast<uint32>(std::clamp<int64>(Y0 + 1, 0, Height - 1));
			return {
				.PixelIndices = {
					static_cast<size_t>(ClampedY0) * Width + WrappedX0,
					static_cast<size_t>(ClampedY0) * Width + WrappedX1,
					static_cast<size_t>(ClampedY1) * Width + WrappedX0,
					static_cast<size_t>(ClampedY1) * Width + WrappedX1,
				},
				.Weights = {
					(1.0 - FractionX) * (1.0 - FractionY),
					FractionX * (1.0 - FractionY),
					(1.0 - FractionX) * FractionY,
					FractionX * FractionY,
				},
			};
		}

		auto FilmicToneMap(double Value) -> double
		{
			const double Numerator = Value * (2.51 * Value + 0.03);
			const double Denominator = Value * (2.43 * Value + 0.59) + 0.14;
			return std::clamp(Numerator / Denominator, 0.0, 1.0);
		}

		auto InitializeFace(FTextureSourceData& Face, uint32 FaceDimension) -> void
		{
			Face.Width = FaceDimension;
			Face.Height = FaceDimension;
			Face.SourceChannelCount = LDRChannelCount;
			Face.Format = ETextureSourceFormat::RGBA8;
			Face.Pixels.resize(static_cast<size_t>(FaceDimension) * FaceDimension * LDRChannelCount);
		}

		auto ValidateLDRPanorama(const FTexturePanoramaImage& Panorama, std::string& OutError) -> bool
		{
			const uint64 PixelCount = static_cast<uint64>(Panorama.Width) * Panorama.Height;
			if (PixelCount > std::numeric_limits<size_t>::max() / LDRChannelCount
				|| Panorama.Pixels.size() != static_cast<size_t>(PixelCount) * LDRChannelCount)
			{
				OutError = "LDR panorama pixel storage does not match its dimensions.";
				return false;
			}
			return true;
		}

		auto ValidateHDRPanorama(const FTexturePanoramaFloatImage& Panorama, std::string& OutError) -> bool
		{
			const uint64 PixelCount = static_cast<uint64>(Panorama.Width) * Panorama.Height;
			if (PixelCount > std::numeric_limits<size_t>::max() / HDRChannelCount
				|| Panorama.Pixels.size() != static_cast<size_t>(PixelCount) * HDRChannelCount)
			{
				OutError = "HDR panorama pixel storage does not match its dimensions.";
				return false;
			}
			if (std::ranges::any_of(Panorama.Pixels, [](float Value) { return !std::isfinite(Value) || Value < 0.0f; }))
			{
				OutError = "HDR panorama contains a negative or nonfinite channel.";
				return false;
			}
			return true;
		}
	} // namespace

	auto ValidateEquirectangularTextureCubeProjection(uint32 Width, uint32 Height,
		const FEquirectangularTextureCubeProjectionSettings& Settings, bool bHDR,
		uint32& OutFaceDimension, std::string& OutError) -> bool
	{
		OutFaceDimension = 0;
		OutError.clear();
		if (Width == 0 || Height == 0)
		{
			OutError = "Equirectangular panorama dimensions must be nonzero.";
			return false;
		}
		if (static_cast<uint64>(Height) * 2 != Width)
		{
			OutError = std::format("Equirectangular panorama must have an exact 2:1 aspect ratio, but is {}x{}.", Width, Height);
			return false;
		}
		if (Width > MaximumPanoramaDimension || Height > MaximumPanoramaDimension)
		{
			OutError = std::format("Equirectangular panorama dimensions exceed the {} pixel limit.", MaximumPanoramaDimension);
			return false;
		}
		const uint64 PixelCount = static_cast<uint64>(Width) * Height;
		if (PixelCount > MaximumPanoramaPixels)
		{
			OutError = "Equirectangular panorama decoded pixels exceed the 33554432 pixel limit.";
			return false;
		}
		if (!std::isfinite(Settings.ExposureEV)
			|| Settings.ExposureEV < MinimumPanoramaExposureEV
			|| Settings.ExposureEV > MaximumPanoramaExposureEV)
		{
			OutError = "Panorama exposure must be finite and between -16 and 16 EV.";
			return false;
		}
		if (!bHDR && Settings.ExposureEV != 0.0f)
		{
			OutError = "Exposure is available only for Radiance HDR panoramas.";
			return false;
		}

		const uint32 FaceDimension = Settings.FaceDimension == 0
			? std::max(Width / 4, 1u)
			: Settings.FaceDimension;
		if (FaceDimension > MaximumProjectedCubeFaceDimension)
		{
			OutError = std::format("Projected cube face dimension must be between 1 and {}.",
				MaximumProjectedCubeFaceDimension);
			return false;
		}
		const uint64 ProjectedBytes = static_cast<uint64>(TextureCubeFaceCount) * FaceDimension * FaceDimension * LDRChannelCount;
		if (ProjectedBytes > std::numeric_limits<size_t>::max())
		{
			OutError = "Projected cube byte count exceeds the addressable allocation limit.";
			return false;
		}
		OutFaceDimension = FaceDimension;
		return true;
	}

	auto ProjectEquirectangularTextureCube(const FTexturePanoramaImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool
	{
		OutSourceData = {};
		uint32 FaceDimension = 0;
		if (!ValidateEquirectangularTextureCubeProjection(
			Panorama.Width, Panorama.Height, Settings, false, FaceDimension, OutError)
			|| !ValidateLDRPanorama(Panorama, OutError))
		{
			return false;
		}

		FTextureCubeSourceData Projected;
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			FTextureSourceData& Face = Projected.Faces[FaceIndex];
			InitializeFace(Face, FaceDimension);
			for (uint32 Y = 0; Y < FaceDimension; ++Y)
			{
				for (uint32 X = 0; X < FaceDimension; ++X)
				{
					FVector3 Direction{};
					if (!ResolveTextureCubeFacePixelDirection(
						static_cast<ETextureCubeFace>(FaceIndex), X, Y, FaceDimension, Direction))
					{
						OutError = "Unable to resolve a projected cube pixel direction.";
						return false;
					}
					const FBilinearSample Sample = MakeBilinearSample(Direction, Panorama.Width, Panorama.Height);
					const size_t Destination = (static_cast<size_t>(Y) * FaceDimension + X) * LDRChannelCount;
					for (uint32 Channel = 0; Channel < LDRChannelCount; ++Channel)
					{
						double Value = 0.0;
						for (uint32 Tap = 0; Tap < 4; ++Tap)
						{
							const uint8 Encoded = std::to_integer<uint8>(
								Panorama.Pixels[Sample.PixelIndices[Tap] * LDRChannelCount + Channel]);
							Value += Sample.Weights[Tap] * (Channel < 3
								? ColorConvert::SRGB8ToLinear(Encoded)
								: static_cast<double>(Encoded) / 255.0);
						}
						Face.Pixels[Destination + Channel] = static_cast<std::byte>(
							Channel < 3 ? ColorConvert::LinearToSRGB8(Value) : ColorConvert::QuantizeUNorm8(Value));
					}
					Face.bHasTransparency |= Face.Pixels[Destination + 3] != static_cast<std::byte>(255);
				}
			}
		}
		OutSourceData = std::move(Projected);
		return true;
	}

	auto ProjectEquirectangularTextureCube(const FTexturePanoramaFloatImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool
	{
		OutSourceData = {};
		uint32 FaceDimension = 0;
		if (!ValidateEquirectangularTextureCubeProjection(
			Panorama.Width, Panorama.Height, Settings, true, FaceDimension, OutError)
			|| !ValidateHDRPanorama(Panorama, OutError))
		{
			return false;
		}
		const double Exposure = std::exp2(static_cast<double>(Settings.ExposureEV));

		FTextureCubeSourceData Projected;
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			FTextureSourceData& Face = Projected.Faces[FaceIndex];
			InitializeFace(Face, FaceDimension);
			for (uint32 Y = 0; Y < FaceDimension; ++Y)
			{
				for (uint32 X = 0; X < FaceDimension; ++X)
				{
					FVector3 Direction{};
					if (!ResolveTextureCubeFacePixelDirection(
						static_cast<ETextureCubeFace>(FaceIndex), X, Y, FaceDimension, Direction))
					{
						OutError = "Unable to resolve a projected cube pixel direction.";
						return false;
					}
					const FBilinearSample Sample = MakeBilinearSample(Direction, Panorama.Width, Panorama.Height);
					const size_t Destination = (static_cast<size_t>(Y) * FaceDimension + X) * LDRChannelCount;
					for (uint32 Channel = 0; Channel < HDRChannelCount; ++Channel)
					{
						double Linear = 0.0;
						for (uint32 Tap = 0; Tap < 4; ++Tap)
							Linear += Sample.Weights[Tap] * Panorama.Pixels[Sample.PixelIndices[Tap] * HDRChannelCount + Channel];
						const double Exposed = Linear * Exposure;
						if (!std::isfinite(Exposed))
						{
							OutError = "HDR panorama exposure produced a nonfinite channel.";
							return false;
						}
						Face.Pixels[Destination + Channel] = static_cast<std::byte>(
							ColorConvert::LinearToSRGB8(FilmicToneMap(Exposed)));
					}
					Face.Pixels[Destination + 3] = static_cast<std::byte>(255);
				}
			}
		}
		OutSourceData = std::move(Projected);
		return true;
	}
}
