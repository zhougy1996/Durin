#include "AssetMaintenance/PackageMigration.h"

#include "AssetRegistry/LegacyPackageConversion.h"
#include "AssetRegistry/PackageFormat.h"
#include "AssetRegistry/PackageHeader.h"
#include "DObject/PackageFormat.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset
{
	namespace
	{
		struct FFileClosure
		{
			std::filesystem::path MainPath;
			std::filesystem::path BulkPath;
			std::vector<std::byte> Main;
			std::vector<std::byte> Bulk;
			bool bBulkExists = false;
		};

		auto StatusName(EAssetPackageMigrationStatus Status) -> std::string_view
		{
			switch (Status)
			{
			case EAssetPackageMigrationStatus::AlreadyV8: return "AlreadyV8";
			case EAssetPackageMigrationStatus::Ready: return "Ready";
			case EAssetPackageMigrationStatus::Converted: return "Converted";
			case EAssetPackageMigrationStatus::Unsupported: return "Unsupported";
			case EAssetPackageMigrationStatus::Invalid: return "Invalid";
			case EAssetPackageMigrationStatus::Stale: return "Stale";
			case EAssetPackageMigrationStatus::Failed: return "Failed";
			case EAssetPackageMigrationStatus::Cancelled: return "Cancelled";
			}
			return "Failed";
		}

		auto ApplyStatusName(EAssetPackageMigrationApplyStatus Status) -> std::string_view
		{
			switch (Status)
			{
			case EAssetPackageMigrationApplyStatus::Succeeded: return "Succeeded";
			case EAssetPackageMigrationApplyStatus::Partial: return "Partial";
			case EAssetPackageMigrationApplyStatus::Cancelled: return "Cancelled";
			case EAssetPackageMigrationApplyStatus::Blocked: return "Blocked";
			case EAssetPackageMigrationApplyStatus::Failed: return "Failed";
			case EAssetPackageMigrationApplyStatus::RecoveryRequired: return "RecoveryRequired";
			}
			return "Failed";
		}

		auto LoadOptionalFile(const std::filesystem::path& Path,
			bool& OutExists, std::vector<std::byte>& OutBytes) -> bool
		{
			OutExists = false;
			OutBytes.clear();
			std::error_code Error;
			const auto Status = std::filesystem::status(Path, Error);
			if (Error == std::errc::no_such_file_or_directory) return true;
			if (Error || !std::filesystem::is_regular_file(Status)) return false;
			OutExists = true;
			return FFileHelper::LoadFileToArray(OutBytes, Path);
		}

		auto LoadClosure(std::string_view PhysicalPath, FFileClosure& Out) -> bool
		{
			FFileClosure Closure;
			Closure.MainPath = PhysicalPath;
			Closure.BulkPath = Closure.MainPath;
			Closure.BulkPath.replace_extension(".dbulk");
			if (!FFileHelper::LoadFileToArray(Closure.Main, Closure.MainPath)
				|| !LoadOptionalFile(Closure.BulkPath, Closure.bBulkExists, Closure.Bulk))
				return false;
			Out = std::move(Closure);
			return true;
		}

		auto Fingerprint(const FFileClosure& Closure)
			-> FAssetPackageMigrationClosureFingerprint
		{
			return {
				.MainBytes = Closure.Main.size(),
				.MainHash = FXxHash128::HashBuffer(Closure.Main),
				.bBulkExists = Closure.bBulkExists,
				.BulkBytes = Closure.Bulk.size(),
				.BulkHash = Closure.bBulkExists
					? FXxHash128::HashBuffer(Closure.Bulk) : FXxHash128{},
			};
		}

		auto Fingerprint(std::span<const std::byte> Main,
			std::span<const std::byte> Bulk)
			-> FAssetPackageMigrationClosureFingerprint
		{
			return {
				.MainBytes = Main.size(),
				.MainHash = FXxHash128::HashBuffer(Main),
				.bBulkExists = !Bulk.empty(),
				.BulkBytes = Bulk.size(),
				.BulkHash = Bulk.empty() ? FXxHash128{} : FXxHash128::HashBuffer(Bulk),
			};
		}

		auto RemoveFileIfPresent(const std::filesystem::path& Path) -> bool
		{
			std::error_code Error;
			const bool bRemoved = std::filesystem::remove(Path, Error);
			return !Error || (!bRemoved && Error == std::errc::no_such_file_or_directory);
		}

		auto RestoreClosure(const FFileClosure& Before) -> bool
		{
			const bool bMain = FFileHelper::SaveArrayToFileAtomically(
				Before.Main, Before.MainPath, nullptr);
			const bool bBulk = Before.bBulkExists
				? FFileHelper::SaveArrayToFileAtomically(Before.Bulk, Before.BulkPath, nullptr)
				: RemoveFileIfPresent(Before.BulkPath);
			return bMain && bBulk;
		}

		auto ReadHeader(const FFileClosure& Closure, const FAssetPath& PackagePath,
			FAssetPackageHeader& OutHeader, std::string& OutError) -> bool
		{
			uint64 HeaderBytes = 0;
			if (!ReadLittleEndianAt(Closure.Main, 32, HeaderBytes)
				|| HeaderBytes > Closure.Main.size())
			{
				OutError = "MigrationInvalidFrontMatter: package header extent is invalid.";
				return false;
			}
			const FAssetResult Result = ReadAssetPackageHeaderBytes(
				std::span(Closure.Main).first(static_cast<size_t>(HeaderBytes)),
				Closure.Main.size(), Closure.Bulk.size(), PackagePath, OutHeader);
			if (!Result)
			{
				OutError = std::format("MigrationHeaderRejected: {}", Result.Message);
				return false;
			}
			if ((OutHeader.BulkSegmentExtent != 0) != Closure.bBulkExists)
			{
				OutError = "MigrationCompanionMismatch: physical companion presence does not match the package binding.";
				return false;
			}
			return true;
		}

		auto VerifyCanonicalV8(std::span<const std::byte> Main,
			std::span<const std::byte> Bulk, std::string_view PackagePath,
			std::string& OutError) -> bool
		{
			ObjectPackage::FLinkerTables Linker;
			ObjectPackage::FPackageReaderDiagnostic ReadDiagnostic;
			if (!ObjectPackage::ReadPackageV8(
				Main, Bulk, PackagePath, Linker, &ReadDiagnostic))
			{
				OutError = std::format("MigrationV8ReadRejected: {}", ReadDiagnostic.Message);
				return false;
			}
			std::vector<std::byte> CanonicalMain;
			std::vector<std::byte> CanonicalBulk;
			ObjectPackage::FPackageWriterDiagnostic WriteDiagnostic;
			if (!ObjectPackage::WritePackageV8(
				Linker, CanonicalMain, CanonicalBulk, &WriteDiagnostic)
				|| !std::ranges::equal(CanonicalMain, Main)
				|| !std::ranges::equal(CanonicalBulk, Bulk))
			{
				OutError = WriteDiagnostic.Message.empty()
					? "MigrationNonCanonicalV8: package does not re-emit byte-identically."
					: std::format("MigrationV8WriteRejected: {}", WriteDiagnostic.Message);
				return false;
			}
			return true;
		}

		auto AppendFingerprint(FJsonNodeRef Node, std::string_view Name,
			const FAssetPackageMigrationClosureFingerprint& Value) -> void
		{
			FJsonNodeRef FingerprintNode = Node.AddObject(Name);
			FingerprintNode.SetChildValue("mainBytes", Value.MainBytes);
			FingerprintNode.SetChildValue("mainHash", Value.MainHash.ToString());
			FingerprintNode.SetChildValue("bulkExists", Value.bBulkExists);
			FingerprintNode.SetChildValue("bulkBytes", Value.BulkBytes);
			FingerprintNode.SetChildValue("bulkHash",
				Value.bBulkExists ? Value.BulkHash.ToString() : std::string{});
		}

		auto AppendPackages(FJsonNodeRef Packages,
			const FAssetPackageMigrationPlan& Plan) -> void
		{
			for (const auto& Package : Plan.Packages)
			{
				FJsonNodeRef Node = Packages.AppendObject();
				Node.SetChildValue("packagePath", Package.PackagePath.GetView());
				Node.SetChildValue("physicalPath", Package.PhysicalPath);
				Node.SetChildValue("sourceFormatVersion", Package.SourceFormatVersion);
				Node.SetChildValue("status", StatusName(Package.Status));
				Node.SetChildValue("diagnosticCode", Package.DiagnosticCode);
				Node.SetChildValue("diagnostic", Package.Diagnostic);
				AppendFingerprint(Node, "source", Package.Source);
				AppendFingerprint(Node, "target", Package.Target);
			}
		}
	}

	auto PlanAssetPackageMigrationV8(
		std::span<const FAssetPackageCompatibilityProbeInput> Inputs,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetPackageMigrationPlan
	{
		FAssetPackageMigrationPlan Plan;
		Plan.TargetFormatVersion = AssetPackageV8FormatVersion;
		std::vector<const FAssetPackageCompatibilityProbeInput*> Sorted;
		Sorted.reserve(Inputs.size());
		for (const auto& Input : Inputs) Sorted.push_back(&Input);
		std::ranges::sort(Sorted, [](const auto* Left, const auto* Right) {
			return Left->PackagePath.GetView() < Right->PackagePath.GetView();
		});
		for (const auto* Input : Sorted)
		{
			if (IsCancellationRequested && IsCancellationRequested())
			{
				Plan.Status = EAssetPackageMigrationPlanStatus::Cancelled;
				break;
			}
			auto& Record = Plan.Packages.emplace_back();
			Record.PackagePath = Input->PackagePath;
			Record.PhysicalPath = Input->PhysicalPath;
			FFileClosure Closure;
			if (!LoadClosure(Input->PhysicalPath, Closure))
			{
				Record.Status = EAssetPackageMigrationStatus::Invalid;
				Record.DiagnosticCode = "ClosureReadFailed";
				Record.Diagnostic = "MigrationClosureReadFailed: package closure is unreadable.";
				continue;
			}
			Record.Source = Fingerprint(Closure);
			if (Record.Source.MainBytes != Input->ExpectedFileSize
				|| Record.Source.MainHash != Input->ExpectedContentHash)
			{
				Record.Status = EAssetPackageMigrationStatus::Stale;
				Record.DiagnosticCode = "SnapshotStale";
				Record.Diagnostic = "MigrationSnapshotStale: main package changed after discovery.";
				continue;
			}
			FAssetPackageHeader Header;
			if (!ReadHeader(Closure, Input->PackagePath, Header, Record.Diagnostic))
			{
				Record.Status = EAssetPackageMigrationStatus::Invalid;
				Record.DiagnosticCode = "HeaderRejected";
				continue;
			}
			Record.SourceFormatVersion = Header.FormatVersion;
			if (Header.FormatVersion == AssetPackageV8FormatVersion)
			{
				if (!VerifyCanonicalV8(Closure.Main, Closure.Bulk,
					Input->PackagePath.GetView(), Record.Diagnostic))
				{
					Record.Status = EAssetPackageMigrationStatus::Invalid;
					Record.DiagnosticCode = "NonCanonicalV8";
					continue;
				}
				Record.Target = Record.Source;
				Record.Status = EAssetPackageMigrationStatus::AlreadyV8;
				continue;
			}
			if (Header.FormatVersion != AssetPackageV7FormatVersion)
			{
				Record.Status = EAssetPackageMigrationStatus::Unsupported;
				Record.DiagnosticCode = "UnsupportedFormat";
				Record.Diagnostic = std::format(
					"MigrationUnsupportedFormat: DAST v{} is not convertible to v8.",
					Header.FormatVersion);
				continue;
			}
			std::vector<std::byte> TargetMain;
			std::vector<std::byte> TargetBulk;
			FLegacyPackageConversionDiagnostic Diagnostic;
			if (!ConvertDastV7PackageToV8(Closure.Main, Closure.Bulk,
				Input->PackagePath.GetView(), TargetMain, TargetBulk, &Diagnostic))
			{
				Record.Status = Diagnostic.Failure
					== ELegacyPackageConversionFailure::UnsupportedV7Value
					? EAssetPackageMigrationStatus::Unsupported
					: EAssetPackageMigrationStatus::Invalid;
				Record.DiagnosticCode = Record.Status
					== EAssetPackageMigrationStatus::Unsupported
					? "UnsupportedV7Value" : "ConversionRejected";
				Record.Diagnostic = Diagnostic.LogicalPath.empty()
					? Diagnostic.Message
					: std::format("{}: {}", Diagnostic.LogicalPath, Diagnostic.Message);
				continue;
			}
			Record.Target = Fingerprint(TargetMain, TargetBulk);
			Record.Status = EAssetPackageMigrationStatus::Ready;
		}
		return Plan;
	}

	auto ApplyAssetPackageMigrationV8(FAssetPackageMigrationPlan Plan,
		const FAssetPackageMigrationApplyOptions& Options,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetPackageMigrationApplyResult
	{
		FAssetPackageMigrationApplyResult Result{.Plan = std::move(Plan)};
		if (Result.Plan.Status != EAssetPackageMigrationPlanStatus::Completed)
		{
			Result.Status = EAssetPackageMigrationApplyStatus::Cancelled;
			Result.Diagnostic = "MigrationCancelled: planning did not complete.";
			return Result;
		}
		if (std::ranges::any_of(Result.Plan.Packages, [](const auto& Package) {
			return Package.Status == EAssetPackageMigrationStatus::Unsupported
				|| Package.Status == EAssetPackageMigrationStatus::Invalid
				|| Package.Status == EAssetPackageMigrationStatus::Stale;
		}))
		{
			Result.Status = EAssetPackageMigrationApplyStatus::Blocked;
			Result.Diagnostic = "MigrationApplyBlocked: selected plan contains terminal blockers.";
			return Result;
		}

		size_t Converted = 0;
		for (size_t Index = 0; Index < Result.Plan.Packages.size(); ++Index)
		{
			auto& Record = Result.Plan.Packages[Index];
			if (Record.Status != EAssetPackageMigrationStatus::Ready) continue;
			if (IsCancellationRequested && IsCancellationRequested())
			{
				Record.Status = EAssetPackageMigrationStatus::Cancelled;
				Result.Status = Converted ? EAssetPackageMigrationApplyStatus::Partial
					: EAssetPackageMigrationApplyStatus::Cancelled;
				Result.Diagnostic = "MigrationCancelled: admission stopped before the next package.";
				return Result;
			}
			if (Options.ShouldFail
				&& Options.ShouldFail(EAssetPackageMigrationApplyPhase::Revalidate, Index))
			{
				Record.Status = EAssetPackageMigrationStatus::Failed;
				Result.Status = Converted ? EAssetPackageMigrationApplyStatus::Partial
					: EAssetPackageMigrationApplyStatus::Failed;
				Result.Diagnostic = "Injected migration revalidation failure.";
				return Result;
			}
			FFileClosure Before;
			if (!LoadClosure(Record.PhysicalPath, Before)
				|| Fingerprint(Before) != Record.Source)
			{
				Record.Status = EAssetPackageMigrationStatus::Stale;
				Record.DiagnosticCode = "ClosureStale";
				Record.Diagnostic = "MigrationClosureStale: package closure changed after planning.";
				Result.Status = Converted ? EAssetPackageMigrationApplyStatus::Partial
					: EAssetPackageMigrationApplyStatus::Failed;
				Result.Diagnostic = Record.Diagnostic;
				return Result;
			}
			std::vector<std::byte> TargetMain;
			std::vector<std::byte> TargetBulk;
			FLegacyPackageConversionDiagnostic ConvertDiagnostic;
			if ((Options.ShouldFail
					&& Options.ShouldFail(EAssetPackageMigrationApplyPhase::Convert, Index))
				|| !ConvertDastV7PackageToV8(Before.Main, Before.Bulk,
					Record.PackagePath.GetView(), TargetMain, TargetBulk, &ConvertDiagnostic)
				|| Fingerprint(TargetMain, TargetBulk) != Record.Target)
			{
				Record.Status = EAssetPackageMigrationStatus::Failed;
				Record.DiagnosticCode = "ConversionChanged";
				Record.Diagnostic = ConvertDiagnostic.Message.empty()
					? "MigrationConversionChanged: conversion no longer matches the plan."
					: ConvertDiagnostic.Message;
				Result.Status = Converted ? EAssetPackageMigrationApplyStatus::Partial
					: EAssetPackageMigrationApplyStatus::Failed;
				Result.Diagnostic = Record.Diagnostic;
				return Result;
			}

			auto FailAfterPublication = [&](std::string Message) {
				const bool bRestored = RestoreClosure(Before);
				Record.Status = EAssetPackageMigrationStatus::Failed;
				Record.DiagnosticCode = bRestored ? "PublicationRolledBack" : "RecoveryRequired";
				Record.Diagnostic = bRestored
					? std::move(Message) + " Prior closure was restored."
					: "MigrationRecoveryRequired: publication failed and prior closure could not be restored.";
				Result.Status = bRestored
					? (Converted ? EAssetPackageMigrationApplyStatus::Partial
						: EAssetPackageMigrationApplyStatus::Failed)
					: EAssetPackageMigrationApplyStatus::RecoveryRequired;
				Result.Diagnostic = Record.Diagnostic;
			};

			if (!TargetBulk.empty())
			{
				if (!FFileHelper::SaveArrayToFileAtomically(
					TargetBulk, Before.BulkPath, nullptr))
				{
					FailAfterPublication("MigrationBulkPublicationFailed:");
					return Result;
				}
			}
			if (Options.ShouldFail
				&& Options.ShouldFail(EAssetPackageMigrationApplyPhase::PublishBulk, Index))
			{
				FailAfterPublication("Injected migration bulk-publication failure.");
				return Result;
			}
			if (!FFileHelper::SaveArrayToFileAtomically(
				TargetMain, Before.MainPath, nullptr))
			{
				FailAfterPublication("MigrationMainPublicationFailed:");
				return Result;
			}
			if (TargetBulk.empty() && !RemoveFileIfPresent(Before.BulkPath))
			{
				FailAfterPublication("MigrationObsoleteCompanionRemovalFailed:");
				return Result;
			}
			if (Options.ShouldFail
				&& Options.ShouldFail(EAssetPackageMigrationApplyPhase::PublishMain, Index))
			{
				FailAfterPublication("Injected migration main-publication failure.");
				return Result;
			}

			FFileClosure Published;
			std::string VerificationError;
			const bool bVerified = LoadClosure(Record.PhysicalPath, Published)
				&& Fingerprint(Published) == Record.Target
				&& VerifyCanonicalV8(Published.Main, Published.Bulk,
					Record.PackagePath.GetView(), VerificationError)
				&& !(Options.ShouldFail
					&& Options.ShouldFail(EAssetPackageMigrationApplyPhase::Verify, Index));
			if (!bVerified)
			{
				FailAfterPublication(VerificationError.empty()
					? "MigrationVerificationFailed:" : VerificationError);
				return Result;
			}
			Record.Status = EAssetPackageMigrationStatus::Converted;
			Result.ChangedPaths.push_back(Before.MainPath.generic_string());
			if (Before.bBulkExists || !TargetBulk.empty())
				Result.ChangedPaths.push_back(Before.BulkPath.generic_string());
			++Converted;
		}
		Result.Status = EAssetPackageMigrationApplyStatus::Succeeded;
		return Result;
	}

	auto SerializeAssetPackageMigrationPlanReport(
		const FAssetPackageMigrationPlan& Plan) -> std::string
	{
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", AssetPackageMigrationReportSchemaVersion);
		Root.SetChildValue("operation", "migrate-v8");
		Root.SetChildValue("mode", "plan");
		Root.SetChildValue("status", Plan.Status
			== EAssetPackageMigrationPlanStatus::Completed ? "Completed" : "Cancelled");
		Root.SetChildValue("targetFormatVersion", Plan.TargetFormatVersion);
		AppendPackages(Root.AddArray("packages"), Plan);
		return Document.ToString();
	}

	auto SerializeAssetPackageMigrationApplyReport(
		const FAssetPackageMigrationApplyResult& Result) -> std::string
	{
		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("schemaVersion", AssetPackageMigrationReportSchemaVersion);
		Root.SetChildValue("operation", "migrate-v8");
		Root.SetChildValue("mode", "apply");
		Root.SetChildValue("status", ApplyStatusName(Result.Status));
		Root.SetChildValue("diagnostic", Result.Diagnostic);
		Root.SetChildValue("targetFormatVersion", Result.Plan.TargetFormatVersion);
		AppendPackages(Root.AddArray("packages"), Result.Plan);
		FJsonNodeRef ChangedPaths = Root.AddArray("changedPaths");
		for (const auto& Path : Result.ChangedPaths) ChangedPaths.AppendValue(Path);
		return Document.ToString();
	}
}
