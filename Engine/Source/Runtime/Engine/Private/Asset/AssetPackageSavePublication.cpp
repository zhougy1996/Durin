#include "AssetPackageSaveInternal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "AssetLiveLoadGuard.h"
#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageVersionPolicy.h"
#include "Misc/MountPaths.h"
#include "Asset/Load.h"
#include "AssetRegistry/RegistryResult.h"
#include "DObject/Package.h"
#include "Misc/FileTime.h"

namespace Durin::AssetPrivate
{
	auto BuildSavedAssetMetadata(const FAssetPackageHeader& Header,
		const std::filesystem::path& Destination, uintmax_t FileSize,
		std::filesystem::file_time_type LastWriteTime) -> FPackageData
	{
		// The captured file describes the saved revision, even if live objects change.
		return {
			.PackagePath = Header.PackagePath,
			.PhysicalPath = Destination.generic_string(),
			.TopLevelAssets = Header.TopLevelAssets,
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
		FAssetWriteResult Out{.Error = std::move(Result.Error)};
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
			if (Out.Error) Out.Error->PushContext("retained package backup", File.generic_string());
		return Out;
	}
	namespace
	{
		auto Error(Durin::UnifiedError::FErrorId Id, std::string Message) -> std::expected<void, Durin::UnifiedError::FError> { return std::unexpected(Durin::UnifiedError::FError{Id, std::move(Message)}); }
		auto WriteFailure(UnifiedError::FErrorId Code, std::string Message) -> FAssetWriteResult { return {.Error = UnifiedError::FError{Code, std::move(Message)}}; }

		auto ValidatePackageWriteAdmission(const FPackagePath& Path) -> std::expected<void, UnifiedError::FError>
		{
			if (IsPackageLoading(Path)) return Error(AssetLoadErrors::InUse, "An incomplete package cannot be saved.");
			const auto Mount =
				FMountPaths::FindMountForVirtualPath(Path.GetView());
			if (!Mount)
				return Error(AssetLoadErrors::InvalidPath,
					std::format("Package {} does not use a registered content mount.",
						Path.ToString()));
			if (!Mount->Mount->bContentWritable)
				return Error(UnifiedError::FErrorId{"AssetMutation", "ReadOnlyMode"},
					std::format("Content mount {} is read-only.",
						Mount->Mount->VirtualRoot));
			return {};
		}

		auto ValidateSaveVersion(
			const FPackagePath& Path) -> std::expected<void, UnifiedError::FError>
		{
			const FPackageCatalogEntry Existing = Durin::FindPackageExact(Path);
			if (!Existing || ObjectPackage::IsSupportedPackageReaderVersion(Existing->FormatVersion))
				return {};
			return Error(PackageReadErrors::UnsupportedVersion, std::format(
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
				if (Rollback.Error)
				{
					Failure.Error->PushContext("rollback domain", Rollback.Error->GetDomain())
						.PushContext("rollback code", Rollback.Error->GetCode())
						.PushContext("rollback message", Rollback.Error->GetMessage());
					for (const auto& Context : Rollback.Error->GetContexts())
						Failure.Error->PushContext("rollback " + Context.Key, Context.Value);
				}
			}
			return Failure;
		}
	}

	auto ValidatePackageSaveAdmission(const FPackagePath& Path) -> std::expected<void, UnifiedError::FError>
	{
		if (auto Result = ValidatePackageWriteAdmission(Path); !Result) return Result;
		return ValidateSaveVersion(Path);
	}

	auto CommitPreparedPackageSave(const FPreparedPackageSave& Prepared,
		const FPackageSaveCommitContext& Context,
		const FAssetRegistryPublication& Expected) -> FAssetWriteResult
	{
		DPackage* Package = Prepared.Package.Get();
		const auto& Path = Prepared.Header.PackagePath;
		const uint64 Revision = Prepared.Revision;
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return {.Error = Guard.error()};
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
			return WriteFailure(AssetWriteErrors::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (auto Admission = ValidatePackageSaveAdmission(Path); !Admission) return FAssetWriteResult{Admission.error()};
		if (Package->GetEditRevision() != Revision || Package->GetPackagePathIdentity() != Path)
			return WriteFailure(AssetWriteErrors::StaleData, "Package changed during save preparation.");
		if (Context.Fault)
			for (auto Phase : {Testing::EPackageSaveFaultPoint::CreateDirectories, Testing::EPackageSaveFaultPoint::PublishCompanion,
				Testing::EPackageSaveFaultPoint::StagePackage, Testing::EPackageSaveFaultPoint::PublishPackage})
				if ((Phase != Testing::EPackageSaveFaultPoint::PublishCompanion || Prepared.Header.BulkSegmentExtent)
					&& Context.Fault(Phase, Context.PackageIndex)) return WriteFailure(AssetWriteErrors::IoError, "Injected save publication failure.");
		const auto& Header = Prepared.Header;
		const auto& Destination = Prepared.Destination;
		auto& Write = *Prepared.Write;
		// Until physical commit succeeds, the writer owns failed-write cleanup.
		if (auto Result = ToAssetWriteResult(Write.Commit()); !Result) return Result;
		FFilePublicationStamp Stamp;
		std::optional<FFileSystemError> InspectionError;
		if (!FFilePublicationStamp::Inspect(Destination, Stamp, &InspectionError))
			return RollbackAssetWrite(Write, FAssetWriteResult{.Error = ToError(*InspectionError)});
		if (!Stamp.Exists)
			return RollbackAssetWrite(Write, FAssetWriteResult{.Error = UnifiedError::FError{AssetWriteErrors::StaleData, "Committed package disappeared."}});
		const bool Inject = Context.Fault && Context.Fault(Testing::EPackageSaveFaultPoint::PublishRegistry, Context.PackageIndex);
		FAssetWriteResult RegistryResult;
		if (Inject) RegistryResult.Error.emplace(AssetWriteErrors::StaleData, "Injected Registry publication failure.");
		else if (auto Published = AssetsSaved({BuildSavedAssetMetadata(Header, Destination, Stamp.Size, Stamp.Time)}, Expected); !Published)
			RegistryResult.Error = Durin::ToError(Published);
		if (FindResidentPackage(Path) == Package)
		{
			Package->RecordDiskExports(Prepared.DiskExports, Prepared.MainDigest);
			Package->MarkAsPublished();
		}
		if (Package->GetEditRevision() == Revision) Package->ClearDirty();
		auto Finalized = ToAssetWriteResult(Write.Finalize());
		if (!RegistryResult)
		{
			FenceAssetRegistryProjection(std::span(&Path, 1));
			RegistryResult.Effect = EAssetWriteEffect::ContentCommittedProjectionPending;
			if (Finalized.Error)
			{
				RegistryResult.Error->PushContext("finalize domain", Finalized.Error->GetDomain())
					.PushContext("finalize code", Finalized.Error->GetCode())
					.PushContext("finalize message", Finalized.Error->GetMessage());
				for (const auto& Context : Finalized.Error->GetContexts())
					RegistryResult.Error->PushContext("finalize " + Context.Key, Context.Value);
			}
			return RegistryResult;
		}
		return Finalized;
	}
}
