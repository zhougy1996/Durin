#include "Image/ImageDecoder.h"

#include "Misc/FileHelper.h"
#include "Misc/StringHelper.h"

#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image.h"

namespace Durin::Image
{
	namespace
	{
		constexpr uint32 OutputChannelCount = 4;
		constexpr uint32 HDRChannelCount = 3;
		constexpr size_t MaximumRadianceHeaderBytes = 64 * 1024;
		constexpr size_t MaximumRadianceHeaderLineBytes = 4096;

		auto ToUint8(std::byte Value) -> uint8
		{
			return std::to_integer<uint8>(Value);
		}

		auto LowercaseExtension(std::string_view Extension) -> std::string
		{
			return StringUtils::FoldAscii(Extension);
		}

		auto ReadPngU32(FByteView Bytes, size_t Offset, uint32& OutValue) -> bool
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < 4) return false;
			OutValue = static_cast<uint32>(ToUint8(Bytes[Offset])) << 24
				| static_cast<uint32>(ToUint8(Bytes[Offset + 1])) << 16
				| static_cast<uint32>(ToUint8(Bytes[Offset + 2])) << 8
				| static_cast<uint32>(ToUint8(Bytes[Offset + 3]));
			return true;
		}

		auto ReadRadianceLine(FByteView Bytes, size_t& Offset, std::string_view& OutLine) -> bool
		{
			if (Offset >= Bytes.size()) return false;
			const size_t Begin = Offset;
			while (Offset < Bytes.size() && Bytes[Offset] != static_cast<std::byte>('\n'))
			{
				if (Offset - Begin >= MaximumRadianceHeaderLineBytes) return false;
				++Offset;
			}
			if (Offset >= Bytes.size()) return false;
			size_t End = Offset++;
			if (End > Begin && Bytes[End - 1] == static_cast<std::byte>('\r')) --End;
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

		auto DecodeRadianceNewScanline(FByteView Bytes, size_t& Offset, uint32 Width,
			std::vector<std::array<uint8, 4>>& OutScanline) -> std::expected<void, ERadianceHDRDecodeError>
		{
			for (uint32 Channel = 0; Channel < 4; ++Channel)
			{
				uint32 X = 0;
				while (X < Width)
				{
					if (Offset >= Bytes.size())
					{
						return std::unexpected(ERadianceHDRDecodeError::TruncatedScanline);
					}
					const uint8 Packet = ToUint8(Bytes[Offset++]);
					if (Packet == 0)
					{
						return std::unexpected(ERadianceHDRDecodeError::ZeroLengthPacket);
					}
					if (Packet > 128)
					{
						const uint32 Count = Packet - 128;
						if (Count > Width - X)
						{
							return std::unexpected(ERadianceHDRDecodeError::RunExceedsWidth);
						}
						if (Offset >= Bytes.size())
						{
							return std::unexpected(ERadianceHDRDecodeError::TruncatedRun);
						}
						const uint8 Value = ToUint8(Bytes[Offset++]);
						for (uint32 Index = 0; Index < Count; ++Index) OutScanline[X++][Channel] = Value;
					}
					else
					{
						const uint32 Count = Packet;
						if (Count > Width - X)
						{
							return std::unexpected(ERadianceHDRDecodeError::LiteralExceedsWidth);
						}
						if (Count > Bytes.size() - Offset)
						{
							return std::unexpected(ERadianceHDRDecodeError::TruncatedLiteral);
						}
						for (uint32 Index = 0; Index < Count; ++Index) OutScanline[X++][Channel] = ToUint8(Bytes[Offset++]);
					}
				}
			}
			return {};
		}

		auto DecodeRadianceOldScanline(FByteView Bytes, size_t& Offset, uint32 Width,
			const std::array<uint8, 4>& FirstToken, std::vector<std::array<uint8, 4>>& OutScanline) -> std::expected<void, ERadianceHDRDecodeError>
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
						return std::unexpected(ERadianceHDRDecodeError::InvalidRepeat);
					}
					const uint64 Count = static_cast<uint64>(Token[3]) << RepeatShift;
					if (Count == 0 || Count > Width - X)
					{
						return std::unexpected(ERadianceHDRDecodeError::RepeatExceedsWidth);
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
					return std::unexpected(ERadianceHDRDecodeError::TruncatedOldScanline);
				}
				for (size_t Index = 0; Index < Token.size(); ++Index)
					Token[Index] = ToUint8(Bytes[Offset + Index]);
				Offset += 4;
			}
			return {};
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

	auto ToString(const FImageDecodeError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EImageDecodeError::None: return {};
		case EImageDecodeError::Empty: return "The image data is empty.";
		case EImageDecodeError::EncodedLimit: return "The encoded image is too large.";
		case EImageDecodeError::InvalidImage: return "The image is unsupported or corrupt.";
		case EImageDecodeError::PixelLimit: return "The decoded image is too large.";
		case EImageDecodeError::FileStat: return Error.FileError ? Error.FileError->ToString() : "Unable to inspect the image file.";
		case EImageDecodeError::FileSize: return "The image file is empty or too large.";
		case EImageDecodeError::FileRead: return Error.FileError ? Error.FileError->ToString() : "Unable to read the image file.";
		}
		return {};
	}

	auto DecodeImageFromMemory(FByteView EncodedBytes, const FImageDecodeLimits& Limits) -> std::expected<FDecodedImage, FImageDecodeError>
	{
		FDecodedImage OutImage;
		int Width = 0;
		int Height = 0;
		auto Fail = [&](EImageDecodeError Code) -> std::expected<FDecodedImage, FImageDecodeError> {
			return std::unexpected(FImageDecodeError{.Code = Code, .EncodedBytes = EncodedBytes.size(),
				.Width = Width, .Height = Height, .Limits = Limits});
		};
		if (EncodedBytes.empty())
		{
			return Fail(EImageDecodeError::Empty);
		}
		if (EncodedBytes.size() > Limits.MaximumEncodedBytes || EncodedBytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		{
			return Fail(EImageDecodeError::EncodedLimit);
		}

		int Channels = 0;
		const int EncodedSize = static_cast<int>(EncodedBytes.size());
		const auto* StbiBytes = reinterpret_cast<const stbi_uc*>(EncodedBytes.data());
		if (stbi_info_from_memory(StbiBytes, EncodedSize, &Width, &Height, &Channels) == 0 || Width <= 0 || Height <= 0 || Channels <= 0)
		{
			return Fail(EImageDecodeError::InvalidImage);
		}

		const uint64 PixelCount = static_cast<uint64>(Width) * static_cast<uint64>(Height);
		if (PixelCount > Limits.MaximumDecodedPixels)
		{
			return Fail(EImageDecodeError::PixelLimit);
		}

		stbi_uc* Decoded = stbi_load_from_memory(StbiBytes, EncodedSize, &Width, &Height, &Channels, STBI_rgb_alpha);
		if (!Decoded)
		{
			return Fail(EImageDecodeError::InvalidImage);
		}

		OutImage.Width = static_cast<uint32>(Width);
		OutImage.Height = static_cast<uint32>(Height);
		OutImage.SourceChannelCount = static_cast<uint8>(Channels);
		const size_t DecodedSize = static_cast<size_t>(PixelCount * OutputChannelCount);
		const auto* DecodedBytes = reinterpret_cast<const std::byte*>(Decoded);
		OutImage.Pixels.assign(DecodedBytes, DecodedBytes + DecodedSize);
		stbi_image_free(Decoded);

		for (size_t AlphaIndex = 3; AlphaIndex < OutImage.Pixels.size(); AlphaIndex += OutputChannelCount)
		{
			if (OutImage.Pixels[AlphaIndex] != static_cast<std::byte>(255))
			{
				OutImage.bHasTransparency = true;
				break;
			}
		}
		return OutImage;
	}

	auto DecodeImageFromFile(std::string_view FilePath, const FImageDecodeLimits& Limits) -> std::expected<FDecodedImage, FImageDecodeError>
	{
		std::error_code ErrorCode;
		const uintmax_t FileSize = std::filesystem::file_size(std::filesystem::path(FilePath), ErrorCode);
		if (ErrorCode)
		{
			return std::unexpected(FImageDecodeError{.Code = EImageDecodeError::FileStat, .Limits = Limits,
				.Filename = std::string(FilePath),
				.FileError = FFileError{EFileOperation::QuerySize, ErrorCode, FFilePath(FilePath)}});
		}
		if (FileSize == 0 || FileSize > Limits.MaximumEncodedBytes || FileSize > static_cast<uintmax_t>(std::numeric_limits<int>::max()))
		{
			return std::unexpected(FImageDecodeError{.Code = EImageDecodeError::FileSize, .EncodedBytes = FileSize,
				.Limits = Limits, .Filename = std::string(FilePath)});
		}

		auto EncodedBytes = FFileHelper::LoadFileToArray(FFilePath(FilePath));
		if (!EncodedBytes)
		{
			return std::unexpected(FImageDecodeError{.Code = EImageDecodeError::FileRead, .EncodedBytes = FileSize,
				.Limits = Limits, .Filename = std::string(FilePath),
				.FileError = std::move(EncodedBytes.error())});
		}
		auto Result = DecodeImageFromMemory(*EncodedBytes, Limits);
		if (!Result) Result.error().Filename = FilePath;
		return Result;
	}

	auto ToString(const FGrayscale16DecodeError& Error) -> std::string
	{
		if (Error.FileError) return Error.FileError->ToString();
		std::string_view Reason;
		switch (Error.Code)
		{
		case EGrayscale16DecodeError::EncodedLimit: Reason = "The encoded grayscale16 PNG exceeds the configured limit."; break;
		case EGrayscale16DecodeError::InvalidSignature: Reason = "The grayscale16 source is not a complete PNG header."; break;
		case EGrayscale16DecodeError::InvalidHeader: Reason = "The grayscale16 PNG IHDR is missing or invalid."; break;
		case EGrayscale16DecodeError::UnsupportedSampleFormat: Reason = "Grayscale16 decoding requires PNG color type 0 with exactly 16 bits per sample."; break;
		case EGrayscale16DecodeError::UnsupportedEncoding: Reason = "Grayscale16 decoding requires standard PNG compression/filtering and non-interlaced rows."; break;
		case EGrayscale16DecodeError::PixelLimit: Reason = "The decoded grayscale16 dimensions exceed the configured limit."; break;
		case EGrayscale16DecodeError::InvalidImage: Reason = "The grayscale16 PNG is malformed or could not be decoded losslessly."; break;
		case EGrayscale16DecodeError::FileStat: Reason = "Image file metadata is unavailable."; break;
		case EGrayscale16DecodeError::FileSize: Reason = "Image file is empty or exceeds the configured limit."; break;
		case EGrayscale16DecodeError::FileRead: Reason = "Image file could not be read."; break;
		}
		return Error.Filename.empty() ? std::string(Reason) : std::format("{}: {}", Error.Filename, Reason);
	}

	auto ToString(const FRadianceHDRDecodeError& Error) -> std::string
	{
		if (Error.FileError) return Error.FileError->ToString();
		std::string_view Reason;
		switch (Error.Code)
		{
		case ERadianceHDRDecodeError::TruncatedScanline: Reason = "Radiance HDR scanline payload is truncated."; break;
		case ERadianceHDRDecodeError::ZeroLengthPacket: Reason = "Radiance HDR scanline contains a zero-length packet."; break;
		case ERadianceHDRDecodeError::RunExceedsWidth: Reason = "Radiance HDR scanline run exceeds its declared width."; break;
		case ERadianceHDRDecodeError::TruncatedRun: Reason = "Radiance HDR scanline run payload is truncated."; break;
		case ERadianceHDRDecodeError::LiteralExceedsWidth: Reason = "Radiance HDR scanline literal exceeds its declared width."; break;
		case ERadianceHDRDecodeError::TruncatedLiteral: Reason = "Radiance HDR scanline literal payload is truncated."; break;
		case ERadianceHDRDecodeError::InvalidRepeat: Reason = "Radiance HDR old scanline contains an invalid repeat packet."; break;
		case ERadianceHDRDecodeError::RepeatExceedsWidth: Reason = "Radiance HDR old scanline repeat exceeds its declared width."; break;
		case ERadianceHDRDecodeError::TruncatedOldScanline: Reason = "Radiance HDR old scanline payload is truncated."; break;
		case ERadianceHDRDecodeError::Empty: Reason = "Radiance HDR data is empty."; break;
		case ERadianceHDRDecodeError::EncodedLimit: Reason = "Radiance HDR encoded data exceeds the configured limit."; break;
		case ERadianceHDRDecodeError::InvalidSignature: Reason = "Radiance HDR signature is missing or invalid."; break;
		case ERadianceHDRDecodeError::InvalidHeader: Reason = "Radiance HDR header is truncated or too large."; break;
		case ERadianceHDRDecodeError::MissingFormat: Reason = "Radiance HDR FORMAT=32-bit_rle_rgbe declaration is missing."; break;
		case ERadianceHDRDecodeError::MissingResolution: Reason = "Radiance HDR resolution line is missing."; break;
		case ERadianceHDRDecodeError::UnsupportedOrientation: Reason = "Radiance HDR resolution must use nonzero '-Y height +X width' orientation."; break;
		case ERadianceHDRDecodeError::DimensionLimit: Reason = "Radiance HDR dimensions exceed the configured limit."; break;
		case ERadianceHDRDecodeError::PixelLimit: Reason = "Radiance HDR decoded pixels exceed the configured limit."; break;
		case ERadianceHDRDecodeError::TruncatedScanlineHeader: Reason = "Radiance HDR scanline header is truncated."; break;
		case ERadianceHDRDecodeError::ScanlineWidthMismatch: Reason = "Radiance HDR scanline width does not match the resolution."; break;
		case ERadianceHDRDecodeError::InvalidChannel: Reason = "Radiance HDR decoded a negative or nonfinite channel."; break;
		case ERadianceHDRDecodeError::TrailingBytes: Reason = "Radiance HDR payload contains trailing bytes."; break;
		case ERadianceHDRDecodeError::FileStat: Reason = "Image file metadata is unavailable."; break;
		case ERadianceHDRDecodeError::FileSize: Reason = "Image file is empty or exceeds the configured limit."; break;
		case ERadianceHDRDecodeError::FileRead: Reason = "Image file could not be read."; break;
		}
		return Error.Filename.empty() ? std::string(Reason) : std::format("{}: {}", Error.Filename, Reason);
	}

	auto DecodeGrayscale16PngFromMemory(
		FByteView EncodedBytes,
		const FImageDecodeLimits& Limits) -> std::expected<FDecodedGrayscale16Image, FGrayscale16DecodeError>
	{
		uint32 Width = 0, Height = 0;
		auto Fail = [&](EGrayscale16DecodeError Code) {
			return std::unexpected(FGrayscale16DecodeError{.Code = Code,
				.EncodedBytes = EncodedBytes.size(), .Width = Width, .Height = Height, .Limits = Limits});
		};
		constexpr std::array<std::byte, 8> Signature{
			std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
			std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
		if (EncodedBytes.size() > Limits.MaximumEncodedBytes
			|| EncodedBytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		{
			return Fail(EGrayscale16DecodeError::EncodedLimit);
		}
		if (EncodedBytes.size() < 33
			|| !std::equal(Signature.begin(), Signature.end(), EncodedBytes.begin()))
		{
			return Fail(EGrayscale16DecodeError::InvalidSignature);
		}
		uint32 IhdrSize = 0;
		if (!ReadPngU32(EncodedBytes, 8, IhdrSize) || IhdrSize != 13
			|| std::memcmp(EncodedBytes.data() + 12, "IHDR", 4) != 0
			|| !ReadPngU32(EncodedBytes, 16, Width)
			|| !ReadPngU32(EncodedBytes, 20, Height))
		{
			return Fail(EGrayscale16DecodeError::InvalidHeader);
		}
		if (EncodedBytes[24] != std::byte{16} || EncodedBytes[25] != std::byte{0})
		{
			return Fail(EGrayscale16DecodeError::UnsupportedSampleFormat);
		}
		if (EncodedBytes[26] != std::byte{0} || EncodedBytes[27] != std::byte{0} || EncodedBytes[28] != std::byte{0})
		{
			return Fail(EGrayscale16DecodeError::UnsupportedEncoding);
		}
		const uint64 PixelCount = static_cast<uint64>(Width) * Height;
		if (Width == 0 || Height == 0 || PixelCount > Limits.MaximumDecodedPixels
			|| PixelCount > std::numeric_limits<size_t>::max() / sizeof(uint16))
		{
			return Fail(EGrayscale16DecodeError::PixelLimit);
		}

		int DecodedWidth = 0;
		int DecodedHeight = 0;
		int Channels = 0;
		stbi_us* Decoded = stbi_load_16_from_memory(
			reinterpret_cast<const stbi_uc*>(EncodedBytes.data()), static_cast<int>(EncodedBytes.size()),
			&DecodedWidth, &DecodedHeight, &Channels, 0);
		if (!Decoded || DecodedWidth != static_cast<int>(Width)
			|| DecodedHeight != static_cast<int>(Height) || Channels != 1)
		{
			if (Decoded) stbi_image_free(Decoded);
			return Fail(EGrayscale16DecodeError::InvalidImage);
		}
		FDecodedGrayscale16Image Candidate;
		Candidate.Width = Width;
		Candidate.Height = Height;
		Candidate.Samples.assign(Decoded, Decoded + static_cast<size_t>(PixelCount));
		stbi_image_free(Decoded);
		return Candidate;
	}

	auto DecodeGrayscale16PngFromFile(std::string_view FilePath, const FImageDecodeLimits& Limits)
		-> std::expected<FDecodedGrayscale16Image, FGrayscale16DecodeError>
	{
		std::error_code NativeError;
		const auto FileSize = std::filesystem::file_size(FFilePath(FilePath), NativeError);
		if (NativeError) return std::unexpected(FGrayscale16DecodeError{.Code = EGrayscale16DecodeError::FileStat,
			.Limits = Limits, .Filename = std::string(FilePath),
			.FileError = FFileError{.Operation = EFileOperation::QuerySize,
				.NativeError = NativeError, .Path = FFilePath(FilePath)}});
		if (FileSize == 0 || FileSize > Limits.MaximumEncodedBytes
			|| FileSize > std::numeric_limits<size_t>::max())
			return std::unexpected(FGrayscale16DecodeError{.Code = EGrayscale16DecodeError::FileSize,
				.EncodedBytes = FileSize, .Limits = Limits, .Filename = std::string(FilePath)});
		auto Bytes = FFileHelper::LoadFileToArray(FFilePath(FilePath));
		if (!Bytes) return std::unexpected(FGrayscale16DecodeError{.Code = EGrayscale16DecodeError::FileRead,
			.EncodedBytes = FileSize, .Limits = Limits, .Filename = std::string(FilePath), .FileError = Bytes.error()});
		auto Result = DecodeGrayscale16PngFromMemory(*Bytes, Limits);
		if (!Result) Result.error().Filename = FilePath;
		return Result;
	}

	auto DecodeRadianceHDRFromMemory(FByteView EncodedBytes, const FRadianceHDRDecodeLimits& Limits)
		-> std::expected<FDecodedFloatImage, FRadianceHDRDecodeError>
	{
		size_t Offset = 0;
		uint32 Width = 0, Height = 0;
		auto Fail = [&](ERadianceHDRDecodeError Code) {
			return std::unexpected(FRadianceHDRDecodeError{.Code = Code,
				.EncodedBytes = EncodedBytes.size(), .Offset = Offset,
				.Width = Width, .Height = Height, .Limits = Limits});
		};
		if (EncodedBytes.empty())
		{
			return Fail(ERadianceHDRDecodeError::Empty);
		}
		if (EncodedBytes.size() > Limits.MaximumEncodedBytes)
		{
			return Fail(ERadianceHDRDecodeError::EncodedLimit);
		}

		std::string_view Line;
		if (!ReadRadianceLine(EncodedBytes, Offset, Line) || (Line != "#?RADIANCE" && Line != "#?RGBE"))
		{
			return Fail(ERadianceHDRDecodeError::InvalidSignature);
		}

		bool bFoundFormat = false;
		while (true)
		{
			if (Offset > MaximumRadianceHeaderBytes || !ReadRadianceLine(EncodedBytes, Offset, Line))
			{
				return Fail(ERadianceHDRDecodeError::InvalidHeader);
			}
			if (Line.empty()) break;
			if (Line == "FORMAT=32-bit_rle_rgbe") bFoundFormat = true;
		}
		if (!bFoundFormat)
		{
			return Fail(ERadianceHDRDecodeError::MissingFormat);
		}
		if (!ReadRadianceLine(EncodedBytes, Offset, Line))
		{
			return Fail(ERadianceHDRDecodeError::MissingResolution);
		}

		if (!ParseRadianceResolution(Line, Width, Height))
		{
			return Fail(ERadianceHDRDecodeError::UnsupportedOrientation);
		}
		if (Width > Limits.MaximumDimension || Height > Limits.MaximumDimension)
		{
			return Fail(ERadianceHDRDecodeError::DimensionLimit);
		}
		const uint64 PixelCount = static_cast<uint64>(Width) * Height;
		if (PixelCount > Limits.MaximumDecodedPixels
			|| PixelCount > std::numeric_limits<size_t>::max() / HDRChannelCount / sizeof(float))
		{
			return Fail(ERadianceHDRDecodeError::PixelLimit);
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
				return Fail(ERadianceHDRDecodeError::TruncatedScanlineHeader);
			}
			std::array<uint8, 4> Header;
			for (size_t Index = 0; Index < Header.size(); ++Index)
				Header[Index] = ToUint8(EncodedBytes[Offset + Index]);
			Offset += 4;

			const bool bNewEncoding = Width >= 8 && Width <= 32767
				&& Header[0] == 2 && Header[1] == 2 && (Header[2] & 0x80) == 0;
			if (bNewEncoding)
			{
				const uint32 ScanlineWidth = (static_cast<uint32>(Header[2]) << 8) | Header[3];
				if (ScanlineWidth != Width)
				{
					return Fail(ERadianceHDRDecodeError::ScanlineWidthMismatch);
				}
				if (auto Result = DecodeRadianceNewScanline(EncodedBytes, Offset, Width, Scanline); !Result)
					return Fail(Result.error());
			}
			else if (auto Result = DecodeRadianceOldScanline(EncodedBytes, Offset, Width, Header, Scanline); !Result)
				return Fail(Result.error());

			for (uint32 X = 0; X < Width; ++X)
			{
				float* Pixel = Decoded.Pixels.data() + (static_cast<size_t>(Y) * Width + X) * HDRChannelCount;
				if (!DecodeRGBE(Scanline[X], Pixel))
				{
					return Fail(ERadianceHDRDecodeError::InvalidChannel);
				}
			}
		}
		if (Offset != EncodedBytes.size())
		{
			return Fail(ERadianceHDRDecodeError::TrailingBytes);
		}
		return Decoded;
	}

	auto DecodeRadianceHDRFromFile(std::string_view FilePath, const FRadianceHDRDecodeLimits& Limits)
		-> std::expected<FDecodedFloatImage, FRadianceHDRDecodeError>
	{
		std::error_code NativeError;
		const auto FileSize = std::filesystem::file_size(FFilePath(FilePath), NativeError);
		if (NativeError) return std::unexpected(FRadianceHDRDecodeError{.Code = ERadianceHDRDecodeError::FileStat,
			.Limits = Limits, .Filename = std::string(FilePath),
			.FileError = FFileError{.Operation = EFileOperation::QuerySize,
				.NativeError = NativeError, .Path = FFilePath(FilePath)}});
		if (FileSize == 0 || FileSize > Limits.MaximumEncodedBytes
			|| FileSize > std::numeric_limits<size_t>::max())
			return std::unexpected(FRadianceHDRDecodeError{.Code = ERadianceHDRDecodeError::FileSize,
				.EncodedBytes = FileSize, .Limits = Limits, .Filename = std::string(FilePath)});
		auto Bytes = FFileHelper::LoadFileToArray(FFilePath(FilePath));
		if (!Bytes) return std::unexpected(FRadianceHDRDecodeError{.Code = ERadianceHDRDecodeError::FileRead,
			.EncodedBytes = FileSize, .Limits = Limits, .Filename = std::string(FilePath), .FileError = Bytes.error()});
		auto Result = DecodeRadianceHDRFromMemory(*Bytes, Limits);
		if (!Result) Result.error().Filename = FilePath;
		return Result;
	}

} // namespace Durin::Image
