#include "AssetTools/MutationTesting.h"
#include "AssetMutationStagingInternal.h"
#include "Asset/PackageEditing.h"
#include "Asset/Load.h"
#include "AssetRegistry/Publication.h"
#include "Asset/AssetWriteResult.h"
#include "Asset/RegistryOperations.h"
#include "Asset/EditorBulkDataStorage.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Profiling/Profiling.h"
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
		constexpr std::string_view RedirectorClassName =
			"Durin::DAssetRedirector";

		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}

		auto GetRelocationPhysicalPath(const FPackagePath& Path) -> std::string
		{
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


	}


	struct FAssetRelocationState;
	static auto PrepareAssetRelocationState(
		std::span<const FAssetRelocationMapping> Mappings,
		std::shared_ptr<FAssetRelocationState>& OutState) -> FAssetWriteResult;
	static auto RevalidateAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetWriteResult;
	static auto ApplyAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetWriteResult;

	struct FAssetRelocationState
	{
		uint64 ExpectedRegistryRevision = 0;
		std::vector<FAssetRelocationMapping> Mappings;
		FAssetMutationStaging Staging;
		std::vector<FLoadedRelocationState> LoadedPackages;
		std::vector<FAssetOwnedPayloadRelocation> OwnedPayloads;
	};

	static auto PrepareAssetRelocationState(
		std::span<const FAssetRelocationMapping> Mappings,
		std::shared_ptr<FAssetRelocationState>& OutState) -> FAssetWriteResult
	{
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return Guard;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		OutState.reset();
		if (Mappings.empty())
			return Error(EAssetWriteError::InvalidPath,
				"An asset relocation batch must not be empty.");

		auto State = std::make_shared<FAssetRelocationState>();
		State->ExpectedRegistryRevision = GetAssetCatalogRevision();
		const FAssetRegistryPublication Prepared = CaptureAssetRegistryPublication();
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
		InitializeMutationStaging(
			State->Staging);

		std::unordered_set<FPackagePath> Sources;
		std::unordered_set<FPackagePath> Destinations;
		auto AddFileEntry = [&](const std::filesystem::path& PhysicalPath,
			const FPackagePath& RegistryPath,
			EAssetMutationPublicationRole Role,
			std::optional<FByteBuffer> PreBytes,
			std::optional<FByteBuffer> PostBytes) -> FAssetWriteResult {
			if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
					EAssetRelocationFailurePoint::PrepareOutput))
				return Error(EAssetWriteError::IoError,
					"Injected relocation output-preparation failure.");
			size_t IgnoredIndex = 0;
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
					EMutationStagingDuplicatePolicy::Reject}, IgnoredIndex);
		};

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			if (!Mapping.SourcePath.IsValid()
				|| !Mapping.DestinationPath.IsValid()
				|| Mapping.SourcePath == Mapping.DestinationPath)
				return Error(EAssetWriteError::InvalidPath,
					"Asset relocation paths are invalid or identical.");
			if (!Sources.insert(Mapping.SourcePath).second
				|| !Destinations.insert(Mapping.DestinationPath).second)
				return Error(EAssetWriteError::InvalidPath,
					"An asset relocation batch contains duplicate paths.");
		}

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			const FAssetData* SourceData = FindPrepared(Mapping.SourcePath);
			if (!SourceData)
				return Error(EAssetWriteError::NotFound, std::format(
					"Asset {} was not found.", Mapping.SourcePath.ToString()));
			if (SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetWriteError::InvalidData,
					"Redirectors cannot be used as relocation sources.");
			if (IsPackageLoading(Mapping.SourcePath))
				return Error(EAssetWriteError::InUse,
					"A relocation source is currently loading.");
			if (DPackage* Loaded = FindResidentPackage(Mapping.SourcePath))
			{
				if (Loaded->IsDirty())
					return Error(EAssetWriteError::InUse,
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
					return Error(EAssetWriteError::AlreadyExists, std::format(
						"Asset {} already exists.",
						Mapping.DestinationPath.ToString()));
				const FAssetPathResolveResult DestinationResolution =
					Durin::ResolveAssetPathForOperation(Mapping.DestinationPath);
				if (!DestinationResolution
					|| DestinationResolution.FinalPath != Mapping.SourcePath)
					return Error(EAssetWriteError::AlreadyExists, std::format(
						"The destination {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
						Mapping.DestinationPath.ToString(),
						DestinationData->RedirectDestination.ToString()));
				if (FindResidentPackage(Mapping.DestinationPath))
					return Error(EAssetWriteError::InUse,
						"A loaded destination redirector cannot be reclaimed.");
				bReclaimDestinationRedirector = true;
			}

			const std::filesystem::path SourceFile =
				NormalizePhysicalPath(SourceData->PhysicalPath);
			const std::filesystem::path DestinationFile =
				NormalizePhysicalPath(
					GetRelocationPhysicalPath(Mapping.DestinationPath));
			FByteBuffer SourceBytes;
			FAssetWriteResult Result = AssetWriteResultFromRead(LoadRelocationBytes(SourceFile, SourceBytes));
			if (!Result) return Result;
			FByteBuffer DestinationPreBytes;
			if (bReclaimDestinationRedirector)
			{
				Result = AssetWriteResultFromRead(LoadRelocationBytes(
					DestinationFile, DestinationPreBytes));
				if (!Result) return Result;
			}
			else if (std::filesystem::exists(DestinationFile))
				return Error(EAssetWriteError::AlreadyExists, std::format(
					"Relocation destination file {} already exists.",
					DestinationFile.generic_string()));

			FByteBuffer MovedBytes;
			FByteBuffer SourceBulkBytes;
			std::filesystem::path SourceBulkFile = SourceFile;
			SourceBulkFile.replace_extension(".dbulk");
			if (std::filesystem::is_regular_file(SourceBulkFile))
			{
				auto Loaded = FFileHelper::LoadFileToArray(SourceBulkFile);
				if (!Loaded) return Error(EAssetWriteError::IoError, Loaded.error().ToString());
				SourceBulkBytes = std::move(*Loaded);
			}
			Result = BuildRelocatedAssetPackageBytes(
				SourceBytes, Mapping.SourcePath, SourceBulkBytes,
				Mapping.DestinationPath, MovedBytes);
			if (!Result) return Result;
			FByteBuffer SourceRedirectorBytes;
			std::vector<FAssetRedirectorWriteMapping> RedirectMappings;
			RedirectMappings.reserve(SourceData->TopLevelAssets.size());
			for (const FTopLevelAssetData& Asset : SourceData->TopLevelAssets)
			{
				FTopLevelAssetPath DestinationAsset;
				FObjectPath DestinationObject;
				if (!FTopLevelAssetPath::TryCreate(Mapping.DestinationPath,
						Asset.AssetPath.GetAssetName(), DestinationAsset)
					|| !FObjectPath::TryCreate(DestinationAsset,
						std::span<const std::string>{}, DestinationObject))
					return Error(EAssetWriteError::InvalidPath,
						"Relocation could not preserve a top-level asset identity in its redirector.");
				RedirectMappings.push_back({Asset.AssetPath, std::move(DestinationObject)});
			}
			Result = BuildAssetRedirectorPackageBytes(
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
			Result = AssetWriteResultFromRead(InspectAssetPackage(
				SourceFile.generic_string(), Mapping.SourcePath, BulkInspection));
			if (!Result) return Result;
			std::vector<std::filesystem::path> SourceBulkFiles;
			std::vector<std::filesystem::path> DestinationBulkFiles;
			for (const auto& Pair : {std::pair{&SourceFile, &SourceBulkFiles},
				std::pair{&DestinationFile, &DestinationBulkFiles}})
				if (auto Storage = InspectEditorBulkDataCompanionPaths(*Pair.first, BulkInspection); !Storage)
					return {.Error = EAssetWriteError::InvalidData, .Message = FormatEditorBulkDataStorageError(Storage.error())};
				else { *Pair.second = std::move(*Storage); }
			if (SourceBulkFiles.size() != DestinationBulkFiles.size())
				return Error(EAssetWriteError::InvalidData, "Authored bulk relocation inspection failed.");
			for (size_t BulkIndex = 0; BulkIndex < SourceBulkFiles.size(); ++BulkIndex)
			{
				FByteBuffer PayloadBytes;
				Result = AssetWriteResultFromRead(LoadRelocationBytes(SourceBulkFiles[BulkIndex], PayloadBytes));
				if (!Result) return Result;
				if (std::filesystem::exists(DestinationBulkFiles[BulkIndex]))
					return Error(EAssetWriteError::AlreadyExists,
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
			const auto Relocator = FindAssetOwnedPayloadRelocator(AssetClass);
			if (Relocator)
			{
				const auto AssetRecord = std::ranges::find(
					SourceData->TopLevelAssets, SourceData->AssetClassName,
					&FTopLevelAssetData::AssetClassName);
				if (AssetRecord == SourceData->TopLevelAssets.end())
					return Error(EAssetWriteError::InvalidData,
						"The package has no exact top-level asset for its payload relocator.");
				FObjectPath AssetPath;
				if (!FObjectPath::TryCreate(
					AssetRecord->AssetPath, std::span<const std::string>{}, AssetPath))
					return Error(EAssetWriteError::InvalidPath,
						"The payload relocator asset path is invalid.");
				DObject* AssetObject = nullptr;
				auto Loaded = LoadObject(AssetPath, nullptr);
				AssetObject = Loaded.value_or(nullptr);
				Result = AssetWriteResultFromRead(Loaded);
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
						return Error(EAssetWriteError::InvalidPath,
							"An owned payload relocation has identical paths.");
					FByteBuffer PayloadBytes;
					Result = AssetWriteResultFromRead(LoadRelocationBytes(SourcePayload, PayloadBytes));
					if (!Result) return Result;
					if (std::filesystem::exists(DestinationPayload))
						return Error(EAssetWriteError::AlreadyExists,
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

		OutState = std::move(State);
		return {};
	}

	static auto RelocateAssetsImpl(
		std::span<const FAssetRelocationMapping> Mappings,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails
	{
		std::shared_ptr<FAssetRelocationState> Relocation;
		FAssetMutationResultDetails Details;
		Details.Result = PrepareAssetRelocationState(Mappings, Relocation);
		if (Details.Result)
		{
			if (BeforeCommit) BeforeCommit();
			Details.Result = ApplyAssetRelocation(Relocation);
			Details.AffectedFiles = Relocation->Staging.PublishedFiles;
			if (Relocation->Staging.bRetainBackups)
				Details.BackupLocations = Relocation->Staging.Roots;
		}
		return Details;
	}

	static auto RevalidateAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetWriteResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetWriteError::StaleData,
				"The relocation job state is empty.");
		const auto& State = *Relocation;
		if (GetAssetCatalogRevision() != State.ExpectedRegistryRevision)
			return Error(EAssetWriteError::StaleData,
				"The asset registry changed after relocation analysis.");
		for (const FAssetMutationStagingEntry& Entry : State.Staging.Entries)
		{
			const bool bExpectedExists = Entry.bPreExists;
			std::error_code ExistsError;
			const bool bExists = std::filesystem::exists(
				Entry.PhysicalPath, ExistsError);
			if (ExistsError || bExists != bExpectedExists)
				return Error(EAssetWriteError::StaleData, std::format(
					"Relocation participant occupancy changed: {}.",
					Entry.PhysicalPath.generic_string()));
			if (bExists)
			{
				FAssetPackageFingerprint Fingerprint;
				FAssetWriteResult Result = AssetWriteResultFromRead(FingerprintRelocationFile(
					Entry.PhysicalPath, Fingerprint));
				if (!Result) return Result;
				const FAssetPackageFingerprint& Expected = Entry.ExpectedPreFingerprint;
				if (Fingerprint != Expected)
					return Error(EAssetWriteError::StaleData, std::format(
						"Relocation participant changed: {}.",
						Entry.PhysicalPath.generic_string()));
			}
			const bool bOutputExists = Entry.bPostExists;
			const std::filesystem::path& Staged = Entry.StagedPostPath;
			const FXxHash128& ExpectedHash = Entry.StagedPostHash;
			if (bOutputExists)
			{
				FByteBuffer StagedBytes;
				FAssetWriteResult Result = AssetWriteResultFromRead(LoadRelocationBytes(Staged, StagedBytes));
				if (!Result || FXxHash128::HashBuffer(StagedBytes) != ExpectedHash)
					return Error(EAssetWriteError::StaleData,
						"A staged relocation output changed.");
			}
		}
		for (size_t Index = 0; Index < State.LoadedPackages.size(); ++Index)
		{
			const FLoadedRelocationState& Loaded = State.LoadedPackages[Index];
			const FPackagePath& ExpectedPath = Loaded.Mapping.SourcePath;
			if (FindResidentPackage(ExpectedPath) != Loaded.Package)
				return Error(EAssetWriteError::StaleData,
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

	static auto ApplyAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetWriteResult
	{
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return Guard;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetWriteError::StaleData,
				"The relocation job state is empty.");
		auto& State = *Relocation;
		FAssetWriteResult Result = RevalidateAssetRelocation(Relocation);
		if (!Result) return Result;
		if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
				EAssetRelocationFailurePoint::StageOriginal))
			return Error(EAssetWriteError::IoError,
				"Injected relocation original-staging failure.");

		std::vector<size_t> Order(State.Staging.Entries.size());
		for (size_t Index = 0; Index < Order.size(); ++Index)
			Order[Index] = Index;
		std::ranges::stable_sort(Order, [&](size_t A, size_t B) {
			return State.Staging.Entries[A].Role
				< State.Staging.Entries[B].Role;
		});
		State.Staging.bRetainBackups = true;
		auto PublicationFailed = [&](std::string Message) -> FAssetWriteResult {
			std::vector<FPackagePath> Paths;
			for (const FAssetRelocationMapping& Mapping : State.Mappings)
			{
				Paths.push_back(Mapping.SourcePath);
				Paths.push_back(Mapping.DestinationPath);
			}
			FenceAssetRegistryProjection(Paths);
			return {
				.Error = EAssetWriteError::IoError,
				.Message = std::format(
					"AssetMutationFailed: operation {} stopped; inspect partial changes before preparing another operation. {}",
					State.Staging.OperationId, Message),
				.Effect = State.Staging.PublishedFiles.empty() ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten};
		};

		for (size_t Index : Order)
		{
			FAssetMutationStagingEntry& Entry = State.Staging.Entries[Index];
			if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
					FailurePointForRole(Entry.Role)))
				return PublicationFailed("Injected relocation publication failure.");
			Result = PublishRelocationFile(Entry);
			if (!Result) return PublicationFailed(Result.Message);
			State.Staging.PublishedFiles.push_back(Entry.PhysicalPath);
		}

		for (FLoadedRelocationState& Loaded : State.LoadedPackages)
		{
			if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
					EAssetRelocationFailurePoint::UpdateLoadedPackage))
				return PublicationFailed("Injected loaded-package relocation failure.");
			if (!Loaded.Package->RelocateAssetPackage(
					Loaded.Mapping.DestinationPath))
				return PublicationFailed("A loaded relocation destination became occupied.");
			Loaded.Package->Rename(FName(
				Loaded.Mapping.DestinationPath.GetPackageName()));
			Loaded.Package->ClearDirty();
		}
		for (FAssetOwnedPayloadRelocation& Payload : State.OwnedPayloads)
		{
			if (Payload.Apply) Payload.Apply();
		}
		if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
				EAssetRelocationFailurePoint::PublishRegistry))
			return PublicationFailed("Injected relocation Registry-publication failure.");

		std::vector<FPackagePath> Paths;
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
		{
			Paths.push_back(Mapping.SourcePath);
			Paths.push_back(Mapping.DestinationPath);
		}
		Result = AssetWriteResultFromRead(RefreshSavedPackages(Paths));
		if (!Result) return PublicationFailed(Result.Message);
		State.Staging.bRetainBackups = false;
		for (const auto& Loaded : State.LoadedPackages)
			for (DObject* Object : Loaded.Package->GetTopLevelAssets())
				if (auto* Function = Cast<DMaterialFunctionInterface>(Object))
					NotifyMaterialFunctionChanged(*Function);
		NotifyAssetMoveObservers(State.Mappings);
		return {};
	}


	auto RelocateAssets(std::span<const FAssetRelocationMapping> Mappings)
		-> FAssetMutationResultDetails
	{ return RelocateAssetsImpl(Mappings, {}); }

	auto RelocateAssetsWithBeforeCommitForTesting(
		std::span<const FAssetRelocationMapping> Mappings,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails
	{ return RelocateAssetsImpl(Mappings, BeforeCommit); }
}
