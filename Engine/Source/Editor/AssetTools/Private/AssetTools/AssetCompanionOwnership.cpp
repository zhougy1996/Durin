#include "AssetTools/AssetCompanionOwnership.h"
#include "AssetDeletionInternal.h"
#include "AssetRegistry/PackageHeader.h"

#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFile.h"

namespace Durin
{
	namespace
	{
		auto Reject(std::optional<UnifiedError::FError>* OutError, UnifiedError::FError Error) -> bool
		{ if (OutError) *OutError = std::move(Error); return false; }
	}

	namespace AssetToolsPrivate
	{
		auto InspectAssetCompanionFiles(
			const FPackageData& Data,
			std::vector<std::filesystem::path>& OutFiles,
			std::optional<UnifiedError::FError>* OutError) -> bool
		{
			if (OutError) OutError->reset();
			OutFiles.clear();
			// Ownership is a package metadata fact, not a payload-integrity check.
			// Read current bounded front matter so external edits cannot hide behind
			// a stale catalog entry; the reader also checks the physical bulk extent.
			FAssetPackageHeader Header;
			const FAssetRegistryResult HeaderResult = ReadAssetPackageHeader(
				Data.PhysicalPath, Data.PackagePath, Header);
			if (!HeaderResult)
			{
				return Reject(OutError, Durin::ToError(HeaderResult));
			}
			if (Header.BulkSegmentExtent != 0)
			{
				std::filesystem::path BulkPath =
					std::filesystem::absolute(Data.PhysicalPath).lexically_normal();
				BulkPath.replace_extension(".dbulk");
				OutFiles.push_back(std::move(BulkPath));
			}
			return true;
		}

		auto MayOwnCompanionInRoots(const FPackageData& Data,
			std::span<const std::filesystem::path> NormalizedRoots) -> bool
		{
			std::filesystem::path BulkPath =
				std::filesystem::absolute(Data.PhysicalPath).lexically_normal();
			BulkPath.replace_extension(".dbulk");
			const auto Candidate = BulkPath.generic_string();
			// Do not filter on cached bulk extent: an external edit may add a segment.
			return std::ranges::any_of(NormalizedRoots, [&](const auto& Root) {
				const auto PhysicalRoot = Root.generic_string();
				return Candidate == PhysicalRoot
					|| FPaths::IsLexicalDescendantPath(Candidate, PhysicalRoot, true);
			});
		}

	}

	auto QueryAssetCompanionOwnership(
		const std::filesystem::path& PhysicalPath,
		FAssetCompanionOwnership& OutOwnership, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		OutOwnership = {};
		const std::filesystem::path Candidate =
			std::filesystem::absolute(PhysicalPath).lexically_normal();
		for (const auto& [Path, Data] : CaptureAssetCatalogSnapshot().Assets)
		{
			if (!AssetToolsPrivate::MayOwnCompanionInRoots(Data, std::span{&Candidate, 1})) continue;
			if (!FMountPaths::FindMountForVirtualPath(Path.GetView()))
				continue;
			std::error_code ExistenceError;
			const bool bPackageExists =
				std::filesystem::is_regular_file(Data.PhysicalPath, ExistenceError);
			if (!bPackageExists
				&& (!ExistenceError
					|| ExistenceError == std::errc::no_such_file_or_directory
					|| ExistenceError == std::errc::not_a_directory))
				continue;
			if (ExistenceError)
				return Reject(OutError, ToError(FFileSystemError{std::format("Failed to inspect: {}.", FFilePath(Data.PhysicalPath).generic_string()), ExistenceError}).PushContext("companion owner", Path.ToString()));
			std::vector<std::filesystem::path> CompanionFiles;
			const bool Result = AssetToolsPrivate::InspectAssetCompanionFiles(
				Data, CompanionFiles, OutError);
			if (!Result)
			{
				if (OutError && *OutError) (*OutError)->PushContext("companion owner", Path.ToString());
				return false;
			}
			if (std::ranges::contains(CompanionFiles, Candidate))
				OutOwnership.Owners.push_back(Path);
		}
		std::ranges::sort(
			OutOwnership.Owners,
			[](const FPackagePath& A, const FPackagePath& B) {
				return A.GetView() < B.GetView();
			});
		OutOwnership.State = OutOwnership.Owners.empty()
			? EAssetCompanionOwnershipState::Unclaimed
			: OutOwnership.Owners.size() == 1
			? EAssetCompanionOwnershipState::Owned
			: EAssetCompanionOwnershipState::Ambiguous;
		return true;
	}
}
