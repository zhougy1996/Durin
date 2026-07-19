#include "ImageDecoder.h"

#include "Misc/FileHelper.h"

#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 OutputChannelCount = 4;

		auto LowercaseExtension(std::string_view Extension) -> std::string
		{
			std::string Result(Extension);
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return Result;
		}
	} // namespace

	auto IsSupportedImageExtension(std::string_view Extension) -> bool
	{
		const std::string Lowercase = LowercaseExtension(Extension);
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg" || Lowercase == ".bmp" || Lowercase == ".tga";
	}

	auto DecodeImageFromMemory(std::span<const uint8> EncodedBytes, FDecodedImage& OutImage, std::string& OutError, const FImageDecodeLimits& Limits) -> bool
	{
		OutImage = {};
		OutError.clear();
		if (EncodedBytes.empty())
		{
			OutError = "The image data is empty.";
			return false;
		}
		if (EncodedBytes.size() > Limits.MaximumEncodedBytes || EncodedBytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		{
			OutError = "The encoded image is too large.";
			return false;
		}

		int Width = 0;
		int Height = 0;
		int Channels = 0;
		const int EncodedSize = static_cast<int>(EncodedBytes.size());
		if (stbi_info_from_memory(EncodedBytes.data(), EncodedSize, &Width, &Height, &Channels) == 0 || Width <= 0 || Height <= 0 || Channels <= 0)
		{
			OutError = "The image is unsupported or corrupt.";
			return false;
		}

		const uint64 PixelCount = static_cast<uint64>(Width) * static_cast<uint64>(Height);
		if (PixelCount > Limits.MaximumDecodedPixels)
		{
			OutError = "The decoded image is too large.";
			return false;
		}

		stbi_uc* Decoded = stbi_load_from_memory(EncodedBytes.data(), EncodedSize, &Width, &Height, &Channels, STBI_rgb_alpha);
		if (!Decoded)
		{
			OutError = "The image is unsupported or corrupt.";
			return false;
		}

		OutImage.Width = static_cast<uint32>(Width);
		OutImage.Height = static_cast<uint32>(Height);
		OutImage.SourceChannelCount = static_cast<uint8>(Channels);
		const size_t DecodedSize = static_cast<size_t>(PixelCount * OutputChannelCount);
		OutImage.Pixels.assign(Decoded, Decoded + DecodedSize);
		stbi_image_free(Decoded);

		for (size_t AlphaIndex = 3; AlphaIndex < OutImage.Pixels.size(); AlphaIndex += OutputChannelCount)
		{
			if (OutImage.Pixels[AlphaIndex] != 255)
			{
				OutImage.bHasTransparency = true;
				break;
			}
		}
		return true;
	}

	auto DecodeImageFromFile(std::string_view FilePath, FDecodedImage& OutImage, std::string& OutError, const FImageDecodeLimits& Limits) -> bool
	{
		OutImage = {};
		OutError.clear();

		std::error_code ErrorCode;
		const uintmax_t FileSize = std::filesystem::file_size(std::filesystem::path(FilePath), ErrorCode);
		if (ErrorCode)
		{
			OutError = "Unable to open the image file.";
			return false;
		}
		if (FileSize == 0 || FileSize > Limits.MaximumEncodedBytes || FileSize > static_cast<uintmax_t>(std::numeric_limits<int>::max()))
		{
			OutError = "The image file is empty or too large.";
			return false;
		}

		std::vector<uint8> EncodedBytes;
		if (!FFileHelper::LoadFileToArray(EncodedBytes, FilePath))
		{
			OutError = "Unable to read the image file.";
			return false;
		}
		return DecodeImageFromMemory(EncodedBytes, OutImage, OutError, Limits);
	}
} // namespace Durin::Asset
