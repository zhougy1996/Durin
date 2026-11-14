#include "AssetTools/MutationTesting.h"
#include "Misc/UnifiedError.h"
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
		auto Reject(std::optional<UnifiedError::FError>* OutError, UnifiedError::FError Error) -> bool
		{ if (OutError) *OutError = std::move(Error); return false; }
		constexpr std::string_view RedirectorClassName =
			"Durin::DAssetRedirector";

		auto WriteFailure(UnifiedError::FErrorId Code, std::string Message) -> FAssetWriteResult
		{ return {.Error = UnifiedError::FError{Code, std::move(Message)}};
		}

		auto GetRelocationPhysicalPath(const FPackagePath& Path) -> std::string
		{
			const auto Resolved =
				FMountPaths::ResolveAssetPath(
					Path.GetView(), EMountPathExistence::AllowMissing);
			if (!Resolved)
				DURIN_WARN_CATEGORY(
					"AssetSystem",
					"Failed to resolve asset path {}: {}",
					Path.ToString(),
					(Resolved ? std::string{} : ToString(Resolved.error())));
			return Resolved
				? Resolved->PhysicalPath.generic_string() + ".dasset"
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
		std::optional<UnifiedError::FError>* OutError) -> std::shared_ptr<FAssetRelocationState>;
	static auto RevalidateAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation, std::optional<UnifiedError::FError>* OutError) -> bool;
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
		std::optional<UnifiedError::FError>* OutError) -> std::shared_ptr<FAssetRelocationState>
	{
		if (OutError) OutError->reset();
		const auto RejectPreparation = [&](UnifiedError::FError Error) -> std::shared_ptr<FAssetRelocationState> {
			if (OutError) *OutError = std::move(Error);
			return {};
		};
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return RejectPreparation(Guard.error());
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (Mappings.empty())
			return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
				"An asset relocation batch must not be empty."});

		auto State = std::make_shared<FAssetRelocationState>();
		State->ExpectedRegistryRevision = GetAssetCatalogRevision();
		const FAssetRegistryPublication Prepared = CaptureAssetRegistryPublication();
		const auto FindPrepared = [&](const FPackagePath& Path) -> const FPackageData* {
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
			std::optional<FByteBuffer> PostBytes) -> bool {
			if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
					EAssetRelocationFailurePoint::PrepareOutput))
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::IoError,
					"Injected relocation output-preparation failure."});
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
					EMutationStagingDuplicatePolicy::Reject}, IgnoredIndex, OutError);
		};

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			if (!Mapping.SourcePath.IsValid()
				|| !Mapping.DestinationPath.IsValid()
				|| Mapping.SourcePath == Mapping.DestinationPath)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
					"Asset relocation paths are invalid or identical."});
			if (!Sources.insert(Mapping.SourcePath).second
				|| !Destinations.insert(Mapping.DestinationPath).second)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
					"An asset relocation batch contains duplicate paths."});
		}

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			const FPackageData* SourceData = FindPrepared(Mapping.SourcePath);
			if (!SourceData)
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::NotFound, std::format(
					"Asset {} was not found.", Mapping.SourcePath.ToString())});
			if ((!SourceData->TopLevelAssets.empty() && std::ranges::all_of(SourceData->TopLevelAssets, &FAssetData::IsRedirector)))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidData,
					"Redirectors cannot be used as relocation sources."});
			if (IsPackageLoading(Mapping.SourcePath))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
					"A relocation source is currently loading."});
			if (DPackage* Loaded = FindResidentPackage(Mapping.SourcePath))
			{
				if (Loaded->IsDirty())
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
						"A dirty loaded asset must be saved before relocation."});
				State->LoadedPackages.push_back({
					.Mapping = Mapping,
					.Package = Loaded,
					.PrePackageName = Loaded->GetName()});
			}

			bool bReclaimDestinationRedirector = false;
			if (const FPackageData* DestinationData = FindPrepared(Mapping.DestinationPath))
			{
				if (!(!DestinationData->TopLevelAssets.empty() && std::ranges::all_of(DestinationData->TopLevelAssets, &FAssetData::IsRedirector)))
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::AlreadyExists, std::format(
						"Asset {} already exists.",
						Mapping.DestinationPath.ToString())});
				const FPackagePathResolveResult DestinationResolution =
					ResolvePackagePathForOperation(Mapping.DestinationPath);
				if (!DestinationResolution
					|| DestinationResolution.FinalPath != Mapping.SourcePath)
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::AlreadyExists, std::format(
						"The destination {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
						Mapping.DestinationPath.ToString(),
						DestinationResolution.FinalPath.ToString())});
				if (FindResidentPackage(Mapping.DestinationPath))
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InUse,
						"A loaded destination redirector cannot be reclaimed."});
				bReclaimDestinationRedirector = true;
			}

			const std::filesystem::path SourceFile =
				NormalizePhysicalPath(SourceData->PhysicalPath);
			const std::filesystem::path DestinationFile =
				NormalizePhysicalPath(
					GetRelocationPhysicalPath(Mapping.DestinationPath));
			FByteBuffer SourceBytes;
			bool Result = true;
			if (auto Prepared = LoadRelocationBytes(SourceFile, SourceBytes); !Prepared)
				return RejectPreparation(std::move(Prepared.error()));
			FByteBuffer DestinationPreBytes;
			if (bReclaimDestinationRedirector)
			{
				if (auto Prepared = LoadRelocationBytes(
					DestinationFile, DestinationPreBytes); !Prepared)
					return RejectPreparation(std::move(Prepared.error()));
			}
			else if (std::filesystem::exists(DestinationFile))
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::AlreadyExists, std::format(
					"Relocation destination file {} already exists.",
					DestinationFile.generic_string())});

			FByteBuffer MovedBytes;
			FByteBuffer SourceBulkBytes;
			std::filesystem::path SourceBulkFile = SourceFile;
			SourceBulkFile.replace_extension(".dbulk");
			if (std::filesystem::is_regular_file(SourceBulkFile))
			{
				auto Loaded = FFileHelper::LoadFileToArray(SourceBulkFile);
				if (!Loaded) return RejectPreparation(ToError(Loaded.error()));
				SourceBulkBytes = std::move(*Loaded);
			}
			if (auto Prepared = BuildRelocatedAssetPackageBytes(
				SourceBytes, Mapping.SourcePath, SourceBulkBytes,
				Mapping.DestinationPath, MovedBytes); !Prepared)
				return RejectPreparation(std::move(Prepared.error()));
			FByteBuffer SourceRedirectorBytes;
			std::vector<FAssetRedirectorWriteMapping> RedirectMappings;
			RedirectMappings.reserve(SourceData->TopLevelAssets.size());
			for (const FAssetData& Asset : SourceData->TopLevelAssets)
			{
				FTopLevelAssetPath DestinationAsset;
				FSoftObjectPath DestinationObject;
				if (!FTopLevelAssetPath::TryCreate(Mapping.DestinationPath,
						Asset.AssetPath.GetAssetName(), DestinationAsset)
					|| !FSoftObjectPath::TryCreate(DestinationAsset,
						std::span<const std::string>{}, DestinationObject))
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
						"Relocation could not preserve a top-level asset identity in its redirector."});
				RedirectMappings.push_back({Asset.AssetPath, std::move(DestinationObject)});
			}
			if (auto Prepared = BuildAssetRedirectorPackageBytes(
				Mapping.SourcePath, RedirectMappings, SourceRedirectorBytes); !Prepared)
				return RejectPreparation(std::move(Prepared.error()));

			Result = AddFileEntry(
				DestinationFile,
				Mapping.DestinationPath,
				EAssetMutationPublicationRole::RealAsset,
				bReclaimDestinationRedirector
					? std::optional<FByteBuffer>(DestinationPreBytes)
					: std::nullopt,
				std::move(MovedBytes));
			if (!Result) return {};
			Result = AddFileEntry(
				SourceFile,
				Mapping.SourcePath,
				EAssetMutationPublicationRole::Redirector,
				SourceBytes,
				std::move(SourceRedirectorBytes));
			if (!Result) return {};

			FAssetPackageInspection BulkInspection;
			if (auto Prepared = InspectAssetPackage(
				SourceFile.generic_string(), Mapping.SourcePath, BulkInspection); !Prepared)
				return RejectPreparation(std::move(Prepared.error()));
			std::vector<std::filesystem::path> SourceBulkFiles;
			std::vector<std::filesystem::path> DestinationBulkFiles;
			for (const auto& Pair : {std::pair{&SourceFile, &SourceBulkFiles},
				std::pair{&DestinationFile, &DestinationBulkFiles}})
				if (auto Storage = InspectEditorBulkDataCompanionPaths(*Pair.first, BulkInspection); !Storage)
					return RejectPreparation(ToError(Storage.error()));
				else { *Pair.second = std::move(*Storage); }
			if (SourceBulkFiles.size() != DestinationBulkFiles.size())
				return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidData, "Authored bulk relocation inspection failed."});
			for (size_t BulkIndex = 0; BulkIndex < SourceBulkFiles.size(); ++BulkIndex)
			{
				FByteBuffer PayloadBytes;
				if (auto Prepared = LoadRelocationBytes(SourceBulkFiles[BulkIndex], PayloadBytes); !Prepared)
					return RejectPreparation(std::move(Prepared.error()));
				if (std::filesystem::exists(DestinationBulkFiles[BulkIndex]))
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::AlreadyExists,
						"Authored bulk relocation destination already exists."});
				Result = AddFileEntry(DestinationBulkFiles[BulkIndex], {},
					EAssetMutationPublicationRole::OwnedPayload,
					std::nullopt, PayloadBytes);
				if (!Result) return {};
				Result = AddFileEntry(SourceBulkFiles[BulkIndex], {},
					EAssetMutationPublicationRole::OwnedPayload,
					std::move(PayloadBytes), std::nullopt);
				if (!Result) return {};
			}

			for (const FAssetData& AssetRecord : SourceData->TopLevelAssets)
			{
				std::optional<Durin::UnifiedError::FError> LoadedError;
				DClass* AssetClass = FindClassByQualifiedName(FName(AssetRecord.AssetClassName));
				const auto Relocator = FindAssetOwnedPayloadRelocator(AssetClass);
				if (!Relocator) continue;
				FSoftObjectPath AssetPath;
				if (!FSoftObjectPath::TryCreate(
					AssetRecord.AssetPath, std::span<const std::string>{}, AssetPath))
					return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
						"The payload relocator asset path is invalid."});
				DObject* AssetObject = nullptr;
				auto Loaded = LoadObject(AssetPath, nullptr, nullptr, &LoadedError);
				AssetObject = Loaded;
				if (!Loaded) return RejectPreparation(*LoadedError);
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
				if (auto Prepared = Relocator(
					AssetObject, Mapping.SourcePath,
					Mapping.DestinationPath, Payload); !Prepared)
					return RejectPreparation(std::move(Prepared.error()));
				for (const auto& [From, To] : Payload.Files)
				{
					const std::filesystem::path SourcePayload =
						NormalizePhysicalPath(From);
					const std::filesystem::path DestinationPayload =
						NormalizePhysicalPath(To);
					if (SourcePayload == DestinationPayload)
						return RejectPreparation(UnifiedError::FError{AssetWriteErrors::InvalidPath,
							"An owned payload relocation has identical paths."});
					FByteBuffer PayloadBytes;
					if (auto Prepared = LoadRelocationBytes(SourcePayload, PayloadBytes); !Prepared)
						return RejectPreparation(std::move(Prepared.error()));
					if (std::filesystem::exists(DestinationPayload))
						return RejectPreparation(UnifiedError::FError{AssetWriteErrors::AlreadyExists,
							"An owned payload destination already exists."});
					Result = AddFileEntry(
						DestinationPayload, {},
						EAssetMutationPublicationRole::OwnedPayload,
						std::nullopt, PayloadBytes);
					if (!Result) return {};
					Result = AddFileEntry(
						SourcePayload, {},
						EAssetMutationPublicationRole::OwnedPayload,
						std::move(PayloadBytes), std::nullopt);
					if (!Result) return {};
				}
				State->OwnedPayloads.push_back(std::move(Payload));
			}
		}

		return State;
	}

	static auto RelocateAssetsImpl(
		std::span<const FAssetRelocationMapping> Mappings,
		const std::function<void()>& BeforeCommit) -> FAssetMutationResultDetails
	{
		FAssetMutationResultDetails Details;
		auto Relocation = PrepareAssetRelocationState(Mappings, &Details.Result.Error);
		if (Relocation)
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
		const std::shared_ptr<FAssetRelocationState>& Relocation, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
				"The relocation job state is empty."});
		const auto& State = *Relocation;
		if (GetAssetCatalogRevision() != State.ExpectedRegistryRevision)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
				"The asset registry changed after relocation analysis."});
		for (const FAssetMutationStagingEntry& Entry : State.Staging.Entries)
		{
			const bool bExpectedExists = Entry.bPreExists;
			std::error_code ExistsError;
			const bool bExists = std::filesystem::exists(
				Entry.PhysicalPath, ExistsError);
			if (ExistsError || bExists != bExpectedExists)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, std::format(
					"Relocation participant occupancy changed: {}.",
					Entry.PhysicalPath.generic_string())});
			if (bExists)
			{
				FAssetPackageFingerprint Fingerprint;
				if (auto Prepared = FingerprintRelocationFile(
					Entry.PhysicalPath, Fingerprint); !Prepared)
					return Reject(OutError, std::move(Prepared.error()));
				const FAssetPackageFingerprint& Expected = Entry.ExpectedPreFingerprint;
				if (Fingerprint != Expected)
					return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, std::format(
						"Relocation participant changed: {}.",
						Entry.PhysicalPath.generic_string())});
			}
			const bool bOutputExists = Entry.bPostExists;
			const std::filesystem::path& Staged = Entry.StagedPostPath;
			const FXxHash128& ExpectedHash = Entry.StagedPostHash;
			if (bOutputExists)
			{
				FByteBuffer StagedBytes;
				if (auto Prepared = LoadRelocationBytes(Staged, StagedBytes); !Prepared)
					return Reject(OutError, std::move(Prepared.error()));
				if (FXxHash128::HashBuffer(StagedBytes) != ExpectedHash)
					return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
						"A staged relocation output changed."});
			}
		}
		for (size_t Index = 0; Index < State.LoadedPackages.size(); ++Index)
		{
			const FLoadedRelocationState& Loaded = State.LoadedPackages[Index];
			const FPackagePath& ExpectedPath = Loaded.Mapping.SourcePath;
			if (FindResidentPackage(ExpectedPath) != Loaded.Package)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
					"A loaded relocation participant changed identity."});
		}
		return true;
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
		if (auto Guard = CheckAssetPackageMutationAllowed(); !Guard) return FAssetWriteResult{Guard.error()};
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return WriteFailure(AssetWriteErrors::StaleData,
				"The relocation job state is empty.");
		auto& State = *Relocation;
		FAssetWriteResult Result;
		if (!RevalidateAssetRelocation(Relocation, &Result.Error)) return Result;
		if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
				EAssetRelocationFailurePoint::StageOriginal))
			return WriteFailure(AssetWriteErrors::IoError,
				"Injected relocation original-staging failure.");

		std::vector<size_t> Order(State.Staging.Entries.size());
		for (size_t Index = 0; Index < Order.size(); ++Index)
			Order[Index] = Index;
		std::ranges::stable_sort(Order, [&](size_t A, size_t B) {
			return State.Staging.Entries[A].Role
				< State.Staging.Entries[B].Role;
		});
		State.Staging.bRetainBackups = true;
		auto PublicationFailed = [&](UnifiedError::FError Failure) -> FAssetWriteResult {
			std::vector<FPackagePath> Paths;
			for (const FAssetRelocationMapping& Mapping : State.Mappings)
			{
				Paths.push_back(Mapping.SourcePath);
				Paths.push_back(Mapping.DestinationPath);
			}
			FenceAssetRegistryProjection(Paths);
			Failure.PushContext("operation", std::format(
				"AssetMutationFailed: operation {} stopped; inspect partial changes before preparing another operation.",
				State.Staging.OperationId));
			return {.Error = std::move(Failure), .Effect = State.Staging.PublishedFiles.empty() ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten};
		};

		for (size_t Index : Order)
		{
			FAssetMutationStagingEntry& Entry = State.Staging.Entries[Index];
			if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
					FailurePointForRole(Entry.Role)))
				return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "Injected relocation publication failure."});
			if (!PublishRelocationFile(Entry, &Result.Error)) return PublicationFailed(*Result.Error);
			State.Staging.PublishedFiles.push_back(Entry.PhysicalPath);
		}

		for (FLoadedRelocationState& Loaded : State.LoadedPackages)
		{
			if (AssetToolsPrivate::ConsumeAssetRelocationFailure(
					EAssetRelocationFailurePoint::UpdateLoadedPackage))
				return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "Injected loaded-package relocation failure."});
			if (!Loaded.Package->RelocateAssetPackage(
					Loaded.Mapping.DestinationPath))
				return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "A loaded relocation destination became occupied."});
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
			return PublicationFailed(UnifiedError::FError{AssetWriteErrors::IoError, "Injected relocation Registry-publication failure."});

		std::vector<FPackagePath> Paths;
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
		{
			Paths.push_back(Mapping.SourcePath);
			Paths.push_back(Mapping.DestinationPath);
		}
		if (auto Prepared = RefreshSavedPackages(Paths); !Prepared)
			return PublicationFailed(std::move(Prepared.error()));
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
