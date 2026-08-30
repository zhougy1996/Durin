#include "AssetRuntimeStateInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetMutationTransactionInternal.h"
#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"
#include "AssetRelocationExtensionsInternal.h"
#include "Asset/EditorBulkDataStorage.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Misc/FileTime.h"
#include "Misc/MountPaths.h"
#include "Profiling/Profiling.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset
{
	using Private::EAssetMutationState;
	using Private::EMutationJournalDuplicatePolicy;
	using Private::EAssetMutationPublicationRole;
	using Private::FAssetMutationJournal;
	using Private::FAssetMutationJournalEntry;
	using Private::FingerprintRelocationFile;
	using Private::InitializeMutationJournal;
	using Private::IsMutationJournalRecoveryRequired;
	using Private::LoadRelocationBytes;
	using Private::NormalizePhysicalPath;
	using Private::PublishRelocationFile;
	using Private::RebuildReferenceProjectionForPublishedEntries;
	using Private::StageMutationJournalEntry;
	using Private::TransitionMutationJournalState;
	using Private::WriteMutationJournalState;

	namespace
	{
		constexpr std::string_view RedirectorClassName =
			"Durin::Asset::DAssetRedirector";

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto GetRelocationPhysicalPath(const FAssetPath& Path) -> std::string
		{
			const FAssetRuntimeConfiguration& Context =
				FAssetRuntimeState::Get().GetRuntimeConfiguration();
			if (Context.IsCooked())
			{
				std::filesystem::path CookedPath;
				if (!ResolveCookedPackagePath(
					Context.GetCookRoot(), Path.GetView(), CookedPath)) return {};
				return CookedPath.generic_string();
			}
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(
					Path.GetView(), EMountPathExistence::AllowMissing);
			if (!Resolved)
				DURIN_WARN_CATEGORY(
					"AssetSystem",
					"Failed to resolve asset path {}: {}",
					Path.ToString(),
					Resolved.Message);
			return Resolved
				? Resolved.PhysicalPath.generic_string() + ".dasset"
				: std::string{};
		}

		struct FLoadedRelocationState
		{
			FAssetRelocationMapping Mapping;
			DPackage* Package = nullptr;
			std::string PrePackageName;
			std::string PreAssetName;
		};

		auto BuildMovedPackageBytes(
			std::span<const std::byte> SourceBytes,
			const FAssetPath& DestinationPath,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = Private::ResolveAssetPackageReader(
				SourceBytes, Codec); !Result)
				return Result;
			if (!Codec->bCanMutate)
				return Error(EAssetError::UnsupportedVersion,
					"Relocation requires package mutation capability.");
			if (FAssetResult Result = Codec->Relocate(
				SourceBytes, DestinationPath, OutBytes); !Result)
				return Result;
			return Codec->Validate(OutBytes);
		}

		auto BuildRedirectorPackageBytes(
			const FAssetPath& SourcePath,
			const FAssetPath& DestinationPath,
			uint32 FormatVersion,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec =
				Private::FindAssetPackageWriter(FormatVersion);
			if (!Codec || !Codec->bCanMutate)
				return Error(EAssetError::UnsupportedVersion,
					"Redirector creation requires package mutation capability.");
			return Codec->WriteRedirector(
				SourcePath, DestinationPath, OutBytes);
		}
	}


	struct FAssetRelocationState
	{
		uint64 ExpectedRegistryRevision = 0;
		std::vector<FAssetRelocationMapping> Mappings;
		FAssetMutationJournal Journal;
		std::vector<FLoadedRelocationState> LoadedPackages;
		std::vector<FAssetOwnedPayloadRelocation> OwnedPayloads;
		std::unordered_map<FAssetPath, FAssetData> PreAssets;
		std::unordered_map<FAssetPath, FAssetData> PostAssets;
		std::unordered_map<FAssetPath, FAssetData> ExpectedAssets;
		std::vector<FAssetReferenceEdge> PreReferenceEdges;
		std::vector<FAssetReferenceEdge> PostReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> PreReferenceFingerprints;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> PostReferenceFingerprints;
	};

	auto FAssetMutationCoordinator::PrepareAssetRelocationState(
		std::span<const FAssetRelocationMapping> Mappings,
		std::shared_ptr<FAssetRelocationState>& OutState) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		OutState.reset();
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown,
				"Asset relocation is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit asset relocation.");
		if (Mappings.empty())
			return Error(EAssetError::InvalidPath,
				"An asset relocation batch must not be empty.");

		auto State = std::make_shared<FAssetRelocationState>();
		State->ExpectedRegistryRevision = GetAssetCatalogRevision();
		const FAssetPublicationState Prepared = Registry.CapturePreparedState();
		const auto FindPrepared = [&](const FAssetPath& Path) -> const FAssetData* {
			const auto It = Prepared.Assets.find(Path);
			return It == Prepared.Assets.end() ? nullptr : &It->second;
		};
		State->Mappings.assign(Mappings.begin(), Mappings.end());
		std::ranges::sort(State->Mappings,
			[](const FAssetRelocationMapping& A,
				const FAssetRelocationMapping& B) {
				return A.SourcePath.GetView() < B.SourcePath.GetView();
			});
		State->PreAssets = Prepared.Assets;
		State->PostAssets = State->PreAssets;
		State->ExpectedAssets = State->PreAssets;
		State->PreReferenceEdges = Prepared.ReferenceEdges;
		State->PostReferenceEdges = State->PreReferenceEdges;
		State->PreReferenceFingerprints = Prepared.ReferenceFingerprints;
		State->PostReferenceFingerprints = State->PreReferenceFingerprints;
		InitializeMutationJournal(
			State->Journal, EAssetMutationOperationKind::Relocation);

		std::unordered_set<FAssetPath> Sources;
		std::unordered_set<FAssetPath> Destinations;
		auto AddFileEntry = [&](const std::filesystem::path& PhysicalPath,
			const FAssetPath& RegistryPath,
			EAssetMutationPublicationRole Role,
			std::optional<std::vector<std::byte>> PreBytes,
			std::optional<std::vector<std::byte>> PostBytes) -> FAssetResult {
			if (Private::ConsumeAssetRelocationFailure(
					EAssetRelocationFailurePoint::PrepareOutput))
				return Error(EAssetError::IoError,
					"Injected relocation output-preparation failure.");
			size_t IgnoredIndex = 0;
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
					EMutationJournalDuplicatePolicy::Reject}, IgnoredIndex);
		};

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			if (!Mapping.SourcePath.IsValid()
				|| !Mapping.DestinationPath.IsValid()
				|| Mapping.SourcePath == Mapping.DestinationPath)
				return Error(EAssetError::InvalidPath,
					"Asset relocation paths are invalid or identical.");
			if (!Sources.insert(Mapping.SourcePath).second
				|| !Destinations.insert(Mapping.DestinationPath).second)
				return Error(EAssetError::InvalidPath,
					"An asset relocation batch contains duplicate paths.");
		}

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			const FAssetData* SourceData = FindPrepared(Mapping.SourcePath);
			if (!SourceData)
				return Error(EAssetError::NotFound, std::format(
					"Asset {} was not found.", Mapping.SourcePath.ToString()));
			if (SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetError::InvalidPackageType,
					"Redirectors cannot be used as relocation sources.");
			if (LoadingPackages.contains(Mapping.SourcePath))
				return Error(EAssetError::InUse,
					"A relocation source is currently loading.");
			if (DPackage* Loaded = FindResidentPackage(Mapping.SourcePath))
			{
				if (Loaded->IsDirty())
					return Error(EAssetError::InUse,
						"A dirty loaded asset must be saved before relocation.");
				State->LoadedPackages.push_back({
					.Mapping = Mapping,
					.Package = Loaded,
					.PrePackageName = Loaded->GetName(),
					.PreAssetName = Loaded->GetAsset()->GetName()});
			}

			bool bReclaimDestinationRedirector = false;
			if (const FAssetData* DestinationData = FindPrepared(Mapping.DestinationPath))
			{
				if (DestinationData->EntryKind
						!= EAssetRegistryEntryKind::Redirector)
					return Error(EAssetError::AlreadyExists, std::format(
						"Asset {} already exists.",
						Mapping.DestinationPath.ToString()));
				const FAssetPathResolveResult DestinationResolution =
					Durin::Asset::ResolveAssetPath(Mapping.DestinationPath);
				if (!DestinationResolution
					|| DestinationResolution.FinalPath != Mapping.SourcePath)
					return Error(EAssetError::AlreadyExists, std::format(
						"The destination {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
						Mapping.DestinationPath.ToString(),
						DestinationData->RedirectDestination.ToString()));
				if (FindResidentPackage(Mapping.DestinationPath))
					return Error(EAssetError::InUse,
						"A loaded destination redirector cannot be reclaimed.");
				bReclaimDestinationRedirector = true;
			}

			const std::filesystem::path SourceFile =
				NormalizePhysicalPath(SourceData->PhysicalPath);
			const std::filesystem::path DestinationFile =
				NormalizePhysicalPath(
					GetRelocationPhysicalPath(Mapping.DestinationPath));
			std::vector<std::byte> SourceBytes;
			FAssetResult Result = LoadRelocationBytes(SourceFile, SourceBytes);
			if (!Result) return Result;
			std::vector<std::byte> DestinationPreBytes;
			if (bReclaimDestinationRedirector)
			{
				Result = LoadRelocationBytes(
					DestinationFile, DestinationPreBytes);
				if (!Result) return Result;
			}
			else if (std::filesystem::exists(DestinationFile))
				return Error(EAssetError::AlreadyExists, std::format(
					"Relocation destination file {} already exists.",
					DestinationFile.generic_string()));

			std::vector<std::byte> MovedBytes;
			Result = BuildMovedPackageBytes(
				SourceBytes, Mapping.DestinationPath, MovedBytes);
			if (!Result) return Result;
			std::vector<std::byte> SourceRedirectorBytes;
			Result = BuildRedirectorPackageBytes(
				Mapping.SourcePath, Mapping.DestinationPath,
				SourceData->FormatVersion,
				SourceRedirectorBytes);
			if (!Result) return Result;

			Result = AddFileEntry(
				DestinationFile,
				Mapping.DestinationPath,
				EAssetMutationPublicationRole::RealAsset,
				bReclaimDestinationRedirector
					? std::optional<std::vector<std::byte>>(DestinationPreBytes)
					: std::nullopt,
				std::move(MovedBytes));
			if (!Result) return Result;
			Result = AddFileEntry(
				SourceFile,
				Mapping.SourcePath,
				EAssetMutationPublicationRole::Redirector,
				SourceBytes,
				std::move(SourceRedirectorBytes));
			if (!Result) return Result;

			FAssetPackageInspection BulkInspection;
			Result = InspectAssetPackage(SourceFile.generic_string(), BulkInspection);
			if (!Result) return Result;
			std::vector<std::filesystem::path> SourceBulkFiles;
			std::vector<std::filesystem::path> DestinationBulkFiles;
			std::string BulkError;
			if (!InspectEditorBulkDataCompanionPaths(
					SourceFile, BulkInspection, SourceBulkFiles, &BulkError)
				|| !InspectEditorBulkDataCompanionPaths(
					DestinationFile, BulkInspection, DestinationBulkFiles, &BulkError)
				|| SourceBulkFiles.size() != DestinationBulkFiles.size())
				return Error(EAssetError::CorruptFile,
					BulkError.empty() ? "Authored bulk relocation inspection failed." : BulkError);
			for (size_t BulkIndex = 0; BulkIndex < SourceBulkFiles.size(); ++BulkIndex)
			{
				std::vector<std::byte> PayloadBytes;
				Result = LoadRelocationBytes(SourceBulkFiles[BulkIndex], PayloadBytes);
				if (!Result) return Result;
				if (std::filesystem::exists(DestinationBulkFiles[BulkIndex]))
					return Error(EAssetError::AlreadyExists,
						"Authored bulk relocation destination already exists.");
				Result = AddFileEntry(DestinationBulkFiles[BulkIndex], {},
					EAssetMutationPublicationRole::OwnedPayload,
					std::nullopt, PayloadBytes);
				if (!Result) return Result;
				Result = AddFileEntry(SourceBulkFiles[BulkIndex], {},
					EAssetMutationPublicationRole::OwnedPayload,
					std::move(PayloadBytes), std::nullopt);
				if (!Result) return Result;
			}

			FAssetData MovedData = *SourceData;
			MovedData.PackagePath = Mapping.DestinationPath;
			MovedData.PhysicalPath = DestinationFile.generic_string();
			State->PostAssets.erase(Mapping.SourcePath);
			State->PostAssets.erase(Mapping.DestinationPath);
			State->PostAssets.emplace(Mapping.DestinationPath,
				std::move(MovedData));
			State->PostAssets.emplace(Mapping.SourcePath, FAssetData{
				.PackagePath = Mapping.SourcePath,
				.PhysicalPath = SourceFile.generic_string(),
				.AssetClassName = std::string(RedirectorClassName),
				.EntryKind = EAssetRegistryEntryKind::Redirector,
				.RedirectDestination = Mapping.DestinationPath,
				.FormatVersion = SourceData->FormatVersion,
				.Dependencies = {Mapping.DestinationPath}});

			for (const auto& [AliasPath, AliasData] : State->PreAssets)
			{
				if (AliasData.EntryKind != EAssetRegistryEntryKind::Redirector
					|| AliasPath == Mapping.DestinationPath)
					continue;
				const FAssetPathResolveResult AliasResolution =
					Durin::Asset::ResolveAssetPath(AliasPath);
				if (!AliasResolution
					|| AliasResolution.FinalPath != Mapping.SourcePath)
					continue;
				if (FindResidentPackage(AliasPath))
					return Error(EAssetError::InUse,
						"A loaded upstream redirector cannot be retargeted.");
				std::vector<std::byte> AliasPreBytes;
				Result = LoadRelocationBytes(
					AliasData.PhysicalPath, AliasPreBytes);
				if (!Result) return Result;
				std::vector<std::byte> AliasPostBytes;
				Result = BuildRedirectorPackageBytes(
					AliasPath, Mapping.DestinationPath,
					AliasData.FormatVersion, AliasPostBytes);
				if (!Result) return Result;
				Result = AddFileEntry(
					AliasData.PhysicalPath,
					AliasPath,
					EAssetMutationPublicationRole::Redirector,
					std::move(AliasPreBytes),
					std::move(AliasPostBytes));
				if (!Result) return Result;
				FAssetData& PostAlias = State->PostAssets.at(AliasPath);
				PostAlias.RedirectDestination = Mapping.DestinationPath;
				PostAlias.Dependencies = {Mapping.DestinationPath};
			}

			for (FAssetReferenceEdge& Reference : State->PostReferenceEdges)
				if (Reference.SourcePackage == Mapping.SourcePath)
					Reference.SourcePackage = Mapping.DestinationPath;
			if (auto ReferenceSource = State->PostReferenceFingerprints.find(
					Mapping.SourcePath);
				ReferenceSource != State->PostReferenceFingerprints.end())
			{
				State->PostReferenceFingerprints.insert_or_assign(
					Mapping.DestinationPath, ReferenceSource->second);
				State->PostReferenceFingerprints.erase(ReferenceSource);
			}

			DClass* AssetClass = FindClassByQualifiedName(
				FName(SourceData->AssetClassName));
			Private::FAssetOwnedPayloadRelocatorInvocation RelocatorInvocation;
			Result = Private::AcquireAssetOwnedPayloadRelocator(
				AssetClass, RelocatorInvocation);
			if (!Result) return Result;
			if (RelocatorInvocation.Relocator)
			{
				DObject* AssetObject = nullptr;
				Result = LoadAsset(Mapping.SourcePath, AssetObject);
				if (!Result) return Result;
				if (std::ranges::none_of(
						State->LoadedPackages,
						[&](const FLoadedRelocationState& Loaded) {
							return Loaded.Mapping.SourcePath
								== Mapping.SourcePath;
						}))
				{
					DPackage* LoadedPackage = AssetObject->GetPackage();
					State->LoadedPackages.push_back({
						.Mapping = Mapping,
						.Package = LoadedPackage,
						.PrePackageName = LoadedPackage->GetName(),
						.PreAssetName = AssetObject->GetName()});
				}
				FAssetOwnedPayloadRelocation Payload;
				Result = RelocatorInvocation.Relocator(
					AssetObject, Mapping.SourcePath,
					Mapping.DestinationPath, Payload);
				if (!Result) return Result;
				for (const auto& [From, To] : Payload.Files)
				{
					const std::filesystem::path SourcePayload =
						NormalizePhysicalPath(From);
					const std::filesystem::path DestinationPayload =
						NormalizePhysicalPath(To);
					if (SourcePayload == DestinationPayload)
						return Error(EAssetError::InvalidPath,
							"An owned payload relocation has identical paths.");
					std::vector<std::byte> PayloadBytes;
					Result = LoadRelocationBytes(SourcePayload, PayloadBytes);
					if (!Result) return Result;
					if (std::filesystem::exists(DestinationPayload))
						return Error(EAssetError::AlreadyExists,
							"An owned payload destination already exists.");
					Result = AddFileEntry(
						DestinationPayload, {},
						EAssetMutationPublicationRole::OwnedPayload,
						std::nullopt, PayloadBytes);
					if (!Result) return Result;
					Result = AddFileEntry(
						SourcePayload, {},
						EAssetMutationPublicationRole::OwnedPayload,
						std::move(PayloadBytes), std::nullopt);
					if (!Result) return Result;
				}
				State->OwnedPayloads.push_back(std::move(Payload));
				break;
			}
		}

		FAssetResult JournalResult = TransitionMutationJournalState(
			State->Journal, EAssetMutationState::Prepared);
		if (!JournalResult) return JournalResult;
		OutState = std::move(State);
		return {};
	}

	auto FAssetMutationCoordinator::PrepareAssetRelocationTransaction(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetMutationSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		OutSummary = {};
		OutTransaction = {};
		std::shared_ptr<FAssetRelocationState> Relocation;
		FAssetResult Result = PrepareAssetRelocationState(Mappings, Relocation);
		if (!Result) return Result;

		std::vector<FAssetPath> Scope;
		Scope.reserve(Mappings.size() * 2);
		for (const FAssetRelocationMapping& Mapping : Mappings)
		{
			Scope.push_back(Mapping.SourcePath);
			Scope.push_back(Mapping.DestinationPath);
		}
		OutSummary = FAssetMutationSummary(
			EAssetMutationOperationKind::Relocation,
			Relocation->ExpectedRegistryRevision,
			std::move(Scope));
		auto TransactionState = std::make_shared<FAssetMutationTransaction::FState>();
		TransactionState->Summary = OutSummary;
		TransactionState->CommitOperation = [Relocation] {
			return FAssetRuntimeState::Get().GetMutationCoordinator().ApplyAssetRelocation(Relocation);
		};
		TransactionState->UndoOperation = [Relocation] {
			return FAssetRuntimeState::Get().GetMutationCoordinator().RestoreAssetRelocation(Relocation);
		};
		TransactionState->RedoOperation = TransactionState->CommitOperation;
		TransactionState->IsRecoveryRequired = [Relocation] {
			return IsMutationJournalRecoveryRequired(Relocation->Journal);
		};
		TransactionState->LastResult.State =
			EAssetMutationTransactionState::Prepared;
		TransactionState->LastResult.RegistryRevision =
			GetAssetCatalogRevision();
		OutTransaction.State = std::move(TransactionState);
		return {};
	}

	auto FAssetMutationCoordinator::RevalidateAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetError::StaleData,
				"The relocation transaction state is empty.");
		const auto& State = *Relocation;
		if (State.Journal.State == EAssetMutationState::RecoveryRequired)
			return Error(EAssetError::IoError,
				"AssetMutationRecoveryRequired: the relocation journal requires recovery.");
		if (State.Journal.State != EAssetMutationState::Prepared
			&& State.Journal.State != EAssetMutationState::Committed
			&& State.Journal.State != EAssetMutationState::Restored)
			return Error(EAssetError::StaleData,
				"The relocation token is not in a revalidatable state.");
		if (GetAssetCatalogRevision() != State.ExpectedRegistryRevision
			|| CaptureAssetCatalogSnapshot().Assets != State.ExpectedAssets)
			return Error(EAssetError::StaleData,
				"The asset registry changed after relocation analysis.");
		const bool bExpectPost =
			State.Journal.State == EAssetMutationState::Committed;
		for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			const bool bExpectedExists = bExpectPost
				? Entry.bPostExists : Entry.bPreExists;
			std::error_code ExistsError;
			const bool bExists = std::filesystem::exists(
				Entry.PhysicalPath, ExistsError);
			if (ExistsError || bExists != bExpectedExists)
				return Error(EAssetError::StaleData, std::format(
					"Relocation participant occupancy changed: {}.",
					Entry.PhysicalPath.generic_string()));
			if (bExists)
			{
				FAssetPackageFingerprint Fingerprint;
				FAssetResult Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Fingerprint);
				if (!Result) return Result;
				const FAssetPackageFingerprint& Expected = bExpectPost
					? Entry.ExpectedPostFingerprint
					: Entry.ExpectedPreFingerprint;
				if (Fingerprint != Expected)
					return Error(EAssetError::StaleData, std::format(
						"Relocation participant changed: {}.",
						Entry.PhysicalPath.generic_string()));
			}
			const bool bOutputExists = bExpectPost
				? Entry.bPreExists : Entry.bPostExists;
			const std::filesystem::path& Staged = bExpectPost
				? Entry.StagedPrePath : Entry.StagedPostPath;
			const FXxHash128& ExpectedHash = bExpectPost
				? Entry.StagedPreHash : Entry.StagedPostHash;
			if (bOutputExists)
			{
				std::vector<std::byte> StagedBytes;
				FAssetResult Result = LoadRelocationBytes(Staged, StagedBytes);
				if (!Result || FXxHash128::HashBuffer(StagedBytes) != ExpectedHash)
					return Error(EAssetError::StaleData,
						"A staged relocation output changed.");
			}
		}
		for (const FLoadedRelocationState& Loaded : State.LoadedPackages)
		{
			const FAssetPath& ExpectedPath = bExpectPost
				? Loaded.Mapping.DestinationPath
				: Loaded.Mapping.SourcePath;
			if (FindResidentPackage(ExpectedPath) != Loaded.Package)
				return Error(EAssetError::StaleData,
					"A loaded relocation participant changed identity.");
		}
		return {};
	}

	namespace
	{
		auto FailurePointForRole(EAssetMutationPublicationRole Role)
			-> EAssetRelocationFailurePoint
		{
			switch (Role)
			{
			case EAssetMutationPublicationRole::RealAsset:
				return EAssetRelocationFailurePoint::PublishRealAsset;
			case EAssetMutationPublicationRole::OwnedPayload:
				return EAssetRelocationFailurePoint::PublishOwnedPayload;
			case EAssetMutationPublicationRole::Redirector:
				return EAssetRelocationFailurePoint::PublishRedirector;
			}
			return EAssetRelocationFailurePoint::PublishRealAsset;
		}
	}

	auto FAssetMutationCoordinator::ApplyAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetError::StaleData,
				"The relocation transaction state is empty.");
		auto& State = *Relocation;
		if (State.Journal.State != EAssetMutationState::Prepared
			&& State.Journal.State != EAssetMutationState::Restored)
			return Error(EAssetError::StaleData,
				"Only a prepared or restored relocation can be applied.");
		FAssetResult Result = RevalidateAssetRelocation(Relocation);
		if (!Result) return Result;
		if (Private::ConsumeAssetRelocationFailure(
				EAssetRelocationFailurePoint::StageOriginal))
			return Error(EAssetError::IoError,
				"Injected relocation original-staging failure.");

		std::vector<size_t> Order(State.Journal.Entries.size());
		for (size_t Index = 0; Index < Order.size(); ++Index)
			Order[Index] = Index;
		std::ranges::stable_sort(Order, [&](size_t A, size_t B) {
			return State.Journal.Entries[A].Role
				< State.Journal.Entries[B].Role;
		});
		for (size_t OrderIndex = 0; OrderIndex < Order.size(); ++OrderIndex)
		{
			FAssetMutationJournalEntry& Entry =
				State.Journal.Entries[Order[OrderIndex]];
			Entry.PublicationOrder = static_cast<uint64>(OrderIndex);
			Entry.bCompleted = false;
			Entry.bCompensated = false;
		}
		Result = TransitionMutationJournalState(
			State.Journal, EAssetMutationState::Publishing);
		if (!Result) return Result;
		std::vector<size_t> Published;
		size_t RelocatedLoadedCount = 0;
		size_t AppliedPayloadCount = 0;
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
			for (size_t Count = AppliedPayloadCount; Count > 0; --Count)
				if (State.OwnedPayloads[Count - 1].Restore)
					State.OwnedPayloads[Count - 1].Restore();
			for (size_t Count = RelocatedLoadedCount; Count > 0; --Count)
			{
				if (Private::ConsumeAssetRelocationFailure(
						EAssetRelocationFailurePoint::CompensateLoadedPackage))
					return EnterRecovery(
						"loaded-package compensation was interrupted.");
				FLoadedRelocationState& Loaded = State.LoadedPackages[Count - 1];
				if (!Loaded.Package->RelocateAssetPackage(
						Loaded.Mapping.SourcePath))
					return EnterRecovery(
						"a loaded package path could not be restored.");
				Loaded.Package->Rename(FName(Loaded.PrePackageName));
				Loaded.Package->GetAsset()->Rename(FName(Loaded.PreAssetName));
				Loaded.Package->ClearDirty();
			}
			for (auto It = Published.rbegin(); It != Published.rend(); ++It)
			{
				if (Private::ConsumeAssetRelocationFailure(
						EAssetRelocationFailurePoint::CompensateFile))
					return EnterRecovery(
						"file compensation was interrupted.");
				FAssetResult RestoreResult = PublishRelocationFile(
					State.Journal.Entries[*It], false);
				if (!RestoreResult)
					return EnterRecovery(RestoreResult.Message);
				State.Journal.Entries[*It].bCompensated = true;
				JournalResult = WriteMutationJournalState(State.Journal);
				if (!JournalResult) return EnterRecovery(JournalResult.Message);
			}
			for (FAssetMutationJournalEntry& Entry : State.Journal.Entries)
			{
				if (!Entry.bPreExists) continue;
				FAssetResult FingerprintResult = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPreFingerprint);
				if (!FingerprintResult)
					return EnterRecovery(FingerprintResult.Message);
			}
			JournalResult = TransitionMutationJournalState(
				State.Journal, EAssetMutationState::Prepared);
			if (!JournalResult)
				return Error(EAssetError::IoError, std::format(
					"AssetMutationRecoveryRequired: compensation completed but the restored journal state could not be persisted: {}",
					JournalResult.Message));
			return Failure;
		};

		for (size_t Index : Order)
		{
			FAssetMutationJournalEntry& Entry = State.Journal.Entries[Index];
			if (Private::ConsumeAssetRelocationFailure(
					FailurePointForRole(Entry.Role)))
				return Compensate(Error(EAssetError::IoError,
					"Injected relocation publication failure."));
			Result = PublishRelocationFile(Entry, true);
			if (!Result) return Compensate(std::move(Result));
			Published.push_back(Index);
			Entry.bCompleted = true;
			Result = WriteMutationJournalState(State.Journal);
			if (!Result) return Compensate(std::move(Result));
		}

		for (FLoadedRelocationState& Loaded : State.LoadedPackages)
		{
			if (Private::ConsumeAssetRelocationFailure(
					EAssetRelocationFailurePoint::UpdateLoadedPackage))
				return Compensate(Error(EAssetError::IoError,
					"Injected loaded-package relocation failure."));
			if (!Loaded.Package->RelocateAssetPackage(
					Loaded.Mapping.DestinationPath))
				return Compensate(Error(EAssetError::AlreadyExists,
					"A loaded relocation destination became occupied."));
			Loaded.Package->Rename(FName(
				Loaded.Mapping.DestinationPath.GetAssetName()));
			Loaded.Package->GetAsset()->Rename(FName(
				Loaded.Mapping.DestinationPath.GetAssetName()));
			Loaded.Package->ClearDirty();
			++RelocatedLoadedCount;
		}
		for (FAssetOwnedPayloadRelocation& Payload : State.OwnedPayloads)
		{
			if (Payload.Apply) Payload.Apply();
			++AppliedPayloadCount;
		}
		if (Private::ConsumeAssetRelocationFailure(
				EAssetRelocationFailurePoint::PublishRegistry))
			return Compensate(Error(EAssetError::IoError,
				"Injected relocation registry-publication failure."));

		for (FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			if (Entry.bPostExists)
			{
				Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPostFingerprint);
				if (!Result) return Compensate(std::move(Result));
			}
			if (!Entry.RegistryPath.IsValid()) continue;
			auto Data = State.PostAssets.find(Entry.RegistryPath);
			if (Data == State.PostAssets.end()) continue;
			std::error_code MetadataError;
			Data->second.FileSize = std::filesystem::file_size(
				Entry.PhysicalPath, MetadataError);
			Data->second.LastWriteTime = std::filesystem::last_write_time(
				Entry.PhysicalPath, MetadataError);
			if (MetadataError)
				return Compensate(Error(EAssetError::IoError,
					"Could not read relocated package metadata."));
			Data->second.LastWriteTimeTicks =
				FileTime::ToStableTicks(
					Data->second.LastWriteTime);
		}
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
		{
			const FAssetMutationJournalEntry* DestinationEntry = nullptr;
			for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
				if (Entry.RegistryPath == Mapping.DestinationPath)
				{
					DestinationEntry = &Entry;
					break;
				}
			if (!DestinationEntry) continue;
			for (FAssetReferenceEdge& Reference : State.PostReferenceEdges)
				if (Reference.SourcePackage == Mapping.DestinationPath)
					Reference.SourceFingerprint =
						DestinationEntry->ExpectedPostFingerprint;
			if (State.PostReferenceFingerprints.contains(Mapping.DestinationPath))
				State.PostReferenceFingerprints.insert_or_assign(
					Mapping.DestinationPath,
					DestinationEntry->ExpectedPostFingerprint);
		}
		Result = RebuildReferenceProjectionForPublishedEntries(
			State.Journal.Entries, State.PostAssets,
			State.PostReferenceEdges, State.PostReferenceFingerprints);
		if (!Result) return Compensate(std::move(Result));

		const FAssetPublicationState Current = Registry.CapturePreparedState();
		Result = Registry.PublishPreparedState(State.ExpectedRegistryRevision, {
			.Assets = State.PostAssets,
			.ReferenceEdges = State.PostReferenceEdges,
			.ReferenceFingerprints = State.PostReferenceFingerprints,
			.ReferenceErrors = Current.ReferenceErrors,
			.bReferenceIndexComplete = Current.ReferenceErrors.empty()
				&& State.PostReferenceFingerprints.size() == State.PostAssets.size()});
		if (!Result) return Compensate(std::move(Result));
		State.ExpectedRegistryRevision = GetAssetCatalogRevision();
		State.ExpectedAssets = CaptureAssetCatalogSnapshot().Assets;
		Result = TransitionMutationJournalState(
			State.Journal, EAssetMutationState::Committed);
		if (!Result) return Result;
		Private::NotifyAssetMoveObservers(State.Mappings);
		return {};
	}

	auto FAssetMutationCoordinator::RestoreAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetError::StaleData,
				"The relocation transaction state is empty.");
		auto& State = *Relocation;
		if (State.Journal.State != EAssetMutationState::Committed)
			return Error(EAssetError::StaleData,
				"Only a committed relocation can be restored.");
		FAssetResult Result = RevalidateAssetRelocation(Relocation);
		if (!Result) return Result;

		Result = TransitionMutationJournalState(
			State.Journal, EAssetMutationState::Publishing);
		if (!Result) return Result;
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
		std::vector<size_t> RestoredFiles;
		for (size_t Count = State.Journal.Entries.size(); Count > 0; --Count)
		{
			const size_t Index = Count - 1;
			Result = PublishRelocationFile(
				State.Journal.Entries[Index], false);
			if (!Result)
				return EnterRecovery(Result.Message);
			RestoredFiles.push_back(Index);
			State.Journal.Entries[Index].bCompensated = true;
			Result = WriteMutationJournalState(State.Journal);
			if (!Result) return EnterRecovery(Result.Message);
		}
		for (size_t Count = State.OwnedPayloads.size(); Count > 0; --Count)
			if (State.OwnedPayloads[Count - 1].Restore)
				State.OwnedPayloads[Count - 1].Restore();
		for (size_t Count = State.LoadedPackages.size(); Count > 0; --Count)
		{
			FLoadedRelocationState& Loaded = State.LoadedPackages[Count - 1];
			if (!Loaded.Package->RelocateAssetPackage(
					Loaded.Mapping.SourcePath))
				return EnterRecovery(
					"a loaded package could not be restored.");
			Loaded.Package->Rename(FName(Loaded.PrePackageName));
			Loaded.Package->GetAsset()->Rename(FName(Loaded.PreAssetName));
			Loaded.Package->ClearDirty();
		}

		for (FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			if (Entry.bPreExists)
			{
				Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPreFingerprint);
				if (!Result)
					return EnterRecovery(
						"restored package metadata is unavailable.");
			}
			if (!Entry.RegistryPath.IsValid()) continue;
			auto Data = State.PreAssets.find(Entry.RegistryPath);
			if (Data == State.PreAssets.end()) continue;
			std::error_code MetadataError;
			Data->second.FileSize = std::filesystem::file_size(
				Entry.PhysicalPath, MetadataError);
			Data->second.LastWriteTime = std::filesystem::last_write_time(
				Entry.PhysicalPath, MetadataError);
			if (MetadataError)
				return EnterRecovery(
					"restored package metadata is unavailable.");
			Data->second.LastWriteTimeTicks =
				FileTime::ToStableTicks(
					Data->second.LastWriteTime);
		}
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
		{
			const FAssetMutationJournalEntry* SourceEntry = nullptr;
			for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
				if (Entry.RegistryPath == Mapping.SourcePath)
				{
					SourceEntry = &Entry;
					break;
				}
			if (!SourceEntry) continue;
			for (FAssetReferenceEdge& Reference : State.PreReferenceEdges)
				if (Reference.SourcePackage == Mapping.SourcePath)
					Reference.SourceFingerprint =
						SourceEntry->ExpectedPreFingerprint;
			if (State.PreReferenceFingerprints.contains(Mapping.SourcePath))
				State.PreReferenceFingerprints.insert_or_assign(
					Mapping.SourcePath,
					SourceEntry->ExpectedPreFingerprint);
		}
		FAssetResult ProjectionResult = RebuildReferenceProjectionForPublishedEntries(
			State.Journal.Entries, State.PreAssets,
			State.PreReferenceEdges, State.PreReferenceFingerprints);
		if (!ProjectionResult)
			return EnterRecovery(std::format(
				"restored reference projection failed: {}",
				ProjectionResult.Message));

		const FAssetPublicationState Current = Registry.CapturePreparedState();
		Result = Registry.PublishPreparedState(State.ExpectedRegistryRevision, {
			.Assets = State.PreAssets,
			.ReferenceEdges = State.PreReferenceEdges,
			.ReferenceFingerprints = State.PreReferenceFingerprints,
			.ReferenceErrors = Current.ReferenceErrors,
			.bReferenceIndexComplete = Current.ReferenceErrors.empty()
				&& State.PreReferenceFingerprints.size() == State.PreAssets.size()});
		if (!Result) return Result;
		State.ExpectedRegistryRevision = GetAssetCatalogRevision();
		State.ExpectedAssets = CaptureAssetCatalogSnapshot().Assets;
		Result = TransitionMutationJournalState(
			State.Journal, EAssetMutationState::Restored);
		if (!Result) return Result;
		std::vector<FAssetRelocationMapping> Inverse;
		Inverse.reserve(State.Mappings.size());
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
			Inverse.push_back({
				.SourcePath = Mapping.DestinationPath,
				.DestinationPath = Mapping.SourcePath});
		Private::NotifyAssetMoveObservers(Inverse);
		return {};
	}
}
