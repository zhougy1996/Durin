#include "AssetLiveLoadGuard.h"
#include "Asset/RegistryOperations.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetMutationRegistryInternal.h"
#include "AssetPackageInspectionInternal.h"
#include "AssetRegistryResultAdapter.h"
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
			EAssetPackageSaveMode Mode, FPreparedPackageSave& Out) -> FAssetWriteResult
		{
			if (auto Result = AssetPrivate::ValidatePackageSaveAdmission(Path); !Result) return Result;
			if (!Package->IsFullyLoaded())
				return Error(EAssetWriteError::InUse, "Load the complete package before saving its exports.");
			FPreparedPackageSave Prepared;
			Prepared.Package = Package;
			for (DObject* Asset : Package->GetTopLevelAssets()) Prepared.Exports.emplace_back(Asset);
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
			if (auto Result = AssetPrivate::EncodeAssetPackageClosure(Package, Closure, {.Serialization = Serialization, .OutExports = &Prepared.DiskExports}); !Result)
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
			Prepared.MainDigest = FXxHash128::HashBuffer(Closure.PackageBytes);
			Prepared.DetachedBytes = Closure.PackageBytes.size() + Closure.BulkBytes.size();
			Prepared.Write = BeginAssetWrite(Prepared.Destination, std::move(Closure.PackageBytes), std::move(Closure.BulkBytes),
				MainStamp, BulkStamp);
			if (auto Result = ToAssetWriteResult(Prepared.Write->GetAdmissionResult()); !Result) return Result;
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
		static auto Begin(DPackage*, FAssetWriteResult&, const FAssetPackageSaveArgs&) -> std::shared_ptr<FProtectedAssetSave>;
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
		FAssetWriteResult StagingResult;
		std::optional<FAssetWriteResult> CachedResult;
		bool bCommitting = false;
	};

	auto FProtectedAssetSave::Begin(DPackage* Package, FAssetWriteResult& OutResult, const FAssetPackageSaveArgs& Args)
		-> std::shared_ptr<FProtectedAssetSave>
	{
		check(IsInGameThread());
		if (!IsTaskSchedulerRunning() || !FAssetRuntimeState::Get().IsAcceptingRequests())
		{
			OutResult = Error(EAssetWriteError::StaleData, "Asset saves are not accepting asynchronous work.");
			return {};
		}
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
		{
			OutResult = Error(EAssetWriteError::InvalidData, "Async save requires a loaded asset package.");
			return {};
		}
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard)
		{ OutResult = AssetWriteResultFromRead(Guard); return {}; }
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
		{ OutResult = Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved."); return {}; }
		auto Operation = std::shared_ptr<FProtectedAssetSave>(new FProtectedAssetSave());
		auto& Data = *Operation;
		Data.CommitContext.Fault = Testing::CapturePackageSaveFault();
		FPackagePath Path;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path))
		{ OutResult = Error(EAssetWriteError::InvalidPath, "Invalid package path."); return {}; }
		if (FindResidentPackage(Path) != Package)
		{ OutResult = Error(EAssetWriteError::StaleData, "Package destination is unavailable."); return {}; }
		OutResult = PreparePackageSave(Package, Path, Args.Mode, Data.Prepared);
		if (!OutResult) return {};
		Data.Participants.emplace(Path, FindPackageExact(Path).Data);
		for (const auto& Dependency : Data.Prepared.Header.Dependencies)
			Data.Participants.emplace(Dependency, FindPackageExact(Dependency).Data);
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
		Data.CachedResult = CommitPreparedPackageSave(Data.Prepared, Data.CommitContext, CaptureAssetRegistryPublication());
		Data.bCommitting = false;
		return *Data.CachedResult;
	}

	auto FAssetPackageSaveContext::SaveAsync(DPackage* Package, FAssetWriteResult& Admission) const
		-> Tasks::TTask<FAssetWriteResult>
	{

		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(); !Result)
		{ Admission = Error(EAssetWriteError::ShuttingDown, Result.Message); return {}; }
		auto Operation = FProtectedAssetSave::Begin(Package, Admission, Args);
		if (!Operation) return {};
		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(Operation->GetDetachedBytes()); !Result)
		{ Admission = Error(EAssetWriteError::ShuttingDown, Result.Message); return {}; }
		FPackageSaveResult Submitted;
		auto Task = PackageSavePrivate::SubmitTypedAsyncSave<FAssetWriteResult>(Operation->GetDetachedBytes(),
			[Operation] { Operation->Stage(); },
			[Operation, IsCancellationRequested = IsCancellationRequested](bool bSucceeded) mutable {
				const bool bCancelled = IsCancellationRequested && IsCancellationRequested();
				auto Result = bCancelled ? Error(EAssetWriteError::Cancelled, "Save was cancelled before publication.")
					: !bSucceeded ? Error(EAssetWriteError::IoError, "Save I/O task failed.")
					: Operation->Complete();
				Operation.reset();
				return Result;
			}, [Operation] { Operation->Abandon(); }, Submitted);
		if (!Submitted) { Admission = Error(EAssetWriteError::InUse, Submitted.Message); return {}; }
		Admission = {};
		return Task;
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
			Result.Message = std::format("Saved {} of {} packages; {}: {}",
				Batch.SavedPackages.size(), Packages.size(), Path.IsValid() ? Path.ToString() : "batch admission", Result.Message);
			Batch.Result = std::move(Result);
			return Batch;
		};
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return Fail(AssetWriteResultFromRead(Guard));
		if (Packages.empty()) return Fail(Error(EAssetWriteError::InvalidData, "A save batch must contain at least one package."));
		if (RuntimeConfiguration.IsCooked()) return Fail(Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved."));
		const auto Fault = Testing::CapturePackageSaveFault();
		const std::vector<DPackage*> Ordered(Packages.begin(), Packages.end());
		std::unordered_set<FPackagePath> Paths;
		for (size_t Index = 0; Index < Ordered.size(); ++Index)
		{
			DPackage* Package = Ordered[Index];
			FPackagePath Path;
			if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate()
				|| !FPackagePath::TryCreate(Package->GetPackagePath(), Path))
				return Fail(Error(EAssetWriteError::InvalidData, "The save batch contains an invalid package."));
			if (!Paths.insert(Path).second) return Fail(Error(EAssetWriteError::AlreadyExists, "The save batch contains a duplicate package."), Path);
			const auto Expected = CaptureAssetRegistryPublication();

			FPreparedPackageSave Staged;
			if (auto Result = PreparePackageSave(Package, Path, Args.Mode, Staged); !Result) return Fail(Result, Path);
			if (auto Result = ToAssetWriteResult(Staged.Write->Stage()); !Result) return Fail(Result, Path);
			if (auto Result = CommitPreparedPackageSave(Staged, {.Fault = Fault, .PackageIndex = Index}, Expected); !Result)
				return Fail(Result, Path);
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
		if (Durin::FindPackageExact(Path) || FindResidentPackage(Path))
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
		if (auto Admission = FAssetRuntimeState::Get().CheckRequestAdmission(); !Admission)
			return AssetWriteResultFromRead(Admission);
		if (RuntimeConfiguration.IsCooked()) return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
			return Error(EAssetWriteError::InvalidData, "A live persistent package is required.");
		const auto Expected = CaptureAssetRegistryPublication();
		FPreparedPackageSave Prepared;
		if (auto Result = PreparePackageSave(Package, Package->GetPackagePathIdentity(), Mode, Prepared); !Result) return Result;
		if (auto Result = ToAssetWriteResult(Prepared.Write->Stage()); !Result) return Result;
		return CommitPreparedPackageSave(Prepared, {.Fault = Testing::CapturePackageSaveFault()}, Expected);
	}

}
