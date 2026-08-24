#include "AssetPackageCodec.h"

#include "Asset/PackageV4Reader.h"
#include "Asset/PackageV4Writer.h"
#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageV5Codec.h"

namespace Durin::Asset::Private
{
	namespace
	{
		auto ReadV4Header(
			std::span<const std::byte> Bytes,
			uint64 PackageSize,
			FAssetPackageHeader& OutHeader)
			-> FAssetResult
		{
			DastV4::FValidatedHeader Header;
			DastV4::FReaderDiagnostic Diagnostic;
			if (!DastV4::ReadHeader(
				Bytes, Header, {}, &Diagnostic, PackageSize))
				return {EAssetError::CorruptFile, Diagnostic.Message};

			FAssetPackageHeader Result{
				.AssetClassName = std::move(Header.AssetClass),
				.EntryKind = Header.EntryKind,
				.FormatVersion = DastV4::Version,
				.ObjectCount = Header.ObjectCount,
				.BytesRead = Header.BytesRead};
			if (!Header.RedirectDestination.empty()
				&& !FAssetPath::TryCreate(Header.RedirectDestination, Result.RedirectDestination))
				return {EAssetError::CorruptFile, "The redirect destination path is invalid."};
			for (const std::string& DependencyString : Header.Dependencies)
			{
				FAssetPath Dependency;
				if (!FAssetPath::TryCreate(DependencyString, Dependency))
					return {EAssetError::CorruptFile, "A dependency path is invalid."};
				Result.Dependencies.push_back(std::move(Dependency));
			}
			OutHeader = std::move(Result);
			return {};
		}

		auto ValidateV4(std::span<const std::byte> Bytes) -> FAssetResult
		{
			DastV4::FDecodedPackage Package;
			DastV4::FReaderDiagnostic Diagnostic;
			if (!DastV4::DecodePackage(Bytes, Package, {}, &Diagnostic))
				return {EAssetError::CorruptFile, Diagnostic.Message};
			return {};
		}

		auto InspectV4(std::span<const std::byte> Bytes, FAssetPackageInspection& OutInspection)
			-> FAssetResult
		{
			DastV4::FReaderDiagnostic Diagnostic;
			return DastV4::InspectPackage(Bytes, OutInspection, {}, &Diagnostic);
		}

		auto ExtractV4References(
			std::span<const std::byte> Bytes, const FAssetPath& SourcePackage,
			std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult
		{
			DastV4::FReaderDiagnostic Diagnostic;
			return DastV4::ExtractReferences(
				Bytes, SourcePackage, OutReferences, {}, &Diagnostic);
		}

		auto ProbeV4Compatibility(
			std::span<const std::byte> Bytes, const FAssetPath& PackagePath,
			const FReflectionCompatibilityCatalog& Catalog,
			FAssetPackageCompatibilityRecord& OutRecord,
			FAssetCompatibilityProbeStats* OutStats) -> FAssetResult
		{
			DastV4::FReaderDiagnostic Diagnostic;
			return DastV4::ProbeCompatibility(
				Bytes, PackagePath, Catalog, OutRecord, OutStats, {}, &Diagnostic);
		}

		auto LoadV4(
			std::span<const std::byte> Bytes, const FAssetPath& PackagePath,
			DPackage*& OutPackage, FAssetLoadReport* OutReport,
			const std::function<FAssetResult(DPackage*)>& OnSkeletonReady,
			const std::function<void(DPackage*)>& OnSkeletonRollback) -> FAssetResult
		{
			OutPackage = nullptr;
			DastV4::FLoadedAssetPackage Loaded;
			DastV4::FReaderDiagnostic Diagnostic;
			FAssetResult Result = DastV4::LoadAssetPackage(
				Bytes, PackagePath, Loaded, OutReport,
				{.OnSkeletonReady = OnSkeletonReady,
					.OnSkeletonRollback = OnSkeletonRollback}, {}, &Diagnostic);
			if (!Result) return Result;
			OutPackage = Loaded.Release();
			return {};
		}

		auto WriteV4(
			DPackage* Package, std::vector<std::byte>& OutBytes,
			EDefaultDeltaMode DeltaMode,
			const FAssetPackageSerializationOptions& Serialization) -> FAssetResult
		{
			DastV4::FWriterDiagnostic Diagnostic;
			return DastV4::WriteAssetPackage(Package, OutBytes,
				{.DeltaMode = DeltaMode, .Serialization = Serialization}, &Diagnostic);
		}

		const std::array Codecs{
			FAssetPackageCodec{
				.CodecId = "dast-v4",
				.FormatVersion = DastV4::Version,
				.bCanRead = true,
				.bCanWrite = true,
				.bCanMutate = true,
				.ReadHeader = &ReadV4Header,
				.Validate = &ValidateV4,
				.Inspect = &InspectV4,
				.ExtractReferences = &ExtractV4References,
				.ProbeCompatibility = &ProbeV4Compatibility,
				.Load = &LoadV4,
				.Write = &WriteV4,
				.RewriteReferences = &DastV4::RewriteReferences,
				.Relocate = &DastV4::RelocatePackage,
				.WriteRedirector = &DastV4::WriteRedirectorPackage},
			DastV5::GetCodec()};

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
