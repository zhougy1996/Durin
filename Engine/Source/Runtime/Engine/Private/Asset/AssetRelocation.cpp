#include "AssetRuntimeStateInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetMutationJobInternal.h"
#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"
#include "AssetRelocationExtensionsInternal.h"
#include "Asset/EditorBulkDataStorage.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"
#include "Profiling/Profiling.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	using AssetPrivate::EAssetMutationState;
	using AssetPrivate::EAssetMutationJournalKind;
	using AssetPrivate::EMutationJournalDuplicatePolicy;
	using AssetPrivate::EAssetMutationPublicationRole;
	using AssetPrivate::FAssetMutationJournal;
	using AssetPrivate::FAssetMutationJournalEntry;
	using AssetPrivate::FingerprintRelocationFile;
	using AssetPrivate::InitializeMutationJournal;
	using AssetPrivate::IsMutationJournalRecoveryRequired;
	using AssetPrivate::LoadRelocationBytes;
	using AssetPrivate::NormalizePhysicalPath;
	using AssetPrivate::PublishRelocationFile;
	using AssetPrivate::StageMutationJournalEntry;
	using AssetPrivate::TransitionMutationJournalState;
	using AssetPrivate::WriteMutationJournalState;

	namespace
	{
		constexpr std::string_view RedirectorClassName =
			"Durin::DAssetRedirector";

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto GetRelocationPhysicalPath(const FPackagePath& Path) -> std::string
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
		};

		auto BuildMovedPackageBytes(
			FByteView SourceBytes,
			const FPackagePath& SourcePath,
			FByteView SourceBulkBytes,
			const FPackagePath& DestinationPath,
			FByteBuffer& OutBytes) -> FAssetResult
		{
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = AssetPrivate::ResolveAssetPackageReader(
				SourceBytes, Codec); !Result)
				return Result;
			if (!Codec->bCanMutate)
				return Error(EAssetError::UnsupportedVersion,
					"Relocation requires package mutation capability.");
			AssetPrivate::FAssetPackageEncodedClosure Closure;
			if (FAssetResult Result = Codec->Relocate(
				{.PackageBytes = SourceBytes,
					.BulkBytes = SourceBulkBytes,
					.PackagePath = SourcePath,
					.PhysicalPackageBytes = SourceBytes.size()},
				DestinationPath, Closure); !Result)
				return Result;
			FAssetResult Result = Codec->Validate({
				.PackageBytes = Closure.PackageBytes,
				.BulkBytes = Closure.BulkBytes,
				.PackagePath = DestinationPath,
				.PhysicalPackageBytes = Closure.PackageBytes.size()});
			if (!Result) return Result;
			OutBytes = std::move(Closure.PackageBytes);
			return {};
		}

		auto BuildRedirectorPackageBytes(
			const FPackagePath& SourcePath,
			std::span<const AssetPrivate::FAssetRedirectorWriteMapping> Mappings,
			uint32 FormatVersion,
			FByteBuffer& OutBytes) -> FAssetResult
		{
			const AssetPrivate::FAssetPackageCodec* Codec =
				AssetPrivate::FindAssetPackageWriter(FormatVersion);
			if (!Codec || !Codec->bCanMutate)
				return Error(EAssetError::UnsupportedVersion,
					"Redirector creation requires package mutation capability.");
			AssetPrivate::FAssetPackageEncodedClosure Closure;
			FAssetResult Result = Codec->WriteRedirector(
				SourcePath, Mappings, Closure);
			if (!Result) return Result;
			OutBytes = std::move(Closure.PackageBytes);
			return {};
		}
	}


	struct FAssetRelocationState
	{
		uint64 ExpectedRegistryRevision = 0;
		std::vector<FAssetRelocationMapping> Mappings;
		FAssetMutationJournal Journal;
		std::vector<FLoadedRelocationState> LoadedPackages;
		std::vector<FAssetOwnedPayloadRelocation> OwnedPayloads;
		size_t FinalizedLoadedCount = 0;
		size_t FinalizedPayloadCount = 0;
		bool bProjectionPublished = false;
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
		const auto FindPrepared = [&](const FPackagePath& Path) -> const FAssetData* {
			const auto It = Prepared.Assets.find(Path);
			return It == Prepared.Assets.end() ? nullptr : &It->second;
		};
		State->Mappings.assign(Mappings.begin(), Mappings.end());
		std::ranges::sort(State->Mappings,
			[](const FAssetRelocationMapping& A,
				const FAssetRelocationMapping& B) {
				return A.SourcePath.GetView() < B.SourcePath.GetView();
			});
		InitializeMutationJournal(
			State->Journal, EAssetMutationJournalKind::Relocation);

		std::unordered_set<FPackagePath> Sources;
		std::unordered_set<FPackagePath> Destinations;
		auto AddFileEntry = [&](const std::filesystem::path& PhysicalPath,
			const FPackagePath& RegistryPath,
			EAssetMutationPublicationRole Role,
			std::optional<FByteBuffer> PreBytes,
			std::optional<FByteBuffer> PostBytes) -> FAssetResult {
			if (AssetPrivate::ConsumeAssetRelocationFailure(
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
					? FByteView(*PreBytes)
					: FByteView{},
				.PostBytes = PostBytes
					? FByteView(*PostBytes)
					: FByteView{},
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
					.PrePackageName = Loaded->GetName()});
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
					Durin::ResolveAssetPath(Mapping.DestinationPath);
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
			FByteBuffer SourceBytes;
			FAssetResult Result = LoadRelocationBytes(SourceFile, SourceBytes);
			if (!Result) return Result;
			FByteBuffer DestinationPreBytes;
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

			FByteBuffer MovedBytes;
			FByteBuffer SourceBulkBytes;
			std::filesystem::path SourceBulkFile = SourceFile;
			SourceBulkFile.replace_extension(".dbulk");
			if (std::filesystem::is_regular_file(SourceBulkFile)
				&& !FFileHelper::LoadFileToArray(SourceBulkBytes, SourceBulkFile))
				return Error(EAssetError::IoError,
					"Relocation source bulk companion is unreadable.");
			Result = BuildMovedPackageBytes(
				SourceBytes, Mapping.SourcePath, SourceBulkBytes,
				Mapping.DestinationPath, MovedBytes);
			if (!Result) return Result;
			FByteBuffer SourceRedirectorBytes;
			std::vector<AssetPrivate::FAssetRedirectorWriteMapping> RedirectMappings;
			RedirectMappings.reserve(SourceData->TopLevelAssets.size());
			for (const FTopLevelAssetData& Asset : SourceData->TopLevelAssets)
			{
				FTopLevelAssetPath DestinationAsset;
				FObjectPath DestinationObject;
				if (!FTopLevelAssetPath::TryCreate(Mapping.DestinationPath,
						Asset.AssetPath.GetAssetName(), DestinationAsset)
					|| !FObjectPath::TryCreate(DestinationAsset,
						std::span<const std::string>{}, DestinationObject))
					return Error(EAssetError::InvalidPath,
						"Relocation could not preserve a top-level asset identity in its redirector.");
				RedirectMappings.push_back({Asset.AssetPath, std::move(DestinationObject)});
			}
			Result = BuildRedirectorPackageBytes(
				Mapping.SourcePath, RedirectMappings,
				SourceData->FormatVersion,
				SourceRedirectorBytes);
			if (!Result) return Result;

			Result = AddFileEntry(
				DestinationFile,
				Mapping.DestinationPath,
				EAssetMutationPublicationRole::RealAsset,
				bReclaimDestinationRedirector
					? std::optional<FByteBuffer>(DestinationPreBytes)
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
			Result = InspectAssetPackage(
				SourceFile.generic_string(), Mapping.SourcePath, BulkInspection);
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
				FByteBuffer PayloadBytes;
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

			DClass* AssetClass = FindClassByQualifiedName(
				FName(SourceData->AssetClassName));
			const auto Relocator = AssetPrivate::FindAssetOwnedPayloadRelocator(AssetClass);
			if (Relocator)
			{
				const auto AssetRecord = std::ranges::find(
					SourceData->TopLevelAssets, SourceData->AssetClassName,
					&FTopLevelAssetData::AssetClassName);
				if (AssetRecord == SourceData->TopLevelAssets.end())
					return Error(EAssetError::InvalidObjectGraph,
						"The package has no exact top-level asset for its payload relocator.");
				FObjectPath AssetPath;
				if (!FObjectPath::TryCreate(
					AssetRecord->AssetPath, std::span<const std::string>{}, AssetPath))
					return Error(EAssetError::InvalidPath,
						"The payload relocator asset path is invalid.");
				DObject* AssetObject = nullptr;
				Result = LoadObject(AssetPath, nullptr, AssetObject);
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
						.PrePackageName = LoadedPackage->GetName()});
				}
				FAssetOwnedPayloadRelocation Payload;
				Result = Relocator(
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
					FByteBuffer PayloadBytes;
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

	auto FAssetMutationCoordinator::PrepareAssetRelocationJob(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetRelocationSummary& OutSummary,
		FAssetMutationJob& OutJob) -> FAssetResult
	{
		OutSummary = {};
		OutJob = {};
		std::shared_ptr<FAssetRelocationState> Relocation;
		FAssetResult Result = PrepareAssetRelocationState(Mappings, Relocation);
		if (!Result) return Result;

		std::vector<FPackagePath> Scope;
		Scope.reserve(Mappings.size() * 2);
		for (const FAssetRelocationMapping& Mapping : Mappings)
		{
			Scope.push_back(Mapping.SourcePath);
			Scope.push_back(Mapping.DestinationPath);
		}
		OutSummary = FAssetRelocationSummary(
			Relocation->ExpectedRegistryRevision,
			std::move(Scope));
		auto JobState = std::make_shared<FAssetMutationJob::FState>();
		JobState->ResumeOperation = [Relocation] {
			return FAssetRuntimeState::Get().GetMutationCoordinator().ApplyAssetRelocation(Relocation);
		};
		JobState->IsRecoveryRequired = [Relocation] {
			return IsMutationJournalRecoveryRequired(Relocation->Journal);
		};
		JobState->LastResult.State =
			EAssetMutationJobState::Prepared;
		JobState->LastResult.RegistryRevision =
			GetAssetCatalogRevision();
		OutJob.State = std::move(JobState);
		return {};
	}

	auto FAssetMutationCoordinator::RevalidateAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetError::StaleData,
				"The relocation job state is empty.");
		const auto& State = *Relocation;
		if (State.Journal.State == EAssetMutationState::RecoveryRequired)
			return {
				.Error = EAssetError::IoError,
				.Message = "AssetMutationRecoveryRequired: the relocation journal requires recovery.",
				.Disposition = EAssetResultDisposition::RecoveryRequired,
				.OperationId = State.Journal.OperationId,
				.DesiredDirection = "Forward",
				.FailedParticipant = "MutationJournal",
				.RecoveryLocation = State.Journal.LocatorPath};
		if (State.Journal.State != EAssetMutationState::Prepared
			&& State.Journal.State != EAssetMutationState::Committed
			&& State.Journal.State != EAssetMutationState::Publishing)
			return Error(EAssetError::StaleData,
				"The relocation token is not in a revalidatable state.");
		if (!State.bProjectionPublished
			&& GetAssetCatalogRevision() != State.ExpectedRegistryRevision)
			return Error(EAssetError::StaleData,
				"The asset registry changed after relocation analysis.");
		const bool bExpectAllPost =
			State.Journal.State == EAssetMutationState::Committed;
		for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			const bool bExpectPost = bExpectAllPost
				|| (State.Journal.State == EAssetMutationState::Publishing
					&& Entry.bCompleted);
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
				FByteBuffer StagedBytes;
				FAssetResult Result = LoadRelocationBytes(Staged, StagedBytes);
				if (!Result || FXxHash128::HashBuffer(StagedBytes) != ExpectedHash)
					return Error(EAssetError::StaleData,
						"A staged relocation output changed.");
			}
		}
		for (size_t Index = 0; Index < State.LoadedPackages.size(); ++Index)
		{
			const FLoadedRelocationState& Loaded = State.LoadedPackages[Index];
			const bool bLoadedPost = bExpectAllPost
				|| (State.Journal.State == EAssetMutationState::Publishing
					&& Index < State.FinalizedLoadedCount);
			const FPackagePath& ExpectedPath = bLoadedPost
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
				"The relocation job state is empty.");
		auto& State = *Relocation;
		if (State.Journal.State != EAssetMutationState::Prepared
			&& State.Journal.State != EAssetMutationState::Publishing)
			return Error(EAssetError::StaleData,
				"Only a prepared or publishing relocation can resume forward.");
		FAssetResult Result = RevalidateAssetRelocation(Relocation);
		if (!Result) return Result;
		if (AssetPrivate::ConsumeAssetRelocationFailure(
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
		const bool bResuming = State.Journal.State == EAssetMutationState::Publishing;
		for (size_t OrderIndex = 0; OrderIndex < Order.size(); ++OrderIndex)
		{
			FAssetMutationJournalEntry& Entry =
				State.Journal.Entries[Order[OrderIndex]];
			Entry.PublicationOrder = static_cast<uint64>(OrderIndex);
			if (!bResuming)
			{
				Entry.bCompleted = false;
			}
		}
		if (!bResuming)
		{
			Result = TransitionMutationJournalState(
				State.Journal, EAssetMutationState::Publishing);
			if (!Result) return Result;
		}
		auto EnterRecovery = [&](std::string FailedParticipant,
			std::string Message) -> FAssetResult {
			FAssetResult JournalResult = TransitionMutationJournalState(
				State.Journal, EAssetMutationState::RecoveryRequired);
			return {
				.Error = EAssetError::IoError,
				.Message = !JournalResult
					? std::format(
						"AssetMutationRecoveryRequired: {}; additionally failed to persist recovery state: {}",
						Message, JournalResult.Message)
					: std::format("AssetMutationRecoveryRequired: {}", Message),
				.Disposition = EAssetResultDisposition::RecoveryRequired,
				.OperationId = State.Journal.OperationId,
				.DesiredDirection = "Forward",
				.FailedParticipant = std::move(FailedParticipant),
				.RecoveryLocation = State.Journal.LocatorPath};
		};
		auto ForwardPending = [&](std::string Message) -> FAssetResult {
			std::vector<FPackagePath> Paths;
			for (const FAssetRelocationMapping& Mapping : State.Mappings)
			{
				Paths.push_back(Mapping.SourcePath);
				Paths.push_back(Mapping.DestinationPath);
			}
			FenceAssetRegistryProjection(Paths);
			return {
				.Error = EAssetError::IoError,
				.Message = std::format(
					"AssetMutationForwardResumable: operation {} will resume forward. {}",
					State.Journal.OperationId, Message),
				.Disposition = EAssetResultDisposition::ForwardPending,
				.OperationId = State.Journal.OperationId,
				.DesiredDirection = "Forward",
				.RecoveryLocation = State.Journal.LocatorPath};
		};

		for (size_t Index : Order)
		{
			FAssetMutationJournalEntry& Entry = State.Journal.Entries[Index];
			if (Entry.bCompleted) continue;
			if (AssetPrivate::ConsumeAssetRelocationFailure(
					FailurePointForRole(Entry.Role)))
				return ForwardPending("Injected relocation publication failure.");
			Result = PublishRelocationFile(Entry);
			if (!Result) return ForwardPending(Result.Message);
			if (Entry.bPostExists)
			{
				Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPostFingerprint);
				if (!Result) return EnterRecovery(
					"ArtifactFingerprint", Result.Message);
			}
			Entry.bCompleted = true;
			Result = WriteMutationJournalState(State.Journal);
			if (!Result) return EnterRecovery("MutationJournal", Result.Message);
		}

		for (; State.FinalizedLoadedCount < State.LoadedPackages.size();
			++State.FinalizedLoadedCount)
		{
			FLoadedRelocationState& Loaded =
				State.LoadedPackages[State.FinalizedLoadedCount];
			if (AssetPrivate::ConsumeAssetRelocationFailure(
					EAssetRelocationFailurePoint::UpdateLoadedPackage))
				return ForwardPending("Injected loaded-package relocation failure.");
			if (!Loaded.Package->RelocateAssetPackage(
					Loaded.Mapping.DestinationPath))
				return ForwardPending("A loaded relocation destination became occupied.");
			Loaded.Package->Rename(FName(
				Loaded.Mapping.DestinationPath.GetAssetName()));
			Loaded.Package->ClearDirty();
		}
		for (; State.FinalizedPayloadCount < State.OwnedPayloads.size();
			++State.FinalizedPayloadCount)
		{
			FAssetOwnedPayloadRelocation& Payload =
				State.OwnedPayloads[State.FinalizedPayloadCount];
			if (Payload.Apply) Payload.Apply();
		}
		if (AssetPrivate::ConsumeAssetRelocationFailure(
				EAssetRelocationFailurePoint::PublishRegistry))
			return ForwardPending("Injected relocation Registry-publication failure.");

		std::vector<FPackagePath> Paths;
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
		{
			Paths.push_back(Mapping.SourcePath);
			Paths.push_back(Mapping.DestinationPath);
		}
		Result = Registry.ReconcileProjection(Paths);
		if (!Result) return ForwardPending(Result.Message);
		State.bProjectionPublished = true;
		State.ExpectedRegistryRevision = GetAssetCatalogRevision();
		Result = TransitionMutationJournalState(
			State.Journal, EAssetMutationState::Committed);
		if (!Result) return Result;
		AssetPrivate::NotifyAssetMoveObservers(State.Mappings);
		return {};
	}

}
