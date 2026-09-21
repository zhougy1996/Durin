#include "Serialization/BinaryEnvelope.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::byte, 4> Magic{
			std::byte{'D'}, std::byte{'U'}, std::byte{'R'}, std::byte{'F'}};
		constexpr uint64 HeaderHashOffset = 48;
		constexpr uint64 HeaderHashBytes = 16;

		template<typename T>
		auto ReadLittleEndian(FByteView Bytes, size_t Offset, T& OutValue) -> bool
		{
			if (Offset > Bytes.size() || sizeof(T) > Bytes.size() - Offset) return false;
			T Value = 0;
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Value |= static_cast<T>(std::to_integer<uint8>(Bytes[Offset + Index])) << (Index * 8);
			OutValue = Value;
			return true;
		}

		template<typename T>
		auto WriteLittleEndian(FMutableByteView Bytes, size_t Offset, T Value) -> void
		{
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Bytes[Offset + Index] = static_cast<std::byte>((Value >> (Index * 8)) & 0xff);
		}

		auto ValidateLimits(const FBinaryEnvelopeLimits& Limits) -> std::expected<void, EBinaryEnvelopeError>
		{
			if (Limits.MaximumHeaderBytes < BinaryEnvelopePreambleBytes
				|| Limits.MaximumFileBytes < BinaryEnvelopePreambleBytes
				|| Limits.MaximumHeaderBytes > Limits.MaximumFileBytes)
				return std::unexpected(EBinaryEnvelopeError::InvalidLimits);
			return {};
		}

		auto ValidatePreambleValues(const FBinaryEnvelopePreamble& Preamble) -> std::expected<void, EBinaryEnvelopeError>
		{
			if (!Preamble.FormatId.IsValid())
				return std::unexpected(EBinaryEnvelopeError::InvalidFormatIdentity);
			if (Preamble.FormatVersion == 0)
				return std::unexpected(EBinaryEnvelopeError::UnsupportedFormatVersion);
			if (Preamble.HeaderBytes < BinaryEnvelopePreambleBytes
				|| Preamble.HeaderBytes > Preamble.FileBytes)
				return std::unexpected(EBinaryEnvelopeError::InvalidExtent);
			return {};
		}

		auto HashHeaderWithZeroedField(FByteView Header) -> FXxHash128
		{
			FXxHash128Builder Builder;
			Builder.Update(Header.first(HeaderHashOffset));
			constexpr std::array<std::byte, HeaderHashBytes> Zeros{};
			Builder.Update(Zeros);
			Builder.Update(Header.subspan(HeaderHashOffset + HeaderHashBytes));
			return Builder.Finalize();
		}
	}

	auto FBinaryFormatRegistry::Create(
		std::span<const FBinaryFormatDescriptor> InputDescriptors,
		FBinaryFormatRegistry& OutRegistry) -> std::expected<void, EBinaryEnvelopeError>
	{
		std::vector<FBinaryFormatDescriptor> Validated;
		Validated.reserve(InputDescriptors.size());
		for (const FBinaryFormatDescriptor& Descriptor : InputDescriptors)
		{
			if (!Descriptor.FormatId.IsValid() || Descriptor.DebugName.empty()
				|| Descriptor.MinimumFormatVersion == 0
				|| Descriptor.MinimumFormatVersion > Descriptor.MaximumFormatVersion
				|| !ValidateLimits(Descriptor.Limits))
				return std::unexpected(EBinaryEnvelopeError::InvalidDescriptor);
			if (std::ranges::any_of(Validated, [&](const FBinaryFormatDescriptor& Existing) {
				return Existing.FormatId == Descriptor.FormatId;
			}))
				return std::unexpected(EBinaryEnvelopeError::DuplicateFormatIdentity);
			if (std::ranges::any_of(Validated, [&](const FBinaryFormatDescriptor& Existing) {
				return Existing.DebugName == Descriptor.DebugName;
			}))
				return std::unexpected(EBinaryEnvelopeError::DuplicateFormatName);
			Validated.push_back(Descriptor);
		}
		FBinaryFormatRegistry Result;
		Result.Descriptors = std::move(Validated);
		OutRegistry = std::move(Result);
		return {};
	}

	auto FBinaryFormatRegistry::Find(const FGuid& FormatId) const
		-> const FBinaryFormatDescriptor*
	{
		const auto It = std::ranges::find(Descriptors, FormatId, &FBinaryFormatDescriptor::FormatId);
		return It == Descriptors.end() ? nullptr : &*It;
	}

	auto ParseBinaryEnvelopePrefix(
		FByteView PrefixBytes,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& Limits,
		FBinaryEnvelopePreamble& OutPreamble) -> std::expected<void, EBinaryEnvelopeError>
	{
		if (auto Result = ValidateLimits(Limits); !Result) return Result;
		if (PrefixBytes.size() < BinaryEnvelopePreambleBytes)
			return std::unexpected(EBinaryEnvelopeError::Truncated);
		if (!std::ranges::equal(Magic, PrefixBytes.first(Magic.size())))
			return std::unexpected(EBinaryEnvelopeError::InvalidMagic);

		uint16 HeaderVersion = 0;
		uint16 PreambleByteCount = 0;
		FBinaryEnvelopePreamble Parsed;
		if (!ReadLittleEndian(PrefixBytes, 4, HeaderVersion)
			|| !ReadLittleEndian(PrefixBytes, 6, PreambleByteCount)
			|| !ReadLittleEndian(PrefixBytes, 8, Parsed.FormatId.A)
			|| !ReadLittleEndian(PrefixBytes, 12, Parsed.FormatId.B)
			|| !ReadLittleEndian(PrefixBytes, 16, Parsed.FormatId.C)
			|| !ReadLittleEndian(PrefixBytes, 20, Parsed.FormatId.D)
			|| !ReadLittleEndian(PrefixBytes, 24, Parsed.FormatVersion)
			|| !ReadLittleEndian(PrefixBytes, 28, Parsed.RequiredFeatures)
			|| !ReadLittleEndian(PrefixBytes, 32, Parsed.HeaderBytes)
			|| !ReadLittleEndian(PrefixBytes, 40, Parsed.FileBytes)
			|| !ReadLittleEndian(PrefixBytes, 48, Parsed.HeaderHash.HashLow)
			|| !ReadLittleEndian(PrefixBytes, 56, Parsed.HeaderHash.HashHigh))
			return std::unexpected(EBinaryEnvelopeError::Truncated);
		if (HeaderVersion != BinaryEnvelopeHeaderVersion)
			return std::unexpected(EBinaryEnvelopeError::UnsupportedHeaderVersion);
		if (PreambleByteCount != BinaryEnvelopePreambleBytes)
			return std::unexpected(EBinaryEnvelopeError::InvalidPreambleSize);
		if (auto Result = ValidatePreambleValues(Parsed); !Result) return Result;
		if (Parsed.HeaderBytes > Limits.MaximumHeaderBytes || Parsed.FileBytes > Limits.MaximumFileBytes)
			return std::unexpected(EBinaryEnvelopeError::InvalidExtent);
		if (Parsed.FileBytes != PhysicalFileBytes)
			return std::unexpected(EBinaryEnvelopeError::FileSizeMismatch);
		OutPreamble = Parsed;
		return {};
	}

	auto EncodeBinaryEnvelopePreamble(
		const FBinaryEnvelopePreamble& Preamble,
		FMutableByteView Destination) -> std::expected<void, EBinaryEnvelopeError>
	{
		if (Destination.size() < BinaryEnvelopePreambleBytes)
			return std::unexpected(EBinaryEnvelopeError::DestinationTooSmall);
		if (auto Result = ValidatePreambleValues(Preamble); !Result) return Result;

		std::array<std::byte, BinaryEnvelopePreambleBytes> Encoded{};
		std::ranges::copy(Magic, Encoded.begin());
		WriteLittleEndian(std::span(Encoded), 4, BinaryEnvelopeHeaderVersion);
		WriteLittleEndian(std::span(Encoded), 6, BinaryEnvelopePreambleBytes);
		WriteLittleEndian(std::span(Encoded), 8, Preamble.FormatId.A);
		WriteLittleEndian(std::span(Encoded), 12, Preamble.FormatId.B);
		WriteLittleEndian(std::span(Encoded), 16, Preamble.FormatId.C);
		WriteLittleEndian(std::span(Encoded), 20, Preamble.FormatId.D);
		WriteLittleEndian(std::span(Encoded), 24, Preamble.FormatVersion);
		WriteLittleEndian(std::span(Encoded), 28, Preamble.RequiredFeatures);
		WriteLittleEndian(std::span(Encoded), 32, Preamble.HeaderBytes);
		WriteLittleEndian(std::span(Encoded), 40, Preamble.FileBytes);
		WriteLittleEndian(std::span(Encoded), 48, Preamble.HeaderHash.HashLow);
		WriteLittleEndian(std::span(Encoded), 56, Preamble.HeaderHash.HashHigh);
		std::ranges::copy(Encoded, Destination.begin());
		return {};
	}

	auto ValidateBinaryEnvelopeHeader(
		FByteView FrontMatter,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& DiscoveryLimits,
		const FBinaryFormatRegistry& Registry,
		FValidatedBinaryEnvelope& OutEnvelope) -> std::expected<void, EBinaryEnvelopeError>
	{
		FBinaryEnvelopePreamble Preamble;
		if (auto Result = ParseBinaryEnvelopePrefix(
			FrontMatter, PhysicalFileBytes, DiscoveryLimits, Preamble); !Result) return Result;
		if (Preamble.HeaderBytes != FrontMatter.size())
			return std::unexpected(Preamble.HeaderBytes > FrontMatter.size()
					? EBinaryEnvelopeError::Truncated : EBinaryEnvelopeError::InvalidExtent);
		const FBinaryFormatDescriptor* Descriptor = Registry.Find(Preamble.FormatId);
		if (!Descriptor)
			return std::unexpected(EBinaryEnvelopeError::UnknownFormat);
		if (Preamble.FormatVersion < Descriptor->MinimumFormatVersion
			|| Preamble.FormatVersion > Descriptor->MaximumFormatVersion)
			return std::unexpected(EBinaryEnvelopeError::UnsupportedFormatVersion);
		if ((Preamble.RequiredFeatures & ~Descriptor->SupportedRequiredFeatures) != 0)
			return std::unexpected(EBinaryEnvelopeError::UnsupportedRequiredFeatures);
		if (Preamble.HeaderBytes > Descriptor->Limits.MaximumHeaderBytes
			|| Preamble.FileBytes > Descriptor->Limits.MaximumFileBytes)
			return std::unexpected(EBinaryEnvelopeError::InvalidExtent);
		if (HashHeaderWithZeroedField(FrontMatter) != Preamble.HeaderHash)
			return std::unexpected(EBinaryEnvelopeError::HeaderHashMismatch);

		FValidatedBinaryEnvelope Validated{
			.Preamble = Preamble,
			.Descriptor = Descriptor,
			.HeaderBytes = FrontMatter,
			.FormatHeaderBytes = FrontMatter.subspan(BinaryEnvelopePreambleBytes)};
		OutEnvelope = Validated;
		return {};
	}

	auto FinalizeBinaryEnvelopeHeader(
		FMutableByteView FrontMatter,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& Limits) -> std::expected<void, EBinaryEnvelopeError>
	{
		FBinaryEnvelopePreamble Preamble;
		const FByteView ReadOnly(FrontMatter);
		if (auto Result = ParseBinaryEnvelopePrefix(ReadOnly, PhysicalFileBytes, Limits, Preamble); !Result) return Result;
		if (Preamble.HeaderBytes != FrontMatter.size())
			return std::unexpected(Preamble.HeaderBytes > FrontMatter.size()
					? EBinaryEnvelopeError::Truncated : EBinaryEnvelopeError::InvalidExtent);
		const FXxHash128 Hash = HashHeaderWithZeroedField(ReadOnly);
		WriteLittleEndian(FrontMatter, HeaderHashOffset, Hash.HashLow);
		WriteLittleEndian(FrontMatter, HeaderHashOffset + sizeof(uint64), Hash.HashHigh);
		return {};
	}

	auto ToString(EBinaryEnvelopeError Error) -> std::string_view
	{
		switch (Error)
		{
		case EBinaryEnvelopeError::None: return {};
		case EBinaryEnvelopeError::InvalidLimits: return "BinaryEnvelopeInvalidLimits: limits must bound a complete preamble and file.";
		case EBinaryEnvelopeError::InvalidFormatIdentity: return "BinaryEnvelopeInvalidFormatIdentity: FormatId must be nonzero.";
		case EBinaryEnvelopeError::UnsupportedFormatVersion: return "BinaryEnvelopeUnsupportedFormatVersion: format version is not supported.";
		case EBinaryEnvelopeError::InvalidExtent: return "BinaryEnvelopeInvalidExtent: declared extents are inconsistent or exceed limits.";
		case EBinaryEnvelopeError::InvalidDescriptor: return "BinaryEnvelopeInvalidDescriptor: descriptor fields or limits are invalid.";
		case EBinaryEnvelopeError::DuplicateFormatIdentity: return "BinaryEnvelopeDuplicateFormatIdentity: FormatId values must be unique.";
		case EBinaryEnvelopeError::DuplicateFormatName: return "BinaryEnvelopeDuplicateFormatName: debug names must be unique.";
		case EBinaryEnvelopeError::Truncated: return "BinaryEnvelopeTruncated: the 64-byte preamble is incomplete.";
		case EBinaryEnvelopeError::InvalidMagic: return "BinaryEnvelopeInvalidMagic: expected DURF.";
		case EBinaryEnvelopeError::UnsupportedHeaderVersion: return "BinaryEnvelopeUnsupportedHeaderVersion: HeaderVersion is not supported.";
		case EBinaryEnvelopeError::InvalidPreambleSize: return "BinaryEnvelopeInvalidPreambleSize: PreambleBytes must equal 64.";
		case EBinaryEnvelopeError::FileSizeMismatch: return "BinaryEnvelopeFileSizeMismatch: FileBytes must equal the physical file size.";
		case EBinaryEnvelopeError::DestinationTooSmall: return "BinaryEnvelopeDestinationTooSmall: destination cannot hold the preamble.";
		case EBinaryEnvelopeError::UnknownFormat: return "BinaryEnvelopeUnknownFormat: FormatId is not registered.";
		case EBinaryEnvelopeError::UnsupportedRequiredFeatures: return "BinaryEnvelopeUnsupportedRequiredFeatures: required feature bits are not supported.";
		case EBinaryEnvelopeError::HeaderHashMismatch: return "BinaryEnvelopeHeaderHashMismatch: front matter integrity check failed.";
		}
		return "Unknown binary envelope error.";
	}
}
