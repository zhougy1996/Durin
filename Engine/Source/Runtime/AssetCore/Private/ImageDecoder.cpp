#include "ImageDecoder.h"

#include "Misc/FileHelper.h"

#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 OutputChannelCount = 4;
		constexpr uint32 HDRChannelCount = 3;
		constexpr size_t MaximumRadianceHeaderBytes = 64 * 1024;
		constexpr size_t MaximumRadianceHeaderLineBytes = 4096;

		auto LowercaseExtension(std::string_view Extension) -> std::string
		{
			std::string Result(Extension);
			std::ranges::transform(Result, Result.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return Result;
		}

		auto ReadRadianceLine(std::span<const uint8> Bytes, size_t& Offset, std::string_view& OutLine) -> bool
		{
			if (Offset >= Bytes.size()) return false;
			const size_t Begin = Offset;
			while (Offset < Bytes.size() && Bytes[Offset] != '\n')
			{
				if (Offset - Begin >= MaximumRadianceHeaderLineBytes) return false;
				++Offset;
			}
			if (Offset >= Bytes.size()) return false;
			size_t End = Offset++;
			if (End > Begin && Bytes[End - 1] == '\r') --End;
			OutLine = std::string_view(reinterpret_cast<const char*>(Bytes.data() + Begin), End - Begin);
			return true;
		}

		auto ParseRadianceResolution(std::string_view Line, uint32& OutWidth, uint32& OutHeight) -> bool
		{
			std::istringstream Stream{std::string(Line)};
			std::string YAxis;
			std::string XAxis;
			uint64 Width = 0;
			uint64 Height = 0;
			std::string Extra;
			if (!(Stream >> YAxis >> Height >> XAxis >> Width) || (Stream >> Extra)
				|| YAxis != "-Y" || XAxis != "+X"
				|| Width == 0 || Height == 0
				|| Width > std::numeric_limits<uint32>::max()
				|| Height > std::numeric_limits<uint32>::max())
			{
				return false;
			}
			OutWidth = static_cast<uint32>(Width);
			OutHeight = static_cast<uint32>(Height);
			return true;
		}

		auto DecodeRadianceNewScanline(std::span<const uint8> Bytes, size_t& Offset, uint32 Width,
			std::vector<std::array<uint8, 4>>& OutScanline, std::string& OutError) -> bool
		{
			for (uint32 Channel = 0; Channel < 4; ++Channel)
			{
				uint32 X = 0;
				while (X < Width)
				{
					if (Offset >= Bytes.size())
					{
						OutError = "Radiance HDR scanline payload is truncated.";
						return false;
					}
					const uint8 Packet = Bytes[Offset++];
					if (Packet == 0)
					{
						OutError = "Radiance HDR scanline contains a zero-length packet.";
						return false;
					}
					if (Packet > 128)
					{
						const uint32 Count = Packet - 128;
						if (Count > Width - X)
						{
							OutError = "Radiance HDR scanline run exceeds its declared width.";
							return false;
						}
						if (Offset >= Bytes.size())
						{
							OutError = "Radiance HDR scanline run payload is truncated.";
							return false;
						}
						const uint8 Value = Bytes[Offset++];
						for (uint32 Index = 0; Index < Count; ++Index) OutScanline[X++][Channel] = Value;
					}
					else
					{
						const uint32 Count = Packet;
						if (Count > Width - X)
						{
							OutError = "Radiance HDR scanline literal exceeds its declared width.";
							return false;
						}
						if (Count > Bytes.size() - Offset)
						{
							OutError = "Radiance HDR scanline literal payload is truncated.";
							return false;
						}
						for (uint32 Index = 0; Index < Count; ++Index) OutScanline[X++][Channel] = Bytes[Offset++];
					}
				}
			}
			return true;
		}

		auto DecodeRadianceOldScanline(std::span<const uint8> Bytes, size_t& Offset, uint32 Width,
			const std::array<uint8, 4>& FirstToken, std::vector<std::array<uint8, 4>>& OutScanline,
			std::string& OutError) -> bool
		{
			uint32 X = 0;
			uint32 RepeatShift = 0;
			std::array<uint8, 4> Token = FirstToken;
			while (X < Width)
			{
				if (Token[0] == 1 && Token[1] == 1 && Token[2] == 1)
				{
					if (X == 0 || RepeatShift > 24)
					{
						OutError = "Radiance HDR old scanline contains an invalid repeat packet.";
						return false;
					}
					const uint64 Count = static_cast<uint64>(Token[3]) << RepeatShift;
					if (Count == 0 || Count > Width - X)
					{
						OutError = "Radiance HDR old scanline repeat exceeds its declared width.";
						return false;
					}
					const std::array<uint8, 4> Previous = OutScanline[X - 1];
					for (uint64 Index = 0; Index < Count; ++Index) OutScanline[X++] = Previous;
					RepeatShift += 8;
				}
				else
				{
					OutScanline[X++] = Token;
					RepeatShift = 0;
				}

				if (X == Width) break;
				if (Bytes.size() - Offset < 4)
				{
					OutError = "Radiance HDR old scanline payload is truncated.";
					return false;
				}
				std::copy_n(Bytes.data() + Offset, 4, Token.begin());
				Offset += 4;
			}
			return true;
		}

		auto DecodeRGBE(const std::array<uint8, 4>& RGBE, float* OutRGB) -> bool
		{
			if (RGBE[3] == 0)
			{
				OutRGB[0] = 0.0f;
				OutRGB[1] = 0.0f;
				OutRGB[2] = 0.0f;
				return true;
			}
			const float Scale = std::ldexp(1.0f, static_cast<int>(RGBE[3]) - (128 + 8));
			for (uint32 Channel = 0; Channel < HDRChannelCount; ++Channel)
			{
				OutRGB[Channel] = static_cast<float>(RGBE[Channel]) * Scale;
				if (!std::isfinite(OutRGB[Channel]) || OutRGB[Channel] < 0.0f) return false;
			}
			return true;
		}
	} // namespace

	auto IsSupportedImageExtension(std::string_view Extension) -> bool
	{
		const std::string Lowercase = LowercaseExtension(Extension);
		return Lowercase == ".png" || Lowercase == ".jpg" || Lowercase == ".jpeg" || Lowercase == ".bmp" || Lowercase == ".tga";
	}

	auto IsRadianceHDRExtension(std::string_view Extension) -> bool
	{
		return LowercaseExtension(Extension) == ".hdr";
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

	auto DecodeRadianceHDRFromMemory(std::span<const uint8> EncodedBytes, FDecodedFloatImage& OutImage,
		std::string& OutError, const FRadianceHDRDecodeLimits& Limits) -> bool
	{
		OutImage = {};
		OutError.clear();
		if (EncodedBytes.empty())
		{
			OutError = "Radiance HDR data is empty.";
			return false;
		}
		if (EncodedBytes.size() > Limits.MaximumEncodedBytes)
		{
			OutError = "Radiance HDR encoded data exceeds the configured limit.";
			return false;
		}

		size_t Offset = 0;
		std::string_view Line;
		if (!ReadRadianceLine(EncodedBytes, Offset, Line) || (Line != "#?RADIANCE" && Line != "#?RGBE"))
		{
			OutError = "Radiance HDR signature is missing or invalid.";
			return false;
		}

		bool bFoundFormat = false;
		while (true)
		{
			if (Offset > MaximumRadianceHeaderBytes || !ReadRadianceLine(EncodedBytes, Offset, Line))
			{
				OutError = "Radiance HDR header is truncated or too large.";
				return false;
			}
			if (Line.empty()) break;
			if (Line == "FORMAT=32-bit_rle_rgbe") bFoundFormat = true;
		}
		if (!bFoundFormat)
		{
			OutError = "Radiance HDR FORMAT=32-bit_rle_rgbe declaration is missing.";
			return false;
		}
		if (!ReadRadianceLine(EncodedBytes, Offset, Line))
		{
			OutError = "Radiance HDR resolution line is missing.";
			return false;
		}

		uint32 Width = 0;
		uint32 Height = 0;
		if (!ParseRadianceResolution(Line, Width, Height))
		{
			OutError = "Radiance HDR resolution must use nonzero '-Y height +X width' orientation.";
			return false;
		}
		if (Width > Limits.MaximumDimension || Height > Limits.MaximumDimension)
		{
			OutError = "Radiance HDR dimensions exceed the configured limit.";
			return false;
		}
		const uint64 PixelCount = static_cast<uint64>(Width) * Height;
		if (PixelCount > Limits.MaximumDecodedPixels
			|| PixelCount > std::numeric_limits<size_t>::max() / HDRChannelCount / sizeof(float))
		{
			OutError = "Radiance HDR decoded pixels exceed the configured limit.";
			return false;
		}

		FDecodedFloatImage Decoded;
		Decoded.Width = Width;
		Decoded.Height = Height;
		Decoded.Pixels.resize(static_cast<size_t>(PixelCount) * HDRChannelCount);
		std::vector<std::array<uint8, 4>> Scanline(Width);
		for (uint32 Y = 0; Y < Height; ++Y)
		{
			if (EncodedBytes.size() - Offset < 4)
			{
				OutError = "Radiance HDR scanline header is truncated.";
				return false;
			}
			std::array<uint8, 4> Header;
			std::copy_n(EncodedBytes.data() + Offset, 4, Header.begin());
			Offset += 4;

			const bool bNewEncoding = Width >= 8 && Width <= 32767
				&& Header[0] == 2 && Header[1] == 2 && (Header[2] & 0x80) == 0;
			if (bNewEncoding)
			{
				const uint32 ScanlineWidth = (static_cast<uint32>(Header[2]) << 8) | Header[3];
				if (ScanlineWidth != Width)
				{
					OutError = "Radiance HDR scanline width does not match the resolution.";
					return false;
				}
				if (!DecodeRadianceNewScanline(EncodedBytes, Offset, Width, Scanline, OutError)) return false;
			}
			else if (!DecodeRadianceOldScanline(EncodedBytes, Offset, Width, Header, Scanline, OutError))
			{
				return false;
			}

			for (uint32 X = 0; X < Width; ++X)
			{
				float* Pixel = Decoded.Pixels.data() + (static_cast<size_t>(Y) * Width + X) * HDRChannelCount;
				if (!DecodeRGBE(Scanline[X], Pixel))
				{
					OutError = "Radiance HDR decoded a negative or nonfinite channel.";
					return false;
				}
			}
		}
		if (Offset != EncodedBytes.size())
		{
			OutError = "Radiance HDR payload contains trailing bytes.";
			return false;
		}
		OutImage = std::move(Decoded);
		return true;
	}

	auto DecodeRadianceHDRFromFile(std::string_view FilePath, FDecodedFloatImage& OutImage,
		std::string& OutError, const FRadianceHDRDecodeLimits& Limits) -> bool
	{
		OutImage = {};
		OutError.clear();
		std::error_code ErrorCode;
		const uintmax_t FileSize = std::filesystem::file_size(std::filesystem::path(FilePath), ErrorCode);
		if (ErrorCode)
		{
			OutError = "Unable to open the Radiance HDR file.";
			return false;
		}
		if (FileSize == 0 || FileSize > Limits.MaximumEncodedBytes
			|| FileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
		{
			OutError = "The Radiance HDR file is empty or too large.";
			return false;
		}
		std::vector<uint8> EncodedBytes;
		if (!FFileHelper::LoadFileToArray(EncodedBytes, FilePath))
		{
			OutError = "Unable to read the Radiance HDR file.";
			return false;
		}
		return DecodeRadianceHDRFromMemory(EncodedBytes, OutImage, OutError, Limits);
	}
} // namespace Durin::Asset
