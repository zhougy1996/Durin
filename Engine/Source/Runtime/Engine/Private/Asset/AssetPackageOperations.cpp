#include "AssetLiveLoadGuard.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "Asset/RegistryOperations.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetMutationRegistryInternal.h"
#include "AssetPackageInspectionInternal.h"
#include "AssetRegistry/RegistryResult.h"
#include "Asset/PackageSerialization.h"
#include "AssetPackageCodec.h"
#include "AssetPackageSaveInternal.h"
#include "Asset/PackageVersionPolicy.h"
#include "Asset/PackageResource.h"
#include "DObject/DefaultDeltaPlan.h"

#include "CoreGlobals.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "DObject/PackagePersistence.h"
#include "Misc/FileTime.h"
#include "Misc/MountPaths.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"
#include "Misc/Guid.h"

namespace Durin
{
	namespace { thread_local const Testing::FPackageSaveFault* ActiveSaveFault = nullptr; }
	Testing::FScopedPackageSaveFault::FScopedPackageSaveFault(FPackageSaveFault InFault)
		: Fault(std::move(InFault)), Previous(ActiveSaveFault)
	{
		check(IsInGameThread());
		ActiveSaveFault = &Fault;
	}
	Testing::FScopedPackageSaveFault::~FScopedPackageSaveFault()
	{
		check(IsInGameThread());
		ActiveSaveFault = Previous;
	}
	auto Testing::CapturePackageSaveFault() -> FPackageSaveFault
	{
		check(IsInGameThread());
		return ActiveSaveFault ? *ActiveSaveFault : FPackageSaveFault{};
	}

	using AssetPrivate::LoadPackageBulkBytes;
	using AssetPrivate::FPreparedPackageSave;
	using AssetPrivate::BuildSavedAssetMetadata;
	using AssetPrivate::ToAssetWriteResult;
	using AssetPrivate::CommitPreparedPackageSave;

	namespace
	{
		auto Error(Durin::UnifiedError::FErrorId Id, std::string Message) -> std::expected<void, Durin::UnifiedError::FError> { return std::unexpected(Durin::UnifiedError::FError{Id, std::move(Message)}); }
		auto WriteFailure(UnifiedError::FErrorId Code, std::string Message) -> FAssetWriteResult { return {.Error = UnifiedError::FError{Code, std::move(Message)}}; }

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

	}

	auto AssetPrivate::EncodeAssetPackageClosure(DPackage* Package,
		FAssetPackageEncodedClosure& OutClosure,
		const FAssetPackageCaptureOptions& Options) -> ObjectPackage::FPackageWriterResult
	{
		const auto* Codec = FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion);
		if (!Codec)
			return {.Failure = ObjectPackage::EPackageWriterFailure::InvalidInput,
				.Reason = ObjectPackage::EPackageWriterReason::UnsupportedVersion,
				.Message = "The ordinary asset package writer is unavailable."};
		return Codec->Write(Package, OutClosure,
			Options.Serialization.Mode == EAssetPackageSaveMode::Complete
				? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, Options);
	}

	auto SerializeAssetPackageBytes(
		DPackage* Package,
		FByteBuffer& OutBytes,
		const FAssetPackageSerializationOptions& Options) -> ObjectPackage::FPackageWriterResult
	{
		AssetPrivate::FAssetPackageEncodedClosure Closure;
		auto Result = AssetPrivate::EncodeAssetPackageClosure(Package, Closure, {Options});
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
		auto Result = AssetPrivate::EncodeAssetPackageClosure(Package, Closure, {Options});
		if (!Result) return Result;
		OutBytes = std::move(Closure.PackageBytes);
		OutBulkBytes = std::move(Closure.BulkBytes);
		return {};
	}

	namespace
	{
		auto BeginAssetWrite(const std::filesystem::path& Destination, FByteBuffer Bytes, FByteBuffer Bulk,
			const FFilePublicationStamp& MainStamp, const FFilePublicationStamp& BulkStamp)
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
			return GetFilePackageWriter()->Begin(std::move(Files));
		}
		// Snapshot by value: callers may pass the package's mutable identity member.
		auto PreparePackageSave(DPackage* Package, const FPackagePath Path,
			EAssetPackageSaveMode Mode, FPreparedPackageSave& Out) -> std::expected<void, UnifiedError::FError>
		{
			if (auto Result = AssetPrivate::ValidatePackageSaveAdmission(Path); !Result) return Result;
			if (!Package->IsFullyLoaded())
				return Error(AssetLoadErrors::InUse, "Load the complete package before saving its exports.");
			FPreparedPackageSave Prepared;
			Prepared.Package = Package;
			for (DObject* Asset : Package->GetTopLevelAssets()) Prepared.Exports.emplace_back(Asset);
			Prepared.Destination = GetPhysicalPath(Path);
			if (Prepared.Destination.empty()) return Error(AssetLoadErrors::InvalidPath, "Cannot resolve package destination.");
			auto ReadAccess = FPackageFileAccess::TryReadPackage(Prepared.Destination);
			if (!ReadAccess) return Error(AssetLoadErrors::InUse, "Package output is being written.");
			auto Companion = Prepared.Destination; Companion.replace_extension(".dbulk");
			FFilePublicationStamp MainStamp, BulkStamp;
			std::optional<FFileSystemError> InspectionError;
			if (!FFilePublicationStamp::Inspect(Prepared.Destination, MainStamp, &InspectionError)
				|| !FFilePublicationStamp::Inspect(Companion, BulkStamp, &InspectionError))
				return std::unexpected(ToError(*InspectionError));
			Prepared.Revision = Package->GetEditRevision();
			AssetPrivate::FAssetPackageEncodedClosure Closure;
			FAssetPackageSerializationOptions Serialization; Serialization.Mode = Mode;
			if (auto Result = AssetPrivate::EncodeAssetPackageClosure(Package, Closure, {.Serialization = Serialization, .OutExports = &Prepared.DiskExports}); !Result)
				return std::unexpected(ObjectPackage::ToError(Result));
			// The codec validates the closure. Capture its canonical header once;
			// publication never rebuilds metadata from mutable live objects.
			const AssetPrivate::FAssetPackageReadContext Context{
				.PackageBytes = Closure.PackageBytes, .BulkBytes = Closure.BulkBytes,
				.PackagePath = Path, .PhysicalPackageBytes = Closure.PackageBytes.size()};
			if (auto Result = AssetPrivate::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion)
				->ReadHeader(Context, Prepared.Header); !Result) return Result;
			if (Package->GetEditRevision() != Prepared.Revision || Package->GetPackagePathIdentity() != Prepared.Header.PackagePath
				|| GetPhysicalPath(Prepared.Header.PackagePath) != Prepared.Destination.generic_string())
				return Error(AssetLoadErrors::StaleData, "Package or destination changed during capture.");
			Prepared.MainDigest = FXxHash128::HashBuffer(Closure.PackageBytes);
			Prepared.DetachedBytes = Closure.PackageBytes.size() + Closure.BulkBytes.size();
			Prepared.Write = BeginAssetWrite(Prepared.Destination, std::move(Closure.PackageBytes), std::move(Closure.BulkBytes),
				MainStamp, BulkStamp);
			Out = std::move(Prepared);
			return {};
		}

		using FSaveParticipants = std::unordered_map<FPackagePath, std::optional<FPackageData>>;
		auto SaveParticipantsChanged(const FSaveParticipants& Participants, const FPackagePath& Path) -> bool
		{
			return std::ranges::any_of(Participants, [&](const auto& Entry) {
				const auto Current = FindPackageExact(Entry.first).Data;
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

	class FProtectedAssetSave
	{
	public:
		static auto Begin(DPackage*, std::optional<UnifiedError::FError>*, const FAssetPackageSaveArgs&) -> std::shared_ptr<FProtectedAssetSave>;
		~FProtectedAssetSave() { check(IsInGameThread()); }
		auto Stage() -> void;
		auto Complete() -> FAssetWriteResult;
		auto Abandon() -> void { check(IsInGameThread()); Prepared.Write.reset(); }
		auto GetDetachedBytes() const -> uint64;
	private:
		FProtectedAssetSave() = default;
		FPreparedPackageSave Prepared;
		FSaveParticipants Participants;
		AssetPrivate::FPackageSaveCommitContext CommitContext;
		std::optional<UnifiedError::FError> StagingError;
		std::optional<FAssetWriteResult> CachedResult;
		bool bCommitting = false;
	};

	auto FProtectedAssetSave::Begin(DPackage* Package, std::optional<UnifiedError::FError>* OutError, const FAssetPackageSaveArgs& Args)
		-> std::shared_ptr<FProtectedAssetSave>
	{
		if (OutError) OutError->reset();
		check(IsInGameThread());
		if (!IsTaskSchedulerRunning() || !FAssetRuntimeState::Get().IsAcceptingRequests())
		{
			if (OutError) *OutError = UnifiedError::FError{AssetWriteErrors::StaleData, "Asset saves are not accepting asynchronous work."};
			return {};
		}
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
		{
			if (OutError) *OutError = UnifiedError::FError{AssetWriteErrors::InvalidData, "Async save requires a loaded asset package."};
			return {};
		}
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard)
		{ if (OutError) *OutError = Guard.error(); return {}; }
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
		{ if (OutError) *OutError = UnifiedError::FError{AssetWriteErrors::ReadOnlyMode, "Cooked packages cannot be saved."}; return {}; }
		auto Operation = std::shared_ptr<FProtectedAssetSave>(new FProtectedAssetSave());
		auto& Data = *Operation;
		Data.CommitContext.Fault = Testing::CapturePackageSaveFault();
		FPackagePath Path;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path))
		{ if (OutError) *OutError = UnifiedError::FError{AssetWriteErrors::InvalidPath, "Invalid package path."}; return {}; }
		if (FindResidentPackage(Path) != Package)
		{ if (OutError) *OutError = UnifiedError::FError{AssetWriteErrors::StaleData, "Package destination is unavailable."}; return {}; }
		if (auto Prepared = PreparePackageSave(Package, Path, Args.Mode, Data.Prepared); !Prepared)
		{ if (OutError) *OutError = std::move(Prepared.error()); return {}; }

		Data.Participants.emplace(Path, FindPackageExact(Path).Data);
		for (const auto& Dependency : Data.Prepared.Header.Dependencies)
			Data.Participants.emplace(Dependency, FindPackageExact(Dependency).Data);
		return Operation;
	}

	auto FProtectedAssetSave::Stage() -> void
	{
		if (!Prepared.Write->Stage(&StagingError) && !StagingError)
			StagingError.emplace(AssetWriteErrors::IoError, "Writer rejected staging.");
	}
	auto FProtectedAssetSave::GetDetachedBytes() const -> uint64 { return Prepared.DetachedBytes; }
	auto FProtectedAssetSave::Complete() -> FAssetWriteResult
	{
		check(IsInGameThread());
		auto& Data = *this;
		if (Data.CachedResult) return *Data.CachedResult;
		if (Data.bCommitting) return WriteFailure(AssetWriteErrors::StaleData, "Save publication is already running.");
		if (Data.StagingError) return *(Data.CachedResult = FAssetWriteResult{.Error = Data.StagingError});
		const bool ParticipantsChanged = SaveParticipantsChanged(Data.Participants, Data.Prepared.Header.PackagePath);
		if (FindResidentPackage(Data.Prepared.Header.PackagePath) != Data.Prepared.Package.Get()
			|| Data.Prepared.Package->GetPackagePath() != Data.Prepared.Header.PackagePath.GetView()
			|| Data.Prepared.Package->GetEditRevision() != Data.Prepared.Revision
			|| ParticipantsChanged
			|| GetPhysicalPath(Data.Prepared.Header.PackagePath) != Data.Prepared.Destination.generic_string())
			return *(Data.CachedResult = WriteFailure(AssetWriteErrors::StaleData,
				"Asset or destination changed while saving; retry the current version."));
		Data.bCommitting = true;
		Data.CachedResult = CommitPreparedPackageSave(Data.Prepared, Data.CommitContext, CaptureAssetRegistryPublication());
		Data.bCommitting = false;
		return *Data.CachedResult;
	}

	auto SavePackageAsync(DPackage* Package, const FAssetPackageSaveArgs& Args,
		std::function<bool()> IsCancellationRequested) -> Tasks::TTask<FAssetWriteResult>
	{
		if (auto Admission = PackageSavePrivate::CheckAsyncAdmission(); !Admission)
			return Tasks::MakeCompletedTask(FAssetWriteResult{.Error = std::move(Admission.error())});
		std::optional<UnifiedError::FError> Error;
		auto Operation = FProtectedAssetSave::Begin(Package, &Error, Args);
		if (!Operation) return Tasks::MakeCompletedTask(FAssetWriteResult{.Error = std::move(Error)});
		auto Submitted = PackageSavePrivate::SubmitTypedAsyncSave<FAssetWriteResult>(Operation->GetDetachedBytes(),
			[Operation] { Operation->Stage(); },
			[Operation, IsCancellationRequested = std::move(IsCancellationRequested)](bool bSucceeded) mutable {
				const bool bCancelled = IsCancellationRequested && IsCancellationRequested();
				auto Result = bCancelled ? WriteFailure(AssetWriteErrors::Cancelled, "Save was cancelled before publication.")
					: !bSucceeded ? WriteFailure(AssetWriteErrors::IoError, "Save I/O task failed.")
					: Operation->Complete();
				Operation.reset();
				return Result;
			}, [Operation] { Operation->Abandon(); });
		if (!Submitted) return Tasks::MakeCompletedTask(FAssetWriteResult{.Error = std::move(Submitted.error())});
		return std::move(*Submitted);
	}

	auto SavePackages(
		std::span<DPackage* const> Packages,
		const FAssetPackageSaveArgs& Args) -> FAssetBatchSaveResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().SavePackages(Packages, Args);
	}

	auto FAssetMutationCoordinator::SavePackages(
		std::span<DPackage* const> Packages, const FAssetPackageSaveArgs& Args) -> FAssetBatchSaveResult
	{
		FAssetBatchSaveResult Batch;
		auto Fail = [&](FAssetWriteResult Result, const FPackagePath& Path = {}) {
			Batch.FailedPackage = Path;
			Batch.Result = std::move(Result);
			return Batch;
		};
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return Fail(FAssetWriteResult{.Error = Guard.error()});
		if (Packages.empty()) return Fail(WriteFailure(AssetWriteErrors::InvalidData, "A save batch must contain at least one package."));
		if (RuntimeConfiguration.IsCooked()) return Fail(WriteFailure(AssetWriteErrors::ReadOnlyMode, "Cooked packages cannot be saved."));
		const auto Fault = Testing::CapturePackageSaveFault();
		const std::vector<DPackage*> Ordered(Packages.begin(), Packages.end());
		std::unordered_set<FPackagePath> Paths;
		for (size_t Index = 0; Index < Ordered.size(); ++Index)
		{
			DPackage* Package = Ordered[Index];
			FPackagePath Path;
			if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate()
				|| !FPackagePath::TryCreate(Package->GetPackagePath(), Path))
				return Fail(WriteFailure(AssetWriteErrors::InvalidData, "The save batch contains an invalid package."));
			if (!Paths.insert(Path).second) return Fail(WriteFailure(AssetWriteErrors::AlreadyExists, "The save batch contains a duplicate package."), Path);
			const auto Expected = CaptureAssetRegistryPublication();

			FPreparedPackageSave Staged;
			if (auto Result = PreparePackageSave(Package, Path, Args.Mode, Staged); !Result) return Fail(FAssetWriteResult{Result.error()}, Path);
			std::optional<UnifiedError::FError> StageError;
			if (!Staged.Write->Stage(&StageError))
			{ if (!StageError) StageError.emplace(AssetWriteErrors::IoError, "Writer rejected staging."); return Fail(FAssetWriteResult{.Error = std::move(StageError)}, Path); }
			if (auto Result = CommitPreparedPackageSave(Staged, {.Fault = Fault, .PackageIndex = Index}, Expected); !Result)
				return Fail(Result, Path);
			Batch.SavedPackages.push_back(Path);
		}
		return Batch;
	}

	auto AdmitAssetPackageToCatalog(const FPackagePath& Path) -> std::expected<void, UnifiedError::FError>
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().AdmitAssetPackageToCatalog(Path);
	}

	auto FAssetMutationCoordinator::AdmitAssetPackageToCatalog(
		const FPackagePath& Path) -> std::expected<void, UnifiedError::FError>
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return std::unexpected(Guard.error());
		if (!Path.IsValid())
			return Error(AssetWriteErrors::InvalidPath, "The asset admission path is invalid.");
		if (Durin::FindPackageExact(Path) || FindResidentPackage(Path))
			return Error(AssetWriteErrors::AlreadyExists,
				"The asset admission path is already occupied.");
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (PhysicalPath.empty())
			return Error(AssetWriteErrors::InvalidPath,
				"The asset admission path is outside mounted package content.");
		FAssetPackageHeader Header;
		if (FAssetRegistryResult Result = ReadAssetPackageHeader(
			PhysicalPath, Path, Header); !Result)
			return std::unexpected(Durin::ToError(Result));
		auto ReadAccess = FPackageFileAccess::TryReadPackage(std::filesystem::path(PhysicalPath));
		if (!ReadAccess) return Error(AssetWriteErrors::InUse, "Package output is being written.");
		auto Loaded = FFileHelper::LoadFileToArray(PhysicalPath);
		if (!Loaded) return std::unexpected(ToError(Loaded.error()));
		FByteBuffer Bytes = std::move(*Loaded);
		FByteBuffer BulkBytes;
		if (auto Result = LoadPackageBulkBytes(PhysicalPath, BulkBytes); !Result)
			return std::unexpected(Result.error());
		if (auto Result = ValidateAssetPackageBytes(Bytes, Path, BulkBytes); !Result)
			return std::unexpected(Result.error());
		std::error_code ErrorCode;
		const auto LastWriteTime = std::filesystem::last_write_time(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(AssetWriteErrors::IoError,
				"The asset package timestamp could not be read for admission.");
		const uintmax_t FileSize = std::filesystem::file_size(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(AssetWriteErrors::IoError,
				"The asset package size could not be read for admission.");
		const auto Expected = CaptureAssetRegistryPublication();
		if (!Expected.bReferenceIndexComplete || !Expected.ReferenceErrors.empty())
			return Error(AssetWriteErrors::StaleData, "Asset metadata cannot publish while the reference index is incomplete.");
		if (auto Published = AssetsSaved({
			BuildSavedAssetMetadata(Header, PhysicalPath, FileSize, LastWriteTime)}, Expected); !Published)
			return std::unexpected(Durin::ToError(Published));
		return {};
	}


	auto FAssetMutationCoordinator::SavePackage(DPackage* Package, EAssetPackageSaveMode Mode) -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return {.Error = Guard.error()};
		if (auto Admission = FAssetRuntimeState::Get().CheckRequestAdmission(); !Admission)
			return {.Error = Admission.error()};
		if (RuntimeConfiguration.IsCooked()) return WriteFailure(AssetWriteErrors::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
			return WriteFailure(AssetWriteErrors::InvalidData, "A live persistent package is required.");
		const auto Expected = CaptureAssetRegistryPublication();
		FPreparedPackageSave Prepared;
		if (auto Result = PreparePackageSave(Package, Package->GetPackagePathIdentity(), Mode, Prepared); !Result) return FAssetWriteResult{Result.error()};
		std::optional<UnifiedError::FError> StageError;
		if (!Prepared.Write->Stage(&StageError))
		{ if (!StageError) StageError.emplace(AssetWriteErrors::IoError, "Writer rejected staging."); return {.Error = std::move(StageError)}; }
		return CommitPreparedPackageSave(Prepared, {.Fault = Testing::CapturePackageSaveFault()}, Expected);
	}

}
