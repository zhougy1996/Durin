#include "AssetDeletionInternal.h"
#include "AssetRegistry/PackageHeader.h"

#include "DObject/Class.h"
#include "Misc/MountPaths.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

	struct FRegisteredDeleteContributor
	{
		FAssetDeleteContributorHandle Handle = 0;
		FAssetDeleteContributor Contributor;
	};

	auto GetDeleteContributors()
		-> std::unordered_map<DClass*, FRegisteredDeleteContributor>&
	{
		static std::unordered_map<DClass*, FRegisteredDeleteContributor> Contributors;
		return Contributors;
	}

	auto NextDeleteContributorHandle() -> FAssetDeleteContributorHandle&
	{
		static FAssetDeleteContributorHandle Handle = 1;
		return Handle;
	}

	auto InspectAssetCompanionFiles(
		const FAssetData& Data,
		std::vector<std::filesystem::path>& OutFiles,
		bool* OutHasContributor = nullptr) -> FAssetResult
	{
		OutFiles.clear();
		if (OutHasContributor) *OutHasContributor = false;
		// Ownership is a package metadata fact, not a payload-integrity check.
		// Read current bounded front matter so external edits cannot hide behind
		// a stale catalog entry; the reader also checks the physical bulk extent.
		FAssetPackageHeader Header;
		const FAssetRegistryResult HeaderResult = ReadAssetPackageHeader(
			Data.PhysicalPath, Data.PackagePath, Header);
		if (!HeaderResult)
		{
			EAssetError Code = EAssetError::CorruptFile;
			switch (HeaderResult.Error)
			{
			case EAssetRegistryError::IoError: Code = EAssetError::IoError; break;
			case EAssetRegistryError::InvalidPath: Code = EAssetError::InvalidPath; break;
			case EAssetRegistryError::UnsupportedVersion: Code = EAssetError::UnsupportedVersion; break;
			default: break;
			}
			return Error(Code, HeaderResult.Message);
		}
		if (Header.BulkSegmentExtent != 0)
		{
			std::filesystem::path BulkPath =
				std::filesystem::absolute(Data.PhysicalPath).lexically_normal();
			BulkPath.replace_extension(".dbulk");
			OutFiles.push_back(std::move(BulkPath));
		}
		if (OutHasContributor && !OutFiles.empty()) *OutHasContributor = true;
		DClass* AssetClass = FindClassByQualifiedName(FName(Data.AssetClassName));
		for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
		{
			const auto It = GetDeleteContributors().find(Class);
			if (It == GetDeleteContributors().end()) continue;
			if (OutHasContributor) *OutHasContributor = true;
			// Custom contributors retain the complete field-inspection contract.
			FAssetPackageInspection Inspection;
			const FAssetResult InspectionResult = InspectAssetPackage(
				Data.PhysicalPath, Data.PackagePath, Inspection);
			if (!InspectionResult) return InspectionResult;
			FAssetDeleteContribution Contribution;
			FAssetResult Result = It->second.Contributor(Data, Inspection, Contribution);
			if (!Result) return Result;
			for (const std::filesystem::path& File : Contribution.Files)
			{
				const std::filesystem::path Normalized =
					std::filesystem::absolute(File).lexically_normal();
				if (std::ranges::find(OutFiles, Normalized) == OutFiles.end())
					OutFiles.push_back(Normalized);
			}
			std::ranges::sort(OutFiles);
			break;
		}
		return {};
	}

	}

	namespace AssetToolsPrivate
	{
		auto InspectAssetCompanionFilesForDeletion(
			const FAssetData& Data,
			std::vector<std::filesystem::path>& OutFiles) -> FAssetResult
		{
			return InspectAssetCompanionFiles(Data, OutFiles);
		}

	}

	auto RegisterAssetDeleteContributor(
		DClass* Class,
		FAssetDeleteContributor Contributor) -> FAssetDeleteContributorHandle
	{
		if (!Class || !Contributor) return 0;
		auto& Contributors = GetDeleteContributors();
		if (Contributors.contains(Class)) return 0;
		auto& NextHandle = NextDeleteContributorHandle();
		const FAssetDeleteContributorHandle Handle = NextHandle++;
		Contributors.emplace(Class, FRegisteredDeleteContributor{
			.Handle = Handle,
			.Contributor = std::move(Contributor),
			});
		return Handle;
	}

	auto UnregisterAssetDeleteContributor(
		FAssetDeleteContributorHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Contributors = GetDeleteContributors();
		std::erase_if(Contributors, [Handle](const auto& Pair) {
			return Pair.second.Handle == Handle;
		});
	}

	auto QueryAssetCompanionOwnership(
		const std::filesystem::path& PhysicalPath,
		FAssetCompanionOwnership& OutOwnership) -> FAssetResult
	{
		OutOwnership = {};
		const std::filesystem::path Candidate =
			std::filesystem::absolute(PhysicalPath).lexically_normal();
		for (const auto& [Path, Data] : CaptureAssetCatalogSnapshot().Assets)
		{
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
				return {
					EAssetError::IoError,
					std::format(
						"Could not inspect companion owner package {}: {}",
						Path.ToString(), ExistenceError.message())};
			std::vector<std::filesystem::path> CompanionFiles;
			bool bHasContributor = false;
			const FAssetResult Result = InspectAssetCompanionFiles(
				Data, CompanionFiles, &bHasContributor);
			if (!Result)
				return {
					Result.Error,
					std::format(
						"Could not inspect companion ownership for {}: {}",
						Path.ToString(), Result.Message)};
			if (bHasContributor
				&& std::ranges::find(CompanionFiles, Candidate)
					!= CompanionFiles.end())
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
		return {};
	}
}
