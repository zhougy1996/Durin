#include "AssetTools/MutationTesting.h"
#include "AssetMutationStagingInternal.h"
#include "Asset/PackageEditing.h"
#include "Asset/Load.h"
#include "AssetRegistry/Publication.h"
#include "Asset/AssetWriteResult.h"
#include "Asset/RegistryOperations.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"
#include "Misc/FileTime.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	using AssetToolsPrivate::EMutationStagingDuplicatePolicy;
	using AssetToolsPrivate::EAssetMutationPublicationRole;
	using AssetToolsPrivate::FAssetMutationStaging;
	using AssetToolsPrivate::FAssetMutationStagingEntry;
	using AssetToolsPrivate::FingerprintRelocationFile;
	using AssetToolsPrivate::InitializeMutationStaging;
	using AssetToolsPrivate::LoadRelocationBytes;
	using AssetToolsPrivate::NormalizePhysicalPath;
	using AssetToolsPrivate::PublishRelocationFile;
	using AssetToolsPrivate::StageMutationEntry;

	namespace
	{
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}

		struct FFixupFailureInjection
		{
			std::map<EAssetRedirectorFixupFailurePoint, uint32>
				RemainingOccurrences;
		};

		auto GetFixupFailureInjection() -> FFixupFailureInjection&
		{
			static FFixupFailureInjection Injection;
			return Injection;
		}

		auto ConsumeFixupFailure(
			EAssetRedirectorFixupFailurePoint Point) -> bool
		{
			auto& Remaining =
				GetFixupFailureInjection().RemainingOccurrences;
			auto Injected = Remaining.find(Point);
			if (Injected == Remaining.end() || Injected->second == 0)
				return false;
			if (--Injected->second != 0) return false;
			Remaining.erase(Injected);
			return true;
		}

		auto LoadBulkClosure(std::string_view PhysicalPath,
			FByteBuffer& OutBytes) -> FAssetWriteResult
		{
			OutBytes.clear();
			std::filesystem::path BulkPath(PhysicalPath);
			BulkPath.replace_extension(".dbulk");
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(BulkPath, ErrorCode))
			{
				if (!ErrorCode || ErrorCode == std::errc::no_such_file_or_directory)
					return {};
				return Error(EAssetWriteError::IoError,
					"A reference rewrite bulk companion could not be inspected.");
			}
			return AssetWriteResultFromRead(LoadRelocationBytes(BulkPath, OutBytes));
		}
	}

	auto SetAssetRedirectorFixupFailurePointForTesting(
		EAssetRedirectorFixupFailurePoint Point,
		uint32 Occurrence) -> void
	{
		auto& Injection = GetFixupFailureInjection();
		if (Point == EAssetRedirectorFixupFailurePoint::None)
		{
			Injection.RemainingOccurrences.clear();
			return;
		}
		Injection.RemainingOccurrences.insert_or_assign(
			Point, std::max(Occurrence, 1u));
	}

	namespace
	{
		struct FFixupPackageState
		{
			FPackagePath SourcePath;
			size_t StagingEntry = 0;
			DPackage* LoadedPackage = nullptr;
		};

		struct FFixupLiveSoftReference
		{
			FSoftObjectPtr* Value = nullptr;
			FPackagePath PrePath;
			FPackagePath PostPath;
		};

		struct FFixupStoreState
		{
			FAssetReferenceStoreHandle Handle = 0;
			IAssetReferenceStore* Store = nullptr;
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetReferenceStoreRewriteContribution Contribution;
		};

		auto FindFixupDestination(
			const FPackagePath& Source,
			std::span<const FAssetPackageReferenceMapping> Mappings)
			-> const FPackagePath*
		{
			const auto It = std::ranges::find(
				Mappings,
				Source,
				&FAssetPackageReferenceMapping::SourcePath);
			return It == Mappings.end() ? nullptr : &It->DestinationPath;
		}
	}

	struct FAssetRedirectorFixupState;
	static auto PrepareRedirectorFixupState(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		std::shared_ptr<FAssetRedirectorFixupState>& OutState) -> FAssetWriteResult;
	static auto ValidateRedirectorFixupCommit(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetWriteResult;
	static auto CommitRedirectorFixup(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetWriteResult;

	struct FAssetRedirectorFixupState
	{
		EAssetRedirectorFixupMode Mode = EAssetRedirectorFixupMode::RewriteAndDelete;
		uint64 ExpectedRegistryRevision = 0;
		uint64 ExpectedStoreRevision = 0;
		std::vector<FPackagePath> Redirectors;
		std::vector<FAssetPackageReferenceMapping> Mappings;
		std::vector<FAssetReferenceEdge> PackageOccurrences;
		std::vector<FAssetReferenceStoreOccurrence> StoreOccurrences;
		std::vector<FPackagePath> DeletableRedirectors;
		std::vector<FFixupPackageState> Packages;
		std::vector<FFixupLiveSoftReference> LiveSoftReferences;
		std::vector<FFixupStoreState> Stores;
		FAssetMutationStaging Staging;
	};

	static auto PrepareRedirectorFixupState(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		std::shared_ptr<FAssetRedirectorFixupState>& OutState) -> FAssetWriteResult
	{
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return Guard;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		OutState.reset();
		if (Redirectors.empty())
			return Error(EAssetWriteError::InvalidPath,
				"Redirector Fix Up requires at least one redirector.");
		if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
			&& !CaptureAssetReferenceIndex().IsComplete())
			return Error(EAssetWriteError::StaleData,
				"Redirector Fix Up cannot delete aliases because the reference index is incomplete.");

		auto State = std::make_shared<FAssetRedirectorFixupState>();
		const FAssetRegistryPublication Prepared = CaptureAssetRegistryPublication();
		const auto FindPrepared = [&](const FPackagePath& Path) -> const FAssetData* {
			const auto It = Prepared.Assets.find(Path);
			return It == Prepared.Assets.end() ? nullptr : &It->second;
		};
		State->Mode = Mode;
		State->ExpectedRegistryRevision = GetAssetCatalogRevision();
		InitializeMutationStaging(
			State->Staging);

		std::unordered_set<FPackagePath> Closure;
		std::vector<FPackagePath> Pending(Redirectors.begin(), Redirectors.end());
		while (!Pending.empty())
		{
			FPackagePath Alias = std::move(Pending.back());
			Pending.pop_back();
			if (!Alias.IsValid())
				return Error(EAssetWriteError::InvalidPath,
					"Redirector Fix Up contains an invalid path.");
			if (!Closure.insert(Alias).second) continue;
			const FAssetData* Data = FindPrepared(Alias);
			if (!Data)
				return Error(EAssetWriteError::NotFound, std::format(
					"Fix Up redirector {} is not registered.", Alias.ToString()));
			if (Data->EntryKind != EAssetRegistryEntryKind::Redirector)
				return Error(EAssetWriteError::InvalidData, std::format(
					"Fix Up selection {} is not a redirector.", Alias.ToString()));
			if (IsPackageLoading(Alias))
				return Error(EAssetWriteError::InUse,
					"A selected redirector is currently loading.");
			if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
				&& FindResidentPackage(Alias))
				return Error(EAssetWriteError::InUse,
					"A loaded redirector must be unloaded before Fix Up deletion.");
			for (FPackagePath Upstream : Durin::FindRedirectorsTo(Alias))
				Pending.push_back(std::move(Upstream));
		}
		State->Redirectors.assign(Closure.begin(), Closure.end());
		std::ranges::sort(State->Redirectors,
			[](const FPackagePath& Left, const FPackagePath& Right) {
				return Left.GetView() < Right.GetView();
			});
		for (const FPackagePath& Alias : State->Redirectors)
		{
			const FAssetPathResolveResult Resolution = Durin::ResolveAssetPathForOperation(Alias);
			if (!Resolution)
				return Error(EAssetWriteError::InvalidData, std::format(
					"Fix Up could not resolve {} (state {}).", Alias.ToString(),
					static_cast<uint32>(Resolution.State)));
			State->Mappings.push_back({Alias, Resolution.FinalPath});
		}
		if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
			State->DeletableRedirectors = State->Redirectors;

		std::map<FPackagePath, uint64, decltype([](const FPackagePath& Left,
			const FPackagePath& Right) { return Left.GetView() < Right.GetView(); })>
			PackageRewriteCounts;
		std::unordered_set<FPackagePath> CandidatePackages;
		std::vector<FAssetReferenceEdge> CandidateEdges;
		for (const FAssetPackageReferenceEdge& Edge : Prepared.ReferenceEdges)
			if (Closure.contains(Edge.TargetPath))
				CandidatePackages.insert(Edge.SourcePackage);
		for (const FPackagePath& SourcePath : CandidatePackages)
		{
			const FAssetData* Data = FindPrepared(SourcePath);
			if (!Data) return Error(EAssetWriteError::StaleData,
				"A package referencer is no longer registered.");
			FAssetPackageInspection Inspection;
			FAssetWriteResult InspectionResult = AssetWriteResultFromRead(InspectAssetPackage(
				Data->PhysicalPath, SourcePath, Inspection));
			if (!InspectionResult) return InspectionResult;
			std::vector<FAssetReferenceEdge> References;
			InspectionResult = AssetWriteResultFromRead(ExtractAssetReferences(
				SourcePath, Inspection, References));
			if (!InspectionResult) return InspectionResult;
			CandidateEdges.insert(CandidateEdges.end(),
				std::make_move_iterator(References.begin()),
				std::make_move_iterator(References.end()));
		}
		std::ranges::sort(CandidateEdges, &AssetReferenceLess);
		for (const FAssetReferenceEdge& Edge : CandidateEdges)
		{
			if (!Closure.contains(Edge.TargetPath.GetPackagePath())) continue;
			State->PackageOccurrences.push_back(Edge);
			if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
				&& Closure.contains(Edge.SourcePackage))
				continue;
			++PackageRewriteCounts[Edge.SourcePackage];
		}

		auto AddStagingEntry = [&](const std::filesystem::path& PhysicalPath,
			const FPackagePath& RegistryPath,
			EAssetMutationPublicationRole Role,
			std::optional<FByteBuffer> PreBytes,
			std::optional<FByteBuffer> PostBytes,
			size_t& OutIndex) -> FAssetWriteResult {
			return StageMutationEntry(State->Staging, {
				.PhysicalPath = PhysicalPath,
				.RegistryPath = RegistryPath,
				.Role = Role,
				.bPreExists = PreBytes.has_value(),
				.bPostExists = PostBytes.has_value(),
				.PreBytes = PreBytes
					? FByteView(*PreBytes)
					: FByteView{},
				.PostBytes = PostBytes
					? FByteView(*PostBytes)
					: FByteView{},
				.DuplicatePolicy =
					EMutationStagingDuplicatePolicy::Reject}, OutIndex);
		};

		for (const auto& [SourcePath, ExpectedCount] : PackageRewriteCounts)
		{
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PreparePackage))
				return Error(EAssetWriteError::IoError,
					"Injected Fix Up package-preparation failure.");
			const FAssetData* Data = FindPrepared(SourcePath);
			if (!Data)
				return Error(EAssetWriteError::StaleData,
					"A package referencer is no longer registered.");
			if (IsPackageLoading(SourcePath))
				return Error(EAssetWriteError::InUse,
					"A package referencer is currently loading.");
			DPackage* Loaded = FindResidentPackage(SourcePath);
			if (Loaded && Loaded->IsDirty())
				return Error(EAssetWriteError::InUse,
					"A dirty loaded package blocks redirector Fix Up.");
			FByteBuffer PreBytes;
			FAssetWriteResult Result = AssetWriteResultFromRead(LoadRelocationBytes(Data->PhysicalPath, PreBytes));
			if (!Result) return Result;
			const auto Fingerprint = Prepared.ReferenceFingerprints.find(SourcePath);
			if (Fingerprint == Prepared.ReferenceFingerprints.end())
				return Error(EAssetWriteError::StaleData,
					"A package referencer has no complete index fingerprint.");
			FAssetPackageFingerprint CurrentFingerprint;
			Result = AssetWriteResultFromRead(FingerprintAssetPackageBytes(Data->PhysicalPath, PreBytes, CurrentFingerprint));
			if (!Result) return Result;
			if (CurrentFingerprint.FileSize != Fingerprint->second.FileSize
				|| CurrentFingerprint.LastWriteTimeTicks
					!= Fingerprint->second.LastWriteTimeTicks)
				return Error(EAssetWriteError::StaleData,
					"A package referencer changed after reference indexing.");
			FByteBuffer PostBytes;
			FByteBuffer BulkBytes;
			Result = LoadBulkClosure(Data->PhysicalPath, BulkBytes);
			if (!Result) return Result;
			Result = RewriteAssetPackageReferences(
				PreBytes, BulkBytes, SourcePath,
				State->Mappings, ExpectedCount, PostBytes);
			if (!Result) return Result;
			size_t StagingEntry = 0;
			Result = AddStagingEntry(
				Data->PhysicalPath, SourcePath,
				EAssetMutationPublicationRole::RealAsset,
				std::move(PreBytes), PostBytes, StagingEntry);
			if (!Result) return Result;
			State->Packages.push_back({SourcePath, StagingEntry, Loaded});

			if (Loaded)
			{
				std::unordered_set<FSoftObjectPtr*> Seen;
				for (const FAssetPackageReferenceMapping& Mapping : State->Mappings)
				{
					std::vector<FSoftObjectPtr*> Values;
					Result = AssetWriteResultFromRead(CollectLoadedAssetPackageSoftReferences(
						Loaded, Mapping.SourcePath, Values));
					if (!Result) return Result;
					for (FSoftObjectPtr* Value : Values)
					{
						if (!Value || !Seen.insert(Value).second) continue;
						State->LiveSoftReferences.push_back({
							.Value = Value,
							.PrePath = Mapping.SourcePath,
							.PostPath = Mapping.DestinationPath});
					}
				}
			}
		}

		const auto StoreRegistry = CaptureAssetReferenceStoreRegistrations();
		State->ExpectedStoreRevision = StoreRegistry.Revision;
		for (const auto [Handle, Store] : StoreRegistry.Stores)
		{
			if (!Store)
				return Error(EAssetWriteError::StaleData,
					"A registered asset reference store is unavailable.");

			FFixupStoreState StoreState{
				.Handle = Handle,
				.Store = Store};
			FAssetWriteResult Result = AssetWriteResultFromRead(Store->CaptureSnapshot(StoreState.Snapshot));
			if (!Result) return Result;
			if (GetAssetReferenceStoreRevision() != State->ExpectedStoreRevision)
				return Error(EAssetWriteError::StaleData, "Asset reference store registration changed during capture.");
			if (StoreState.Snapshot.ProviderId.empty()
				|| StoreState.Snapshot.ProviderVersion == 0
				|| StoreState.Snapshot.Fingerprint.empty())
				return Error(EAssetWriteError::StaleData,
					"An asset reference store returned an invalid identity or fingerprint.");
			std::ranges::sort(StoreState.Snapshot.Occurrences,
				[](const FAssetReferenceStoreOccurrence& Left,
					const FAssetReferenceStoreOccurrence& Right) {
					if (Left.StableId != Right.StableId)
						return Left.StableId < Right.StableId;
					return Left.TargetPath.GetView() < Right.TargetPath.GetView();
				});
			std::vector<FAssetReferenceRewrite> Rewrites;
			for (const FAssetReferenceStoreOccurrence& Occurrence :
				StoreState.Snapshot.Occurrences)
			{
				if (Occurrence.ProviderId != StoreState.Snapshot.ProviderId
					|| Occurrence.StableId.empty())
					return Error(EAssetWriteError::StaleData,
						"An asset reference store returned an invalid occurrence.");
				if (const FPackagePath* Destination = FindFixupDestination(
						Occurrence.TargetPath, State->Mappings))
				{
					State->StoreOccurrences.push_back(Occurrence);
					Rewrites.push_back({
						.StableId = Occurrence.StableId,
						.SourcePath = Occurrence.TargetPath,
						.DestinationPath = *Destination});
				}
			}
			if (!Rewrites.empty())
			{
				if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PrepareStore))
					return Error(EAssetWriteError::IoError,
						"Injected Fix Up store-preparation failure.");
				Result = Store->PrepareRewrite(
					Rewrites, StoreState.Snapshot.Fingerprint,
					StoreState.Contribution);
				if (!Result) return Result;
				if (GetAssetReferenceStoreRevision() != State->ExpectedStoreRevision)
					return Error(EAssetWriteError::StaleData, "Asset reference store registration changed during preparation.");
				if (StoreState.Contribution.Fingerprint
						!= StoreState.Snapshot.Fingerprint
					|| StoreState.Contribution.Rewrites != Rewrites
					|| !StoreState.Contribution.Revalidate
					|| !StoreState.Contribution.Apply
					|| !StoreState.Contribution.Verify)
					return Error(EAssetWriteError::StaleData,
						"An asset reference store returned an incomplete rewrite contribution.");
				for (FAssetReferenceStorePackageRewrite& PackageRewrite :
					StoreState.Contribution.PackageRewrites)
				{
					const FAssetData* Data = FindPrepared(PackageRewrite.PackagePath);
					if (!PackageRewrite.PackagePath.IsValid() || !Data
						|| Data->EntryKind == EAssetRegistryEntryKind::Redirector)
						return Error(EAssetWriteError::StaleData,
							"An asset reference store returned an invalid package participant.");
					if (IsPackageLoading(PackageRewrite.PackagePath))
						return Error(EAssetWriteError::InUse,
							"An asset reference-store package is currently loading.");
					DPackage* Loaded = FindResidentPackage(PackageRewrite.PackagePath);
					if (Loaded && Loaded->IsDirty())
						return Error(EAssetWriteError::InUse,
							"A dirty external-reference package blocks redirector Fix Up.");
					FByteBuffer CurrentBytes;
					Result = AssetWriteResultFromRead(LoadRelocationBytes(Data->PhysicalPath, CurrentBytes));
					if (!Result) return Result;
					if (CurrentBytes != PackageRewrite.PreBytes)
						return Error(EAssetWriteError::StaleData,
							"An asset reference-store package changed during rewrite preparation.");
					FByteBuffer BulkBytes;
					Result = LoadBulkClosure(Data->PhysicalPath, BulkBytes);
					if (!Result) return Result;
					Result = AssetWriteResultFromRead(ValidateAssetPackageBytes(
						PackageRewrite.PostBytes, PackageRewrite.PackagePath, BulkBytes));
					if (!Result) return Result;
					size_t StagingEntry = 0;
					Result = AddStagingEntry(
						Data->PhysicalPath, PackageRewrite.PackagePath,
						EAssetMutationPublicationRole::RealAsset,
						std::move(PackageRewrite.PreBytes),
						PackageRewrite.PostBytes, StagingEntry);
					if (!Result) return Result;
					State->Packages.push_back({
						PackageRewrite.PackagePath, StagingEntry, Loaded});

				}
			}
			State->Stores.push_back(std::move(StoreState));
		}
		std::ranges::sort(State->Stores,
			[](const FFixupStoreState& Left, const FFixupStoreState& Right) {
				return Left.Snapshot.ProviderId < Right.Snapshot.ProviderId;
			});
		for (size_t Index = 1; Index < State->Stores.size(); ++Index)
			if (State->Stores[Index - 1].Snapshot.ProviderId
				== State->Stores[Index].Snapshot.ProviderId)
				return Error(EAssetWriteError::AlreadyExists,
					"Asset reference store provider ids must be unique.");
		for (const FFixupStoreState& Store : State->Stores)
		{
			if (Store.Contribution.Rewrites.empty()) continue;
		}
		std::ranges::sort(State->StoreOccurrences,
			[](const FAssetReferenceStoreOccurrence& Left,
				const FAssetReferenceStoreOccurrence& Right) {
				if (Left.ProviderId != Right.ProviderId)
					return Left.ProviderId < Right.ProviderId;
				if (Left.StableId != Right.StableId)
					return Left.StableId < Right.StableId;
				return Left.TargetPath.GetView() < Right.TargetPath.GetView();
			});

		if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
		{
			for (const FPackagePath& Alias : State->Redirectors)
			{
				const FAssetData& Data = Prepared.Assets.at(Alias);
				FByteBuffer PreBytes;
				FAssetWriteResult Result = AssetWriteResultFromRead(LoadRelocationBytes(Data.PhysicalPath, PreBytes));
				if (!Result) return Result;
				size_t Ignored = 0;
				Result = AddStagingEntry(
					Data.PhysicalPath, Alias,
					EAssetMutationPublicationRole::Redirector,
					std::move(PreBytes), std::nullopt, Ignored);
				if (!Result) return Result;
			}
		}

		OutState = std::move(State);
		return {};
	}

	static auto FixUpRedirectorsImpl(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails
	{
		std::shared_ptr<FAssetRedirectorFixupState> Fixup;
		FAssetMutationResultDetails Details;
		Details.Result = PrepareRedirectorFixupState(Redirectors, Mode, Fixup);
		if (!Details.Result) return Details;
		if (BeforeCommit) BeforeCommit();
		Details.Result = CommitRedirectorFixup(Fixup);
		Details.AffectedFiles = Fixup->Staging.PublishedFiles;
		if (Fixup->Staging.bRetainBackups)
			Details.BackupLocations = Fixup->Staging.Roots;
		return Details;
	}

	static auto ValidateRedirectorFixupCommit(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetWriteResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return Error(EAssetWriteError::StaleData,
				"The redirector Fix Up job state is empty.");
		const auto& State = *Fixup;
		if (GetAssetCatalogRevision() != State.ExpectedRegistryRevision)
			return Error(EAssetWriteError::StaleData,
				"The asset registry changed after redirector Fix Up analysis.");
		const auto Stores = CaptureAssetReferenceStoreRegistrations();
		if (Stores.Revision != State.ExpectedStoreRevision
			|| Stores.Stores.size() != State.Stores.size())
			return Error(EAssetWriteError::StaleData,
				"Asset reference store registration changed after Fix Up analysis.");
		for (const FFixupStoreState& StoreState : State.Stores)
		{
			const auto Current = Stores.Stores.find(StoreState.Handle);
			if (Current == Stores.Stores.end()
				|| Current->second != StoreState.Store)
				return Error(EAssetWriteError::StaleData,
					"An asset reference store became unavailable.");

			FAssetReferenceStoreSnapshot Snapshot;
			FAssetWriteResult Result = AssetWriteResultFromRead(StoreState.Store->CaptureSnapshot(Snapshot));
			if (!Result) return Result;
			std::ranges::sort(Snapshot.Occurrences,
				[](const FAssetReferenceStoreOccurrence& Left,
					const FAssetReferenceStoreOccurrence& Right) {
					if (Left.StableId != Right.StableId)
						return Left.StableId < Right.StableId;
					return Left.TargetPath.GetView() < Right.TargetPath.GetView();
				});
			if (Snapshot.ProviderId != StoreState.Snapshot.ProviderId
				|| Snapshot.ProviderVersion != StoreState.Snapshot.ProviderVersion
				|| Snapshot.Fingerprint != StoreState.Snapshot.Fingerprint
				|| Snapshot.Occurrences != StoreState.Snapshot.Occurrences)
				return Error(EAssetWriteError::StaleData,
					"An asset reference store changed after Fix Up analysis.");
			if (StoreState.Contribution.Revalidate)
			{
				Result = StoreState.Contribution.Revalidate();
				if (!Result) return Result;
			}
		}
		for (const FFixupPackageState& PackageState : State.Packages)
		{
			if (PackageState.LoadedPackage)
			{
				if (FindResidentPackage(PackageState.SourcePath)
						!= PackageState.LoadedPackage
					|| PackageState.LoadedPackage->IsDirty())
					return Error(EAssetWriteError::StaleData,
						"A loaded Fix Up package changed after analysis.");
			}
		}
		for (const FAssetMutationStagingEntry& Entry : State.Staging.Entries)
		{
			std::error_code ExistsError;
			if (!Entry.bPreExists
				|| !std::filesystem::exists(Entry.PhysicalPath, ExistsError)
				|| ExistsError)
				return Error(EAssetWriteError::StaleData,
					"A Fix Up file participant changed occupancy.");
			FAssetPackageFingerprint Fingerprint;
			FAssetWriteResult Result = AssetWriteResultFromRead(FingerprintRelocationFile(
				Entry.PhysicalPath, Fingerprint));
			if (!Result) return Result;
			if (Fingerprint != Entry.ExpectedPreFingerprint)
				return Error(EAssetWriteError::StaleData,
					"A Fix Up file participant changed after analysis.");
			if (Entry.bPostExists)
			{
				FByteBuffer StagedBytes;
				Result = AssetWriteResultFromRead(LoadRelocationBytes(Entry.StagedPostPath, StagedBytes));
				if (!Result || FXxHash128::HashBuffer(StagedBytes)
						!= Entry.StagedPostHash)
					return Error(EAssetWriteError::StaleData,
						"A staged Fix Up output changed after analysis.");
			}
		}
		return {};
	}

	static auto CommitRedirectorFixup(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetWriteResult
	{
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return Guard;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return Error(EAssetWriteError::StaleData,
				"The redirector Fix Up job state is empty.");
		auto& State = *Fixup;
		FAssetWriteResult Result = ValidateRedirectorFixupCommit(Fixup);
		if (!Result) return Result;
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::StageOriginal))
			return Error(EAssetWriteError::IoError,
				"Injected Fix Up original-staging failure.");

		State.Staging.bRetainBackups = true;
		bool bStoreWriteAttempted = false;
		auto PublicationFailed = [&](std::string Message) -> FAssetWriteResult {
			std::vector<FPackagePath> Paths = State.Redirectors;
			for (const FFixupPackageState& Package : State.Packages)
				Paths.push_back(Package.SourcePath);
			FenceAssetRegistryProjection(Paths);
			return {
				.Error = EAssetWriteError::IoError,
				.Message = std::format(
					"AssetMutationFailed: operation {} stopped; inspect partial changes before preparing another operation. {}",
					State.Staging.OperationId, Message),
				.Effect = State.Staging.PublishedFiles.empty() && !bStoreWriteAttempted ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten};
		};

		for (size_t Index = 0; Index < State.Staging.Entries.size(); ++Index)
		{
			FAssetMutationStagingEntry& Entry = State.Staging.Entries[Index];
			if (Entry.Role == EAssetMutationPublicationRole::Redirector) continue;
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishPackage))
				return PublicationFailed("Injected Fix Up package-publication failure.");
			Result = PublishRelocationFile(Entry);
			if (!Result) return PublicationFailed(Result.Message);
			State.Staging.PublishedFiles.push_back(Entry.PhysicalPath);
		}
		for (FFixupStoreState& Store : State.Stores)
		{
			if (Store.Contribution.Rewrites.empty()) continue;

			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::ApplyStore))
				return PublicationFailed("Injected Fix Up store-publication failure.");
			bStoreWriteAttempted = true;
			Result = Store.Contribution.Apply();
			if (!Result) return PublicationFailed(Result.Message);
		}
		for (FFixupLiveSoftReference& Live : State.LiveSoftReferences)
		{
			FObjectPath Path;
			if (!FObjectPath::TryCreate(Live.PostPath.GetView(), Path))
				return PublicationFailed(
					"A prepared live soft-reference destination became invalid.");
			Live.Value->SetPath(std::move(Path));
		}

		std::vector<FAssetReferenceEdge> VerifiedEdges;
		std::unordered_set<FPackagePath> VerifiedPackages;
		for (const FFixupPackageState& Package : State.Packages)
		{
			if (!VerifiedPackages.insert(Package.SourcePath).second) continue;
			const FAssetMutationStagingEntry& Entry =
				State.Staging.Entries[Package.StagingEntry];
			FAssetPackageInspection Inspection;
			Result = AssetWriteResultFromRead(InspectAssetPackage(
				Entry.PhysicalPath.generic_string(), Package.SourcePath, Inspection));
			if (!Result) return PublicationFailed(Result.Message);
			std::vector<FAssetReferenceEdge> References;
			Result = AssetWriteResultFromRead(ExtractAssetReferences(
				Package.SourcePath, Inspection, References));
			if (!Result) return PublicationFailed(Result.Message);
			VerifiedEdges.insert(VerifiedEdges.end(),
				std::make_move_iterator(References.begin()),
				std::make_move_iterator(References.end()));
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::Verify))
			return PublicationFailed("Injected Fix Up verification failure.");
		for (const FAssetReferenceEdge& Edge : VerifiedEdges)
			if (FindFixupDestination(Edge.TargetPath.GetPackagePath(), State.Mappings))
				return PublicationFailed(std::format(
					"Fix Up verification found a remaining package occurrence at {}:{}.",
					Edge.SourcePackage.ToString(), Edge.DisplayRoute));
		for (FFixupStoreState& Store : State.Stores)
		{
			if (Store.Contribution.Verify)
			{
				Result = Store.Contribution.Verify();
				if (!Result) return PublicationFailed(Result.Message);
			}
			FAssetReferenceStoreSnapshot Snapshot;
			Result = AssetWriteResultFromRead(Store.Store->CaptureSnapshot(Snapshot));
			if (!Result) return PublicationFailed(Result.Message);
			for (const FAssetReferenceStoreOccurrence& Occurrence : Snapshot.Occurrences)
				if (FindFixupDestination(Occurrence.TargetPath, State.Mappings))
					return PublicationFailed(
						"Fix Up verification found a remaining external occurrence.");
		}

		if (State.Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
		{
			for (size_t Index = 0; Index < State.Staging.Entries.size(); ++Index)
			{
				FAssetMutationStagingEntry& Entry = State.Staging.Entries[Index];
				if (Entry.Role != EAssetMutationPublicationRole::Redirector) continue;
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::DeleteRedirector))
					return PublicationFailed(
						"Injected Fix Up redirector-deletion failure.");
				Result = PublishRelocationFile(Entry);
				if (!Result) return PublicationFailed(Result.Message);
				State.Staging.PublishedFiles.push_back(Entry.PhysicalPath);
			}
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishRegistry))
			return PublicationFailed("Injected Fix Up Registry-publication failure.");

		std::vector<FPackagePath> FencedPaths = State.Redirectors;
		for (const FFixupPackageState& Package : State.Packages)
			FencedPaths.push_back(Package.SourcePath);
		Result = AssetWriteResultFromRead(RefreshSavedPackages(FencedPaths));
		if (!Result) return PublicationFailed(Result.Message);
		State.Staging.bRetainBackups = false;
		return {};
	}

	auto FixUpRedirectors(std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode) -> FAssetMutationResultDetails
	{ return FixUpRedirectorsImpl(Redirectors, Mode, {}); }

	auto FixUpRedirectorsWithBeforeCommitForTesting(
		std::span<const FPackagePath> Redirectors, EAssetRedirectorFixupMode Mode,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails
	{ return FixUpRedirectorsImpl(Redirectors, Mode, BeforeCommit); }
}
