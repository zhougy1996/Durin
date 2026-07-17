#include "SourceImageThumbnailDecoder.h"

#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image.h"

namespace Durin
{
	namespace
	{
		constexpr uintmax_t MaximumEncodedImageBytes = 512ull * 1024ull * 1024ull;

		auto LowercaseExtension(std::string_view Extension) -> std::string
		{
			std::string Result(Extension);
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return Result;
		}

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
		const std::string Lowercase = LowercaseExtension(Extension);
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg" || Lowercase == ".bmp" || Lowercase == ".tga";
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

		std::ifstream Stream(std::filesystem::path(FilePath), std::ios::binary | std::ios::ate);
		if (!Stream)
		{
			OutError = "Unable to open the image file.";
			return false;
		}
		const std::streamsize FileSize = Stream.tellg();
		if (FileSize <= 0 || static_cast<uintmax_t>(FileSize) > MaximumEncodedImageBytes || FileSize > std::numeric_limits<int>::max())
		{
			OutError = "The image file is empty or too large to preview.";
			return false;
		}
		std::vector<uint8> Encoded(static_cast<size_t>(FileSize));
		Stream.seekg(0, std::ios::beg);
		if (!Stream.read(reinterpret_cast<char*>(Encoded.data()), FileSize))
		{
			OutError = "Unable to read the image file.";
			return false;
		}

		int Width = 0;
		int Height = 0;
		int Channels = 0;
		stbi_uc* Decoded = stbi_load_from_memory(Encoded.data(), static_cast<int>(Encoded.size()), &Width, &Height, &Channels, STBI_rgb_alpha);
		if (!Decoded || Width <= 0 || Height <= 0)
		{
			OutError = "The image is unsupported or corrupt.";
			if (Decoded) stbi_image_free(Decoded);
			return false;
		}

		const uint32 SourceWidth = static_cast<uint32>(Width);
		const uint32 SourceHeight = static_cast<uint32>(Height);
		const uint32 LongestEdge = std::max(SourceWidth, SourceHeight);
		const float Scale = LongestEdge > MaximumDimension ? static_cast<float>(MaximumDimension) / static_cast<float>(LongestEdge) : 1.0f;
		OutThumbnail.Width = std::max(1u, static_cast<uint32>(std::lround(static_cast<float>(SourceWidth) * Scale)));
		OutThumbnail.Height = std::max(1u, static_cast<uint32>(std::lround(static_cast<float>(SourceHeight) * Scale)));
		if (OutThumbnail.Width == SourceWidth && OutThumbnail.Height == SourceHeight)
			OutThumbnail.Pixels.assign(Decoded, Decoded + static_cast<size_t>(SourceWidth) * SourceHeight * 4);
		else
			OutThumbnail.Pixels = ResizeBilinear(Decoded, SourceWidth, SourceHeight, OutThumbnail.Width, OutThumbnail.Height);
		stbi_image_free(Decoded);

		for (size_t AlphaIndex = 3; AlphaIndex < OutThumbnail.Pixels.size(); AlphaIndex += 4)
			if (OutThumbnail.Pixels[AlphaIndex] != 255)
			{
				OutThumbnail.bHasTransparency = true;
				break;
			}
		return true;
	}
} // namespace Durin
