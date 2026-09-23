#include "Texture/TextureCubeBuilder.h"

#include "Math/Color.h"
#include "RHIResources.h"

namespace Durin::TextureCubeBuilder
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

		auto ValidateLDRPanorama(const FTexturePanoramaImage& Panorama) -> std::expected<void, FTextureBuildError>
		{
			const uint64 PixelCount = static_cast<uint64>(Panorama.Width) * Panorama.Height;
			if (PixelCount > std::numeric_limits<size_t>::max() / LDRChannelCount
				|| Panorama.Pixels.size() != static_cast<size_t>(PixelCount) * LDRChannelCount)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					"LDR panorama pixel storage does not match its dimensions.", ETextureCubeInputError::PixelStorage});
			}
			return {};
		}

		auto ValidateHDRPanorama(const FTexturePanoramaFloatImage& Panorama) -> std::expected<void, FTextureBuildError>
		{
			const uint64 PixelCount = static_cast<uint64>(Panorama.Width) * Panorama.Height;
			if (PixelCount > std::numeric_limits<size_t>::max() / HDRChannelCount
				|| Panorama.Pixels.size() != static_cast<size_t>(PixelCount) * HDRChannelCount)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					"HDR panorama pixel storage does not match its dimensions.", ETextureCubeInputError::PixelStorage});
			}
			if (std::ranges::any_of(Panorama.Pixels, [](float Value) { return !std::isfinite(Value) || Value < 0.0f; }))
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					"HDR panorama contains a negative or nonfinite channel.", ETextureCubeInputError::InvalidRadiance});
			}
			return {};
		}
	} // namespace

	auto ValidateHDRTextureCubePanorama(const FTexturePanoramaFloatImage& Panorama,
		const FTextureCubePanoramaBuildSettings& Settings) -> std::expected<void, FTextureBuildError>
	{
		uint32 Dimension = 0;
		if (Settings.Output != ETextureCubeOutput::HDR)
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize, "HDR cube output is required.", ETextureCubeInputError::OutputMode});
		if (auto Result = ValidateEquirectangularTextureCubeProjection(Panorama.Width, Panorama.Height,
			{Settings.FaceDimension, Settings.ExposureEV}, true, Dimension); !Result) return Result;
		if (auto Result = ValidateHDRPanorama(Panorama); !Result) return Result;
		if (Dimension > 512)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"HDR cube output is limited to 512 pixels per face.", ETextureCubeInputError::FaceDimensionLimit});
		}
		const double Exposure = std::exp2(static_cast<double>(Settings.ExposureEV));
		if (std::ranges::any_of(Panorama.Pixels, [Exposure](float Value) {
			return static_cast<double>(Value) * Exposure > 16384.0;
		}))
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"Exposed HDR radiance exceeds the 16384 lighting limit.", ETextureCubeInputError::RadianceLimit});
		}
		return {};
	}

	auto BuildHDRTextureCube(const Image::FImage& Panorama,
		const FTextureCubePanoramaBuildSettings& Settings,
		FTextureCubePlatformData& OutData) -> std::expected<void, FTextureBuildError>
	{
		OutData = {};
		if (!Panorama.IsValid() || Panorama.GetInfo().Format != Image::ERawImageFormat::RGBA32F
			|| Panorama.GetInfo().GammaSpace != Image::EImageGammaSpace::Linear
			|| Panorama.GetInfo().Depth != 1 || Panorama.GetInfo().SliceCount != 1)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build,
				"HDR cube requires a single linear RGBA32F panorama."});
		}
		FTexturePanoramaFloatImage Input;
		Input.Width = Panorama.GetInfo().Width;
		Input.Height = Panorama.GetInfo().Height;
		const size_t PixelCount = static_cast<size_t>(Input.Width) * Input.Height;
		Input.Pixels.resize(PixelCount * 3);
		for (size_t Index = 0; Index < PixelCount; ++Index)
			std::memcpy(Input.Pixels.data() + Index * 3,
				Panorama.GetPixels().data() + Index * 16, 3 * sizeof(float));
		if (auto Result = ValidateHDRTextureCubePanorama(Input, Settings); !Result)
		{
			auto Error = std::move(Result.error());
			Error.Code = ETextureBuildFailure::BuildFailed;
			Error.Stage = ETextureBuildStage::Build;
			return std::unexpected(std::move(Error));
		}
		const uint32 BaseDimension = Settings.FaceDimension == 0
			? std::max(Input.Width / 4, 1u) : Settings.FaceDimension;
		const double Exposure = std::exp2(static_cast<double>(Settings.ExposureEV));
		FTextureCubePlatformData Candidate;
		Candidate.PixelFormat = EPixelFormat::RGBA32_FLOAT;
		for (uint32 Face = 0; Face < TextureCubeFaceCount; ++Face)
		{
			auto& Output = Candidate.Faces[Face];
			Output.PixelFormat = Candidate.PixelFormat;
			for (uint32 Dimension = BaseDimension;; Dimension = std::max(Dimension / 2, 1u))
			{
				FTexture2DMipData Mip;
				Mip.Width = Mip.Height = Dimension;
				Mip.RowPitch = Dimension * 16;
				Mip.Pixels.resize(static_cast<size_t>(Mip.RowPitch) * Dimension);
				// Integrate each angular footprint against the original panorama. The
				// cube Jacobian avoids overweighting face corners in ordinary mips.
				const uint32 Grid = Dimension == BaseDimension ? 1u : 8u;
				for (uint32 Y = 0; Y < Dimension; ++Y)
					for (uint32 X = 0; X < Dimension; ++X)
					{
						std::array<double, 3> Sum{};
						double Weight = 0;
						for (uint32 SY = 0; SY < Grid; ++SY)
							for (uint32 SX = 0; SX < Grid; ++SX)
							{
								FVector3 Direction;
								if (!ResolveTextureCubeFacePixelDirection(static_cast<ETextureCubeFace>(Face),
									X * Grid + SX, Y * Grid + SY, Dimension * Grid, Direction)) return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build, "HDR cube projection failed."});
								const double A = 2.0 * (X + (SX + 0.5) / Grid) / Dimension - 1.0;
								const double B = 2.0 * (Y + (SY + 0.5) / Grid) / Dimension - 1.0;
								const double W = std::pow(1.0 + A * A + B * B, -1.5);
								const auto Sample = MakeBilinearSample(Direction, Input.Width, Input.Height);
								for (uint32 Channel = 0; Channel < 3; ++Channel)
									for (uint32 Tap = 0; Tap < 4; ++Tap)
										Sum[Channel] += W * Sample.Weights[Tap]
											* Input.Pixels[Sample.PixelIndices[Tap] * 3 + Channel];
								Weight += W;
							}
						const std::array<float, 4> Pixel{static_cast<float>(Sum[0] / Weight * Exposure),
							static_cast<float>(Sum[1] / Weight * Exposure),
							static_cast<float>(Sum[2] / Weight * Exposure), 1.0f};
						std::memcpy(Mip.Pixels.data() + static_cast<size_t>(Y) * Mip.RowPitch + X * 16,
							Pixel.data(), 16);
					}
				Output.Mips.push_back(std::move(Mip));
				if (Dimension == 1) break;
			}
		}
		if (!Candidate.IsValid()) return std::unexpected(FTextureBuildError{ETextureBuildFailure::BuildFailed, ETextureBuildStage::Build, "HDR cube platform data is invalid."});
		OutData = std::move(Candidate);
		return {};
	}

	auto ValidateEquirectangularTextureCubeProjection(uint32 Width, uint32 Height,
		const FEquirectangularTextureCubeProjectionSettings& Settings, bool bHDR,
		uint32& OutFaceDimension) -> std::expected<void, FTextureBuildError>
	{
		OutFaceDimension = 0;
		if (Width == 0 || Height == 0)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"Equirectangular panorama dimensions must be nonzero.", ETextureCubeInputError::EmptyDimensions});
		}
		if (static_cast<uint64>(Height) * 2 != Width)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				std::format("Equirectangular panorama must have an exact 2:1 aspect ratio, but is {}x{}.", Width, Height), ETextureCubeInputError::AspectRatio});
		}
		if (Width > MaximumPanoramaDimension || Height > MaximumPanoramaDimension)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				std::format("Equirectangular panorama dimensions exceed the {} pixel limit.", MaximumPanoramaDimension), ETextureCubeInputError::DimensionLimit});
		}
		const uint64 PixelCount = static_cast<uint64>(Width) * Height;
		if (PixelCount > MaximumPanoramaPixels)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"Equirectangular panorama decoded pixels exceed the 33554432 pixel limit.", ETextureCubeInputError::PixelLimit});
		}
		if (!std::isfinite(Settings.ExposureEV)
			|| Settings.ExposureEV < MinimumPanoramaExposureEV
			|| Settings.ExposureEV > MaximumPanoramaExposureEV)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"Panorama exposure must be finite and between -16 and 16 EV.", ETextureCubeInputError::Exposure});
		}
		if (!bHDR && Settings.ExposureEV != 0.0f)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"Exposure is available only for Radiance HDR panoramas.", ETextureCubeInputError::LDRExposure});
		}

		const uint32 FaceDimension = Settings.FaceDimension == 0
			? std::max(Width / 4, 1u)
			: Settings.FaceDimension;
		if (FaceDimension > MaximumProjectedCubeFaceDimension)
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				std::format("Projected cube face dimension must be between 1 and {}.",
				MaximumProjectedCubeFaceDimension), ETextureCubeInputError::FaceDimensionLimit});
		}
		const uint64 ProjectedBytes = static_cast<uint64>(TextureCubeFaceCount) * FaceDimension * FaceDimension * LDRChannelCount;
		if (ProjectedBytes > std::numeric_limits<size_t>::max())
		{
			return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"Projected cube byte count exceeds the addressable allocation limit.", ETextureCubeInputError::AllocationLimit});
		}
		OutFaceDimension = FaceDimension;
		return {};
	}

	auto ProjectEquirectangularTextureCube(const FTexturePanoramaImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeDecodedFaces& OutSourceData) -> std::expected<void, FTextureBuildError>
	{
		OutSourceData = {};
		uint32 FaceDimension = 0;
		if (auto Result = ValidateEquirectangularTextureCubeProjection(
			Panorama.Width, Panorama.Height, Settings, false, FaceDimension); !Result) return Result;
		if (auto Result = ValidateLDRPanorama(Panorama); !Result) return Result;

		FTextureCubeDecodedFaces Projected;
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			FByteBuffer Pixels(static_cast<size_t>(FaceDimension) * FaceDimension * LDRChannelCount);
			for (uint32 Y = 0; Y < FaceDimension; ++Y)
			{
				for (uint32 X = 0; X < FaceDimension; ++X)
				{
					FVector3 Direction{};
					if (!ResolveTextureCubeFacePixelDirection(
						static_cast<ETextureCubeFace>(FaceIndex), X, Y, FaceDimension, Direction))
					{
						return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
							"Unable to resolve a projected cube pixel direction."});
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
						Pixels[Destination + Channel] = static_cast<std::byte>(
							Channel < 3 ? ColorConvert::LinearToSRGB8(Value) : ColorConvert::QuantizeUNorm8(Value));
					}
					if (Pixels[Destination + 3] != std::byte{255})
						Projected.TransparencyMask |= static_cast<uint8>(1u << FaceIndex);
				}
			}
			auto ImageResult1 = Image::FImage::TryCreate({.Width = FaceDimension, .Height = FaceDimension,
				.Format = Image::ERawImageFormat::RGBA8}, std::move(Pixels));
			if (!ImageResult1)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					ImageResult1.error().ToString()});
			}
			Projected.Faces[FaceIndex] = std::move(*ImageResult1);
		}
		Projected.SourceChannelCounts.fill(LDRChannelCount);
		OutSourceData = std::move(Projected);
		return {};
	}

	auto ProjectEquirectangularTextureCube(const FTexturePanoramaFloatImage& Panorama,
		const FEquirectangularTextureCubeProjectionSettings& Settings,
		FTextureCubeDecodedFaces& OutSourceData) -> std::expected<void, FTextureBuildError>
	{
		OutSourceData = {};
		uint32 FaceDimension = 0;
		if (auto Result = ValidateEquirectangularTextureCubeProjection(
			Panorama.Width, Panorama.Height, Settings, true, FaceDimension); !Result) return Result;
		if (auto Result = ValidateHDRPanorama(Panorama); !Result) return Result;
		const double Exposure = std::exp2(static_cast<double>(Settings.ExposureEV));

		FTextureCubeDecodedFaces Projected;
		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			FByteBuffer Pixels(static_cast<size_t>(FaceDimension) * FaceDimension * LDRChannelCount);
			for (uint32 Y = 0; Y < FaceDimension; ++Y)
			{
				for (uint32 X = 0; X < FaceDimension; ++X)
				{
					FVector3 Direction{};
					if (!ResolveTextureCubeFacePixelDirection(
						static_cast<ETextureCubeFace>(FaceIndex), X, Y, FaceDimension, Direction))
					{
						return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
							"Unable to resolve a projected cube pixel direction."});
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
							return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
								"HDR panorama exposure produced a nonfinite channel."});
						}
						Pixels[Destination + Channel] = static_cast<std::byte>(
							ColorConvert::LinearToSRGB8(FilmicToneMap(Exposed)));
					}
					Pixels[Destination + 3] = static_cast<std::byte>(255);
				}
			}
			auto ImageResult2 = Image::FImage::TryCreate({.Width = FaceDimension, .Height = FaceDimension,
				.Format = Image::ERawImageFormat::RGBA8}, std::move(Pixels));
			if (!ImageResult2)
			{
				return std::unexpected(FTextureBuildError{ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
					ImageResult2.error().ToString()});
			}
			Projected.Faces[FaceIndex] = std::move(*ImageResult2);
		}
		Projected.SourceChannelCounts.fill(LDRChannelCount);
		OutSourceData = std::move(Projected);
		return {};
	}
}
