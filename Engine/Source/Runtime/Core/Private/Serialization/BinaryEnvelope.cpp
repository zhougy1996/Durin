#include "Serialization/BinaryEnvelope.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::byte, 4> Magic{
			std::byte{'D'}, std::byte{'U'}, std::byte{'R'}, std::byte{'F'}};
		constexpr uint64 HeaderHashOffset = 48;
		constexpr uint64 HeaderHashBytes = 16;

		auto Fail(EBinaryEnvelopeError Error, std::string_view Message,
			FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
		{
			if (OutDiagnostic) *OutDiagnostic = {.Error = Error, .Message = Message};
			return false;
		}

		auto Succeed(FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
		{
			if (OutDiagnostic) *OutDiagnostic = {};
			return true;
		}

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

		auto ValidateLimits(const FBinaryEnvelopeLimits& Limits,
			FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
		{
			if (Limits.MaximumHeaderBytes < BinaryEnvelopePreambleBytes
				|| Limits.MaximumFileBytes < BinaryEnvelopePreambleBytes
				|| Limits.MaximumHeaderBytes > Limits.MaximumFileBytes)
				return Fail(EBinaryEnvelopeError::InvalidLimits,
					"BinaryEnvelopeInvalidLimits: limits must bound a complete preamble and file.",
					OutDiagnostic);
			return true;
		}

		auto ValidatePreambleValues(const FBinaryEnvelopePreamble& Preamble,
			FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
		{
			if (!Preamble.FormatId.IsValid())
				return Fail(EBinaryEnvelopeError::InvalidFormatIdentity,
					"BinaryEnvelopeInvalidFormatIdentity: FormatId must be nonzero.", OutDiagnostic);
			if (Preamble.FormatVersion == 0)
				return Fail(EBinaryEnvelopeError::UnsupportedFormatVersion,
					"BinaryEnvelopeUnsupportedFormatVersion: FormatVersion must be nonzero.", OutDiagnostic);
			if (Preamble.HeaderBytes < BinaryEnvelopePreambleBytes
				|| Preamble.HeaderBytes > Preamble.FileBytes)
				return Fail(EBinaryEnvelopeError::InvalidExtent,
					"BinaryEnvelopeInvalidExtent: declared extents are inconsistent.", OutDiagnostic);
			return true;
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
		FBinaryFormatRegistry& OutRegistry,
		FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
	{
		std::vector<FBinaryFormatDescriptor> Validated;
		Validated.reserve(InputDescriptors.size());
		for (const FBinaryFormatDescriptor& Descriptor : InputDescriptors)
		{
			if (!Descriptor.FormatId.IsValid() || Descriptor.DebugName.empty()
				|| Descriptor.MinimumFormatVersion == 0
				|| Descriptor.MinimumFormatVersion > Descriptor.MaximumFormatVersion
				|| !ValidateLimits(Descriptor.Limits, nullptr))
				return Fail(EBinaryEnvelopeError::InvalidDescriptor,
					"BinaryEnvelopeInvalidDescriptor: descriptor fields or limits are invalid.",
					OutDiagnostic);
			if (std::ranges::any_of(Validated, [&](const FBinaryFormatDescriptor& Existing) {
				return Existing.FormatId == Descriptor.FormatId;
			}))
				return Fail(EBinaryEnvelopeError::DuplicateFormatIdentity,
					"BinaryEnvelopeDuplicateFormatIdentity: FormatId values must be unique.",
					OutDiagnostic);
			if (std::ranges::any_of(Validated, [&](const FBinaryFormatDescriptor& Existing) {
				return Existing.DebugName == Descriptor.DebugName;
			}))
				return Fail(EBinaryEnvelopeError::DuplicateFormatName,
					"BinaryEnvelopeDuplicateFormatName: debug names must be unique.", OutDiagnostic);
			Validated.push_back(Descriptor);
		}
		FBinaryFormatRegistry Result;
		Result.Descriptors = std::move(Validated);
		OutRegistry = std::move(Result);
		return Succeed(OutDiagnostic);
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
		FBinaryEnvelopePreamble& OutPreamble,
		FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
	{
		if (!ValidateLimits(Limits, OutDiagnostic)) return false;
		if (PrefixBytes.size() < BinaryEnvelopePreambleBytes)
			return Fail(EBinaryEnvelopeError::Truncated,
				"BinaryEnvelopeTruncated: the 64-byte preamble is incomplete.", OutDiagnostic);
		if (!std::ranges::equal(Magic, PrefixBytes.first(Magic.size())))
			return Fail(EBinaryEnvelopeError::InvalidMagic,
				"BinaryEnvelopeInvalidMagic: expected DURF.", OutDiagnostic);

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
			return Fail(EBinaryEnvelopeError::Truncated,
				"BinaryEnvelopeTruncated: the 64-byte preamble is incomplete.", OutDiagnostic);
		if (HeaderVersion != BinaryEnvelopeHeaderVersion)
			return Fail(EBinaryEnvelopeError::UnsupportedHeaderVersion,
				"BinaryEnvelopeUnsupportedHeaderVersion: HeaderVersion is not supported.", OutDiagnostic);
		if (PreambleByteCount != BinaryEnvelopePreambleBytes)
			return Fail(EBinaryEnvelopeError::InvalidPreambleSize,
				"BinaryEnvelopeInvalidPreambleSize: PreambleBytes must equal 64.", OutDiagnostic);
		if (!ValidatePreambleValues(Parsed, OutDiagnostic)) return false;
		if (Parsed.HeaderBytes > Limits.MaximumHeaderBytes || Parsed.FileBytes > Limits.MaximumFileBytes)
			return Fail(EBinaryEnvelopeError::InvalidExtent,
				"BinaryEnvelopeInvalidExtent: declared extents exceed caller limits.", OutDiagnostic);
		if (Parsed.FileBytes != PhysicalFileBytes)
			return Fail(EBinaryEnvelopeError::FileSizeMismatch,
				"BinaryEnvelopeFileSizeMismatch: FileBytes must equal the physical file size.",
				OutDiagnostic);
		OutPreamble = Parsed;
		return Succeed(OutDiagnostic);
	}

	auto EncodeBinaryEnvelopePreamble(
		const FBinaryEnvelopePreamble& Preamble,
		FMutableByteView Destination,
		FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
	{
		if (Destination.size() < BinaryEnvelopePreambleBytes)
			return Fail(EBinaryEnvelopeError::DestinationTooSmall,
				"BinaryEnvelopeDestinationTooSmall: destination cannot hold the preamble.", OutDiagnostic);
		if (!ValidatePreambleValues(Preamble, OutDiagnostic)) return false;

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
		return Succeed(OutDiagnostic);
	}

	auto ValidateBinaryEnvelopeHeader(
		FByteView FrontMatter,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& DiscoveryLimits,
		const FBinaryFormatRegistry& Registry,
		FValidatedBinaryEnvelope& OutEnvelope,
		FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
	{
		FBinaryEnvelopePreamble Preamble;
		if (!ParseBinaryEnvelopePrefix(
			FrontMatter, PhysicalFileBytes, DiscoveryLimits, Preamble, OutDiagnostic)) return false;
		if (Preamble.HeaderBytes != FrontMatter.size())
			return Fail(Preamble.HeaderBytes > FrontMatter.size()
					? EBinaryEnvelopeError::Truncated : EBinaryEnvelopeError::InvalidExtent,
				"BinaryEnvelopeInvalidFrontMatter: input must equal the declared HeaderBytes.",
				OutDiagnostic);
		const FBinaryFormatDescriptor* Descriptor = Registry.Find(Preamble.FormatId);
		if (!Descriptor)
			return Fail(EBinaryEnvelopeError::UnknownFormat,
				"BinaryEnvelopeUnknownFormat: FormatId is not registered.", OutDiagnostic);
		if (Preamble.FormatVersion < Descriptor->MinimumFormatVersion
			|| Preamble.FormatVersion > Descriptor->MaximumFormatVersion)
			return Fail(EBinaryEnvelopeError::UnsupportedFormatVersion,
				"BinaryEnvelopeUnsupportedFormatVersion: format version is not supported.", OutDiagnostic);
		if ((Preamble.RequiredFeatures & ~Descriptor->SupportedRequiredFeatures) != 0)
			return Fail(EBinaryEnvelopeError::UnsupportedRequiredFeatures,
				"BinaryEnvelopeUnsupportedRequiredFeatures: required feature bits are not supported.",
				OutDiagnostic);
		if (Preamble.HeaderBytes > Descriptor->Limits.MaximumHeaderBytes
			|| Preamble.FileBytes > Descriptor->Limits.MaximumFileBytes)
			return Fail(EBinaryEnvelopeError::InvalidExtent,
				"BinaryEnvelopeInvalidExtent: declared extents exceed format limits.", OutDiagnostic);
		if (HashHeaderWithZeroedField(FrontMatter) != Preamble.HeaderHash)
			return Fail(EBinaryEnvelopeError::HeaderHashMismatch,
				"BinaryEnvelopeHeaderHashMismatch: front matter integrity check failed.", OutDiagnostic);

		FValidatedBinaryEnvelope Validated{
			.Preamble = Preamble,
			.Descriptor = Descriptor,
			.HeaderBytes = FrontMatter,
			.FormatHeaderBytes = FrontMatter.subspan(BinaryEnvelopePreambleBytes)};
		OutEnvelope = Validated;
		return Succeed(OutDiagnostic);
	}

	auto FinalizeBinaryEnvelopeHeader(
		FMutableByteView FrontMatter,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& Limits,
		FBinaryEnvelopeDiagnostic* OutDiagnostic) -> bool
	{
		FBinaryEnvelopePreamble Preamble;
		const FByteView ReadOnly(FrontMatter);
		if (!ParseBinaryEnvelopePrefix(ReadOnly, PhysicalFileBytes, Limits, Preamble, OutDiagnostic))
			return false;
		if (Preamble.HeaderBytes != FrontMatter.size())
			return Fail(Preamble.HeaderBytes > FrontMatter.size()
					? EBinaryEnvelopeError::Truncated : EBinaryEnvelopeError::InvalidExtent,
				"BinaryEnvelopeInvalidFrontMatter: input must equal the declared HeaderBytes.",
				OutDiagnostic);
		const FXxHash128 Hash = HashHeaderWithZeroedField(ReadOnly);
		WriteLittleEndian(FrontMatter, HeaderHashOffset, Hash.HashLow);
		WriteLittleEndian(FrontMatter, HeaderHashOffset + sizeof(uint64), Hash.HashHigh);
		return Succeed(OutDiagnostic);
	}
}
