#include "AssetPackageCodec.h"
#include "AssetPackageByteSource.h"

#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageV9Codec.h"
#include "Serialization/BinaryEnvelope.h"

namespace Durin::Asset::Private
{
	namespace
	{
		const std::array Codecs{DastV9::GetCodec()};
		constexpr FBinaryEnvelopeLimits PackageEnvelopeLimits{
			16ull * 1024ull * 1024ull,
			1024ull * 1024ull * 1024ull};

		auto GetPackageFormatRegistry() -> const FBinaryFormatRegistry&
		{
			static const FBinaryFormatRegistry Registry = [] {
				const std::array Descriptors{FBinaryFormatDescriptor{
					.FormatId = DastBinaryFormatId,
					.DebugName = std::string(DastBinaryFormatName),
					.MinimumFormatVersion = AssetPackageV9FormatVersion,
					.MaximumFormatVersion = AssetPackageV9FormatVersion,
					.SupportedRequiredFeatures = 0,
					.Limits = PackageEnvelopeLimits}};
				FBinaryFormatRegistry Result;
				const bool bCreated = FBinaryFormatRegistry::Create(Descriptors, Result);
				require(bCreated);
				return Result;
			}();
			return Registry;
		}

		auto EnvelopeError(const FBinaryEnvelopeDiagnostic& Diagnostic) -> FAssetResult
		{
			const bool bUnsupported = Diagnostic.Error == EBinaryEnvelopeError::UnknownFormat
				|| Diagnostic.Error == EBinaryEnvelopeError::UnsupportedFormatVersion
				|| Diagnostic.Error == EBinaryEnvelopeError::UnsupportedRequiredFeatures;
			return {bUnsupported ? EAssetError::UnsupportedVersion : EAssetError::CorruptFile,
				std::string(Diagnostic.Message)};
		}
		auto ReadAssetPackageFormatVersion(
			std::span<const std::byte> Bytes, uint32& OutFormatVersion,
			uint64 PhysicalFileBytes) -> FAssetResult
		{
			OutFormatVersion = 0;
			if (Bytes.size() < sizeof(uint32))
				return {EAssetError::CorruptFile, "Truncated asset header."};
			uint32 Magic = 0;
			std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
			if (Magic == DastPackageMagic)
			{
				if (Bytes.size() < sizeof(uint32) * 2)
					return {EAssetError::CorruptFile, "Truncated asset header."};
				uint32 LegacyVersion = 0;
				std::memcpy(&LegacyVersion, Bytes.data() + sizeof(Magic), sizeof(LegacyVersion));
				return {EAssetError::UnsupportedVersion,
					std::format("Unsupported legacy DAST prefix version {}.", LegacyVersion)};
			}

			constexpr uint32 DurfMagic = 0x46525544;
			if (Magic != DurfMagic)
				return {EAssetError::CorruptFile, "Invalid asset magic."};
			FBinaryEnvelopePreamble EnvelopePreamble;
			FBinaryEnvelopeDiagnostic Diagnostic;
			const uint64 FileBytes = PhysicalFileBytes == 0 ? Bytes.size() : PhysicalFileBytes;
			if (!ParseBinaryEnvelopePrefix(
				Bytes, FileBytes, PackageEnvelopeLimits, EnvelopePreamble, &Diagnostic))
				return EnvelopeError(Diagnostic);
			if (EnvelopePreamble.HeaderBytes > Bytes.size())
				return {EAssetError::CorruptFile, "BinaryEnvelopeTruncated: front matter is incomplete."};
			FValidatedBinaryEnvelope Envelope;
			if (!ValidateBinaryEnvelopeHeader(
				Bytes.first(static_cast<size_t>(EnvelopePreamble.HeaderBytes)), FileBytes,
				PackageEnvelopeLimits, GetPackageFormatRegistry(), Envelope, &Diagnostic))
				return EnvelopeError(Diagnostic);
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
		std::span<const std::byte> Bytes, const FAssetPackageCodec*& OutCodec,
		uint32* OutFormatVersion, uint64 PhysicalFileBytes) -> FAssetResult
	{
		OutCodec = nullptr;
		if (OutFormatVersion) *OutFormatVersion = 0;
		uint32 FormatVersion = 0;
		if (FAssetResult Result = ReadAssetPackageFormatVersion(
				Bytes, FormatVersion, PhysicalFileBytes); !Result)
			return Result;
		if (!IsSupportedAssetPackageReaderVersion(FormatVersion))
			return {EAssetError::UnsupportedVersion,
				std::format("Unsupported DAST package version {}.", FormatVersion)};
		OutCodec = FindAssetPackageReader(FormatVersion);
		if (!OutCodec)
			return {EAssetError::UnsupportedVersion,
				std::format("Unsupported DAST package version {}.", FormatVersion)};
		if (OutFormatVersion) *OutFormatVersion = FormatVersion;
		return {};
	}

	auto ResolveAssetPackageReader(IAssetPackageByteSource& Source,
		const FAssetPackageCodec*& OutCodec, uint32* OutFormatVersion,
		const FPackageReadCancellationCheck& IsCancelled) -> FAssetResult
	{
		OutCodec = nullptr;
		if (OutFormatVersion) *OutFormatVersion = 0;
		if (IsCancelled && IsCancelled())
			return {EAssetError::IoError, "Asset schema inspection was cancelled."};
		const size_t PrefixBytes = static_cast<size_t>(std::min<uint64>(
			Source.GetSize(), BinaryEnvelopePreambleBytes));
		std::vector<std::byte> Prefix(PrefixBytes);
		std::string ReadError;
		if (!Source.ReadAt(0, Prefix, &ReadError))
			return {EAssetError::NotFound, std::move(ReadError)};
		if (Prefix.size() < sizeof(uint32))
			return {EAssetError::CorruptFile, "Truncated asset header."};
		uint32 Magic = 0;
		std::memcpy(&Magic, Prefix.data(), sizeof(Magic));
		if (Magic == DastPackageMagic)
		{
			uint32 LegacyVersion = 0;
			if (Prefix.size() >= sizeof(uint32) * 2)
				std::memcpy(&LegacyVersion, Prefix.data() + sizeof(Magic), sizeof(LegacyVersion));
			return {EAssetError::UnsupportedVersion,
				std::format("Unsupported legacy DAST prefix version {}.", LegacyVersion)};
		}
		FBinaryEnvelopePreamble Preamble;
		FBinaryEnvelopeDiagnostic Diagnostic;
		if (!ParseBinaryEnvelopePrefix(Prefix, Source.GetSize(), PackageEnvelopeLimits,
			Preamble, &Diagnostic)) return EnvelopeError(Diagnostic);
		if (Preamble.HeaderBytes > std::numeric_limits<size_t>::max())
			return {EAssetError::CorruptFile, "BinaryEnvelopeTruncated: front matter is too large."};
		if (IsCancelled && IsCancelled())
			return {EAssetError::IoError, "Asset schema inspection was cancelled."};
		std::vector<std::byte> Header(static_cast<size_t>(Preamble.HeaderBytes));
		if (!Source.ReadAt(0, Header, &ReadError))
			return {EAssetError::CorruptFile, std::move(ReadError)};
		uint32 Version = 0;
		if (FAssetResult Result = ReadAssetPackageFormatVersion(
			Header, Version, Source.GetSize()); !Result) return Result;
		if (!IsSupportedAssetPackageReaderVersion(Version) || !(OutCodec = FindAssetPackageReader(Version)))
			return {EAssetError::UnsupportedVersion,
				std::format("Unsupported DAST package version {}.", Version)};
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
		for (uint32 Version : SupportedAssetPackageReaderVersions)
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

namespace Durin::Asset
{
	auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool
	{
		return Private::ValidateAssetPackageCodecPolicy(OutError);
	}

	auto GetAssetPackageReaderPolicyIdentity() -> uint32
	{
		return AssetPackageReaderPolicyFingerprint;
	}
}
