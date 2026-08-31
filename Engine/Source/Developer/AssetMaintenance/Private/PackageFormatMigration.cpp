#include "AssetMaintenance/PackageFormatMigration.h"

#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/FileTime.h"

namespace Durin::Asset
{
	namespace
	{
		struct FFileSnapshot
		{
			std::filesystem::path Path;
			std::vector<std::byte> Bytes;
			bool bExisted = false;
		};

		auto StatusName(EPackageFormatMigrationStatus Status) -> std::string_view
		{
			switch (Status)
			{
			case EPackageFormatMigrationStatus::Ready: return "Ready";
			case EPackageFormatMigrationStatus::Blocked: return "Blocked";
			case EPackageFormatMigrationStatus::Migrated: return "Migrated";
			case EPackageFormatMigrationStatus::Stale: return "Stale";
			case EPackageFormatMigrationStatus::Failed: return "Failed";
			case EPackageFormatMigrationStatus::Cancelled: return "Cancelled";
			}
			return "Failed";
		}

		auto CaptureFile(const std::filesystem::path& Path, uint32 ReaderVersion,
			std::vector<std::byte>& OutBytes, FAssetPackageFingerprint& Out,
			std::string& OutError) -> bool
		{
			std::error_code Error;
			const uintmax_t InitialSize = std::filesystem::file_size(Path, Error);
			const auto InitialTime = Error ? std::filesystem::file_time_type{}
				: std::filesystem::last_write_time(Path, Error);
			if (Error || !FFileHelper::LoadFileToArray(OutBytes, Path))
			{
				OutError = std::format("Unable to read '{}'.", Path.generic_string());
				return false;
			}
			const uintmax_t FinalSize = std::filesystem::file_size(Path, Error);
			const auto FinalTime = Error ? std::filesystem::file_time_type{}
				: std::filesystem::last_write_time(Path, Error);
			if (Error || InitialSize != FinalSize || InitialTime != FinalTime
				|| FinalSize != OutBytes.size())
			{
				OutError = std::format("'{}' changed while it was fingerprinted.",
					Path.generic_string());
				return false;
			}
			Out = {.FileSize = FinalSize,
				.LastWriteTimeTicks = FileTime::ToStableTicks(FinalTime),
				.ContentHash = FXxHash128::HashBuffer(OutBytes),
				.ReaderVersion = ReaderVersion};
			return true;
		}

		auto FingerprintBytes(std::span<const std::byte> Bytes, uint32 ReaderVersion)
			-> FAssetPackageFingerprint
		{
			return {.FileSize = Bytes.size(), .ContentHash = FXxHash128::HashBuffer(Bytes),
				.ReaderVersion = ReaderVersion};
		}

		auto CaptureClosure(const FPackageFormatMigrationInput& Input,
			uint32 ReaderVersion, std::vector<std::byte>& OutMain,
			std::vector<std::byte>& OutBulk, FPackageClosureFingerprint& Out,
			std::string& OutError) -> bool
		{
			if (!CaptureFile(Input.MainPath, ReaderVersion, OutMain, Out.Main, OutError))
				return false;
			std::error_code Error;
			Out.bHasBulk = !Input.BulkPath.empty() && std::filesystem::exists(Input.BulkPath, Error);
			if (Error)
			{
				OutError = std::format("Unable to inspect '{}'.", Input.BulkPath.generic_string());
				return false;
			}
			if (Out.bHasBulk && !CaptureFile(Input.BulkPath, ReaderVersion,
				OutBulk, Out.Bulk, OutError)) return false;
			return true;
		}

		auto CaptureSnapshot(const std::filesystem::path& Path, FFileSnapshot& Out) -> bool
		{
			Out = {.Path = Path};
			if (Path.empty()) return true;
			std::error_code Error;
			Out.bExisted = std::filesystem::exists(Path, Error);
			return !Error && (!Out.bExisted || FFileHelper::LoadFileToArray(Out.Bytes, Path));
		}

		auto RestoreSnapshot(const FFileSnapshot& Snapshot) -> bool
		{
			if (Snapshot.Path.empty()) return true;
			if (Snapshot.bExisted)
				return FFileHelper::SaveArrayToFileAtomically(Snapshot.Bytes, Snapshot.Path);
			std::error_code Error;
			std::filesystem::remove(Snapshot.Path, Error);
			return !Error;
		}

		auto SameSource(const FPackageClosureFingerprint& Left,
			const FPackageClosureFingerprint& Right) -> bool
		{
			return Left == Right;
		}

		auto TargetFingerprint(std::span<const std::byte> Main,
			std::span<const std::byte> Bulk) -> FPackageClosureFingerprint
		{
			return {.Main = FingerprintBytes(Main, ObjectPackage::DastV9FormatVersion),
				.Bulk = FingerprintBytes(Bulk, ObjectPackage::DastV9FormatVersion),
				.bHasBulk = !Bulk.empty()};
		}

		auto VerifyCanonical(std::span<const std::byte> Main,
			std::span<const std::byte> Bulk, const FPackagePath& PackagePath) -> bool
		{
			ObjectPackage::FLinkerTables Linker;
			if (!ObjectPackage::ReadPackageV9(Main, Bulk, PackagePath, Linker)) return false;
			std::vector<std::byte> ReemittedMain;
			std::vector<std::byte> ReemittedBulk;
			return ObjectPackage::WritePackageV9(Linker, ReemittedMain, ReemittedBulk)
				&& std::ranges::equal(Main, ReemittedMain)
				&& std::ranges::equal(Bulk, ReemittedBulk);
		}

		auto AppendFingerprint(FJsonNodeRef Parent, std::string_view Name,
			const FPackageClosureFingerprint& Fingerprint) -> void
		{
			FJsonNodeRef Closure = Parent.AddObject(Name);
			auto AppendFile = [](FJsonNodeRef Node, const FAssetPackageFingerprint& Value) {
				Node.SetChildValue("bytes", static_cast<uint64>(Value.FileSize));
				Node.SetChildValue("hashLow", Value.ContentHash.HashLow);
				Node.SetChildValue("hashHigh", Value.ContentHash.HashHigh);
				Node.SetChildValue("readerVersion", Value.ReaderVersion);
			};
			AppendFile(Closure.AddObject("main"), Fingerprint.Main);
			Closure.SetChildValue("hasBulk", Fingerprint.bHasBulk);
			if (Fingerprint.bHasBulk) AppendFile(Closure.AddObject("bulk"), Fingerprint.Bulk);
		}

		auto AppendPackages(FJsonNodeRef Packages,
			const FPackageFormatMigrationPlan& Plan) -> void
		{
			for (const FPackageFormatMigrationItem& Item : Plan.Packages)
			{
				FJsonNodeRef Node = Packages.AppendObject();
				Node.SetChildValue("packagePath", Item.Input.PackagePath.ToString());
				Node.SetChildValue("mainPath", Item.Input.MainPath.generic_string());
				Node.SetChildValue("bulkPath", Item.Input.BulkPath.generic_string());
				Node.SetChildValue("status", StatusName(Item.Status));
				Node.SetChildValue("diagnostic", Item.Diagnostic);
				AppendFingerprint(Node, "sourceFingerprint", Item.SourceFingerprint);
				AppendFingerprint(Node, "targetFingerprint", Item.TargetFingerprint);
			}
		}
	}

	auto PlanPackageFormatMigration(std::span<const FPackageFormatMigrationInput> Inputs,
		const FPackageFormatMigrationCancellationCheck& IsCancelled)
		-> FPackageFormatMigrationPlan
	{
		FPackageFormatMigrationPlan Plan;
		std::vector<FPackageFormatMigrationInput> Sorted(Inputs.begin(), Inputs.end());
		std::ranges::sort(Sorted, {}, [](const auto& Input) { return Input.PackagePath; });
		for (const FPackageFormatMigrationInput& Input : Sorted)
		{
			if (IsCancelled && IsCancelled())
			{
				Plan.Status = EPackageFormatMigrationPlanStatus::Cancelled;
				break;
			}
			FPackageFormatMigrationItem& Item = Plan.Packages.emplace_back();
			Item.Input = Input;
			std::vector<std::byte> Main;
			std::vector<std::byte> Bulk;
			if (!CaptureClosure(Input, ObjectPackage::DastV8FormatVersion, Main, Bulk,
				Item.SourceFingerprint, Item.Diagnostic)) continue;
			std::vector<std::byte> TargetMain;
			std::vector<std::byte> TargetBulk;
			ObjectPackage::FPackageV8ConversionDiagnostic Diagnostic;
			if (!ObjectPackage::ConvertPackageV8ToV9(Main, Bulk, Input.PackagePath,
				TargetMain, TargetBulk, &Diagnostic))
			{
				Item.Diagnostic = Diagnostic.Message;
				continue;
			}
			Item.TargetFingerprint = TargetFingerprint(TargetMain, TargetBulk);
			Item.Status = EPackageFormatMigrationStatus::Ready;
		}
		return Plan;
	}

	auto ApplyPackageFormatMigration(FPackageFormatMigrationPlan Plan,
		const FPackageFormatMigrationApplyOptions& Options,
		const FPackageFormatMigrationCancellationCheck& IsCancelled)
		-> FPackageFormatMigrationApplyResult
	{
		FPackageFormatMigrationApplyResult Result{.Plan = std::move(Plan)};
		if (Result.Plan.Status != EPackageFormatMigrationPlanStatus::Completed)
		{
			Result.Status = EPackageFormatMigrationApplyStatus::Cancelled;
			Result.Diagnostic = "PackageFormatMigrationCancelled: planning did not complete.";
			return Result;
		}
		if (std::ranges::any_of(Result.Plan.Packages,
			[](const auto& Item) { return Item.Status == EPackageFormatMigrationStatus::Blocked; }))
		{
			Result.Status = EPackageFormatMigrationApplyStatus::Blocked;
			Result.Diagnostic = "PackageFormatMigrationBlocked: the plan contains blockers.";
			return Result;
		}
		size_t Completed = 0;
		for (size_t Index = 0; Index < Result.Plan.Packages.size(); ++Index)
		{
			FPackageFormatMigrationItem& Item = Result.Plan.Packages[Index];
			if (Item.Status != EPackageFormatMigrationStatus::Ready) continue;
			if (IsCancelled && IsCancelled())
			{
				Item.Status = EPackageFormatMigrationStatus::Cancelled;
				Result.Status = Completed ? EPackageFormatMigrationApplyStatus::Partial
					: EPackageFormatMigrationApplyStatus::Cancelled;
				return Result;
			}
			std::vector<std::byte> Main;
			std::vector<std::byte> Bulk;
			FPackageClosureFingerprint Current;
			if ((Options.ShouldFail && Options.ShouldFail(EPackageFormatMigrationApplyPhase::Revalidate, Index))
				|| !CaptureClosure(Item.Input, ObjectPackage::DastV8FormatVersion,
					Main, Bulk, Current, Item.Diagnostic))
			{
				Item.Status = EPackageFormatMigrationStatus::Failed;
				Result.Status = Completed ? EPackageFormatMigrationApplyStatus::Partial
					: EPackageFormatMigrationApplyStatus::Failed;
				return Result;
			}
			if (!SameSource(Current, Item.SourceFingerprint))
			{
				Item.Status = EPackageFormatMigrationStatus::Stale;
				Item.Diagnostic = "PackageFormatMigrationStale: source closure changed after planning.";
				Result.Status = Completed ? EPackageFormatMigrationApplyStatus::Partial
					: EPackageFormatMigrationApplyStatus::Failed;
				return Result;
			}
			std::vector<std::byte> TargetMain;
			std::vector<std::byte> TargetBulk;
			ObjectPackage::FPackageV8ConversionDiagnostic Conversion;
			if ((Options.ShouldFail && Options.ShouldFail(EPackageFormatMigrationApplyPhase::Convert, Index))
				|| !ObjectPackage::ConvertPackageV8ToV9(Main, Bulk, Item.Input.PackagePath,
					TargetMain, TargetBulk, &Conversion)
				|| TargetFingerprint(TargetMain, TargetBulk) != Item.TargetFingerprint)
			{
				Item.Status = EPackageFormatMigrationStatus::Failed;
				Item.Diagnostic = Conversion.Message.empty()
					? "PackageFormatMigrationNondeterministic: target fingerprint changed."
					: Conversion.Message;
				Result.Status = Completed ? EPackageFormatMigrationApplyStatus::Partial
					: EPackageFormatMigrationApplyStatus::Failed;
				return Result;
			}
			FFileSnapshot MainSnapshot;
			FFileSnapshot BulkSnapshot;
			if (!CaptureSnapshot(Item.Input.MainPath, MainSnapshot)
				|| !CaptureSnapshot(Item.Input.BulkPath, BulkSnapshot))
			{
				Item.Status = EPackageFormatMigrationStatus::Failed;
				Result.Status = Completed ? EPackageFormatMigrationApplyStatus::Partial
					: EPackageFormatMigrationApplyStatus::Failed;
				return Result;
			}
			auto Rollback = [&] {
				return RestoreSnapshot(MainSnapshot) && RestoreSnapshot(BulkSnapshot);
			};
			FFileHelper::FAtomicFileError FileError;
			bool bPublished = !(Options.ShouldFail
				&& Options.ShouldFail(EPackageFormatMigrationApplyPhase::PublishMain, Index))
				&& FFileHelper::SaveArrayToFileAtomically(TargetMain, Item.Input.MainPath, &FileError);
			if (bPublished && !TargetBulk.empty())
				bPublished = !(Options.ShouldFail
					&& Options.ShouldFail(EPackageFormatMigrationApplyPhase::PublishBulk, Index))
					&& FFileHelper::SaveArrayToFileAtomically(TargetBulk, Item.Input.BulkPath, &FileError);
			std::vector<std::byte> PublishedMain;
			std::vector<std::byte> PublishedBulk;
			const bool bVerified = bPublished
				&& !(Options.ShouldFail && Options.ShouldFail(EPackageFormatMigrationApplyPhase::Verify, Index))
				&& FFileHelper::LoadFileToArray(PublishedMain, Item.Input.MainPath)
				&& (TargetBulk.empty() || FFileHelper::LoadFileToArray(PublishedBulk, Item.Input.BulkPath))
				&& TargetFingerprint(PublishedMain, PublishedBulk) == Item.TargetFingerprint
				&& VerifyCanonical(PublishedMain, PublishedBulk, Item.Input.PackagePath);
			if (!bVerified)
			{
				const bool bRestored = Rollback();
				Item.Status = EPackageFormatMigrationStatus::Failed;
				Item.Diagnostic = bRestored
					? "PackageFormatMigrationPublicationFailed: prior closure was restored."
					: "PackageFormatMigrationRecoveryRequired: prior closure could not be restored.";
				Result.Status = bRestored
					? (Completed ? EPackageFormatMigrationApplyStatus::Partial
						: EPackageFormatMigrationApplyStatus::Failed)
					: EPackageFormatMigrationApplyStatus::RecoveryRequired;
				return Result;
			}
			Item.Status = EPackageFormatMigrationStatus::Migrated;
			Result.ChangedPaths.push_back(Item.Input.MainPath.generic_string());
			if (!TargetBulk.empty()) Result.ChangedPaths.push_back(Item.Input.BulkPath.generic_string());
			++Completed;
		}
		Result.Status = EPackageFormatMigrationApplyStatus::Succeeded;
		return Result;
	}

	auto SerializePackageFormatMigrationPlanReport(
		const FPackageFormatMigrationPlan& Plan) -> std::string
	{
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", PackageFormatMigrationReportSchemaVersion);
		Root.SetChildValue("operation", "package-format-migration");
		Root.SetChildValue("mode", "plan");
		Root.SetChildValue("status", Plan.Status == EPackageFormatMigrationPlanStatus::Completed
			? "Completed" : "Cancelled");
		Root.SetChildValue("sourceFormatVersion", Plan.SourceFormatVersion);
		Root.SetChildValue("targetFormatVersion", Plan.TargetFormatVersion);
		AppendPackages(Root.AddArray("packages"), Plan);
		return Document.ToString();
	}

	auto SerializePackageFormatMigrationApplyReport(
		const FPackageFormatMigrationApplyResult& Result) -> std::string
	{
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", PackageFormatMigrationReportSchemaVersion);
		Root.SetChildValue("operation", "package-format-migration");
		Root.SetChildValue("mode", "apply");
		const auto Status = [&] {
			switch (Result.Status)
			{
			case EPackageFormatMigrationApplyStatus::Succeeded: return "Succeeded";
			case EPackageFormatMigrationApplyStatus::Partial: return "Partial";
			case EPackageFormatMigrationApplyStatus::Cancelled: return "Cancelled";
			case EPackageFormatMigrationApplyStatus::Blocked: return "Blocked";
			case EPackageFormatMigrationApplyStatus::Failed: return "Failed";
			case EPackageFormatMigrationApplyStatus::RecoveryRequired: return "RecoveryRequired";
			}
			return "Failed";
		}();
		Root.SetChildValue("status", Status);
		Root.SetChildValue("diagnostic", Result.Diagnostic);
		Root.SetChildValue("sourceFormatVersion", Result.Plan.SourceFormatVersion);
		Root.SetChildValue("targetFormatVersion", Result.Plan.TargetFormatVersion);
		AppendPackages(Root.AddArray("packages"), Result.Plan);
		return Document.ToString();
	}
}
