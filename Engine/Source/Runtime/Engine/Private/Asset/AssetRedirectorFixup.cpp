#include "AssetRuntimeStateInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetMutationRegistryInternal.h"
#include "AssetMutationTransactionInternal.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"
#include "Misc/FileTime.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset
{
	using Private::EAssetMutationState;
	using Private::EMutationJournalDuplicatePolicy;
	using Private::EAssetMutationPublicationRole;
	using Private::FAssetMutationJournal;
	using Private::FAssetMutationJournalEntry;
	using Private::FAssetReferenceStoreRegistry;
	using Private::FMutationPackageMetadata;
	using Private::CollectLoadedPackageSoftReferencesForMutation;
	using Private::FingerprintRelocationFile;
	using Private::InitializeMutationJournal;
	using Private::IsMutationJournalRecoveryRequired;
	using Private::GetAssetReferenceStoreRegistry;
	using Private::LoadRelocationBytes;
	using Private::MakePackageFingerprint;
	using Private::NormalizePhysicalPath;
	using Private::PublishRelocationFile;
	using Private::ReadMutationPackageMetadata;
	using Private::RebuildReferenceProjectionForPublishedEntries;
	using Private::RewritePackageReferencesForMutation;
	using Private::StageMutationJournalEntry;
	using Private::TransitionMutationJournalState;
	using Private::WriteMutationJournalState;

	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
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
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			OutBytes.clear();
			std::filesystem::path BulkPath(PhysicalPath);
			BulkPath.replace_extension(".dbulk");
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(BulkPath, ErrorCode))
			{
				if (!ErrorCode || ErrorCode == std::errc::no_such_file_or_directory)
					return {};
				return Error(EAssetError::IoError,
					"A reference rewrite bulk companion could not be inspected.");
			}
			return LoadRelocationBytes(BulkPath, OutBytes);
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
			size_t JournalEntry = 0;
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
			FModuleOwnedResourceLease OwnerResource;
			FModuleOwnedCallbackGate OwnerGate;
			FAssetReferenceStoreHandle Handle = 0;
			IAssetReferenceStore* Store = nullptr;
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetReferenceStoreRewriteContribution Contribution;
			bool bApplied = false;
		};

		auto FindFixupDestination(
			const FPackagePath& Source,
			std::span<const FAssetRedirectorFixupMapping> Mappings)
			-> const FPackagePath*
		{
			const auto It = std::ranges::find(
				Mappings,
				Source,
				&FAssetRedirectorFixupMapping::RedirectorPath);
			return It == Mappings.end() ? nullptr : &It->FinalPath;
		}
	}

	struct FAssetRedirectorFixupState
	{
		EAssetRedirectorFixupMode Mode = EAssetRedirectorFixupMode::RewriteAndDelete;
		uint64 ExpectedRegistryRevision = 0;
		uint64 ExpectedStoreRevision = 0;
		std::vector<FPackagePath> Redirectors;
		std::vector<FAssetRedirectorFixupMapping> Mappings;
		std::vector<FAssetReferenceEdge> PackageOccurrences;
		std::vector<FAssetReferenceStoreOccurrence> StoreOccurrences;
		std::vector<FPackagePath> DeletableRedirectors;
		std::vector<FFixupPackageState> Packages;
		std::vector<FFixupLiveSoftReference> LiveSoftReferences;
		std::vector<FFixupStoreState> Stores;
		FAssetMutationJournal Journal;
		std::unordered_map<FPackagePath, FAssetData> ExpectedAssets;
		std::unordered_map<FPackagePath, FAssetData> PostAssets;
		std::vector<FAssetReferenceEdge> PostEdges;
		std::unordered_map<FPackagePath, FAssetPackageFingerprint> PostFingerprints;
		std::vector<FAssetResult> PostErrors;
		bool bPostIndexComplete = false;
	};

	auto FAssetRedirectorFixupSummary::GetMode() const
		-> EAssetRedirectorFixupMode
	{
		return Mode;
	}

	auto FAssetRedirectorFixupSummary::GetRegistryRevision() const -> uint64
	{
		return RegistryRevision;
	}

	auto FAssetRedirectorFixupSummary::GetRedirectors() const
		-> std::span<const FPackagePath>
	{
		return Redirectors;
	}

	auto FAssetRedirectorFixupSummary::GetFinalPathMappings() const
		-> std::span<const FAssetRedirectorFixupMapping>
	{
		return FinalPathMappings;
	}

	auto FAssetRedirectorFixupSummary::GetPackageOccurrences() const
		-> std::span<const FAssetReferenceEdge>
	{
		return PackageOccurrences;
	}

	auto FAssetRedirectorFixupSummary::GetStoreOccurrences() const
		-> std::span<const FAssetReferenceStoreOccurrence>
	{
		return StoreOccurrences;
	}

	auto FAssetRedirectorFixupSummary::GetDeletableRedirectors() const
		-> std::span<const FPackagePath>
	{
		return DeletableRedirectors;
	}

	auto FAssetMutationCoordinator::PrepareRedirectorFixupState(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		std::shared_ptr<FAssetRedirectorFixupState>& OutState) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		OutState.reset();
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown,
				"Redirector Fix Up is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit redirector Fix Up.");
		if (Redirectors.empty())
			return Error(EAssetError::InvalidPath,
				"Redirector Fix Up requires at least one redirector.");
		if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
			&& !CaptureAssetReferenceIndex().IsComplete())
			return Error(EAssetError::StaleData,
				"Redirector Fix Up cannot delete aliases because the reference index is incomplete.");

		auto State = std::make_shared<FAssetRedirectorFixupState>();
		const FAssetPublicationState Prepared = Registry.CapturePreparedState();
		const auto FindPrepared = [&](const FPackagePath& Path) -> const FAssetData* {
			const auto It = Prepared.Assets.find(Path);
			return It == Prepared.Assets.end() ? nullptr : &It->second;
		};
		State->Mode = Mode;
		State->ExpectedRegistryRevision = GetAssetCatalogRevision();
		State->ExpectedAssets = Prepared.Assets;
		State->PostAssets = Prepared.Assets;
		State->PostFingerprints = Prepared.ReferenceFingerprints;
		State->PostErrors = Prepared.ReferenceErrors;
		State->bPostIndexComplete = Prepared.bReferenceIndexComplete;
		InitializeMutationJournal(
			State->Journal, EAssetMutationOperationKind::RedirectorFixup);

		std::unordered_set<FPackagePath> Closure;
		std::vector<FPackagePath> Pending(Redirectors.begin(), Redirectors.end());
		while (!Pending.empty())
		{
			FPackagePath Alias = std::move(Pending.back());
			Pending.pop_back();
			if (!Alias.IsValid())
				return Error(EAssetError::InvalidPath,
					"Redirector Fix Up contains an invalid path.");
			if (!Closure.insert(Alias).second) continue;
			const FAssetData* Data = FindPrepared(Alias);
			if (!Data)
				return Error(EAssetError::NotFound, std::format(
					"Fix Up redirector {} is not registered.", Alias.ToString()));
			if (Data->EntryKind != EAssetRegistryEntryKind::Redirector)
				return Error(EAssetError::InvalidPackageType, std::format(
					"Fix Up selection {} is not a redirector.", Alias.ToString()));
			if (LoadingPackages.contains(Alias))
				return Error(EAssetError::InUse,
					"A selected redirector is currently loading.");
			if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
				&& FindResidentPackage(Alias))
				return Error(EAssetError::InUse,
					"A loaded redirector must be unloaded before Fix Up deletion.");
			for (FPackagePath Upstream : Durin::Asset::FindRedirectorsTo(Alias))
				Pending.push_back(std::move(Upstream));
		}
		State->Redirectors.assign(Closure.begin(), Closure.end());
		std::ranges::sort(State->Redirectors,
			[](const FPackagePath& Left, const FPackagePath& Right) {
				return Left.GetView() < Right.GetView();
			});
		for (const FPackagePath& Alias : State->Redirectors)
		{
			const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(Alias);
			if (!Resolution)
				return Error(EAssetError::CorruptFile, std::format(
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
		for (const FAssetPackageReferenceEdge& Edge : Prepared.ReferenceEdges)
			if (Closure.contains(Edge.TargetPath))
				CandidatePackages.insert(Edge.SourcePackage);
		for (const FPackagePath& SourcePath : CandidatePackages)
		{
			const FAssetData* Data = FindPrepared(SourcePath);
			if (!Data) return Error(EAssetError::StaleData,
				"A package referencer is no longer registered.");
			FAssetPackageInspection Inspection;
			FAssetResult InspectionResult = InspectAssetPackage(
				Data->PhysicalPath, SourcePath, Inspection);
			if (!InspectionResult) return InspectionResult;
			std::vector<FAssetReferenceEdge> References;
			InspectionResult = ExtractAssetReferences(
				SourcePath, Inspection, References);
			if (!InspectionResult) return InspectionResult;
			State->PostEdges.insert(State->PostEdges.end(),
				std::make_move_iterator(References.begin()),
				std::make_move_iterator(References.end()));
		}
		std::ranges::sort(State->PostEdges, &Private::AssetReferenceLess);
		for (const FAssetReferenceEdge& Edge : State->PostEdges)
		{
			if (!Closure.contains(Edge.TargetPath.GetPackagePath())) continue;
			State->PackageOccurrences.push_back(Edge);
			if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
				&& Closure.contains(Edge.SourcePackage))
				continue;
			++PackageRewriteCounts[Edge.SourcePackage];
		}

		auto AddJournalEntry = [&](const std::filesystem::path& PhysicalPath,
			const FPackagePath& RegistryPath,
			EAssetMutationPublicationRole Role,
			std::optional<std::vector<std::byte>> PreBytes,
			std::optional<std::vector<std::byte>> PostBytes,
			size_t& OutIndex) -> FAssetResult {
			return StageMutationJournalEntry(State->Journal, {
				.PhysicalPath = PhysicalPath,
				.RegistryPath = RegistryPath,
				.Role = Role,
				.bPreExists = PreBytes.has_value(),
				.bPostExists = PostBytes.has_value(),
				.PreBytes = PreBytes
					? std::span<const std::byte>(*PreBytes)
					: std::span<const std::byte>{},
				.PostBytes = PostBytes
					? std::span<const std::byte>(*PostBytes)
					: std::span<const std::byte>{},
				.DuplicatePolicy =
					EMutationJournalDuplicatePolicy::Reject}, OutIndex);
		};

		for (const auto& [SourcePath, ExpectedCount] : PackageRewriteCounts)
		{
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PreparePackage))
				return Error(EAssetError::IoError,
					"Injected Fix Up package-preparation failure.");
			const FAssetData* Data = FindPrepared(SourcePath);
			if (!Data)
				return Error(EAssetError::StaleData,
					"A package referencer is no longer registered.");
			if (LoadingPackages.contains(SourcePath))
				return Error(EAssetError::InUse,
					"A package referencer is currently loading.");
			DPackage* Loaded = FindResidentPackage(SourcePath);
			if (Loaded && Loaded->IsDirty())
				return Error(EAssetError::InUse,
					"A dirty loaded package blocks redirector Fix Up.");
			std::vector<std::byte> PreBytes;
			FAssetResult Result = LoadRelocationBytes(Data->PhysicalPath, PreBytes);
			if (!Result) return Result;
			const auto Fingerprint = Prepared.ReferenceFingerprints.find(SourcePath);
			if (Fingerprint == Prepared.ReferenceFingerprints.end())
				return Error(EAssetError::StaleData,
					"A package referencer has no complete index fingerprint.");
			FAssetPackageFingerprint CurrentFingerprint;
			Result = MakePackageFingerprint(Data->PhysicalPath, PreBytes, CurrentFingerprint);
			if (!Result) return Result;
			if (CurrentFingerprint.FileSize != Fingerprint->second.FileSize
				|| CurrentFingerprint.LastWriteTimeTicks
					!= Fingerprint->second.LastWriteTimeTicks)
				return Error(EAssetError::StaleData,
					"A package referencer changed after reference indexing.");
			std::vector<std::byte> PostBytes;
			std::vector<std::byte> BulkBytes;
			Result = LoadBulkClosure(Data->PhysicalPath, BulkBytes);
			if (!Result) return Result;
			Result = RewritePackageReferencesForMutation(
				PreBytes, BulkBytes, SourcePath,
				State->Mappings, ExpectedCount, PostBytes);
			if (!Result) return Result;
			size_t JournalEntry = 0;
			Result = AddJournalEntry(
				Data->PhysicalPath, SourcePath,
				EAssetMutationPublicationRole::RealAsset,
				std::move(PreBytes), PostBytes, JournalEntry);
			if (!Result) return Result;
			State->Packages.push_back({SourcePath, JournalEntry, Loaded});

			FMutationPackageMetadata PostFile;
			Result = ReadMutationPackageMetadata(
				PostBytes, BulkBytes, SourcePath, PostFile);
			if (!Result) return Result;
			FAssetData& PostData = State->PostAssets.at(SourcePath);
			PostData.AssetClassName = PostFile.AssetClassName;
			PostData.EntryKind = PostFile.EntryKind;
			PostData.RedirectDestination = PostFile.RedirectDestination;
			PostData.FormatVersion = PostFile.FormatVersion;
			PostData.Dependencies = PostFile.Dependencies;
			PostData.SoftDependencies = PostFile.SoftDependencies;

			if (Loaded)
			{
				std::unordered_set<FSoftObjectPtr*> Seen;
				for (const FAssetRedirectorFixupMapping& Mapping : State->Mappings)
				{
					std::vector<FSoftObjectPtr*> Values;
					Result = CollectLoadedPackageSoftReferencesForMutation(
						Loaded, Mapping.RedirectorPath, Values);
					if (!Result) return Result;
					for (FSoftObjectPtr* Value : Values)
					{
						if (!Value || !Seen.insert(Value).second) continue;
						State->LiveSoftReferences.push_back({
							.Value = Value,
							.PrePath = Mapping.RedirectorPath,
							.PostPath = Mapping.FinalPath});
					}
				}
			}
		}

		auto& StoreRegistry = GetAssetReferenceStoreRegistry();
		State->ExpectedStoreRevision = StoreRegistry.Revision;
		for (const auto& [Handle, Entry] : StoreRegistry.Stores)
		{
			IAssetReferenceStore* Store = Entry.Store;
			if (!Store)
				return Error(EAssetError::StaleData,
					"A registered asset reference store is unavailable.");
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.OwnerGate.IsValid() && !Call)
				return Error(EAssetError::StaleData,
					"An asset reference store owner is retiring.");
			FModuleOwnedResourceLease Resource = Entry.OwnerGate.RetainResource();
			if (Entry.OwnerGate.IsValid() && !Resource)
				return Error(EAssetError::StaleData,
					"An asset reference store owner is retiring.");
			FFixupStoreState StoreState{
				.OwnerResource = std::move(Resource),
				.OwnerGate = Entry.OwnerGate,
				.Handle = Handle,
				.Store = Store};
			FAssetResult Result = Store->CaptureSnapshot(StoreState.Snapshot);
			if (!Result) return Result;
			if (StoreState.Snapshot.ProviderId.empty()
				|| StoreState.Snapshot.ProviderVersion == 0
				|| StoreState.Snapshot.Fingerprint.empty())
				return Error(EAssetError::StaleData,
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
					return Error(EAssetError::StaleData,
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
					return Error(EAssetError::IoError,
						"Injected Fix Up store-preparation failure.");
				Result = Store->PrepareRewrite(
					Rewrites, StoreState.Snapshot.Fingerprint,
					StoreState.Contribution);
				if (!Result) return Result;
				if (StoreState.Contribution.Fingerprint
						!= StoreState.Snapshot.Fingerprint
					|| StoreState.Contribution.Rewrites != Rewrites
					|| !StoreState.Contribution.Revalidate
					|| !StoreState.Contribution.Apply
					|| !StoreState.Contribution.Restore
					|| !StoreState.Contribution.Verify)
					return Error(EAssetError::StaleData,
						"An asset reference store returned an incomplete rewrite contribution.");
				for (FAssetReferenceStorePackageRewrite& PackageRewrite :
					StoreState.Contribution.PackageRewrites)
				{
					const FAssetData* Data = FindPrepared(PackageRewrite.PackagePath);
					if (!PackageRewrite.PackagePath.IsValid() || !Data
						|| Data->EntryKind == EAssetRegistryEntryKind::Redirector)
						return Error(EAssetError::StaleData,
							"An asset reference store returned an invalid package participant.");
					if (LoadingPackages.contains(PackageRewrite.PackagePath))
						return Error(EAssetError::InUse,
							"An asset reference-store package is currently loading.");
					DPackage* Loaded = FindResidentPackage(PackageRewrite.PackagePath);
					if (Loaded && Loaded->IsDirty())
						return Error(EAssetError::InUse,
							"A dirty external-reference package blocks redirector Fix Up.");
					std::vector<std::byte> CurrentBytes;
					Result = LoadRelocationBytes(Data->PhysicalPath, CurrentBytes);
					if (!Result) return Result;
					if (CurrentBytes != PackageRewrite.PreBytes)
						return Error(EAssetError::StaleData,
							"An asset reference-store package changed during rewrite preparation.");
					std::vector<std::byte> BulkBytes;
					Result = LoadBulkClosure(Data->PhysicalPath, BulkBytes);
					if (!Result) return Result;
					Result = ValidateAssetPackageBytes(
						PackageRewrite.PostBytes, PackageRewrite.PackagePath, BulkBytes);
					if (!Result) return Result;
					size_t JournalEntry = 0;
					Result = AddJournalEntry(
						Data->PhysicalPath, PackageRewrite.PackagePath,
						EAssetMutationPublicationRole::RealAsset,
						std::move(PackageRewrite.PreBytes),
						PackageRewrite.PostBytes, JournalEntry);
					if (!Result) return Result;
					State->Packages.push_back({
						PackageRewrite.PackagePath, JournalEntry, Loaded});

					FMutationPackageMetadata PostFile;
					Result = ReadMutationPackageMetadata(
						PackageRewrite.PostBytes, BulkBytes,
						PackageRewrite.PackagePath, PostFile);
					if (!Result) return Result;
					FAssetData& PostData = State->PostAssets.at(
						PackageRewrite.PackagePath);
					PostData.AssetClassName = PostFile.AssetClassName;
					PostData.EntryKind = PostFile.EntryKind;
					PostData.RedirectDestination =
						PostFile.RedirectDestination;
					PostData.FormatVersion = PostFile.FormatVersion;
					PostData.Dependencies = PostFile.Dependencies;
					PostData.SoftDependencies = PostFile.SoftDependencies;
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
				return Error(EAssetError::AlreadyExists,
					"Asset reference store provider ids must be unique.");
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
				const FAssetData& Data = State->ExpectedAssets.at(Alias);
				std::vector<std::byte> PreBytes;
				FAssetResult Result = LoadRelocationBytes(Data.PhysicalPath, PreBytes);
				if (!Result) return Result;
				size_t Ignored = 0;
				Result = AddJournalEntry(
					Data.PhysicalPath, Alias,
					EAssetMutationPublicationRole::Redirector,
					std::move(PreBytes), std::nullopt, Ignored);
				if (!Result) return Result;
				State->PostAssets.erase(Alias);
			}
		}

		FAssetResult JournalResult = TransitionMutationJournalState(
			State->Journal, EAssetMutationState::Prepared);
		if (!JournalResult) return JournalResult;
		OutState = std::move(State);
		return {};
	}

	auto FAssetMutationCoordinator::PrepareRedirectorFixupTransaction(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		OutSummary = {};
		OutTransaction = {};
		std::shared_ptr<FAssetRedirectorFixupState> Fixup;
		FAssetResult Result = PrepareRedirectorFixupState(
			Redirectors, Mode, Fixup);
		if (!Result) return Result;

		OutSummary.Mode = Fixup->Mode;
		OutSummary.RegistryRevision = Fixup->ExpectedRegistryRevision;
		OutSummary.Redirectors = Fixup->Redirectors;
		OutSummary.FinalPathMappings = Fixup->Mappings;
		OutSummary.PackageOccurrences = Fixup->PackageOccurrences;
		OutSummary.StoreOccurrences = Fixup->StoreOccurrences;
		OutSummary.DeletableRedirectors = Fixup->DeletableRedirectors;

		auto TransactionState = std::make_shared<FAssetMutationTransaction::FState>();
		TransactionState->Summary = FAssetMutationSummary(
			EAssetMutationOperationKind::RedirectorFixup,
			Fixup->ExpectedRegistryRevision,
			Fixup->Redirectors);
		TransactionState->CommitOperation = [Fixup] {
			return FAssetRuntimeState::Get().GetMutationCoordinator().CommitRedirectorFixup(Fixup);
		};
		TransactionState->IsRecoveryRequired = [Fixup] {
			return IsMutationJournalRecoveryRequired(Fixup->Journal);
		};
		TransactionState->PopulateResultDetails = [Fixup](
			FAssetMutationResultDetails& Details) {
			if (!Details.Result)
			{
				Details.FailedPaths = Fixup->Redirectors;
				return;
			}
			for (const FAssetReferenceEdge& Occurrence :
				Fixup->PackageOccurrences)
				Details.RewrittenPaths.push_back(Occurrence.SourcePackage);
			std::ranges::sort(Details.RewrittenPaths,
				[](const FPackagePath& Left, const FPackagePath& Right) {
					return Left.GetView() < Right.GetView();
				});
			Details.RewrittenPaths.erase(std::ranges::unique(
				Details.RewrittenPaths).begin(), Details.RewrittenPaths.end());
			if (Fixup->Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
				Details.DeletedPaths = Fixup->DeletableRedirectors;
			else
				Details.RetainedPaths = Fixup->Redirectors;
		};
		TransactionState->LastResult.State =
			EAssetMutationTransactionState::Prepared;
		TransactionState->LastResult.RegistryRevision = GetAssetCatalogRevision();
		OutTransaction.State = std::move(TransactionState);
		return {};
	}

	auto FAssetMutationCoordinator::ValidateRedirectorFixupCommit(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return Error(EAssetError::StaleData,
				"The redirector Fix Up transaction state is empty.");
		const auto& State = *Fixup;
		if (State.Journal.State == EAssetMutationState::RecoveryRequired)
			return Error(EAssetError::IoError,
				"AssetMutationRecoveryRequired: the Fix Up journal requires recovery.");
		if (State.Journal.State != EAssetMutationState::Prepared)
			return Error(EAssetError::StaleData,
				"The redirector Fix Up plan is no longer prepared.");
		if (GetAssetCatalogRevision() != State.ExpectedRegistryRevision
			|| CaptureAssetCatalogSnapshot().Assets != State.ExpectedAssets)
			return Error(EAssetError::StaleData,
				"The asset registry changed after redirector Fix Up analysis.");
		const auto& Stores = GetAssetReferenceStoreRegistry();
		if (Stores.Revision != State.ExpectedStoreRevision
			|| Stores.Stores.size() != State.Stores.size())
			return Error(EAssetError::StaleData,
				"Asset reference store registration changed after Fix Up analysis.");
		for (const FFixupStoreState& StoreState : State.Stores)
		{
			const auto Current = Stores.Stores.find(StoreState.Handle);
			if (Current == Stores.Stores.end()
				|| Current->second.Store != StoreState.Store)
				return Error(EAssetError::StaleData,
					"An asset reference store became unavailable.");
			auto Call = StoreState.OwnerGate.TryEnter();
			if (StoreState.OwnerGate.IsValid() && !Call)
				return Error(EAssetError::StaleData,
					"An asset reference store owner is retiring.");
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetResult Result = StoreState.Store->CaptureSnapshot(Snapshot);
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
				return Error(EAssetError::StaleData,
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
					return Error(EAssetError::StaleData,
						"A loaded Fix Up package changed after analysis.");
			}
		}
		for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			std::error_code ExistsError;
			if (!Entry.bPreExists
				|| !std::filesystem::exists(Entry.PhysicalPath, ExistsError)
				|| ExistsError)
				return Error(EAssetError::StaleData,
					"A Fix Up file participant changed occupancy.");
			FAssetPackageFingerprint Fingerprint;
			FAssetResult Result = FingerprintRelocationFile(
				Entry.PhysicalPath, Fingerprint);
			if (!Result) return Result;
			if (Fingerprint != Entry.ExpectedPreFingerprint)
				return Error(EAssetError::StaleData,
					"A Fix Up file participant changed after analysis.");
			if (Entry.bPostExists)
			{
				std::vector<std::byte> StagedBytes;
				Result = LoadRelocationBytes(Entry.StagedPostPath, StagedBytes);
				if (!Result || FXxHash128::HashBuffer(StagedBytes)
						!= Entry.StagedPostHash)
					return Error(EAssetError::StaleData,
						"A staged Fix Up output changed after analysis.");
			}
		}
		return {};
	}

	auto FAssetMutationCoordinator::CommitRedirectorFixup(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return Error(EAssetError::StaleData,
				"The redirector Fix Up transaction state is empty.");
		auto& State = *Fixup;
		FAssetResult Result = ValidateRedirectorFixupCommit(Fixup);
		if (!Result) return Result;
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::StageOriginal))
			return Error(EAssetError::IoError,
				"Injected Fix Up original-staging failure.");

		Result = TransitionMutationJournalState(
			State.Journal, EAssetMutationState::Publishing);
		if (!Result) return Result;
		std::vector<size_t> PublishedPackages;
		std::vector<size_t> PublishedRedirectors;
		size_t ChangedLiveCount = 0;
		auto EnterRecovery = [&](std::string Message) -> FAssetResult {
			FAssetResult JournalResult = TransitionMutationJournalState(
				State.Journal, EAssetMutationState::RecoveryRequired);
			if (!JournalResult)
				return Error(EAssetError::IoError, std::format(
					"AssetMutationRecoveryRequired: {}; additionally failed to persist recovery state: {}",
					Message, JournalResult.Message));
			return Error(EAssetError::IoError,
				std::format("AssetMutationRecoveryRequired: {}", Message));
		};
		auto Compensate = [&](FAssetResult Failure) -> FAssetResult {
			FAssetResult JournalResult = TransitionMutationJournalState(
				State.Journal, EAssetMutationState::Compensating);
			if (!JournalResult)
				return Error(EAssetError::IoError, std::format(
					"AssetMutationRecoveryRequired: compensation did not start because its journal state could not be persisted: {}",
					JournalResult.Message));
			for (auto It = PublishedRedirectors.rbegin();
				It != PublishedRedirectors.rend(); ++It)
			{
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::CompensatePackage))
					return EnterRecovery("redirector compensation was interrupted.");
				Result = PublishRelocationFile(State.Journal.Entries[*It], false);
				if (!Result) return EnterRecovery(Result.Message);
				State.Journal.Entries[*It].bCompensated = true;
				JournalResult = WriteMutationJournalState(State.Journal);
				if (!JournalResult) return EnterRecovery(JournalResult.Message);
			}
			for (size_t Count = ChangedLiveCount; Count > 0; --Count)
			{
				FFixupLiveSoftReference& Live = State.LiveSoftReferences[Count - 1];
				FObjectPath Path;
				FObjectPath::TryCreate(Live.PrePath.GetView(), Path);
				Live.Value->SetPath(std::move(Path));
			}
			for (auto It = State.Stores.rbegin(); It != State.Stores.rend(); ++It)
			{
				if (!It->bApplied) continue;
				auto Call = It->OwnerGate.TryEnter();
				if (It->OwnerGate.IsValid() && !Call)
					return EnterRecovery("reference-store owner retired before compensation.");
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::CompensateStore))
					return EnterRecovery("reference-store compensation was interrupted.");
				FAssetResult RestoreResult = It->Contribution.Restore();
				if (!RestoreResult) return EnterRecovery(RestoreResult.Message);
				It->bApplied = false;
			}
			for (auto It = PublishedPackages.rbegin();
				It != PublishedPackages.rend(); ++It)
			{
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::CompensatePackage))
					return EnterRecovery("package compensation was interrupted.");
				Result = PublishRelocationFile(State.Journal.Entries[*It], false);
				if (!Result) return EnterRecovery(Result.Message);
				State.Journal.Entries[*It].bCompensated = true;
				JournalResult = WriteMutationJournalState(State.Journal);
				if (!JournalResult) return EnterRecovery(JournalResult.Message);
			}
			JournalResult = TransitionMutationJournalState(
				State.Journal, EAssetMutationState::Restored);
			if (!JournalResult)
				return Error(EAssetError::IoError, std::format(
					"AssetMutationRecoveryRequired: compensation completed but the restored journal state could not be persisted: {}",
					JournalResult.Message));
			return Failure;
		};

		uint64 PublicationOrder = 0;
		for (size_t Index = 0; Index < State.Journal.Entries.size(); ++Index)
		{
			FAssetMutationJournalEntry& Entry = State.Journal.Entries[Index];
			if (Entry.Role == EAssetMutationPublicationRole::Redirector) continue;
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishPackage))
				return Compensate(Error(EAssetError::IoError,
					"Injected Fix Up package-publication failure."));
			Entry.PublicationOrder = PublicationOrder++;
			Result = PublishRelocationFile(Entry, true);
			if (!Result) return Compensate(std::move(Result));
			Entry.bCompleted = true;
			PublishedPackages.push_back(Index);
			Result = WriteMutationJournalState(State.Journal);
			if (!Result) return Compensate(std::move(Result));
		}
		for (FFixupStoreState& Store : State.Stores)
		{
			if (Store.Contribution.Rewrites.empty()) continue;
			auto Call = Store.OwnerGate.TryEnter();
			if (Store.OwnerGate.IsValid() && !Call)
				return Compensate(Error(EAssetError::StaleData,
					"An asset reference store owner is retiring."));
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::ApplyStore))
				return Compensate(Error(EAssetError::IoError,
					"Injected Fix Up store-publication failure."));
			Result = Store.Contribution.Apply();
			if (!Result) return Compensate(std::move(Result));
			Store.bApplied = true;
		}
		for (FFixupLiveSoftReference& Live : State.LiveSoftReferences)
		{
			FObjectPath Path;
			if (!FObjectPath::TryCreate(Live.PostPath.GetView(), Path))
				return Compensate(Error(EAssetError::InvalidPath,
					"A prepared live soft-reference destination became invalid."));
			Live.Value->SetPath(std::move(Path));
			++ChangedLiveCount;
		}

		Result = RebuildReferenceProjectionForPublishedEntries(
			State.Journal.Entries, State.PostAssets,
			State.PostEdges, State.PostFingerprints);
		if (!Result) return Compensate(std::move(Result));
		if (State.Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
		{
			std::erase_if(State.PostEdges, [&](const FAssetReferenceEdge& Edge) {
				return std::ranges::binary_search(
					State.Redirectors, Edge.SourcePackage,
					[](const FPackagePath& Left, const FPackagePath& Right) {
						return Left.GetView() < Right.GetView();
					});
			});
			for (const FPackagePath& Alias : State.Redirectors)
				State.PostFingerprints.erase(Alias);
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::Verify))
			return Compensate(Error(EAssetError::IoError,
				"Injected Fix Up verification failure."));
		for (const FAssetReferenceEdge& Edge : State.PostEdges)
			if (FindFixupDestination(Edge.TargetPath.GetPackagePath(), State.Mappings))
				return Compensate(Error(EAssetError::InUse, std::format(
					"Fix Up verification found a remaining package occurrence at {}:{}.",
					Edge.SourcePackage.ToString(), Edge.DisplayRoute)));
		for (FFixupStoreState& Store : State.Stores)
		{
			auto Call = Store.OwnerGate.TryEnter();
			if (Store.OwnerGate.IsValid() && !Call)
				return Compensate(Error(EAssetError::StaleData,
					"An asset reference store owner is retiring."));
			if (Store.Contribution.Verify)
			{
				Result = Store.Contribution.Verify();
				if (!Result) return Compensate(std::move(Result));
			}
			FAssetReferenceStoreSnapshot Snapshot;
			Result = Store.Store->CaptureSnapshot(Snapshot);
			if (!Result) return Compensate(std::move(Result));
			for (const FAssetReferenceStoreOccurrence& Occurrence : Snapshot.Occurrences)
				if (FindFixupDestination(Occurrence.TargetPath, State.Mappings))
					return Compensate(Error(EAssetError::InUse,
						"Fix Up verification found a remaining external occurrence."));
		}

		if (State.Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
		{
			for (size_t Index = 0; Index < State.Journal.Entries.size(); ++Index)
			{
				FAssetMutationJournalEntry& Entry = State.Journal.Entries[Index];
				if (Entry.Role != EAssetMutationPublicationRole::Redirector) continue;
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::DeleteRedirector))
					return Compensate(Error(EAssetError::IoError,
						"Injected Fix Up redirector-deletion failure."));
				Entry.PublicationOrder = PublicationOrder++;
				Result = PublishRelocationFile(Entry, true);
				if (!Result) return Compensate(std::move(Result));
				Entry.bCompleted = true;
				PublishedRedirectors.push_back(Index);
				Result = WriteMutationJournalState(State.Journal);
				if (!Result) return Compensate(std::move(Result));
			}
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishRegistry))
			return Compensate(Error(EAssetError::IoError,
				"Injected Fix Up registry-publication failure."));

		for (const FFixupPackageState& PackageState : State.Packages)
		{
			const FAssetMutationJournalEntry& Entry =
				State.Journal.Entries[PackageState.JournalEntry];
			FAssetData& Data = State.PostAssets.at(PackageState.SourcePath);
			std::error_code MetadataError;
			Data.FileSize = std::filesystem::file_size(Entry.PhysicalPath, MetadataError);
			Data.LastWriteTime = std::filesystem::last_write_time(
				Entry.PhysicalPath, MetadataError);
			if (MetadataError)
				return Compensate(Error(EAssetError::IoError,
					"Could not inspect a published Fix Up package."));
			Data.LastWriteTimeTicks = FileTime::ToStableTicks(
				Data.LastWriteTime);
		}
		std::vector<FAssetPackageReferenceEdge> PackageEdges;
		std::unordered_map<FPackagePath, FAssetPackageFingerprint> PackageFingerprints;
		Result = BuildAssetPackageReferenceProjection(
			State.PostAssets, PackageEdges, PackageFingerprints);
		if (!Result) return Compensate(std::move(Result));
		Result = Registry.PublishPreparedState(State.ExpectedRegistryRevision, {
			.Assets = State.PostAssets,
			.ReferenceEdges = std::move(PackageEdges),
			.ReferenceFingerprints = std::move(PackageFingerprints),
			.ReferenceErrors = State.PostErrors,
			.bReferenceIndexComplete = State.Mode
				== EAssetRedirectorFixupMode::RewriteAndDelete
				? true : State.bPostIndexComplete});
		if (!Result) return Compensate(std::move(Result));
		State.ExpectedRegistryRevision = GetAssetCatalogRevision();
		State.ExpectedAssets = CaptureAssetCatalogSnapshot().Assets;
		Result = TransitionMutationJournalState(
			State.Journal, EAssetMutationState::Committed);
		if (!Result) return Result;
		return {};
	}
}
