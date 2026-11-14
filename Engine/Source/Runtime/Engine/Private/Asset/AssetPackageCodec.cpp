#include "AssetPackageCodec.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "AssetPackageByteSource.h"

#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageTaggedCodec.h"
#include "Serialization/BinaryEnvelope.h"

namespace Durin::AssetPrivate
{
	namespace
	{
		const auto Codecs = [] {
			const auto& Current = TaggedPackage::GetCodec();
			auto Legacy = Current;
			Legacy.CodecId = "dast-v11";
			Legacy.FormatVersion = ObjectPackage::DastV11FormatVersion;
			Legacy.bCanWrite = false;
			Legacy.Write = nullptr;
			return std::array{Legacy, Current};
		}();
		constexpr FBinaryEnvelopeLimits PackageEnvelopeLimits{
			16ull * 1024ull * 1024ull,
			1024ull * 1024ull * 1024ull};

		auto GetPackageFormatRegistry() -> const FBinaryFormatRegistry&
		{
			static const FBinaryFormatRegistry Registry = [] {
				const std::array Descriptors{FBinaryFormatDescriptor{
					.FormatId = ObjectPackage::DastFormatId,
					.DebugName = std::string(ObjectPackage::DastFormatName),
					.MinimumFormatVersion = ObjectPackage::DastV11FormatVersion,
					.MaximumFormatVersion = ObjectPackage::DastV12FormatVersion,
					.SupportedRequiredFeatures = 0,
					.Limits = PackageEnvelopeLimits}};
				FBinaryFormatRegistry Result;
				const auto bCreated = FBinaryFormatRegistry::Create(Descriptors, Result);
				require(bCreated);
				return Result;
			}();
			return Registry;
		}

		auto EnvelopeError(EBinaryEnvelopeError Error, bool* OutUnsupportedFormat = nullptr) -> std::expected<void, Durin::UnifiedError::FError>
		{
			const bool bUnsupported = Error == EBinaryEnvelopeError::UnknownFormat
				|| Error == EBinaryEnvelopeError::UnsupportedFormatVersion
				|| Error == EBinaryEnvelopeError::UnsupportedRequiredFeatures;
			if (OutUnsupportedFormat) *OutUnsupportedFormat = bUnsupported;
			return std::unexpected(Durin::UnifiedError::FError{bUnsupported ? PackageReadErrors::UnsupportedVersion : PackageReadErrors::CorruptFile, std::string(Durin::ToString(Error))});
		}
		auto ReadAssetPackageFormatVersion(
			FByteView Bytes, uint32& OutFormatVersion,
			uint64 PhysicalFileBytes, bool* OutUnsupportedFormat = nullptr) -> std::expected<void, Durin::UnifiedError::FError>
		{
			OutFormatVersion = 0;
			if (Bytes.size() < sizeof(uint32))
				return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Truncated asset header."});
			uint32 Magic = 0;
			std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
			if (Magic == ObjectPackage::DastPackageMagic)
			{
				if (Bytes.size() < sizeof(uint32) * 2)
					return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Truncated asset header."});
				uint32 LegacyVersion = 0;
				std::memcpy(&LegacyVersion, Bytes.data() + sizeof(Magic), sizeof(LegacyVersion));
				return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::UnsupportedVersion, std::format("Unsupported legacy DAST prefix version {}.", LegacyVersion)});
			}

			constexpr uint32 DurfMagic = 0x46525544;
			if (Magic != DurfMagic)
				return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Invalid asset magic."});
			FBinaryEnvelopePreamble EnvelopePreamble;
			std::expected<void, EBinaryEnvelopeError> Diagnostic;
			const uint64 FileBytes = PhysicalFileBytes == 0 ? Bytes.size() : PhysicalFileBytes;
			if (!(Diagnostic = ParseBinaryEnvelopePrefix(Bytes, FileBytes, PackageEnvelopeLimits, EnvelopePreamble)))
				return EnvelopeError(Diagnostic.error(), OutUnsupportedFormat);
			if (EnvelopePreamble.HeaderBytes > Bytes.size())
				return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "BinaryEnvelopeTruncated: front matter is incomplete."});
			FValidatedBinaryEnvelope Envelope;
			if (!(Diagnostic = ValidateBinaryEnvelopeHeader(Bytes.first(static_cast<size_t>(EnvelopePreamble.HeaderBytes)), FileBytes, PackageEnvelopeLimits, GetPackageFormatRegistry(), Envelope)))
				return EnvelopeError(Diagnostic.error(), OutUnsupportedFormat);
			OutFormatVersion = Envelope.Preamble.FormatVersion;
			return {};
		}
	}

	auto FindAssetPackageReader(uint32 FormatVersion)
		-> const FAssetPackageCodec*
	{
		const auto It = std::ranges::find_if(Codecs, [&](const FAssetPackageCodec& Codec) {
			return Codec.FormatVersion == FormatVersion;
		});
		return It != Codecs.end() && It->bCanRead ? &*It : nullptr;
	}

	auto FindAssetPackageWriter(uint32 FormatVersion)
		-> const FAssetPackageCodec*
	{
		const auto It = std::ranges::find_if(Codecs, [&](const FAssetPackageCodec& Codec) {
			return Codec.FormatVersion == FormatVersion;
		});
		return It != Codecs.end() && It->bCanWrite ? &*It : nullptr;
	}

	auto ResolveAssetPackageReader(
		FByteView Bytes, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion, uint64 PhysicalFileBytes) -> std::expected<void, Durin::UnifiedError::FError>
	{
		OutCodec = nullptr;
		if (OutFormatVersion) *OutFormatVersion = 0;
		uint32 FormatVersion = 0;
		if (auto Result = ReadAssetPackageFormatVersion(
				Bytes, FormatVersion, PhysicalFileBytes); !Result)
			return Result;
		if (!ObjectPackage::IsSupportedPackageReaderVersion(FormatVersion))
			return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::UnsupportedVersion, std::format("Unsupported DAST package version {}.", FormatVersion)});
		OutCodec = FindAssetPackageReader(FormatVersion);
		if (!OutCodec)
			return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::UnsupportedVersion, std::format("Unsupported DAST package version {}.", FormatVersion)});
		if (OutFormatVersion) *OutFormatVersion = FormatVersion;
		return {};
	}

	auto ResolveAssetPackageReader(IAssetPackageByteSource& Source,
		const FAssetPackageCodec*& OutCodec, uint32* OutFormatVersion,
		const FPackageReadCancellationCheck& IsCancelled, bool* OutUnsupportedFormat) -> std::expected<void, Durin::UnifiedError::FError>
	{
		OutCodec = nullptr;
		if (OutUnsupportedFormat) *OutUnsupportedFormat = false;
		if (OutFormatVersion) *OutFormatVersion = 0;
		if (IsCancelled && IsCancelled())
			return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::Cancelled, "Asset schema inspection was cancelled."});
		const size_t PrefixBytes = static_cast<size_t>(std::min<uint64>(
			Source.GetSize(), BinaryEnvelopePreambleBytes));
		FByteBuffer Prefix(PrefixBytes);
		std::string ReadError;
		if (!Source.ReadAt(0, Prefix, &ReadError))
			return std::unexpected(Durin::UnifiedError::FError{PackageReadErrors::IoError, std::move(ReadError)});
		if (Prefix.size() < sizeof(uint32))
			return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Truncated asset header."});
		uint32 Magic = 0;
		std::memcpy(&Magic, Prefix.data(), sizeof(Magic));
		if (Magic == ObjectPackage::DastPackageMagic)
		{
			uint32 LegacyVersion = 0;
			if (Prefix.size() >= sizeof(uint32) * 2)
				std::memcpy(&LegacyVersion, Prefix.data() + sizeof(Magic), sizeof(LegacyVersion));
			if (OutUnsupportedFormat) *OutUnsupportedFormat = true;
			return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::UnsupportedVersion, std::format("Unsupported legacy DAST prefix version {}.", LegacyVersion)});
		}
		FBinaryEnvelopePreamble Preamble;
		std::expected<void, EBinaryEnvelopeError> Diagnostic;
		if (!(Diagnostic = ParseBinaryEnvelopePrefix(Prefix, Source.GetSize(), PackageEnvelopeLimits, Preamble)))
		{
			if (OutUnsupportedFormat) *OutUnsupportedFormat = Diagnostic.error() == EBinaryEnvelopeError::UnknownFormat
				|| Diagnostic.error() == EBinaryEnvelopeError::UnsupportedFormatVersion
				|| Diagnostic.error() == EBinaryEnvelopeError::UnsupportedRequiredFeatures;
			return EnvelopeError(Diagnostic.error());
		}
		if (Preamble.HeaderBytes > std::numeric_limits<size_t>::max())
			return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "BinaryEnvelopeTruncated: front matter is too large."});
		if (IsCancelled && IsCancelled())
			return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::Cancelled, "Asset schema inspection was cancelled."});
		FByteBuffer Header(static_cast<size_t>(Preamble.HeaderBytes));
		if (!Source.ReadAt(0, Header, &ReadError))
			return std::unexpected(Durin::UnifiedError::FError{PackageReadErrors::IoError, std::move(ReadError)});
		uint32 Version = 0;
		if (auto Result = ReadAssetPackageFormatVersion(
			Header, Version, Source.GetSize(), OutUnsupportedFormat); !Result) return Result;
		if (!ObjectPackage::IsSupportedPackageReaderVersion(Version) || !(OutCodec = FindAssetPackageReader(Version)))
		{
			if (OutUnsupportedFormat) *OutUnsupportedFormat = true;
			return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::UnsupportedVersion, std::format("Unsupported DAST package version {}.", Version)});
		}
		if (OutFormatVersion) *OutFormatVersion = Version;
		return {};
	}

	auto ValidateAssetPackageCodecTable(
		std::span<const FAssetPackageCodec> CandidateCodecs, std::string& OutError) -> bool
	{
		for (size_t Index = 0; Index < CandidateCodecs.size(); ++Index)
		{
			const FAssetPackageCodec& Codec = CandidateCodecs[Index];
			if (Codec.CodecId.empty() || Codec.FormatVersion == 0
				|| (Codec.bCanRead && (!Codec.ReadHeader || !Codec.Validate || !Codec.Inspect
					|| !Codec.ExtractReferences || !Codec.InspectSchema || !Codec.Load))
				|| (Codec.bCanWrite && !Codec.Write)
				|| (Codec.bCanMutate && (!Codec.RewriteReferences
					|| !Codec.Relocate || !Codec.WriteRedirector)))
			{
				OutError = std::format(
					"AssetPackageCodecIncomplete: codec {} has incomplete capabilities.",
					Codec.CodecId);
				return false;
			}
			for (size_t OtherIndex = Index + 1; OtherIndex < CandidateCodecs.size(); ++OtherIndex)
				if (CandidateCodecs[OtherIndex].FormatVersion == Codec.FormatVersion
					|| CandidateCodecs[OtherIndex].CodecId == Codec.CodecId)
				{
					OutError = "AssetPackageCodecDuplicate: codec identities must be unique.";
					return false;
				}
		}
		return true;
	}

	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool
	{
		if (!ValidateAssetPackageCodecTable(Codecs, OutError)) return false;
		for (uint32 Version : ObjectPackage::SupportedPackageReaderVersions)
			if (!FindAssetPackageReader(Version))
			{
				OutError = std::format(
					"AssetPackageReaderPolicyIncomplete: DAST v{} has no complete reader.", Version);
				return false;
			}
		if (!FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion))
		{
			OutError = "AssetPackageWriterPolicyIncomplete: a selected writer is unavailable.";
			return false;
		}
		return true;
	}
}

namespace Durin
{
	auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool
	{
		return AssetPrivate::ValidateAssetPackageCodecPolicy(OutError);
	}

	auto GetAssetPackageReaderPolicyIdentity() -> uint32
	{
		return ObjectPackage::PackageReaderPolicyFingerprint;
	}
}
