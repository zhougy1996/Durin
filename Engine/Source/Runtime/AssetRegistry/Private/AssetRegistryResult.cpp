#include "AssetRegistry/RegistryResult.h"

namespace Durin
{
	static auto FormatAssetRegistryReason(const FAssetRegistryResult& Result) -> std::string
	{
		const auto& Context = Result.Context;
		if (Context.ReaderCause) return std::format("DAST v10 Registry projection failed: {}",
			ObjectPackage::FormatPackageError(*Context.ReaderCause));
		switch (Context.Reason)
		{
		case EAssetRegistryFailure::None: return {};
		case EAssetRegistryFailure::MissingRoot: return std::format("The Asset Registry has no entry for dependency root '{}'.", Context.Path);
		case EAssetRegistryFailure::MissingDependency: return std::format("The Asset Registry has no entry for dependency '{}'.", Context.Path);
		case EAssetRegistryFailure::InvalidBatch: return "Asset metadata batch contains an invalid or duplicate package path.";
		case EAssetRegistryFailure::IncompleteProjection: return "Asset registry publication requires a complete catalog/reference projection.";
		case EAssetRegistryFailure::InconsistentMetadata: return "Asset registry publication contains inconsistent package metadata.";
		case EAssetRegistryFailure::InvalidPublicationAssets: return "Asset registry publication contains invalid exact asset metadata.";
		case EAssetRegistryFailure::FingerprintMismatch: return "Asset registry publication fingerprint drifted from catalog metadata.";
		case EAssetRegistryFailure::EdgeMismatch: return "Asset registry publication package edges drifted from catalog metadata.";
		case EAssetRegistryFailure::InvalidAdd: return "Asset registry delta Add path is invalid, duplicated, or occupied.";
		case EAssetRegistryFailure::InvalidReplace: return "Asset registry delta Replace path is invalid, duplicated, or missing.";
		case EAssetRegistryFailure::InvalidRemove: return "Asset registry delta Remove path is invalid, duplicated, or missing.";
		case EAssetRegistryFailure::InvalidInvalidation: return "Asset registry delta contains an invalid reference-invalidation path.";
		case EAssetRegistryFailure::InvalidRedirector: return "CorruptRedirector: invalid exact asset metadata.";
		case EAssetRegistryFailure::TruncatedFrontMatter: return "DAST v10 front matter is truncated.";
		case EAssetRegistryFailure::InvalidPackageIdentity: return "DAST v10 Registry projection requires the mounted package identity.";
		case EAssetRegistryFailure::InvalidHeaderAssets: return "DAST v10 Registry contains invalid exact asset metadata.";
		case EAssetRegistryFailure::WriteInProgress: return "Package output is being written.";
		case EAssetRegistryFailure::FileTooLarge: return "Asset package exceeds the supported byte bound.";
		case EAssetRegistryFailure::TruncatedLegacyHeader: return "Truncated asset header.";
		case EAssetRegistryFailure::InvalidFrontMatterExtent: return "Asset package declares an invalid front-matter extent.";
		case EAssetRegistryFailure::BulkTooLarge: return "Asset package bulk segment exceeds the supported byte bound.";
		case EAssetRegistryFailure::Envelope: return std::format("Binary envelope parsing failed (code {}).", static_cast<uint8>(Context.EnvelopeCause));
		case EAssetRegistryFailure::Reader: return "DAST v10 Registry projection failed.";
		case EAssetRegistryFailure::UnsupportedVersion: return std::format("Unsupported DAST package format version {}.", Context.Actual);
		case EAssetRegistryFailure::LegacyVersion: return std::format("Unsupported legacy DAST prefix version {}.", Context.Actual);
		case EAssetRegistryFailure::OpenFailed: return std::format("Failed to open asset package {}.", Context.Path);
		case EAssetRegistryFailure::SizeFailed: return std::format("Failed to size asset package {}.", Context.Path);
		case EAssetRegistryFailure::ReadFailed: return std::format("Failed to read asset package {}.", Context.Path);
		case EAssetRegistryFailure::MapPathFailed: return std::format("Failed to map asset path {}.", Context.Path);
		case EAssetRegistryFailure::FingerprintFailed: return std::format("Failed to fingerprint asset {}.", Context.Path);
		case EAssetRegistryFailure::DuplicatePath: return std::format("Duplicate asset path {}.", Context.Path);
		case EAssetRegistryFailure::EnumerationFailed: return std::format("Failed to enumerate mount {}.", Context.Path);
		case EAssetRegistryFailure::PublicationRevision: return std::format("Asset registry publication expected revision {} but current revision is {}.", Context.Expected, Context.Actual);
		case EAssetRegistryFailure::DeltaRevision: return std::format("Asset registry delta expected revision {} but current revision is {}.", Context.Expected, Context.Actual);
		}
		return {};
	}

	auto FormatAssetRegistryError(const FAssetRegistryResult& Result) -> std::string
	{
		std::string Message = FormatAssetRegistryReason(Result);
		if (!Result.Context.Path.empty() && Message.find(Result.Context.Path) == std::string::npos)
			Message += std::format(" ({})", Result.Context.Path);
		return Message;
	}

}
