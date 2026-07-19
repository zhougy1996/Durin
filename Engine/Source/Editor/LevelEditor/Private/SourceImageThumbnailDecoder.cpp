#include "SourceImageThumbnailDecoder.h"

#include "ImageDecoder.h"

namespace Durin
{
	namespace
	{
		// Thumbnail requests run concurrently, so bound the full-resolution intermediate rather than relying on the much larger import limit.
		constexpr Asset::FImageDecodeLimits ThumbnailDecodeLimits{32ull * 1024ull * 1024ull, 16ull * 1024ull * 1024ull};

		auto ResizeBilinear(const uint8* Source, uint32 SourceWidth, uint32 SourceHeight, uint32 DestinationWidth, uint32 DestinationHeight) -> std::vector<uint8>
		{
			std::vector<uint8> Result(static_cast<size_t>(DestinationWidth) * DestinationHeight * 4);
			for (uint32 Y = 0; Y < DestinationHeight; ++Y)
			{
				const float SourceY = (static_cast<float>(Y) + 0.5f) * static_cast<float>(SourceHeight) / static_cast<float>(DestinationHeight) - 0.5f;
				const int32 Y0 = std::clamp(static_cast<int32>(std::floor(SourceY)), 0, static_cast<int32>(SourceHeight) - 1);
				const int32 Y1 = std::min(Y0 + 1, static_cast<int32>(SourceHeight) - 1);
				const float YWeight = std::clamp(SourceY - static_cast<float>(Y0), 0.0f, 1.0f);
				for (uint32 X = 0; X < DestinationWidth; ++X)
				{
					const float SourceX = (static_cast<float>(X) + 0.5f) * static_cast<float>(SourceWidth) / static_cast<float>(DestinationWidth) - 0.5f;
					const int32 X0 = std::clamp(static_cast<int32>(std::floor(SourceX)), 0, static_cast<int32>(SourceWidth) - 1);
					const int32 X1 = std::min(X0 + 1, static_cast<int32>(SourceWidth) - 1);
					const float XWeight = std::clamp(SourceX - static_cast<float>(X0), 0.0f, 1.0f);
					for (uint32 Channel = 0; Channel < 4; ++Channel)
					{
						const float Top = std::lerp(
							static_cast<float>(Source[(static_cast<size_t>(Y0) * SourceWidth + X0) * 4 + Channel]),
							static_cast<float>(Source[(static_cast<size_t>(Y0) * SourceWidth + X1) * 4 + Channel]), XWeight);
						const float Bottom = std::lerp(
							static_cast<float>(Source[(static_cast<size_t>(Y1) * SourceWidth + X0) * 4 + Channel]),
							static_cast<float>(Source[(static_cast<size_t>(Y1) * SourceWidth + X1) * 4 + Channel]), XWeight);
						Result[(static_cast<size_t>(Y) * DestinationWidth + X) * 4 + Channel] = static_cast<uint8>(std::clamp(std::lround(std::lerp(Top, Bottom, YWeight)), 0l, 255l));
					}
				}
			}
			return Result;
		}
	} // namespace

	auto IsSupportedSourceImageExtension(std::string_view Extension) -> bool
	{
		return Asset::IsSupportedImageExtension(Extension);
	}

	auto DecodeSourceImageThumbnail(std::string_view FilePath, uint32 MaximumDimension, FDecodedSourceImageThumbnail& OutThumbnail, std::string& OutError) -> bool
	{
		OutThumbnail = {};
		OutError.clear();
		if (MaximumDimension == 0)
		{
			OutError = "Thumbnail size must be greater than zero.";
			return false;
		}

		Asset::FDecodedImage SourceImage;
		if (!Asset::DecodeImageFromFile(FilePath, SourceImage, OutError, ThumbnailDecodeLimits)) return false;

		const uint32 SourceWidth = SourceImage.Width;
		const uint32 SourceHeight = SourceImage.Height;
		const uint32 LongestEdge = std::max(SourceWidth, SourceHeight);
		const float Scale = LongestEdge > MaximumDimension ? static_cast<float>(MaximumDimension) / static_cast<float>(LongestEdge) : 1.0f;
		OutThumbnail.Width = std::max(1u, static_cast<uint32>(std::lround(static_cast<float>(SourceWidth) * Scale)));
		OutThumbnail.Height = std::max(1u, static_cast<uint32>(std::lround(static_cast<float>(SourceHeight) * Scale)));
		if (OutThumbnail.Width == SourceWidth && OutThumbnail.Height == SourceHeight)
			OutThumbnail.Pixels = std::move(SourceImage.Pixels);
		else
			OutThumbnail.Pixels = ResizeBilinear(SourceImage.Pixels.data(), SourceWidth, SourceHeight, OutThumbnail.Width, OutThumbnail.Height);

		for (size_t AlphaIndex = 3; AlphaIndex < OutThumbnail.Pixels.size(); AlphaIndex += 4)
		{
			if (OutThumbnail.Pixels[AlphaIndex] != 255)
			{
				OutThumbnail.bHasTransparency = true;
				break;
			}
		}
		return true;
	}
} // namespace Durin
