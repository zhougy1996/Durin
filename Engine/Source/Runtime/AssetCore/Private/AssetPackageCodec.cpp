#include "AssetPackageCodec.h"

#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageV5Codec.h"

namespace Durin::Asset::Private
{
	namespace
	{
		const std::array Codecs{DastV5::GetCodec()};

	}

	auto ReadAssetPackagePreamble(
		std::span<const std::byte> Bytes, FAssetPackagePreamble& OutPreamble) -> FAssetResult
	{
		if (Bytes.size() < sizeof(uint32) * 2)
			return {EAssetError::CorruptFile, "Truncated asset header."};
		uint32 Magic = 0;
		uint32 Version = 0;
		std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
		std::memcpy(&Version, Bytes.data() + sizeof(Magic), sizeof(Version));
		if (Magic != DastPackageMagic)
			return {EAssetError::CorruptFile, "Invalid asset magic."};
		OutPreamble = {.FormatVersion = Version};
		return {};
	}

	auto FindAssetPackageReader(uint32 FormatVersion) -> const FAssetPackageCodec*
	{
		const auto It = std::ranges::find(Codecs, FormatVersion, &FAssetPackageCodec::FormatVersion);
		return It != Codecs.end() && It->bCanRead ? &*It : nullptr;
	}

	auto FindAssetPackageWriter(uint32 FormatVersion) -> const FAssetPackageCodec*
	{
		const auto It = std::ranges::find(Codecs, FormatVersion, &FAssetPackageCodec::FormatVersion);
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
		OutCodec = FindAssetPackageReader(Preamble.FormatVersion);
		if (!OutCodec)
			return {EAssetError::UnsupportedVersion,
				std::format("Unsupported asset version {}.", Preamble.FormatVersion)};
		if (OutPreamble) *OutPreamble = Preamble;
		return {};
	}

	auto ValidateAssetPackageCodecPolicy(std::string& OutError) -> bool
	{
		for (size_t Index = 0; Index < Codecs.size(); ++Index)
		{
			const FAssetPackageCodec& Codec = Codecs[Index];
			if (Codec.CodecId.empty() || Codec.FormatVersion == 0
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
			for (size_t OtherIndex = Index + 1; OtherIndex < Codecs.size(); ++OtherIndex)
				if (Codecs[OtherIndex].FormatVersion == Codec.FormatVersion
					|| Codecs[OtherIndex].CodecId == Codec.CodecId)
				{
					OutError = "AssetPackageCodecDuplicate: codec identities must be unique.";
					return false;
				}
		}
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
