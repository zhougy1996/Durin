#include "AssetPackageCodec.h"

#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageV5Codec.h"

namespace Durin::Asset::Private
{
	namespace
	{
		const std::array Codecs{DastV5::GetCodec()};
		constexpr FBinaryEnvelopeLimits PackageEnvelopeLimits{
			16ull * 1024ull * 1024ull,
			1024ull * 1024ull * 1024ull};

		auto GetPackageFormatRegistry() -> const FBinaryFormatRegistry&
		{
			static const FBinaryFormatRegistry Registry = [] {
				const std::array Descriptors{FBinaryFormatDescriptor{
					.FormatId = DastBinaryFormatId,
					.DebugName = std::string(DastBinaryFormatName),
					.MinimumFormatVersion = AssetPackageV6FormatVersion,
					.MaximumFormatVersion = AssetPackageV6FormatVersion,
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

	}

	auto ReadAssetPackagePreamble(
		std::span<const std::byte> Bytes, FAssetPackagePreamble& OutPreamble) -> FAssetResult
	{
		if (Bytes.size() < sizeof(uint32))
			return {EAssetError::CorruptFile, "Truncated asset header."};
		uint32 Magic = 0;
		std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
		if (Magic == DastPackageMagic)
		{
			if (Bytes.size() < sizeof(uint32) * 2)
				return {EAssetError::CorruptFile, "Truncated asset header."};
			uint32 Version = 0;
			std::memcpy(&Version, Bytes.data() + sizeof(Magic), sizeof(Version));
			OutPreamble = {
				.FormatId = DastBinaryFormatId,
				.FormatVersion = Version,
				.bUsesBinaryEnvelope = false};
			return {};
		}

		constexpr uint32 DurfMagic = 0x46525544;
		if (Magic != DurfMagic)
			return {EAssetError::CorruptFile, "Invalid asset magic."};
		FBinaryEnvelopePreamble EnvelopePreamble;
		FBinaryEnvelopeDiagnostic Diagnostic;
		if (!ParseBinaryEnvelopePrefix(
			Bytes, Bytes.size(), PackageEnvelopeLimits, EnvelopePreamble, &Diagnostic))
			return EnvelopeError(Diagnostic);
		if (EnvelopePreamble.HeaderBytes > Bytes.size())
			return {EAssetError::CorruptFile, "BinaryEnvelopeTruncated: front matter is incomplete."};
		FValidatedBinaryEnvelope Envelope;
		if (!ValidateBinaryEnvelopeHeader(
			Bytes.first(static_cast<size_t>(EnvelopePreamble.HeaderBytes)), Bytes.size(),
			PackageEnvelopeLimits, GetPackageFormatRegistry(), Envelope, &Diagnostic))
			return EnvelopeError(Diagnostic);
		OutPreamble = {
			.FormatId = Envelope.Preamble.FormatId,
			.FormatVersion = Envelope.Preamble.FormatVersion,
			.bUsesBinaryEnvelope = true};
		return {};
	}

	auto FindAssetPackageReader(const FGuid& FormatId, uint32 FormatVersion)
		-> const FAssetPackageCodec*
	{
		const auto It = std::ranges::find_if(Codecs, [&](const FAssetPackageCodec& Codec) {
			return Codec.FormatId == FormatId && Codec.FormatVersion == FormatVersion;
		});
		return It != Codecs.end() && It->bCanRead ? &*It : nullptr;
	}

	auto FindAssetPackageWriter(const FGuid& FormatId, uint32 FormatVersion)
		-> const FAssetPackageCodec*
	{
		const auto It = std::ranges::find_if(Codecs, [&](const FAssetPackageCodec& Codec) {
			return Codec.FormatId == FormatId && Codec.FormatVersion == FormatVersion;
		});
		return It != Codecs.end() && It->bCanWrite ? &*It : nullptr;
	}

	auto ResolveAssetPackageReader(
		std::span<const std::byte> Bytes, const FAssetPackageCodec*& OutCodec,
		FAssetPackagePreamble* OutPreamble) -> FAssetResult
	{
		OutCodec = nullptr;
		FAssetPackagePreamble Preamble;
		if (FAssetResult Result = ReadAssetPackagePreamble(Bytes, Preamble); !Result)
			return Result;
		OutCodec = FindAssetPackageReader(Preamble.FormatId, Preamble.FormatVersion);
		if (!OutCodec)
			return {EAssetError::UnsupportedVersion,
				std::format("Unsupported asset format {} version {}.",
					Preamble.FormatId.ToString(), Preamble.FormatVersion)};
		if (OutPreamble) *OutPreamble = Preamble;
		return {};
	}

	auto ValidateAssetPackageCodecTable(
		std::span<const FAssetPackageCodec> CandidateCodecs, std::string& OutError) -> bool
	{
		for (size_t Index = 0; Index < CandidateCodecs.size(); ++Index)
		{
			const FAssetPackageCodec& Codec = CandidateCodecs[Index];
			if (Codec.CodecId.empty() || !Codec.FormatId.IsValid() || Codec.FormatVersion == 0
				|| (Codec.bCanRead && (!Codec.ReadHeader || !Codec.Validate || !Codec.Inspect
					|| !Codec.ExtractReferences || !Codec.ProbeCompatibility || !Codec.Load))
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
				if ((CandidateCodecs[OtherIndex].FormatId == Codec.FormatId
						&& CandidateCodecs[OtherIndex].FormatVersion == Codec.FormatVersion)
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
			if (!FindAssetPackageReader(DastBinaryFormatId, Version))
			{
				OutError = std::format(
					"AssetPackageReaderPolicyIncomplete: DAST v{} has no complete reader.", Version);
				return false;
			}
		if (!FindAssetPackageWriter(DastBinaryFormatId, OrdinaryAssetPackageWriterVersion))
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
