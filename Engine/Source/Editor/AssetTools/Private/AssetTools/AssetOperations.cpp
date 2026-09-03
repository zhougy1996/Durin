#include "AssetTools/IAssetTools.h"

#include "Asset/PackageSerialization.h"
#include "AssetMaintenance/CanonicalResave.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "AssetRegistry/Catalog.h"
#include "AssetOperationResultInternal.h"
#include "Asset/Load.h"
#include "Asset/MutationTypes.h"
#include "Asset/RedirectorFixup.h"
#include "Asset/Relocation.h"
#include "DObject/Object.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"

namespace Durin
{
	auto MakeRejectedAssetOperation(
		EAssetOperationKind Kind, std::string Message) -> FAssetOperationResult
	{
		return {
			.Kind = Kind,
			.State = EAssetOperationTerminalState::Rejected,
			.Message = std::move(Message)};
	}

	namespace
	{
		auto Publish(
			const FPublishAssetOperation& Callback,
			FAssetOperationResult& Result) -> void
		{
			if (!Callback || !Result) return;
			Callback({
				.Kind = Result.Kind,
				.Persistence = Result.Persistence,
				.AffectedAssets = Result.AffectedAssets,
				.Warnings = Result.Warnings});
			Result.bPublished = true;
		}

		auto CommitMutation(
			FAssetMutationJob Job,
			EAssetOperationKind Kind,
			std::span<const FPackagePath> Affected) -> FAssetOperationResult
		{
			const FAssetResult Committed = Job.ResumeForward();
			if (!Committed) return AssetToolsPrivate::FromEngineResult(Kind, Committed, Affected);
			FAssetOperationResult Result{
				.Kind = Kind,
				.Persistence = EAssetOperationPersistenceState::Persisted,
				.bPublished = true};
			Result.AffectedAssets.assign(Affected.begin(), Affected.end());
			return Result;
		}
	}

	auto DuplicateAssetWithEditorPolicy(const FAssetDuplicateRequest& Request)
		-> FAssetOperationResult
	{
		if (!Request.SourcePath.IsValid() || Request.DestinationDirectory.empty()
			|| !Request.ResolvePhysicalPackagePath)
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"Asset duplication requires a source, destination, and path resolver.");
		const FTopLevelAssetCatalogEntry Source =
			FindTopLevelAssetExact(Request.SourcePath);
		if (!Source || Source->IsRedirector())
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"The copied source is no longer an available real asset.");

		std::string Directory = Request.DestinationDirectory;
		if (!Directory.ends_with('/')) Directory.push_back('/');
		const std::string AssetName(Request.SourcePath.GetAssetName());
		FTopLevelAssetPath DestinationAssetPath;
		FPackagePath DestinationPackagePath;
		std::string DestinationPhysicalPath;
		for (uint32 Suffix = 0; Suffix <= 10000; ++Suffix)
		{
			const std::string CandidateName = Suffix == 0 ? AssetName
				: Suffix == 1 ? AssetName + "_Copy"
				: std::format("{}_Copy{}", AssetName, Suffix);
			FPackagePath CandidatePath;
			if (!FPackagePath::TryCreate(Directory + CandidateName, CandidatePath)) continue;
			std::string CandidatePhysical = Request.ResolvePhysicalPackagePath(CandidatePath);
			if (CandidatePhysical.empty() || FindAssetExact(CandidatePath)
				|| FindResidentPackage(CandidatePath)) continue;
			std::error_code Error;
			const bool bExists = std::filesystem::exists(CandidatePhysical, Error);
			if (Error)
				return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
					std::format("Could not inspect the duplicate destination: {}", Error.message()));
			if (bExists) continue;
			if (!FTopLevelAssetPath::TryCreate(
				CandidatePath, CandidateName, DestinationAssetPath)) continue;
			DestinationPackagePath = std::move(CandidatePath);
			DestinationPhysicalPath = std::move(CandidatePhysical);
			break;
		}
		if (!DestinationAssetPath.IsValid())
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"Could not find an available copy name in this folder.");

		FObjectPath SourceObjectPath;
		if (!FObjectPath::TryCreate(
			Request.SourcePath, std::span<const std::string>{}, SourceObjectPath))
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"The source top-level asset path is invalid.");
		DObject* SourceAsset = nullptr;
		FAssetResult EngineResult = LoadObject(
			SourceObjectPath, nullptr, SourceAsset);
		if (!EngineResult)
			return AssetToolsPrivate::FromEngineResult(EAssetOperationKind::Duplicate, EngineResult);
		DPackage* SourcePackage = SourceAsset ? SourceAsset->GetPackage() : nullptr;
		if (!SourceAsset || !SourcePackage || SourceAsset->GetOuter() != SourcePackage
			|| SourcePackage->FindTopLevelAsset(SourceAsset->GetFName()) != SourceAsset)
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"The source is not a registered top-level asset.");

		DPackage* DestinationPackage = CreatePackage(DestinationPackagePath);
		if (!DestinationPackage)
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"The destination package could not be created.");
		DObject* DuplicatedAsset = DuplicateObject(
			SourceAsset, DestinationPackage,
			FName(DestinationAssetPath.GetAssetName()));
		if (!DuplicatedAsset
			|| DestinationPackage->FindTopLevelAsset(
				DuplicatedAsset->GetFName()) != DuplicatedAsset)
		{
			MarkObjectHierarchyAsGarbage(DestinationPackage);
			CollectGarbage();
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"The source object graph could not be duplicated as an asset.");
		}
		DestinationPackage->MarkDirty();
		DestinationPackage->MarkAsNewlyCreated();
		if (Request.bSave)
		{
			EngineResult = SavePackage(DestinationPackage);
			if (!EngineResult)
			{
				FAssetOperationResult Failure = AssetToolsPrivate::FromEngineResult(
					EAssetOperationKind::Duplicate, EngineResult);
				const FAssetResult Cleanup = UnloadPackage(
					DestinationPackagePath,
					EAssetPackageUnloadPolicy::DiscardUnsaved);
				if (!Cleanup)
				{
					Failure.State = EAssetOperationTerminalState::RecoveryRequired;
					Failure.Message += std::format(
						" The unsaved duplicate could not be discarded: {}", Cleanup.Message);
				}
				return Failure;
			}
		}
		FAssetOperationResult Result{
			.Kind = EAssetOperationKind::Duplicate,
			.Persistence = Request.bSave
				? EAssetOperationPersistenceState::Persisted
				: EAssetOperationPersistenceState::Dirty,
			.AffectedAssets = {DestinationPackagePath},
			.Asset = DuplicatedAsset,
			.Package = DuplicatedAsset->GetPackage(),
			.PhysicalPath = std::move(DestinationPhysicalPath)};
		Publish(Request.Publish, Result);
		return Result;
	}

	auto SaveAssetsWithEditorPolicy(const FAssetSaveRequest& Request)
		-> FAssetOperationResult
	{
		if (Request.AssetPaths.empty())
			return MakeRejectedAssetOperation(
				EAssetOperationKind::Save, "No asset packages were selected for saving.");
		if (Request.Mode == EAssetSaveMode::CanonicalResave)
		{
			const FAssetPackageDiscoverySnapshot Snapshot =
				CaptureMountedAssetPackageSnapshot();
			if (Snapshot.Status != EAssetPackageSnapshotStatus::Completed)
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					Snapshot.Error.empty() ? "Canonical-resave discovery did not complete."
						: Snapshot.Error);
			const FReflectionCompatibilityCatalog Catalog =
				FReflectionCompatibilityCatalog::Capture();
			std::vector<FAssetPackageCompatibilityProbeInput> Inputs;
			for (const FPackagePath& Path : Request.AssetPaths)
			{
				const auto Input = std::ranges::find(
					Snapshot.Packages, Path,
					&FAssetPackageCompatibilityProbeInput::PackagePath);
				if (Input == Snapshot.Packages.end())
					return MakeRejectedAssetOperation(EAssetOperationKind::Save, std::format(
						"Package {} is not in a content-writable mounted snapshot.", Path.ToString()));
				Inputs.push_back(*Input);
				Inputs.back().bIncludeNestedMigrationEvidence = true;
			}
			auto Audit = RunAssetCompatibilityAudit(Inputs, Catalog);
			if (Audit.Status != EAssetCompatibilityAuditStatus::Completed
				|| Audit.Records.size() != Inputs.size())
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					"Selected packages could not be inspected completely.");
			FAssetCanonicalResaveSelection Selection{
				.Packages = Request.AssetPaths, .bAllowPlainResave = true};
			auto Plan = PlanAssetCanonicalResaves(Audit.Records, Selection);
			auto Applied = ApplyAssetCanonicalResaves(std::move(Plan), Catalog);
			if (Applied.Status != EAssetCanonicalResaveApplyStatus::Succeeded)
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					Applied.Diagnostic.empty()
						? SerializeAssetCanonicalResaveApplyReport(Applied)
						: Applied.Diagnostic);
			FAssetOperationResult Result{
				.Kind = EAssetOperationKind::Save,
				.Persistence = EAssetOperationPersistenceState::Persisted,
				.AffectedAssets = Request.AssetPaths};
			Publish(Request.Publish, Result);
			return Result;
		}

		for (const FPackagePath& Path : Request.AssetPaths)
		{
			DPackage* Package = FindResidentPackage(Path);
			if (!Package || !Package->IsDirty())
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					"Save Package is available only for a loaded package with authored changes.");
		}
		std::vector<DPackage*> Packages;
		Packages.reserve(Request.AssetPaths.size());
		for (const FPackagePath& Path : Request.AssetPaths)
			Packages.push_back(FindResidentPackage(Path));
		const FAssetResult Saved =
			SavePackagesAtomically(Packages);
		FAssetOperationResult Result = AssetToolsPrivate::FromEngineResult(
			EAssetOperationKind::Save, Saved, Request.AssetPaths);
		Result.Persistence = Result
			? EAssetOperationPersistenceState::Persisted
			: EAssetOperationPersistenceState::Dirty;
		Publish(Request.Publish, Result);
		return Result;
	}

	auto RelocateAssetsWithEditorPolicy(const FAssetRelocationRequest& Request)
		-> FAssetOperationResult
	{
		if (Request.Mappings.empty()) return {.Kind = EAssetOperationKind::Relocate};
		std::vector<FAssetRelocationMapping> Mappings;
		std::vector<FPackagePath> Affected;
		for (const FAssetRelocation& Mapping : Request.Mappings)
		{
			Mappings.push_back({Mapping.SourcePath, Mapping.DestinationPath});
			Affected.push_back(Mapping.SourcePath);
			Affected.push_back(Mapping.DestinationPath);
		}
		FAssetRelocationSummary Summary;
		FAssetMutationJob Job;
		const FAssetResult Prepared = PrepareAssetRelocationJob(
			Mappings, Summary, Job);
		if (!Prepared)
			return AssetToolsPrivate::FromEngineResult(EAssetOperationKind::Relocate, Prepared, Affected);
		return CommitMutation(
			std::move(Job), EAssetOperationKind::Relocate, Affected);
	}

	auto FixUpRedirectorsWithEditorPolicy(
		const FAssetRedirectorFixupRequest& Request) -> FAssetOperationResult
	{
		if (Request.Redirectors.empty())
			return {.Kind = EAssetOperationKind::FixUpRedirectors};
		FAssetRedirectorFixupSummary Summary;
		FAssetMutationJob Job;
		const FAssetResult Prepared = PrepareRedirectorFixupJob(
			Request.Redirectors,
			Request.bDeleteRedirectors
				? EAssetRedirectorFixupMode::RewriteAndDelete
				: EAssetRedirectorFixupMode::RewriteOnly,
			Summary, Job);
		if (!Prepared)
			return AssetToolsPrivate::FromEngineResult(
				EAssetOperationKind::FixUpRedirectors, Prepared, Request.Redirectors);
		return CommitMutation(
			std::move(Job), EAssetOperationKind::FixUpRedirectors,
			Request.Redirectors);
	}

}
