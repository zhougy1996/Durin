#include "Asset/References.h"
#include "AssetTools/MutationTesting.h"
#include "AssetMutationInternal.h"
#include "AssetTools/AssetRenameReferences.h"
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
#include "HAL/PlatformFile.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	using AssetToolsPrivate::FAssetReferenceFixup;
	using AssetToolsPrivate::FSoftObjectPathFixup;
	using AssetToolsPrivate::CollectSoftObjectPathFixups;
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
		auto Reject(std::optional<UnifiedError::FError>* OutError, UnifiedError::FError Error) -> bool
		{ if (OutError) *OutError = std::move(Error); return false; }
		auto WriteFailure(UnifiedError::FErrorId Code, std::string Message) -> FAssetWriteResult
		{ return {.Error = UnifiedError::FError{Code, std::move(Message)}};
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
			FByteBuffer& OutBytes, std::optional<UnifiedError::FError>* OutError) -> bool
		{
		if (OutError) OutError->reset();
			OutBytes.clear();
			std::filesystem::path BulkPath(PhysicalPath);
			BulkPath.replace_extension(".dbulk");
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(BulkPath, ErrorCode))
			{
				if (!ErrorCode || ErrorCode == std::errc::no_such_file_or_directory)
					return true;
				return Reject(OutError, ToError(FFileSystemError{std::format("Failed to inspect: {}.", FFilePath(BulkPath).generic_string()), ErrorCode}));
			}
			{
				auto Prepared = LoadRelocationBytes(BulkPath, OutBytes);
				if (!Prepared) return Reject(OutError, std::move(Prepared.error()));
				return true;
			}
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
		EPackageAliasFixupMode Mode, FAssetReferenceFixup& Facts,
		std::optional<UnifiedError::FError>* OutError) -> std::shared_ptr<FAssetRedirectorFixupState>;
	static auto ValidateRedirectorFixupCommit(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup, std::optional<UnifiedError::FError>* OutError) -> bool;
	static auto CommitRedirectorFixup(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetWriteResult;

	struct FAssetRedirectorFixupState
	{
		EPackageAliasFixupMode Mode = EPackageAliasFixupMode::RewriteAndDelete;
		uint64 ExpectedRegistryRevision = 0;
		uint64 ExpectedStoreRevision = 0;
		std::vector<FPackagePath> Redirectors;
		std::vector<FAssetPackageReferenceMapping> Mappings;
		std::vector<FFixupPackageState> Packages;
		std::vector<FSoftObjectPathFixup> SoftReferences;
		std::vector<FFixupStoreState> Stores;
		FAssetMutationStaging Staging;
	};

	static auto PrepareRedirectorFixupState(
		std::span<const FPackagePath> Redirectors,
		EPackageAliasFixupMode Mode, FAssetReferenceFixup& Facts,
		std::optional<UnifiedError::FError>* OutError) -> std::shared_ptr<FAssetRedirectorFixupState>
	{
		if (OutError) OutError->reset();
		const auto RejectPreparation = [&](UnifiedError::FError Error) -> std::shared_ptr<FAssetRedirectorFixupState> {
			if (OutError) *OutError = std::move(Error);
			return {};
		};
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return RejectPreparation(Guard.error());
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (Redirectors.empty())
			return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
				"Redirector Fix Up requires at least one redirector."});
		if (Mode == EPackageAliasFixupMode::RewriteAndDelete
			&& !CaptureAssetReferenceIndex().IsComplete())
			return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
				"Redirector Fix Up cannot delete aliases because the reference index is incomplete."});

		auto State = std::make_shared<FAssetRedirectorFixupState>();
		const auto& Prepared = Facts.GetRegistry();
		const auto FindPrepared = [&](const FPackagePath& Path) -> const FPackageData* {
			const auto It = Prepared.Catalog.Assets.find(Path);
			return It == Prepared.Catalog.Assets.end() ? nullptr : &It->second;
		};
		State->Mode = Mode;
		State->ExpectedRegistryRevision = Prepared.Revision;
		InitializeMutationStaging(
			State->Staging);

		std::unordered_set<FPackagePath> Closure;
		std::vector<FPackagePath> Pending(Redirectors.begin(), Redirectors.end());
		while (!Pending.empty())
		{
			FPackagePath Alias = std::move(Pending.back());
			Pending.pop_back();
			if (!Alias.IsValid())
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
					"Redirector Fix Up contains an invalid path."});
			if (!Closure.insert(Alias).second) continue;
			const FPackageData* Data = FindPrepared(Alias);
			if (!Data)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::NotFound, std::format(
					"Fix Up redirector {} is not registered.", Alias.ToString())});
			if (!(!Data->TopLevelAssets.empty() && std::ranges::all_of(Data->TopLevelAssets, &FAssetData::IsRedirector)))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidData, std::format(
					"Fix Up selection {} is not a redirector.", Alias.ToString())});
			if (IsPackageLoading(Alias))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
					"A selected redirector is currently loading."});
			if (Mode == EPackageAliasFixupMode::RewriteAndDelete
				&& FindResidentPackage(Alias))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
					"A loaded redirector must be unloaded before Fix Up deletion."});
			for (FPackagePath Upstream : FindRedirectorsTo(Alias))
				Pending.push_back(std::move(Upstream));
		}
		State->Redirectors.assign(Closure.begin(), Closure.end());
		std::ranges::sort(State->Redirectors,
			[](const FPackagePath& Left, const FPackagePath& Right) {
				return Left.GetView() < Right.GetView();
			});
		for (const FPackagePath& Alias : State->Redirectors)
		{
			const FPackagePathResolveResult Resolution = ResolvePackagePathForOperation(Alias);
			if (!Resolution)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidData, std::format(
					"Fix Up could not resolve {} (state {}).", Alias.ToString(),
					static_cast<uint32>(Resolution.State))});
			FAssetPackageReferenceMapping Mapping{Alias, Resolution.FinalPath};
			for (const auto& Asset : FindPrepared(Alias)->TopLevelAssets)
			{
				const auto ObjectResolution = ResolveAssetObjectPathForOperation(FSoftObjectPath(Asset.AssetPath));
				if (!ObjectResolution) return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidData, "An exact redirector destination could not be resolved."});
				Mapping.Objects.emplace_back(Asset.AssetPath, ObjectResolution.FinalPath.GetAssetPath());
			}
			State->Mappings.push_back(std::move(Mapping));
		}
		std::vector<FAssetRenameData> ObjectRenames;
		for (const auto& Mapping : State->Mappings)
			for (const auto& [Old, New] : Mapping.Objects) ObjectRenames.emplace_back(FSoftObjectPath(Old), FSoftObjectPath(New), true);

		std::map<FPackagePath, uint64, decltype([](const FPackagePath& Left,
			const FPackagePath& Right) { return Left.GetView() < Right.GetView(); })>
			PackageRewriteCounts;
		std::unordered_set<FPackagePath> CandidatePackages;
		std::vector<FAssetReferenceEdge> CandidateEdges;
		for (const FAssetPackageReferenceEdge& Edge : Prepared.References.GetEdges())
			if (Closure.contains(Edge.TargetPath))
				CandidatePackages.insert(Edge.SourcePackage);
		for (const FPackagePath& SourcePath : CandidatePackages)
		{
			const FPackageData* Data = FindPrepared(SourcePath);
			if (!Data) return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
				"A package referencer is no longer registered."});
			const auto References = Facts.GetPackageReferences(*Data);
			if (!References) return RejectPreparation(References.error());
			CandidateEdges.insert(CandidateEdges.end(), (*References)->begin(), (*References)->end());
		}
		std::ranges::sort(CandidateEdges, &AssetReferenceLess);
		for (const FAssetReferenceEdge& Edge : CandidateEdges)
		{
			if (!Closure.contains(Edge.TargetPath.GetPackagePath())) continue;
			if (Mode == EPackageAliasFixupMode::RewriteAndDelete
				&& Closure.contains(Edge.SourcePackage))
				continue;
			++PackageRewriteCounts[Edge.SourcePackage];
		}

		auto AddStagingEntry = [&](const std::filesystem::path& PhysicalPath,
			const FPackagePath& RegistryPath,
			EAssetMutationPublicationRole Role,
			std::optional<FByteBuffer> PreBytes,
			std::optional<FByteBuffer> PostBytes,
			size_t& OutIndex) -> bool {
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
					: FByteView{}}, OutIndex, OutError);
		};

		for (const auto& [SourcePath, ExpectedCount] : PackageRewriteCounts)
		{
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PreparePackage))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::IoError,
					"Injected Fix Up package-preparation failure."});
			const FPackageData* Data = FindPrepared(SourcePath);
			if (!Data)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
					"A package referencer is no longer registered."});
			if (IsPackageLoading(SourcePath))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
					"A package referencer is currently loading."});
			DPackage* Loaded = FindResidentPackage(SourcePath);
			if (Loaded && Loaded->IsDirty())
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
					"A dirty loaded package blocks redirector Fix Up."});
			FByteBuffer PreBytes;
			bool Result = true;
			if (auto Prepared = LoadRelocationBytes(Data->PhysicalPath, PreBytes); !Prepared)
				return RejectPreparation(std::move(Prepared.error()));
			const auto Fingerprint = Prepared.References.GetSourceFingerprints().find(SourcePath);
			if (Fingerprint == Prepared.References.GetSourceFingerprints().end())
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
					"A package referencer has no complete index fingerprint."});
			FAssetPackageFingerprint CurrentFingerprint;
			if (auto Prepared = FingerprintAssetPackageBytes(Data->PhysicalPath, PreBytes, CurrentFingerprint); !Prepared)
				return RejectPreparation(std::move(Prepared.error()));
			if (CurrentFingerprint.FileSize != Fingerprint->second.FileSize
				|| CurrentFingerprint.LastWriteTimeTicks
					!= Fingerprint->second.LastWriteTimeTicks)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
					"A package referencer changed after reference indexing."});
			const auto Occurrence = std::ranges::find(CandidateEdges, SourcePath, &FAssetReferenceEdge::SourcePackage);
			if (Occurrence == CandidateEdges.end() || Occurrence->SourceFingerprint.ContentHash != CurrentFingerprint.ContentHash)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
					"A package referencer changed after exact reference collection."});
			FByteBuffer PostBytes;
			FByteBuffer BulkBytes;
			Result = LoadBulkClosure(Data->PhysicalPath, BulkBytes, OutError);
			if (!Result) return {};
			if (auto Prepared = RewriteAssetPackageReferences(
				PreBytes, BulkBytes, SourcePath,
				State->Mappings, ExpectedCount, PostBytes); !Prepared)
				return RejectPreparation(std::move(Prepared.error()));
			size_t StagingEntry = 0;
			Result = AddStagingEntry(
				Data->PhysicalPath, SourcePath,
				EAssetMutationPublicationRole::RealAsset,
				std::move(PreBytes), PostBytes, StagingEntry);
			if (!Result) return {};
			State->Packages.push_back({SourcePath, StagingEntry, Loaded});

			if (Loaded)
				if (auto Collected = CollectSoftObjectPathFixups(Loaded, ObjectRenames, State->SoftReferences); !Collected)
					return RejectPreparation(Collected.error());
		}

		const auto CapturedStores = Facts.GetStores();
		if (!CapturedStores) return RejectPreparation(CapturedStores.error());
		const auto& StoreRegistry = Facts.GetStoreRegistrations();
		State->ExpectedStoreRevision = StoreRegistry.Revision;
		size_t StoreIndex = 0;
		for (const auto [Handle, Store] : StoreRegistry.Stores)
		{
			if (GetAssetReferenceStoreRevision() != State->ExpectedStoreRevision) return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData, "Reference store registrations changed before rewrite preparation."});
			if (!Store)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
					"A registered asset reference store is unavailable."});

			FFixupStoreState StoreState{
				.Handle = Handle,
				.Store = Store};
			bool Result = true;
			StoreState.Snapshot = (*CapturedStores)->Stores[StoreIndex++];
			if (GetAssetReferenceStoreRevision() != State->ExpectedStoreRevision)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData, "Asset reference store registration changed during capture."});
			if (StoreState.Snapshot.ProviderId.empty()
				|| StoreState.Snapshot.ProviderVersion == 0
				|| StoreState.Snapshot.Fingerprint.empty())
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
					"An asset reference store returned an invalid identity or fingerprint."});
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
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
						"An asset reference store returned an invalid occurrence."});
				if (const FPackagePath* Destination = FindFixupDestination(
						Occurrence.TargetPath, State->Mappings))
				{
					Rewrites.push_back({
						.StableId = Occurrence.StableId,
						.SourcePath = Occurrence.TargetPath,
						.DestinationPath = *Destination});
				}
			}
			if (!Rewrites.empty())
			{
				if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PrepareStore))
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::IoError,
						"Injected Fix Up store-preparation failure."});
				if (auto Checked = Store->PrepareRewrite(
					Rewrites, StoreState.Snapshot.Fingerprint,
					StoreState.Contribution); !Checked)
					return RejectPreparation(std::move(Checked.error()));
				if (GetAssetReferenceStoreRevision() != State->ExpectedStoreRevision)
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData, "Asset reference store registration changed during preparation."});
				if (StoreState.Contribution.Fingerprint
						!= StoreState.Snapshot.Fingerprint
					|| StoreState.Contribution.Rewrites != Rewrites
					|| !StoreState.Contribution.Revalidate
					|| !StoreState.Contribution.Apply
					|| !StoreState.Contribution.Verify)
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
						"An asset reference store returned an incomplete rewrite contribution."});
				for (FAssetReferenceStorePackageRewrite& PackageRewrite :
					StoreState.Contribution.PackageRewrites)
				{
					const FPackageData* Data = FindPrepared(PackageRewrite.PackagePath);
					if (!PackageRewrite.PackagePath.IsValid() || !Data
						|| (!Data->TopLevelAssets.empty() && std::ranges::all_of(Data->TopLevelAssets, &FAssetData::IsRedirector)))
						return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
							"An asset reference store returned an invalid package participant."});
					if (IsPackageLoading(PackageRewrite.PackagePath))
						return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
							"An asset reference-store package is currently loading."});
					DPackage* Loaded = FindResidentPackage(PackageRewrite.PackagePath);
					if (Loaded && Loaded->IsDirty())
						return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
							"A dirty external-reference package blocks redirector Fix Up."});
					FByteBuffer CurrentBytes;
					if (auto Prepared = LoadRelocationBytes(Data->PhysicalPath, CurrentBytes); !Prepared)
						return RejectPreparation(std::move(Prepared.error()));
					if (CurrentBytes != PackageRewrite.PreBytes)
						return RejectPreparation(UnifiedError::FError{AssetWriteErrors::StaleData,
							"An asset reference-store package changed during rewrite preparation."});
					FByteBuffer BulkBytes;
					Result = LoadBulkClosure(Data->PhysicalPath, BulkBytes, OutError);
					if (!Result) return {};
					if (auto Prepared = ValidateAssetPackageBytes(
						PackageRewrite.PostBytes, PackageRewrite.PackagePath, BulkBytes); !Prepared)
						return RejectPreparation(std::move(Prepared.error()));
					size_t StagingEntry = 0;
					Result = AddStagingEntry(
						Data->PhysicalPath, PackageRewrite.PackagePath,
						EAssetMutationPublicationRole::RealAsset,
						std::move(PackageRewrite.PreBytes),
						PackageRewrite.PostBytes, StagingEntry);
					if (!Result) return {};
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
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::AlreadyExists,
					"Asset reference store provider ids must be unique."});

		if (Mode == EPackageAliasFixupMode::RewriteAndDelete)
		{
			for (const FPackagePath& Alias : State->Redirectors)
			{
				const FPackageData& Data = Prepared.Catalog.Assets.at(Alias);
				FByteBuffer PreBytes;
				if (auto Read = LoadRelocationBytes(Data.PhysicalPath, PreBytes); !Read)
					return RejectPreparation(std::move(Read.error()));
				size_t Ignored = 0;
				if (!AddStagingEntry(Data.PhysicalPath, Alias,
					EAssetMutationPublicationRole::Redirector,
					std::move(PreBytes), std::nullopt, Ignored)) return {};
			}
		}
		return State;
	}

	static auto FixUpRedirectorsImpl(
		std::span<const FPackagePath> Redirectors,
		EPackageAliasFixupMode Mode,
		const std::function<void()>& BeforeCommit, FAssetReferenceFixup* SharedFacts = nullptr) -> FAssetMutationResultDetails
	{
		FAssetMutationResultDetails Details;
		FAssetReferenceFixup LocalFacts;
		auto& Facts = SharedFacts ? *SharedFacts : LocalFacts;
		auto Fixup = PrepareRedirectorFixupState(Redirectors, Mode, Facts, &Details.Result.Error);
		if (!Fixup) return Details;
		if (BeforeCommit) BeforeCommit();
		Details.Result = CommitRedirectorFixup(Fixup);
		Details.AffectedFiles = Fixup->Staging.PublishedFiles;
		if (Fixup->Staging.bRetainBackups)
			Details.BackupLocations = Fixup->Staging.Roots;
		return Details;
	}

	static auto ValidateRedirectorFixupCommit(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
				"The redirector Fix Up job state is empty."});
		const auto& State = *Fixup;
		if (GetAssetCatalogRevision() != State.ExpectedRegistryRevision)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
				"The asset registry changed after redirector Fix Up analysis."});
		const auto Stores = CaptureAssetReferenceStoreRegistrations();
		if (Stores.Revision != State.ExpectedStoreRevision
			|| Stores.Stores.size() != State.Stores.size())
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
				"Asset reference store registration changed after Fix Up analysis."});
		for (const FFixupStoreState& StoreState : State.Stores)
		{
			const auto Current = Stores.Stores.find(StoreState.Handle);
			if (Current == Stores.Stores.end()
				|| Current->second != StoreState.Store)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
					"An asset reference store became unavailable."});

			FAssetReferenceStoreSnapshot Snapshot;
			if (auto Prepared = StoreState.Store->CaptureSnapshot(Snapshot); !Prepared)
				return Reject(OutError, std::move(Prepared.error()));
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
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
					"An asset reference store changed after Fix Up analysis."});
			if (StoreState.Contribution.Revalidate)
			{
				if (auto Checked = StoreState.Contribution.Revalidate(); !Checked)
					return Reject(OutError, std::move(Checked.error()));
			}
		}
		for (const FFixupPackageState& PackageState : State.Packages)
		{
			if (PackageState.LoadedPackage)
			{
				if (FindResidentPackage(PackageState.SourcePath)
						!= PackageState.LoadedPackage
					|| PackageState.LoadedPackage->IsDirty())
					return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
						"A loaded Fix Up package changed after analysis."});
			}
		}
		for (const auto& Live : State.SoftReferences)
			if (!Live.Value || Live.Value->ToSoftObjectPath() != Live.Before)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "A live soft reference changed after Fix Up analysis."});
		for (const FAssetMutationStagingEntry& Entry : State.Staging.Entries)
		{
			std::error_code ExistsError;
			if (!Entry.bPreExists
				|| !std::filesystem::exists(Entry.PhysicalPath, ExistsError)
				|| ExistsError)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
					"A Fix Up file participant changed occupancy."});
			FAssetPackageFingerprint Fingerprint;
			if (auto Prepared = FingerprintRelocationFile(
				Entry.PhysicalPath, Fingerprint); !Prepared)
				return Reject(OutError, std::move(Prepared.error()));
			if (Fingerprint != Entry.ExpectedPreFingerprint)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
					"A Fix Up file participant changed after analysis."});
			if (Entry.bPostExists)
			{
				FByteBuffer StagedBytes;
				if (auto Prepared = LoadRelocationBytes(Entry.StagedPostPath, StagedBytes); !Prepared)
					return Reject(OutError, std::move(Prepared.error()));
				if (FXxHash128::HashBuffer(StagedBytes)
						!= Entry.StagedPostHash)
					return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
						"A staged Fix Up output changed after analysis."});
			}
		}
		return true;
	}

	static auto CommitRedirectorFixup(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetWriteResult
	{
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return FAssetWriteResult{Guard.error()};
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return WriteFailure(AssetWriteErrors::StaleData,
				"The redirector Fix Up job state is empty.");
		auto& State = *Fixup;
		FAssetWriteResult Result;
		if (!ValidateRedirectorFixupCommit(Fixup, &Result.Error)) return Result;
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::StageOriginal))
			return WriteFailure(AssetWriteErrors::IoError,
				"Injected Fix Up original-staging failure.");

		State.Staging.bRetainBackups = true;
		bool bStoreWriteAttempted = false;
		auto PublicationFailed = [&](UnifiedError::FError Failure) -> FAssetWriteResult {
			std::vector<FPackagePath> Paths = State.Redirectors;
			for (const FFixupPackageState& Package : State.Packages)
				Paths.push_back(Package.SourcePath);
			FenceAssetRegistryProjection(Paths);
			Failure.PushContext("operation", std::format(
				"AssetMutationFailed: operation {} stopped; inspect partial changes before preparing another operation.",
				State.Staging.OperationId));
			return {.Error = std::move(Failure), .Effect = State.Staging.PublishedFiles.empty() && !bStoreWriteAttempted ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten};
		};

		for (size_t Index = 0; Index < State.Staging.Entries.size(); ++Index)
		{
			FAssetMutationStagingEntry& Entry = State.Staging.Entries[Index];
			if (Entry.Role == EAssetMutationPublicationRole::Redirector) continue;
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishPackage))
				return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "Injected Fix Up package-publication failure."});
			if (!PublishRelocationFile(Entry, &Result.Error)) return PublicationFailed(*Result.Error);
			State.Staging.PublishedFiles.push_back(Entry.PhysicalPath);
		}
		for (FFixupStoreState& Store : State.Stores)
		{
			if (Store.Contribution.Rewrites.empty()) continue;

			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::ApplyStore))
				return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "Injected Fix Up store-publication failure."});
			bStoreWriteAttempted = true;
			if (!Store.Contribution.Apply(&Result.Error))
			{
				if (!Result.Error) Result.Error.emplace(AssetWriteErrors::IoError, "Reference store rejected its prepared rewrite.");
				return PublicationFailed(*Result.Error);
			}
		}
		for (auto& Live : State.SoftReferences) *Live.Value = Live.After;

		std::vector<FAssetReferenceEdge> VerifiedEdges;
		std::unordered_set<FPackagePath> VerifiedPackages;
		for (const FFixupPackageState& Package : State.Packages)
		{
			if (!VerifiedPackages.insert(Package.SourcePath).second) continue;
			const FAssetMutationStagingEntry& Entry =
				State.Staging.Entries[Package.StagingEntry];
			FAssetPackageInspection Inspection;
			if (auto Prepared = InspectAssetPackage(
				Entry.PhysicalPath.generic_string(), Package.SourcePath, Inspection); !Prepared)
				return PublicationFailed(std::move(Prepared.error()));
			std::vector<FAssetReferenceEdge> References;
			if (auto Prepared = ExtractAssetReferences(
				Package.SourcePath, Inspection, References); !Prepared)
				return PublicationFailed(std::move(Prepared.error()));
			VerifiedEdges.insert(VerifiedEdges.end(),
				std::make_move_iterator(References.begin()),
				std::make_move_iterator(References.end()));
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::Verify))
			return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "Injected Fix Up verification failure."});
		for (const FAssetReferenceEdge& Edge : VerifiedEdges)
			if (FindFixupDestination(Edge.TargetPath.GetPackagePath(), State.Mappings))
				return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, std::format(
					"Fix Up verification found a remaining package occurrence at {}:{}.",
					Edge.SourcePackage.ToString(), Edge.DisplayRoute)});
		for (FFixupStoreState& Store : State.Stores)
		{
			if (Store.Contribution.Verify)
			{
				if (auto Verified = Store.Contribution.Verify(); !Verified)
					return PublicationFailed(std::move(Verified.error()));
			}
			FAssetReferenceStoreSnapshot Snapshot;
			if (auto Prepared = Store.Store->CaptureSnapshot(Snapshot); !Prepared)
				return PublicationFailed(std::move(Prepared.error()));
			for (const FAssetReferenceStoreOccurrence& Occurrence : Snapshot.Occurrences)
				if (FindFixupDestination(Occurrence.TargetPath, State.Mappings))
					return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError,
						"Fix Up verification found a remaining external occurrence."});
		}

		if (State.Mode == EPackageAliasFixupMode::RewriteAndDelete)
		{
			for (size_t Index = 0; Index < State.Staging.Entries.size(); ++Index)
			{
				FAssetMutationStagingEntry& Entry = State.Staging.Entries[Index];
				if (Entry.Role != EAssetMutationPublicationRole::Redirector) continue;
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::DeleteRedirector))
					return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError,
						"Injected Fix Up redirector-deletion failure."});
				if (!PublishRelocationFile(Entry, &Result.Error)) return PublicationFailed(*Result.Error);
				State.Staging.PublishedFiles.push_back(Entry.PhysicalPath);
			}
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishRegistry))
			return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "Injected Fix Up Registry-publication failure."});

		std::vector<FPackagePath> FencedPaths = State.Redirectors;
		for (const FFixupPackageState& Package : State.Packages)
			FencedPaths.push_back(Package.SourcePath);
		if (auto Prepared = RefreshSavedPackages(FencedPaths); !Prepared)
			return PublicationFailed(std::move(Prepared.error()));
		State.Staging.bRetainBackups = false;
		return {};
	}

	auto FixupPackageAliases(std::span<const FPackagePath> Redirectors,
		EPackageAliasFixupMode Mode, FAssetReferenceFixup* Facts) -> FAssetMutationResultDetails
	{ return FixUpRedirectorsImpl(Redirectors, Mode, {}, Facts); }

	auto FixupPackageFilesWithBeforeCommitForTesting(
		std::span<const FPackagePath> Redirectors, EPackageAliasFixupTestMode Mode,
		const std::function<void()>& BeforeCommit) -> FPackageMutationTestResult
	{ auto Details = FixUpRedirectorsImpl(Redirectors, static_cast<EPackageAliasFixupMode>(Mode), BeforeCommit); return {std::move(Details.Result), std::move(Details.AffectedFiles), std::move(Details.BackupLocations)}; }
	auto FixupPackageFilesForTesting(std::span<const FPackagePath> Paths, EPackageAliasFixupTestMode Mode) -> FPackageMutationTestResult
	{ return FixupPackageFilesWithBeforeCommitForTesting(Paths, Mode, {}); }
}
