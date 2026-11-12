#include "DObject/StrongObjectPtr.h"
#include "AssetTools/IAssetTools.h"
#include "Import/AssetDestinationValidation.h"

#include "Asset/PackageSerialization.h"
#include "AssetMaintenance/CanonicalResave.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "AssetTools/MutationTypes.h"
#include "AssetTools/RedirectorFixup.h"
#include "AssetTools/Relocation.h"
#include "DObject/Object.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"

namespace Durin
{
	namespace
	{
		auto Feedback(FFeedbackContext* Warn) -> FFeedbackContext& { return Warn ? *Warn : FFeedbackContext::Get(); }
		auto ReportMutation(const FAssetMutationResultDetails& Result, FFeedbackContext* Warn) -> bool
		{
			if (!Result.Result) Feedback(Warn).Error(Result.Result.Message);
			for (const auto& Path : Result.BackupLocations) Feedback(Warn).Warning("Recovery backup: " + Path.generic_string());
			if (!Result.Result)
				for (const auto& Path : Result.AffectedFiles) Feedback(Warn).Warning("Affected file: " + Path.generic_string());
			return static_cast<bool>(Result.Result);
		}
	}

	auto DuplicateAssetWithEditorPolicy(const FAssetDuplicateRequest& Request, FFeedbackContext* Warn)
		-> DObject*
	{
		const auto Reject = [Warn](std::string_view Message) -> DObject* {
			(Warn ? *Warn : FFeedbackContext::Get()).Error(Message);
			return nullptr;
		};
		if (!Request.SourcePath.IsValid() || Request.DestinationDirectory.empty())
			return Reject("Asset duplication requires a valid source and destination directory.");
		const FAssetCatalogEntry Source =
			FindAssetExact(Request.SourcePath);
		if (!Source || Source->IsRedirector())
			return Reject(
				"The copied source is no longer an available real asset.");

		std::string Directory = Request.DestinationDirectory;
		if (!Directory.ends_with('/')) Directory.push_back('/');
		const std::string AssetName(Request.SourcePath.GetAssetName());
		FTopLevelAssetPath DestinationAssetPath;
		FPackagePath DestinationPackagePath;
		if (Request.DestinationPath)
		{
			DestinationAssetPath = *Request.DestinationPath;
			if (!DestinationAssetPath.IsValid()) return Reject("The exact duplicate destination is invalid.");
			DestinationPackagePath = DestinationAssetPath.GetPackagePath();
			const auto Destination = InspectAssetDestination(DestinationPackagePath.GetView());
			std::error_code Error;
			if (!Destination || Destination.AssetExists() || std::filesystem::exists(Destination.PhysicalPath, Error) || Error)
				return Reject("The exact duplicate destination is occupied or unavailable.");
		}
		for (uint32 Suffix = 0; Suffix <= 10000; ++Suffix)
		{
			if (Request.DestinationPath) break;
			const std::string CandidateName = Suffix == 0 ? AssetName
				: Suffix == 1 ? AssetName + "_Copy"
				: std::format("{}_Copy{}", AssetName, Suffix);
			FPackagePath CandidatePath;
			if (!FPackagePath::TryCreate(Directory + CandidateName, CandidatePath))
				return Reject("The duplicate destination path is invalid.");
			const auto Destination = InspectAssetDestination(CandidatePath.GetView());
			if (Destination.AssetExists()) continue;
			if (!Destination) return Reject(FormatAssetDestinationValidation(Destination));
			std::string CandidatePhysical = Destination.PhysicalPath.generic_string();
			std::error_code Error;
			const bool bExists = std::filesystem::exists(CandidatePhysical, Error);
			if (Error)
				return Reject(
					std::format("Could not inspect the duplicate destination: {}", Error.message()));
			if (bExists) continue;
			if (!FTopLevelAssetPath::TryCreate(
				CandidatePath, CandidateName, DestinationAssetPath)) continue;
			DestinationPackagePath = std::move(CandidatePath);
			break;
		}
		if (!DestinationAssetPath.IsValid())
			return Reject(
				"Could not find an available copy name in this folder.");

		FSoftObjectPath SourceObjectPath;
		if (!FSoftObjectPath::TryCreate(
			Request.SourcePath, std::span<const std::string>{}, SourceObjectPath))
			return Reject(
				"The source top-level asset path is invalid.");
		DObject* SourceAsset = nullptr;
		auto LoadedSource = LoadObjectDetailed(SourceObjectPath, nullptr);
		SourceAsset = LoadedSource.value_or(nullptr);
		FAssetWriteResult EngineResult = AssetWriteResultFromRead(LoadedSource);
		if (!EngineResult)
			return Reject(EngineResult.Message);
		DPackage* SourcePackage = SourceAsset ? SourceAsset->GetPackage() : nullptr;
		if (!SourceAsset || !SourcePackage || SourceAsset->GetOuter() != SourcePackage
			|| SourcePackage->FindTopLevelAsset(SourceAsset->GetFName()) != SourceAsset)
			return Reject(
				"The source is not a registered top-level asset.");

		DPackage* DestinationPackage = CreatePackage(DestinationPackagePath);
		if (!DestinationPackage)
			return Reject(
				"The destination package could not be created.");
		const TStrongObjectPtr<DObject> SourceOwner(SourceAsset);
		const TStrongObjectPtr<DPackage> CreationOwner(DestinationPackage);
		DestinationPackage->SetFlags(EObjectFlags::Transactional);
		const auto Duplicated = DuplicateObject(
			SourceAsset, DestinationPackage,
			FName(DestinationAssetPath.GetAssetName()));
		DObject* DuplicatedAsset = Duplicated.value_or(nullptr);
		if (!DuplicatedAsset
			|| DestinationPackage->FindTopLevelAsset(
				DuplicatedAsset->GetFName()) != DuplicatedAsset)
		{
			MarkObjectHierarchyAsGarbage(DestinationPackage);
			RequestGarbageCollection();
			return Reject(
				!Duplicated ? ToString(Duplicated.error())
					: "The source object graph could not be duplicated as an asset.");
		}
		DuplicatedAsset->SetFlags(EObjectFlags::Standalone);
		DestinationPackage->MarkDirty();
		DestinationPackage->MarkAsNewlyCreated();
		return DuplicatedAsset;
	}

	auto SaveAssetsWithEditorPolicy(const FAssetSaveRequest& Request, FFeedbackContext* Warn) -> bool
	{
		const auto Reject = [Warn](std::string_view Message) { Feedback(Warn).Error(Message); return false; };
		if (Request.AssetPaths.empty()) return Reject("No asset packages were selected for saving.");
		if (Request.Mode == EAssetSaveMode::CanonicalResave)
		{
			const FAssetPackageDiscoverySnapshot Snapshot =
				CaptureMountedAssetPackageSnapshot();
			if (Snapshot.Status != EAssetPackageSnapshotStatus::Completed)
				return Reject(
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
					return Reject( std::format(
						"Package {} is not in a content-writable mounted snapshot.", Path.ToString()));
				Inputs.push_back(*Input);
				Inputs.back().bIncludeNestedMigrationEvidence = true;
			}
			auto Audit = RunAssetCompatibilityAudit(Inputs, Catalog);
			if (Audit.Status != EAssetCompatibilityAuditStatus::Completed
				|| Audit.Records.size() != Inputs.size())
				return Reject(
					"Selected packages could not be inspected completely.");
			FAssetCanonicalResaveSelection Selection{
				.Packages = Request.AssetPaths, .bAllowPlainResave = true};
			auto Plan = PlanAssetCanonicalResaves(Audit.Records, Selection);
			auto Applied = ApplyAssetCanonicalResaves(std::move(Plan), Catalog);
			if (Request.OnSaved)
				for (const auto& Package : Applied.Plan.Packages)
					if (Package.Status == EAssetCanonicalResavePackageStatus::Resaved) Request.OnSaved(Package.PackagePath);
			if (Applied.Status != EAssetCanonicalResaveApplyStatus::Succeeded)
				return Reject(
					Applied.Diagnostic.empty()
						? SerializeAssetCanonicalResaveApplyReport(Applied)
						: Applied.Diagnostic);
			return true;
		}
		std::unordered_set<FPackagePath> Visited;
		bool bSucceeded = true;
		for (const auto& Path : Request.AssetPaths)
		{
			if (!Visited.insert(Path).second) continue;
			auto* Package = FindResidentPackage(Path);
			const FAssetWriteResult Saved = Package && Package->IsDirty() ? SavePackage(Package)
				: FAssetWriteResult{EAssetWriteError::InvalidData, "Save requires a loaded package with authored changes."};
			if (Request.OnSaved && (Saved || Saved.Effect == EAssetWriteEffect::ContentCommittedProjectionPending))
				Request.OnSaved(Path);
			if (!Saved)
			{
				bSucceeded = false;
				Feedback(Warn).Error(std::format("{}: {}", Path.ToString(), Saved.Message));
			}
		}
		return bSucceeded;
	}

	auto RelocateAssetsWithEditorPolicy(const FAssetRelocationRequest& Request, FFeedbackContext* Warn) -> bool
	{
		if (Request.Mappings.empty()) return true;
		std::vector<FAssetRelocationMapping> Mappings;
		for (const auto& Mapping : Request.Mappings) Mappings.push_back({Mapping.SourcePath, Mapping.DestinationPath});
		return ReportMutation(RelocateAssets(Mappings), Warn);
	}
	auto FixUpRedirectorsWithEditorPolicy(const FAssetRedirectorFixupRequest& Request, FFeedbackContext* Warn) -> bool
	{
		if (Request.Redirectors.empty()) return true;
		return ReportMutation(FixUpRedirectors(Request.Redirectors, Request.bDeleteRedirectors
			? EAssetRedirectorFixupMode::RewriteAndDelete : EAssetRedirectorFixupMode::RewriteOnly), Warn);
	}
}
