#pragma once

#include "CoreAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"

namespace Durin
{
	inline constexpr uint16 BinaryEnvelopeHeaderVersion = 1;
	inline constexpr uint16 BinaryEnvelopePreambleBytes = 64;

	// Classifies common-envelope failures before format-specific dispatch.
	enum class EBinaryEnvelopeError : uint8
	{
		None,
		Truncated,
		InvalidMagic,
		UnsupportedHeaderVersion,
		InvalidPreambleSize,
		InvalidFormatIdentity,
		UnknownFormat,
		UnsupportedFormatVersion,
		UnsupportedRequiredFeatures,
		InvalidLimits,
		InvalidExtent,
		FileSizeMismatch,
		HeaderHashMismatch,
		InvalidDescriptor,
		DuplicateFormatIdentity,
		DuplicateFormatName,
		DestinationTooSmall
	};

	// Carries a stable failure category and diagnostic text without owning input bytes.
	struct FBinaryEnvelopeDiagnostic
	{
		EBinaryEnvelopeError Error = EBinaryEnvelopeError::None;
		std::string_view Message;
	};

	// Bounds common front-matter discovery before any declared extent is trusted.
	struct FBinaryEnvelopeLimits
	{
		uint64 MaximumHeaderBytes = 0;
		uint64 MaximumFileBytes = 0;
	};

	// Represents the explicitly encoded DURF v1 common preamble.
	struct FBinaryEnvelopePreamble
	{
		FGuid FormatId;
		uint32 FormatVersion = 0;
		uint32 RequiredFeatures = 0;
		uint64 HeaderBytes = 0;
		uint64 FileBytes = 0;
		FXxHash128 HeaderHash;
	};

	// Describes one format's common-envelope compatibility and allocation policy.
	struct FBinaryFormatDescriptor
	{
		FGuid FormatId;
		std::string DebugName;
		uint32 MinimumFormatVersion = 0;
		uint32 MaximumFormatVersion = 0;
		uint32 SupportedRequiredFeatures = 0;
		FBinaryEnvelopeLimits Limits;
	};

	// Owns a validated descriptor set that is immutable and thread-safe after creation.
	class FBinaryFormatRegistry
	{
	public:
		// Replaces OutRegistry only when every descriptor and cross-descriptor identity is valid.
		CORE_API static auto Create(
			std::span<const FBinaryFormatDescriptor> Descriptors,
			FBinaryFormatRegistry& OutRegistry,
			FBinaryEnvelopeDiagnostic* OutDiagnostic = nullptr) -> bool;

		[[nodiscard]] CORE_API auto Find(const FGuid& FormatId) const
			-> const FBinaryFormatDescriptor*;
		[[nodiscard]] auto GetDescriptors() const
			-> std::span<const FBinaryFormatDescriptor> { return Descriptors; }

	private:
		std::vector<FBinaryFormatDescriptor> Descriptors;
	};

	// Returns bounded prefix values without reading or interpreting format-owned bytes.
	CORE_API auto ParseBinaryEnvelopePrefix(
		FByteView PrefixBytes,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& Limits,
		FBinaryEnvelopePreamble& OutPreamble,
		FBinaryEnvelopeDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Publishes an encoded preamble only after all values and destination bounds validate.
	CORE_API auto EncodeBinaryEnvelopePreamble(
		const FBinaryEnvelopePreamble& Preamble,
		FMutableByteView Destination,
		FBinaryEnvelopeDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Holds validated common values and non-owning views into the caller-owned front matter.
	struct FValidatedBinaryEnvelope
	{
		FBinaryEnvelopePreamble Preamble;
		const FBinaryFormatDescriptor* Descriptor = nullptr;
		FByteView HeaderBytes;
		FByteView FormatHeaderBytes;
	};

	// Validates exact bounded front matter, explicit registry policy, and the zeroed-field hash.
	CORE_API auto ValidateBinaryEnvelopeHeader(
		FByteView FrontMatter,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& DiscoveryLimits,
		const FBinaryFormatRegistry& Registry,
		FValidatedBinaryEnvelope& OutEnvelope,
		FBinaryEnvelopeDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Writes only the hash field after the complete declared front matter validates.
	CORE_API auto FinalizeBinaryEnvelopeHeader(
		FMutableByteView FrontMatter,
		uint64 PhysicalFileBytes,
		const FBinaryEnvelopeLimits& Limits,
		FBinaryEnvelopeDiagnostic* OutDiagnostic = nullptr) -> bool;
}
