#include "AssetLiveLoadGuard.h"
#include "Asset/RegistryOperations.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetMutationRegistryInternal.h"
#include "AssetPackageInspectionInternal.h"
#include "AssetRegistryResultAdapter.h"
#include "Asset/PackageSerialization.h"
#include "DObject/ObjectGraphReplacement.h"
#include "AssetPackageCodec.h"
#include "AssetPackageSaveInternal.h"
#include "Asset/PackageVersionPolicy.h"
#include "EditorBulkDataSaveRetention.h"
#include "Asset/PackageResource.h"
#include "DObject/DefaultDeltaPlan.h"

#include "CoreGlobals.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "DObject/PackagePersistence.h"
#include "Misc/FileTime.h"
#include "Misc/MountPaths.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"
#include "Threading/Private/ExternalTaskResult.h"
#include "Misc/Guid.h"

namespace Durin
{
	using AssetPrivate::LoadPackageBulkBytes;
	using AssetPrivate::FPreparedPackageSave;
	using AssetPrivate::BuildSavedAssetMetadata;
	using AssetPrivate::ToAssetWriteResult;
	using AssetPrivate::CommitPreparedPackageSave;

	namespace
	{
		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult { return {Code, std::move(Message)}; }
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult { return {Code, std::move(Message)}; }

		auto GetPhysicalPath(const FPackagePath& Path) -> std::string
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
			const auto Resolved =
				FMountPaths::ResolveAssetPath(Path.GetView(), EMountPathExistence::AllowMissing);
			if (!Resolved)
				DURIN_WARN_CATEGORY(
					"AssetSystem", "Failed to resolve asset path {}: {}", Path.ToString(), (Resolved ? std::string{} : Durin::ToString(Resolved.error())));
			return Resolved ? Resolved->PhysicalPath.generic_string() + ".dasset" : std::string{};
		}

		auto EncodePackageClosure(DPackage* Package,
			AssetPrivate::FAssetPackageEncodedClosure& OutClosure,
			const FAssetPackageSerializationOptions& Options) -> ObjectPackage::FPackageWriterResult
		{
			const auto* Codec = AssetPrivate::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion);
			if (!Codec)
				return {.Failure = ObjectPackage::EPackageWriterFailure::InvalidInput,
					.Reason = ObjectPackage::EPackageWriterReason::UnsupportedVersion,
					.Message = "The ordinary asset package writer is unavailable."};
			return Codec->Write(Package, OutClosure,
				Options.Mode == EAssetPackageSaveMode::Complete
					? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, Options);
		}
	}

	auto SerializeAssetPackageBytes(
		DPackage* Package,
		FByteBuffer& OutBytes,
		const FAssetPackageSerializationOptions& Options) -> ObjectPackage::FPackageWriterResult
	{
		AssetPrivate::FAssetPackageEncodedClosure Closure;
		auto Result = EncodePackageClosure(Package, Closure, Options);
		if (Result) OutBytes = std::move(Closure.PackageBytes);
		return Result;
	}

	auto SerializeAssetPackageClosure(
		DPackage* Package,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes,
		const FAssetPackageSerializationOptions& Options) -> ObjectPackage::FPackageWriterResult
	{
		AssetPrivate::FAssetPackageEncodedClosure Closure;
		auto Result = EncodePackageClosure(Package, Closure, Options);
		if (!Result) return Result;
		OutBytes = std::move(Closure.PackageBytes);
		OutBulkBytes = std::move(Closure.BulkBytes);
		return {};
	}

	namespace
	{
		auto BeginAssetWrite(const std::filesystem::path& Destination, FByteBuffer Bytes, FByteBuffer Bulk,
			const FFilePublicationStamp& MainStamp, const FFilePublicationStamp& BulkStamp, bool bDirect = false)
			-> std::unique_ptr<IPackageWriteOperation>
		{
			// Each writer owns only its transaction's siblings; abandoned files from
			// earlier saves neither participate in recovery nor reserve these paths.
			const auto Suffix = ".save-" + FGuid::NewGuid().ToString();
			auto Companion = Destination; Companion.replace_extension(".dbulk");
			const auto MainPrefix = Destination.string() + Suffix;
			const auto BulkPrefix = Companion.string() + Suffix;
			std::vector<FPackageWriteFile> Files;
			Files.push_back({{Companion, Bulk.empty() ? std::filesystem::path{} : std::filesystem::path{BulkPrefix + ".stage.tmp"},
				BulkPrefix + ".backup.tmp"}, BulkStamp, std::move(Bulk)});
			Files.push_back({{Destination, MainPrefix + ".stage.tmp", MainPrefix + ".backup.tmp"}, MainStamp, std::move(Bytes)});
			return (bDirect ? GetDirectFilePackageWriter() : GetFilePackageWriter())->Begin(std::move(Files));
		}
		// Snapshot by value: callers may pass the package's mutable identity member.
		auto PreparePackageSave(DPackage* Package, const FPackagePath Path,
			EAssetPackageSaveMode Mode, FPreparedPackageSave& Out, bool bDirect = false) -> FAssetWriteResult
		{
			if (auto Result = AssetPrivate::ValidatePackageSaveAdmission(Path); !Result) return Result;
			FPreparedPackageSave Prepared;
			Prepared.Package = Package;
			Prepared.Destination = GetPhysicalPath(Path);
			if (Prepared.Destination.empty()) return Error(EAssetWriteError::InvalidPath, "Cannot resolve package destination.");
			auto ReadAccess = FPackageFileAccess::TryReadPackage(Prepared.Destination);
			if (!ReadAccess) return Error(EAssetWriteError::InUse, "Package output is being written.");
			auto Companion = Prepared.Destination; Companion.replace_extension(".dbulk");
			FFilePublicationStamp MainStamp, BulkStamp;
			if (!FFilePublicationStamp::Inspect(Prepared.Destination, MainStamp) || !FFilePublicationStamp::Inspect(Companion, BulkStamp))
				return Error(EAssetWriteError::AlreadyExists, "Package destination is unavailable or occupied.");
			Prepared.Revision = Package->GetEditRevision();
			AssetPrivate::FAssetPackageEncodedClosure Closure;
			FAssetPackageSerializationOptions Serialization; Serialization.Mode = Mode;
			AssetPrivate::FScopedBulkSaveRetention RetainSources(bDirect);
			if (auto Result = EncodePackageClosure(Package, Closure, Serialization); !Result)
				return AssetWriteResultFromEncoding(Result);
			// The codec validates the closure. Capture its canonical header once;
			// publication never rebuilds metadata from mutable live objects.
			const AssetPrivate::FAssetPackageReadContext Context{
				.PackageBytes = Closure.PackageBytes, .BulkBytes = Closure.BulkBytes,
				.PackagePath = Path, .PhysicalPackageBytes = Closure.PackageBytes.size()};
			if (auto Result = AssetPrivate::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion)
				->ReadHeader(Context, Prepared.Header); !Result) return AssetWriteResultFromRead(Result);
			if (Package->GetEditRevision() != Prepared.Revision || Package->GetPackagePathIdentity() != Prepared.Header.PackagePath
				|| GetPhysicalPath(Prepared.Header.PackagePath) != Prepared.Destination.generic_string())
				return Error(EAssetWriteError::StaleData, "Package or destination changed during capture.");
			Prepared.DetachedBytes = Closure.PackageBytes.size() + Closure.BulkBytes.size();
			if (bDirect)
			{
				if (auto Result = PackageSavePrivate::CheckAsyncAdmission(Prepared.DetachedBytes); !Result)
					return Error(EAssetWriteError::InUse, Result.Message);
				ReadAccess.reset();
				GetPackageResourceManager().RetirePackage(Path.ToString());
			}
			Prepared.Write = BeginAssetWrite(Prepared.Destination, std::move(Closure.PackageBytes), std::move(Closure.BulkBytes),
				MainStamp, BulkStamp, bDirect);
			if (auto Result = ToAssetWriteResult(Prepared.Write->GetAdmissionResult()); !Result) return Result;
			// Catch registration which finished between the first retirement and
			// exclusive physical admission. New registration is now excluded.
			if (bDirect) GetPackageResourceManager().RetirePackage(Path.ToString());
			Out = std::move(Prepared);
			return {};
		}

		using FSaveParticipants = std::unordered_map<FPackagePath, std::optional<FAssetData>>;
		auto SaveParticipantsChanged(const FSaveParticipants& Participants, const FPackagePath& Path) -> bool
		{
			return std::ranges::any_of(Participants, [&](const auto& Entry) {
				const auto Current = FindAssetExact(Entry.first).Data;
				// The saved package remains an exact participant. Dependencies only need
				// stable identities and reference routing, not unchanged content metadata.
				if (Entry.first == Path) return Current != Entry.second;
				if (IsAssetRegistryProjectionFenced(Entry.first)) return true;
				if (Current.has_value() != Entry.second.has_value()) return true;
				if (!Current) return false;
				return Current->PhysicalPath != Entry.second->PhysicalPath
					|| !std::ranges::is_permutation(Current->TopLevelAssets, Entry.second->TopLevelAssets);
			});
		}
	}

	struct FPreparedAssetSave::FState
	{
		FPreparedPackageSave Prepared;
		FSaveParticipants Participants;
		FAssetBundleSaveOptions Options;
		FAssetWriteResult StagingResult;
		std::optional<FAssetWriteResult> Result;
		bool bReady = false;
	};

	FPreparedAssetSave::FPreparedAssetSave() : State(std::make_unique<FState>()) {}
	FPreparedAssetSave::~FPreparedAssetSave() { check(IsInGameThread()); }
	auto FPreparedAssetSave::IsReady() const -> bool { check(IsInGameThread()); return State->bReady; }
	auto FPreparedAssetSave::Begin(DPackage* Package, const FAssetBundleSaveOptions& Options,
		FAssetWriteResult& Admission) -> std::shared_ptr<FPreparedAssetSave>
	{
		check(IsInGameThread());
		if (!Package || !Package->IsAssetPackage() || !Package->IsGraphPrivate()
			|| Options.PreparedPublication || !Options.bRollbackOnRegistryFailure
			|| (Options.RootPackage && Options.RootPackage != Package))
		{ Admission = Error(EAssetWriteError::InvalidData, "Prepared staging requires a private asset package and rollback policy."); return {}; }
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard)
		{ Admission = AssetWriteResultFromRead(Guard); return {}; }
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
		{ Admission = Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved."); return {}; }
		if (auto Check = PackageSavePrivate::CheckAsyncAdmission(); !Check)
		{ Admission = Error(EAssetWriteError::ShuttingDown, Check.Message); return {}; }
		const auto Expected = CaptureAssetRegistryPublication();
		if (!Expected.bReferenceIndexComplete || !Expected.ReferenceErrors.empty())
		{ Admission = Error(EAssetWriteError::StaleData, "Prepared publication requires a complete Registry projection."); return {}; }
		auto Operation = std::shared_ptr<FPreparedAssetSave>(new FPreparedAssetSave());
		auto& Data = *Operation->State;
		Data.Options = Options;
		Admission = PreparePackageSave(Package, Package->GetPackagePathIdentity(), Options.Mode, Data.Prepared);
		if (!Admission) return {};
		Data.Options.ShouldFail = [Inject = Options.ShouldFail, bHasBulk = Data.Prepared.Header.BulkSegmentExtent != 0](EAssetBundleSavePhase Phase, size_t Index) {
			return Inject && (Phase != EAssetBundleSavePhase::PublishCompanion || bHasBulk) && Inject(Phase, Index);
		};
		Data.Participants.emplace(Data.Prepared.Header.PackagePath, FindAssetExact(Data.Prepared.Header.PackagePath).Data);
		for (const auto& Dependency : Data.Prepared.Header.Dependencies)
			Data.Participants.emplace(Dependency, FindAssetExact(Dependency).Data);
		auto Submitted = PackageSavePrivate::SubmitAsyncSave(Data.Prepared.DetachedBytes,
			[Operation] { Operation->State->StagingResult = ToAssetWriteResult(Operation->State->Prepared.Write->Stage()); },
			[Operation](bool bSucceeded) {
				if (!bSucceeded) Operation->State->StagingResult = Error(EAssetWriteError::IoError, "Prepared save staging failed.");
				Operation->State->bReady = true;
			});
		if (!Submitted) { Admission = Error(EAssetWriteError::InUse, Submitted.Message); return {}; }
		Admission = {};
		return Operation;
	}
	auto FPreparedAssetSave::Commit(const FObjectGraphReplacement& Publication) -> FAssetWriteResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (!Data.bReady) return Error(EAssetWriteError::InUse, "Prepared save staging is unfinished.");
		if (Data.Result) return *Data.Result;
		if (!Data.StagingResult) return *(Data.Result = Data.StagingResult);
		auto* Package = Data.Prepared.Package.Get();
		const auto Expected = CaptureAssetRegistryPublication();
		if (!Publication.OwnsPreparedPackage(*Package)
			|| SaveParticipantsChanged(Data.Participants, Data.Prepared.Header.PackagePath)
			|| GetPhysicalPath(Data.Prepared.Header.PackagePath) != Data.Prepared.Destination.generic_string()
			|| !Expected.bReferenceIndexComplete || !Expected.ReferenceErrors.empty())
			return *(Data.Result = Error(EAssetWriteError::StaleData, "Prepared save participants changed during staging."));
		return *(Data.Result = CommitPreparedPackageSave(Data.Prepared, Data.Options, Expected));
	}

	class FProtectedAssetSave
	{
	public:
		static auto Begin(DPackage*, FAssetWriteResult&, const FAssetBundleSaveOptions&) -> std::shared_ptr<FProtectedAssetSave>;
		~FProtectedAssetSave() { check(IsInGameThread()); }
		auto Stage() -> void;
		auto Complete() -> FAssetWriteResult;
		auto GetDetachedBytes() const -> uint64;
	private:
		FProtectedAssetSave() = default;
		FPreparedPackageSave Prepared;
		FSaveParticipants Participants;
		FAssetBundleSaveOptions SaveOptions;
		FAssetWriteResult StagingResult;
		std::optional<FAssetWriteResult> CachedResult;
		bool bCommitting = false;
	};

	auto FProtectedAssetSave::Begin(DPackage* Package, FAssetWriteResult& OutResult, const FAssetBundleSaveOptions& Options)
		-> std::shared_ptr<FProtectedAssetSave>
	{
		check(IsInGameThread());
		if (!IsTaskSchedulerRunning() || !FAssetRuntimeState::Get().IsAcceptingRequests())
		{
			OutResult = Error(EAssetWriteError::StaleData, "Asset saves are not accepting asynchronous work.");
			return {};
		}
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate() || !Package->IsDirty())
		{
			OutResult = Error(EAssetWriteError::InvalidData, "Async save requires a loaded dirty asset package.");
			return {};
		}
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard)
		{ OutResult = AssetWriteResultFromRead(Guard); return {}; }
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
		{ OutResult = Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved."); return {}; }
		auto Operation = std::shared_ptr<FProtectedAssetSave>(new FProtectedAssetSave());
		auto& Data = *Operation;
		Data.SaveOptions = Options;
		FPackagePath Path;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path))
		{ OutResult = Error(EAssetWriteError::InvalidPath, "Invalid package path."); return {}; }
		if (FindResidentPackage(Path) != Package)
		{ OutResult = Error(EAssetWriteError::StaleData, "Package destination is unavailable."); return {}; }
		OutResult = PreparePackageSave(Package, Path, Options.Mode, Data.Prepared);
		if (!OutResult) return {};
		Data.Participants.emplace(Path, FindAssetExact(Path).Data);
		for (const auto& Dependency : Data.Prepared.Header.Dependencies)
			Data.Participants.emplace(Dependency, FindAssetExact(Dependency).Data);
		return Operation;
	}

	auto FProtectedAssetSave::Stage() -> void { StagingResult = ToAssetWriteResult(Prepared.Write->Stage()); }
	auto FProtectedAssetSave::GetDetachedBytes() const -> uint64 { return Prepared.DetachedBytes; }
	auto FProtectedAssetSave::Complete() -> FAssetWriteResult
	{
		check(IsInGameThread());
		auto& Data = *this;
		if (Data.CachedResult) return *Data.CachedResult;
		if (Data.bCommitting) return Error(EAssetWriteError::StaleData, "Save publication is already running.");
		const auto& Options = Data.SaveOptions;
		if (!Data.StagingResult) return *(Data.CachedResult = Data.StagingResult);
		const bool ParticipantsChanged = SaveParticipantsChanged(Data.Participants, Data.Prepared.Header.PackagePath);
		if (FindResidentPackage(Data.Prepared.Header.PackagePath) != Data.Prepared.Package.Get()
			|| Data.Prepared.Package->GetPackagePath() != Data.Prepared.Header.PackagePath.GetView()
			|| Data.Prepared.Package->GetEditRevision() != Data.Prepared.Revision
			|| ParticipantsChanged
			|| GetPhysicalPath(Data.Prepared.Header.PackagePath) != Data.Prepared.Destination.generic_string())
			return *(Data.CachedResult = Error(EAssetWriteError::StaleData,
				"Asset or destination changed while saving; retry the current version."));
		Data.bCommitting = true;
		Data.CachedResult = CommitPreparedPackageSave(Data.Prepared, Options, CaptureAssetRegistryPublication());
		Data.bCommitting = false;
		return *Data.CachedResult;
	}

	auto FAssetPackageSaveContext::SaveAsync(DPackage* Package, FAssetWriteResult& Admission) const
		-> Tasks::TTask<FAssetWriteResult>
	{
		if (Flags != SAVE_None || Options.PreparedPublication || (Options.RootPackage && Options.RootPackage != Package))
		{ Admission = Error(EAssetWriteError::InvalidData, "Invalid protected async save policy."); return {}; }
		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(); !Result)
		{ Admission = Error(EAssetWriteError::ShuttingDown, Result.Message); return {}; }
		auto Operation = FProtectedAssetSave::Begin(Package, Admission, Options);
		if (!Operation) return {};
		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(Operation->GetDetachedBytes()); !Result)
		{ Admission = Error(EAssetWriteError::ShuttingDown, Result.Message); return {}; }
		Private::TExternalTaskResult<FAssetWriteResult> Source({.DebugName = "Asset.SaveAsync"});
		auto Task = Source.TakeTask();
		auto Submitted = PackageSavePrivate::SubmitAsyncSave(Operation->GetDetachedBytes(),
			[Operation] { Operation->Stage(); },
			[Operation, Source, Cancellation = Cancellation](bool bSucceeded) mutable {
				const bool bCancelled = Cancellation.IsCancellationRequested()
					|| Source.IsCancellationRequested();
				auto Result = bCancelled ? Error(EAssetWriteError::Cancelled, "Save was cancelled before publication.")
					: !bSucceeded ? Error(EAssetWriteError::IoError, "Save I/O task failed.")
					: Operation->Complete();
				Operation.reset();
				Source.TrySetValue(std::move(Result));
			});
		if (!Submitted)
		{ Admission = Error(EAssetWriteError::InUse, Submitted.Message); Source.TrySetValue(Admission); return {}; }
		Admission = {};
		return Task;
	}
	namespace
	{
		FAsyncPackageSaveSink AsyncSaveSink;
		bool bFailDirectPublication = false;
		struct FDirectAssetSave
		{
			FPreparedPackageSave Prepared;
			FPackageWriteResult Written;
			FSaveParticipants Participants;
			bool bStarted = false;
		};
	}
	auto SetAsyncPackageSaveSink(FAsyncPackageSaveSink Sink) -> void
	{ check(IsInGameThread()); AsyncSaveSink = std::move(Sink); }
	namespace AssetPrivate
	{
		auto SetAsyncSavePublicationFailureForTests(bool bFail) -> void
		{ check(IsInGameThread()); bFailDirectPublication = bFail; }
	}
	auto SavePackage(DPackage* Package, EPackageSaveFlags Flags, EAssetPackageSaveMode Mode) -> FAssetWriteResult
	{
		if (Flags == SAVE_None) return SavePackage(Package, Mode);
		if (Flags != SAVE_Async) return Error(EAssetWriteError::InvalidData, "Unknown package save flags.");
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return AssetWriteResultFromRead(Guard);
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
			return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (!FAssetRuntimeState::Get().IsAcceptingRequests()) return Error(EAssetWriteError::ShuttingDown, "Asset runtime is shutting down.");
		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(); !Result) return Error(EAssetWriteError::InUse, Result.Message);
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate()
			|| FindResidentPackage(Package->GetPackagePathIdentity()) != Package)
			return Error(EAssetWriteError::InvalidData, "A resident asset package is required.");
		auto Data = std::make_shared<FDirectAssetSave>();
		if (auto Result = PreparePackageSave(Package, Package->GetPackagePathIdentity(), Mode, Data->Prepared, true); !Result) return Result;
		Data->Participants.emplace(Data->Prepared.Header.PackagePath, FindAssetExact(Data->Prepared.Header.PackagePath).Data);
		for (const auto& Dependency : Data->Prepared.Header.Dependencies)
			Data->Participants.emplace(Dependency, FindAssetExact(Dependency).Data);
		auto Submitted = PackageSavePrivate::SubmitAsyncSave(Data->Prepared.DetachedBytes,
			[Data] { Data->bStarted = true; Data->Written = Data->Prepared.Write->Stage(); },
			[Data, Sink = AsyncSaveSink, bFailPublication = bFailDirectPublication](bool bSucceeded) mutable {
				auto& Prepared = Data->Prepared;
				const auto Path = Prepared.Header.PackagePath;
				auto Result = ToAssetWriteResult(Data->Written);
				if (!bSucceeded)
				{
					Result = Error(EAssetWriteError::IoError, "Direct write task failed; the closure requires reconciliation.");
					Result.Effect = Data->bStarted ? EAssetWriteEffect::PartiallyWritten : EAssetWriteEffect::None;
					auto Bulk = Prepared.Destination; Bulk.replace_extension(".dbulk");
					DURIN_ERROR("Direct save may have affected {} and {}", Bulk.string(), Prepared.Destination.string());
				}
				if (Result)
				{
					FFilePublicationStamp Stamp;
					if (SaveParticipantsChanged(Data->Participants, Path)
						|| FindResidentPackage(Path) != Data->Prepared.Package.Get() || Data->Prepared.Package->GetPackagePathIdentity() != Path
						|| GetPhysicalPath(Path) != Prepared.Destination.generic_string()
						|| !FFilePublicationStamp::Inspect(Prepared.Destination, Stamp) || !Stamp.Exists)
						Result = Error(EAssetWriteError::StaleData, "Written package identity or destination changed before publication.");
					else
						Result = bFailPublication ? Error(EAssetWriteError::StaleData, "Injected Registry publication failure.")
							: AssetWriteResultFromRead(AssetPrivate::ToAssetResult(AssetsSaved({BuildSavedAssetMetadata(Prepared.Header,
							Prepared.Destination, Stamp.Size, Stamp.Time)}, CaptureAssetRegistryPublication())));
					if (!Result) Result.Effect = EAssetWriteEffect::ContentCommittedProjectionPending;
					else
					{
						Data->Prepared.Package->MarkAsPublished();
						if (Data->Prepared.Package->GetEditRevision() == Prepared.Revision) Data->Prepared.Package->ClearDirty();
					}
				}
				if (!Result && Result.Effect != EAssetWriteEffect::None)
				{
					if (!Data->Prepared.Package->IsDirty()) Data->Prepared.Package->MarkDirty();
					FenceAssetRegistryProjection(std::span(&Path, 1));
				}
				Data.reset();
				if (!Result)
				{
					DURIN_ERROR("Async save {} failed: {}", Path.ToString(), Result.Message);
				}
				if (Sink) Sink(Path, Result);
			});
		return Submitted ? FAssetWriteResult{} : Error(EAssetWriteError::InUse, Submitted.Message);
	}

	auto SavePackages(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options) -> FAssetBatchSaveResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().SavePackages(Packages, Options);
	}

	auto FAssetMutationCoordinator::SavePackages(
		std::span<DPackage* const> Packages, const FAssetBundleSaveOptions& Options) -> FAssetBatchSaveResult
	{
		FAssetBatchSaveResult Batch;
		auto Fail = [&](FAssetWriteResult Result, const FPackagePath& Path = {}) {
			Batch.FailedPackage = Path;
			Result.Message = std::format("Saved {} of {} packages; {}: {}",
				Batch.SavedPackages.size(), Packages.size(), Path.IsValid() ? Path.ToString() : "batch admission", Result.Message);
			Batch.Result = std::move(Result);
			return Batch;
		};
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return Fail(AssetWriteResultFromRead(Guard));
		if (Packages.empty()) return Fail(Error(EAssetWriteError::InvalidData, "A save batch must contain at least one package."));
		if (Options.PreparedPublication && (Packages.size() != 1 || !Options.bRollbackOnRegistryFailure))
			return Fail(Error(EAssetWriteError::InvalidData, "Prepared publication requires one package and rollback on Registry failure."));
		if (RuntimeConfiguration.IsCooked()) return Fail(Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved."));
		if (Options.RootPackage && !std::ranges::contains(Packages, Options.RootPackage))
			return Fail(Error(EAssetWriteError::InvalidData, "The root package is not part of the save batch."));
		std::vector<DPackage*> Ordered(Packages.begin(), Packages.end());
		std::stable_sort(Ordered.begin(), Ordered.end(), [&](DPackage* A, DPackage* B) {
			return Options.RootPackage && A != Options.RootPackage && B == Options.RootPackage;
		});
		std::unordered_set<FPackagePath> Paths;
		for (size_t Index = 0; Index < Ordered.size(); ++Index)
		{
			DPackage* Package = Ordered[Index];
			FPackagePath Path;
			const bool Owned = Package && Options.PreparedPublication && Options.PreparedPublication->OwnsPreparedPackage(*Package);
			if (!Package || !Package->IsAssetPackage() || (Package->IsGraphPrivate() && !Owned)
				|| !FPackagePath::TryCreate(Package->GetPackagePath(), Path))
				return Fail(Error(EAssetWriteError::InvalidData, "The save batch contains an invalid package."));
			if (!Paths.insert(Path).second) return Fail(Error(EAssetWriteError::AlreadyExists, "The save batch contains a duplicate package."), Path);
			const auto Expected = CaptureAssetRegistryPublication();
			if (Options.PreparedPublication && (!Expected.bReferenceIndexComplete || !Expected.ReferenceErrors.empty()))
				return Fail(Error(EAssetWriteError::StaleData, "Prepared publication requires a complete Registry projection."), Path);
			FPreparedPackageSave Staged;
			if (auto Result = PreparePackageSave(Package, Path, Options.Mode, Staged); !Result) return Fail(Result, Path);
			if (auto Result = ToAssetWriteResult(Staged.Write->Stage()); !Result) return Fail(Result, Path);
			auto PackageOptions = Options;
			PackageOptions.RootPackage = Package == Options.RootPackage ? Package : nullptr;
			PackageOptions.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
				return Options.ShouldFail && (Phase != EAssetBundleSavePhase::PublishCompanion || Staged.Header.BulkSegmentExtent)
					&& Options.ShouldFail(Phase, Index);
			};
			if (auto Result = CommitPreparedPackageSave(Staged, PackageOptions, Expected); !Result) return Fail(Result, Path);
			Batch.SavedPackages.push_back(Path);
		}
		return Batch;
	}

	auto AdmitAssetPackageToCatalog(const FPackagePath& Path) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().AdmitAssetPackageToCatalog(Path);
	}

	auto FAssetMutationCoordinator::AdmitAssetPackageToCatalog(
		const FPackagePath& Path) -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return AssetWriteResultFromRead(Guard);
		if (!Path.IsValid())
			return Error(EAssetWriteError::InvalidPath, "The asset admission path is invalid.");
		if (Durin::FindAssetExact(Path) || FindResidentPackage(Path))
			return Error(EAssetWriteError::AlreadyExists,
				"The asset admission path is already occupied.");
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (PhysicalPath.empty())
			return Error(EAssetWriteError::InvalidPath,
				"The asset admission path is outside mounted package content.");
		FAssetPackageHeader Header;
		if (FAssetRegistryResult Result = ReadAssetPackageHeader(
			PhysicalPath, Path, Header); !Result)
			return AssetWriteResultFromRead(AssetPrivate::ToAssetResult(std::move(Result)));
		auto ReadAccess = FPackageFileAccess::TryReadPackage(std::filesystem::path(PhysicalPath));
		if (!ReadAccess) return Error(EAssetWriteError::InUse, "Package output is being written.");
		auto Loaded = FFileHelper::LoadFileToArray(PhysicalPath);
		if (!Loaded) return Error(EAssetWriteError::IoError,
			std::format("Package admission read failed: {}", Loaded.error().ToString()));
		FByteBuffer Bytes = std::move(*Loaded);
		FByteBuffer BulkBytes;
		if (FAssetWriteResult Result = AssetWriteResultFromRead(LoadPackageBulkBytes(PhysicalPath, BulkBytes)); !Result)
			return Result;
		if (FAssetWriteResult Result = AssetWriteResultFromRead(ValidateAssetPackageBytes(Bytes, Path, BulkBytes)); !Result)
			return Result;
		std::error_code ErrorCode;
		const auto LastWriteTime = std::filesystem::last_write_time(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(EAssetWriteError::IoError,
				"The asset package timestamp could not be read for admission.");
		const uintmax_t FileSize = std::filesystem::file_size(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(EAssetWriteError::IoError,
				"The asset package size could not be read for admission.");
		const auto Expected = CaptureAssetRegistryPublication();
		if (!Expected.bReferenceIndexComplete || !Expected.ReferenceErrors.empty())
			return Error(EAssetWriteError::StaleData, "Asset metadata cannot publish while the reference index is incomplete.");
		return AssetWriteResultFromRead(AssetPrivate::ToAssetResult(AssetsSaved({
			BuildSavedAssetMetadata(Header, PhysicalPath, FileSize, LastWriteTime)}, Expected)));
	}


	auto FAssetMutationCoordinator::SavePackage(DPackage* Package, EAssetPackageSaveMode Mode) -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return AssetWriteResultFromRead(Guard);
		if (RuntimeConfiguration.IsCooked()) return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
			return Error(EAssetWriteError::InvalidData, "A live persistent package is required.");
		const auto Expected = CaptureAssetRegistryPublication();
		FPreparedPackageSave Prepared;
		if (auto Result = PreparePackageSave(Package, Package->GetPackagePathIdentity(), Mode, Prepared); !Result) return Result;
		if (auto Result = ToAssetWriteResult(Prepared.Write->Stage()); !Result) return Result;
		return CommitPreparedPackageSave(Prepared, {.RootPackage = Package, .Mode = Mode}, Expected);
	}

}
