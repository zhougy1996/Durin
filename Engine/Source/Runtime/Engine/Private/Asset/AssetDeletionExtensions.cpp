#include "AssetCatalogStoreInternal.h"
#include "AssetDeletionInternal.h"
#include "Asset/EditorBulkDataStorage.h"

#include "DObject/Class.h"

namespace Durin::Asset
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
		FModuleOwnedResourceLease OwnerResource;
		FAssetDeleteContributor Contributor;
		FModuleOwnedCallbackGate OwnerGate;
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
		FAssetPackageInspection Inspection;
		FAssetResult InspectionResult = InspectAssetPackage(Data.PhysicalPath, Inspection);
		if (!InspectionResult) return InspectionResult;
		std::string BulkError;
		if (!InspectEditorBulkDataCompanionPaths(
				Data.PhysicalPath, Inspection, OutFiles, &BulkError))
			return Error(EAssetError::CorruptFile, std::move(BulkError));
		if (OutHasContributor && !OutFiles.empty()) *OutHasContributor = true;
		DClass* AssetClass = FindClassByQualifiedName(FName(Data.AssetClassName));
		for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
		{
			const auto It = GetDeleteContributors().find(Class);
			if (It == GetDeleteContributors().end()) continue;
			if (OutHasContributor) *OutHasContributor = true;
			auto Call = It->second.OwnerGate.TryEnter();
			if (It->second.OwnerGate.IsValid() && !Call)
				return Error(EAssetError::StaleData,
					"The asset deletion contributor is unavailable.");
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

	namespace Private
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
		FAssetDeleteContributor Contributor,
		FModuleOwnedCallbackGate OwnerGate) -> FAssetDeleteContributorHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (!Class || !Contributor || (OwnerGate.IsValid() && !Call)) return 0;
		auto& Contributors = GetDeleteContributors();
		if (Contributors.contains(Class)) return 0;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		auto& NextHandle = NextDeleteContributorHandle();
		const FAssetDeleteContributorHandle Handle = NextHandle++;
		Contributors.emplace(Class, FRegisteredDeleteContributor{
			.Handle = Handle,
			.OwnerResource = std::move(Resource),
			.Contributor = std::move(Contributor),
			.OwnerGate = std::move(OwnerGate),
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
		for (const auto& [Path, Data] : GetAssetCatalogStore().GetAssets())
		{
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
			[](const FAssetPath& A, const FAssetPath& B) {
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
