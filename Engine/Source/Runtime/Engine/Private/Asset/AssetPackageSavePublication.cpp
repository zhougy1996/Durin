#include "AssetPackageSaveInternal.h"
#include "AssetLiveLoadGuard.h"
#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageVersionPolicy.h"
#include "Misc/MountPaths.h"
#include "Asset/Load.h"
#include "AssetRegistryResultAdapter.h"
#include "DObject/Package.h"
#include "Misc/FileTime.h"

namespace Durin::AssetPrivate
{
	auto BuildSavedAssetMetadata(const FAssetPackageHeader& Header,
		const std::filesystem::path& Destination, uintmax_t FileSize,
		std::filesystem::file_time_type LastWriteTime) -> FAssetData
	{
		// The captured file describes the saved revision, even if live objects change.
		return {
			.PackagePath = Header.PackagePath,
			.PhysicalPath = Destination.generic_string(),
			.TopLevelAssets = Header.TopLevelAssets,
			.AssetClassName = Header.AssetClassName,
			.EntryKind = Header.EntryKind,
			.RedirectDestination = Header.RedirectDestination,
			.FormatVersion = Header.FormatVersion,
			.Dependencies = Header.Dependencies,
			.SoftDependencies = Header.SoftDependencies,
			.BuildDependencies = Header.BuildDependencies,
			.SearchableNames = Header.SearchableNames,
			.ObjectCount = Header.ObjectCount,
			.BulkSegmentExtent = Header.BulkSegmentExtent,
			.BulkSegmentDigest = Header.BulkSegmentDigest,
			.FileSize = FileSize,
			.LastWriteTime = LastWriteTime,
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime)};
	}

	auto ToAssetWriteResult(FPackageWriteResult Result) -> FAssetWriteResult
	{
		FAssetWriteResult Out;
		if (!Result) Out.Error = Result.Error == EPackageWriteError::StaleData ? EAssetWriteError::StaleData
			: Result.Error == EPackageWriteError::CorruptFile ? EAssetWriteError::InvalidData : EAssetWriteError::IoError;
		Out.Message = std::move(Result.Message);
		// File diagnostics remain owned by the physical package writer.
		if (!Result)
			for (const auto& File : Result.AffectedFiles)
				DURIN_ERROR("Affected package file: {}", File.string());
		if (Result.State == EPackageWriteState::PartiallyWritten)
			Out.Effect = EAssetWriteEffect::PartiallyWritten;
		if (Result.State == EPackageWriteState::RecoveryRequired
			|| (!Result && Result.State == EPackageWriteState::Committed))
			Out.Effect = EAssetWriteEffect::ContentUncertain;
		for (const auto& File : Result.RecoveryFiles)
			Out.Message += std::format("; retained package backup: {}", File.string());
		return Out;
	}
	namespace
	{
		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult { return {Code, std::move(Message)}; }
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult { return {Code, std::move(Message)}; }

		auto ValidatePackageWriteAdmission(const FPackagePath& Path) -> FAssetWriteResult
		{
			if (IsPackageLoading(Path)) return Error(EAssetWriteError::InUse, "An incomplete package cannot be saved.");
			const auto Mount =
				FMountPaths::FindMountForVirtualPath(Path.GetView());
			if (!Mount)
				return Error(EAssetWriteError::InvalidPath,
					std::format("Package {} does not use a registered content mount.",
						Path.ToString()));
			if (!Mount->Mount->bContentWritable)
				return Error(EAssetWriteError::ReadOnlyMode,
					std::format("Content mount {} is read-only.",
						Mount->Mount->VirtualRoot));
			return {};
		}

		auto ValidateSaveVersion(
			const FPackagePath& Path) -> FAssetReadResult
		{
			const FAssetCatalogEntry Existing = Durin::FindAssetExact(Path);
			if (!Existing || ObjectPackage::IsSupportedPackageReaderVersion(Existing->FormatVersion))
				return {};
			return Error(
				EAssetReadError::UnsupportedVersion,
				std::format(
					"Package {} uses unsupported DAST v{} while ordinary saves write DAST v{}.",
					Path.ToString(),
					Existing->FormatVersion,
					OrdinaryAssetPackageWriterVersion));
		}
		auto RollbackAssetWrite(IPackageWriteOperation& Write, FAssetWriteResult Failure) -> FAssetWriteResult
		{
			auto Restored = Write.Rollback();
			auto Rollback = ToAssetWriteResult(Restored);
			if (Restored.State == EPackageWriteState::RecoveryRequired)
			{
				Failure.Effect = Rollback.Effect;
				Failure.Message += "; rollback: " + Rollback.Message;
			}
			return Failure;
		}
	}

	auto ValidatePackageSaveAdmission(const FPackagePath& Path) -> FAssetWriteResult
	{
		if (auto Result = ValidatePackageWriteAdmission(Path); !Result) return Result;
		return AssetWriteResultFromRead(ValidateSaveVersion(Path));
	}

	auto CommitPreparedPackageSave(const FPreparedPackageSave& Prepared,
		const FAssetBundleSaveOptions& Options,
		const FAssetRegistryPublication& Expected) -> FAssetWriteResult
	{
		DPackage* Package = Prepared.Package.Get();
		const auto& Path = Prepared.Header.PackagePath;
		const uint64 Revision = Prepared.Revision;
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return AssetWriteResultFromRead(Guard);
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
			return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (auto Admission = ValidatePackageSaveAdmission(Path); !Admission) return Admission;
		if (Package->GetEditRevision() != Revision || Package->GetPackagePathIdentity() != Path)
			return Error(EAssetWriteError::StaleData, "Package changed during save preparation.");
		if (Options.RootPackage && Options.RootPackage != Package)
			return Error(EAssetWriteError::InvalidData, "The root package is not the saved package.");
		if (Options.ShouldFail)
			for (auto Phase : {EAssetBundleSavePhase::CreateDirectories, EAssetBundleSavePhase::PublishCompanion,
				EAssetBundleSavePhase::StagePackage, Options.RootPackage == Package
					? EAssetBundleSavePhase::PublishRootPackage : EAssetBundleSavePhase::PublishPackage})
				if (Options.ShouldFail(Phase, 0)) return Error(EAssetWriteError::IoError, "Injected save publication failure.");
		const auto& Header = Prepared.Header;
		const auto& Destination = Prepared.Destination;
		auto& Write = *Prepared.Write;
		// Until physical commit succeeds, the writer owns failed-write cleanup.
		if (auto Result = ToAssetWriteResult(Write.Commit()); !Result) return Result;
		FFilePublicationStamp Stamp;
		if (!FFilePublicationStamp::Inspect(Destination, Stamp) || !Stamp.Exists)
			return RollbackAssetWrite(Write, FAssetWriteResult{EAssetWriteError::IoError, "Cannot inspect committed package."});
		const bool Inject = Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::PublishRegistry, 1);
		auto RegistryResult = Inject ? FAssetWriteResult{EAssetWriteError::StaleData, "Injected Registry publication failure."}
			: AssetWriteResultFromRead(AssetPrivate::ToAssetResult(AssetsSaved({BuildSavedAssetMetadata(Header, Destination, Stamp.Size, Stamp.Time)}, Expected)));
		if (!RegistryResult && Options.bRollbackOnRegistryFailure) return RollbackAssetWrite(Write, RegistryResult);
		if (FindResidentPackage(Path) == Package) Package->MarkAsPublished();
		if (Package->GetEditRevision() == Revision) Package->ClearDirty();
		auto Finalized = ToAssetWriteResult(Write.Finalize());
		if (!RegistryResult)
		{
			FenceAssetRegistryProjection(std::span(&Path, 1));
			RegistryResult.Effect = EAssetWriteEffect::ContentCommittedProjectionPending;
			RegistryResult.Message = "ContentCommittedProjectionPending: " + RegistryResult.Message;
			if (!Finalized) { RegistryResult.Message += "; " + Finalized.Message; }
			return RegistryResult;
		}
		return Finalized;
	}
}
