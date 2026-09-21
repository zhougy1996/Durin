#pragma once

#include "AssetRegistryAPI.h"
#include "DObject/PackageFormat.h"

namespace Durin
{
	enum class EAssetRegistryError : uint8
	{
		None,
		InvalidPath,
		AlreadyExists,
		NotFound,
		IoError,
		CorruptFile,
		UnsupportedVersion,
		MissingDependency,
		StaleData
	};

	enum class EAssetRegistryFailure : uint8
	{
		None, MissingRoot, MissingDependency, InvalidBatch, IncompleteProjection, InconsistentMetadata, InvalidPublicationAssets, FingerprintMismatch, EdgeMismatch, InvalidAdd, InvalidReplace, InvalidRemove, InvalidInvalidation, InvalidRedirector, TruncatedFrontMatter, InvalidPackageIdentity, InvalidHeaderAssets, WriteInProgress, FileTooLarge, TruncatedLegacyHeader, InvalidFrontMatterExtent, BulkTooLarge, Envelope, Reader, UnsupportedVersion, LegacyVersion, OpenFailed, SizeFailed, ReadFailed, MapPathFailed, FingerprintFailed, DuplicatePath, EnumerationFailed, PublicationRevision, DeltaRevision
	};
	struct FAssetRegistryErrorContext
	{
		EAssetRegistryFailure Reason = EAssetRegistryFailure::None;
		std::string Path;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::error_code SystemError;
		EBinaryEnvelopeError EnvelopeCause = EBinaryEnvelopeError::None;
		std::optional<ObjectPackage::FPackageReaderResult> ReaderCause;
	};

	struct FAssetRegistryResult
	{
		EAssetRegistryError Error = EAssetRegistryError::None;
		FAssetRegistryErrorContext Context;

		auto Succeeded() const -> bool { return Error == EAssetRegistryError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	ASSETREGISTRY_API auto FormatAssetRegistryError(const FAssetRegistryResult& Result) -> std::string;
}
