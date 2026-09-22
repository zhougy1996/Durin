#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"

#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Asset/Asset.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/StringHelper.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshCompilation.h"

namespace Durin::AssetForge::Builtins
{
	auto ReconcileStaticMeshMaterialSlots(
		std::span<const FMeshMaterialSlotDefinition> PreviousMaterialSlots,
		std::span<const FStaticMeshImportedMaterialSlot> ImportedSlots) -> std::vector<FMeshMaterialSlotDefinition>
	{
		const std::vector<FMeshMaterialSlotDefinition> PreviousSlots(
			PreviousMaterialSlots.begin(), PreviousMaterialSlots.end());
		std::vector<FMeshMaterialSlotDefinition> ReconciledSlots = PreviousSlots;
		std::vector<bool> OldConsumed(PreviousSlots.size(), false);
		std::vector<bool> NewMatched(ImportedSlots.size(), false);
		std::unordered_map<std::string, uint32> OldNameCounts;
		std::unordered_map<std::string, uint32> NewNameCounts;
		std::unordered_map<uint32, uint32> OldSourceIndexCounts;
		std::unordered_map<uint32, uint32> NewSourceIndexCounts;
		for (const FMeshMaterialSlotDefinition& Slot : PreviousSlots) ++OldNameCounts[Slot.SourceName];
		for (const FStaticMeshImportedMaterialSlot& Slot : ImportedSlots) ++NewNameCounts[Slot.SourceName];
		for (const FMeshMaterialSlotDefinition& Slot : PreviousSlots) ++OldSourceIndexCounts[Slot.SourceMaterialIndex];
		for (const FStaticMeshImportedMaterialSlot& Slot : ImportedSlots) ++NewSourceIndexCounts[Slot.SourceMaterialIndex];

		auto PreserveSlot = [&](size_t ImportedIndex, size_t OldIndex) {
			const FStaticMeshImportedMaterialSlot& Imported = ImportedSlots[ImportedIndex];
			ReconciledSlots[OldIndex].SourceName = Imported.SourceName;
			ReconciledSlots[OldIndex].SourceMaterialIndex = Imported.SourceMaterialIndex;
			OldConsumed[OldIndex] = true;
			NewMatched[ImportedIndex] = true;
		};

		for (size_t NewIndex = 0; NewIndex < ImportedSlots.size(); ++NewIndex)
		{
			const std::string& SourceName = ImportedSlots[NewIndex].SourceName;
			if (SourceName.empty()) continue;
			if (OldNameCounts[SourceName] != 1 || NewNameCounts[SourceName] != 1) continue;
			const auto It = std::ranges::find_if(PreviousSlots, [&](const auto& Slot) {
				return Slot.SourceName == SourceName;
			});
			if (It != PreviousSlots.end()) PreserveSlot(NewIndex, static_cast<size_t>(It - PreviousSlots.begin()));
		}

		for (size_t NewIndex = 0; NewIndex < ImportedSlots.size(); ++NewIndex)
		{
			if (NewMatched[NewIndex]) continue;
			const FStaticMeshImportedMaterialSlot& Imported = ImportedSlots[NewIndex];
			if (OldSourceIndexCounts[Imported.SourceMaterialIndex] != 1
				|| NewSourceIndexCounts[Imported.SourceMaterialIndex] != 1) continue;
			for (size_t OldIndex = 0; OldIndex < PreviousSlots.size(); ++OldIndex)
			{
				const FMeshMaterialSlotDefinition& Previous = PreviousSlots[OldIndex];
				if (OldConsumed[OldIndex]
					|| Previous.SourceMaterialIndex != Imported.SourceMaterialIndex) continue;
				PreserveSlot(NewIndex, OldIndex);
				break;
			}
		}

		auto MakeUniqueSlotName = [&](const FStaticMeshImportedMaterialSlot& Imported) {
			std::string BaseName = Imported.Name.empty() ? Imported.SourceName : Imported.Name;
			if (BaseName.empty() || FName(BaseName).IsNone()) BaseName = "Material";
			FName Candidate(BaseName);
			uint32 Suffix = 1;
			while (std::ranges::find_if(ReconciledSlots, [&](const auto& Slot) {
				return Slot.Name == Candidate;
			})
				!= ReconciledSlots.end())
			{
				Candidate = FName(std::format("{}_{}", BaseName, Suffix++));
			}
			return Candidate;
		};

		for (size_t NewIndex = 0; NewIndex < ImportedSlots.size(); ++NewIndex)
		{
			if (NewMatched[NewIndex]) continue;
			const FStaticMeshImportedMaterialSlot& Imported = ImportedSlots[NewIndex];
			FMeshMaterialSlotDefinition& Definition = ReconciledSlots.emplace_back();
			Definition.Name = MakeUniqueSlotName(Imported);
			Definition.SourceName = Imported.SourceName;
			Definition.SourceMaterialIndex = Imported.SourceMaterialIndex;
			if (NewNameCounts[Imported.SourceName] > 1)
			{
				DURIN_WARN("Static mesh has ambiguous duplicate source material name '{}'; appended a stable slot.",
					Imported.SourceName);
			}
		}

		// Preserved editor slots that no longer map to an imported material must
		// not alias a current source index. Scene publication validates lookups by
		// source index, and an old unmatched slot can otherwise become ambiguous
		// after a reorder followed by removal.
		std::unordered_set<uint32> AssignedSourceIndices;
		for (const FStaticMeshImportedMaterialSlot& Imported : ImportedSlots)
			AssignedSourceIndices.insert(Imported.SourceMaterialIndex);
		uint32 RetiredSourceIndex = 0;
		for (size_t OldIndex = 0; OldIndex < PreviousSlots.size(); ++OldIndex)
		{
			if (OldConsumed[OldIndex]) continue;
			while (AssignedSourceIndices.contains(RetiredSourceIndex))
				++RetiredSourceIndex;
			ReconciledSlots[OldIndex].SourceMaterialIndex = RetiredSourceIndex;
			AssignedSourceIndices.insert(RetiredSourceIndex++);
		}

		return ReconciledSlots;
	}

	namespace
	{
		constexpr uint64 MaximumStaticMeshEncodedBytes = 512ull * 1024ull * 1024ull;

		auto IsSupportedExtension(std::string_view Extension) -> bool
		{
			const std::string Lower = StringUtils::FoldAscii(Extension);
			return Lower == ".obj" || Lower == ".fbx" || Lower == ".gltf"
				|| Lower == ".glb" || Lower == ".dae" || Lower == ".3ds"
				|| Lower == ".ply" || Lower == ".stl";
		}

		auto AxisVector(EStaticMeshImportAxis Axis) -> FVector3f
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: return {1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeX: return {-1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveY: return {0.0f, 1.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeY: return {0.0f, -1.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveZ: return {0.0f, 0.0f, 1.0f};
			case EStaticMeshImportAxis::NegativeZ: return {0.0f, 0.0f, -1.0f};
			}
			return {};
		}

		auto MakeOptions(const FStaticMeshImportSettings& Settings,
			std::string SourceFilename) -> FMeshImportOptions
		{
			const FVector3f Forward = AxisVector(Settings.ForwardAxis);
			const FVector3f Right = AxisVector(Settings.RightAxis);
			const FVector3f Up = AxisVector(Settings.UpAxis);
			FMeshImportOptions Options;
			for (uint32 Component = 0; Component < 3; ++Component)
			{
				Options.SourceToEngine[Component][0] = Forward[Component];
				Options.SourceToEngine[Component][1] = Right[Component];
				Options.SourceToEngine[Component][2] = Up[Component];
			}
			Options.RootSourcePath = std::move(SourceFilename);
			return Options;
		}

		auto MakeImportDataState(std::string Filename,
			ESourceHintBase HintBase,
			const std::filesystem::path& PhysicalPath,
			const FEncodedSourceSnapshot& Snapshot,
			const FStaticMeshImportSettings& Settings) -> FStaticMeshImportDataState
		{
			FStaticMeshImportDataState State;
			State.SourceData.Sources.push_back({
				.Role = "source",
				.DisplayLabel = PhysicalPath.filename().generic_string(),
				.Hint = std::move(Filename),
				.HintBase = HintBase,
				.ContentHashLow = Snapshot.ContentHash.HashLow,
				.ContentHashHigh = Snapshot.ContentHash.HashHigh,
				.ByteCount = Snapshot.FileSize});
			State.ImportSettings = Settings;
			return State;
		}

		auto RebuildFromFilename(DStaticMesh& Mesh, std::string Filename,
			ESourceHintBase HintBase,
			const FStaticMeshImportSettings& Settings,
			const FAssetBundleSaveOptions* SaveOptions,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {},
			FStaticMeshCompilationCompletion Completion = {}, bool bAsync = false) -> FStaticMeshRebuildResult
		{
			FStaticMeshRebuildError Error{.ObjectPath = Mesh.GetObjectPath(), .Filename = Filename};
			auto Reject = [&](EStaticMeshRebuildError Code) -> FStaticMeshRebuildResult {
				Error.Code = Code;
				return std::unexpected(std::move(Error));
			};
			if (const auto Validation = Settings.Validate(); !Validation)
			{ Error.SettingsCause = Validation.error(); return Reject(EStaticMeshRebuildError::Settings); }
			std::filesystem::path OwningPackagePath;
			bool bPackaged = false;
			if (const auto* Package = Mesh.GetPackage())
			{
				const auto Resolved = FMountPaths::ResolveAssetPath(Package->GetPackagePath(), EMountPathExistence::AllowMissing);
				if (Resolved) { OwningPackagePath = Resolved->PhysicalPath; OwningPackagePath += ".dasset"; bPackaged = true; }
				else if (!SelectedPhysicalPath) { Error.MountCause = Resolved.error().Code; return Reject(EStaticMeshRebuildError::Mount); }
			}
			if (!SelectedPhysicalPath && !bPackaged) return Reject(EStaticMeshRebuildError::Package);
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath) PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				if (auto Resolved = ResolveSourceHint(HintBase, Filename, OwningPackagePath.generic_string()); !Resolved)
				{ Error.SourceHintCause = Resolved.error(); return Reject(EStaticMeshRebuildError::SourceHint); }
				else { PhysicalPathText = Resolved->generic_string(); }
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath, Error.SystemError)
				|| !IsSupportedExtension(PhysicalPath.extension().generic_string()))
			{
				Error.Filename = PhysicalPath.generic_string();
				return Reject(EStaticMeshRebuildError::SourceFile);
			}
			if (SelectedPhysicalPath)
			{
				if (bPackaged)
				{
					if (auto Hint = MakeSourceHint(PhysicalPath.generic_string(), OwningPackagePath.generic_string()); !Hint)
					{ Error.SourceHintCause = Hint.error(); return Reject(EStaticMeshRebuildError::SourceHint); }
					else { HintBase = Hint->Base; Filename = std::move(Hint->Hint); }
				}
				else
				{
					HintBase = ESourceHintBase::Absolute;
					Filename = PhysicalPath.generic_string();
				}
			}
			auto Captured = CaptureEncodedSource(Filename, PhysicalPath,
				MaximumStaticMeshEncodedBytes);
			if (!Captured)
			{ Error.CaptureCause = std::make_shared<FEncodedSourceError>(Captured.error()); return Reject(EStaticMeshRebuildError::Capture); }
			auto Snapshot = std::move(*Captured);
			FImportedSceneData Scene;
			if (!ImportGeometryFromMemory(Snapshot.GetBytes(),
				PhysicalPath.extension().generic_string(), Scene,
				MakeOptions(Settings, {})))
			{
				Error.Filename = Filename;
				Error.DecodeCauses = std::move(Scene.Diagnostics);
				return Reject(EStaticMeshRebuildError::Decode);
			}
			auto Geometry = MakeStaticMeshDecodedGeometry(Scene);
			auto MaterialSlots = ReconcileStaticMeshMaterialSlots(Mesh.GetMaterialSlots(), Geometry.MaterialSlots);
			FStaticMeshSource Source;
			if (const auto Initialized = Source.Initialize(std::move(Geometry)); !Initialized)
			{
				Error.SourceCause = Initialized.error();
				return Reject(EStaticMeshRebuildError::Source);
			}
			auto State = MakeImportDataState(Filename, HintBase, PhysicalPath, Snapshot, Settings);
			const auto Owner = FObjectKey(&Mesh);
			State.SourceData.Normalize();
			if (const auto Validation = State.Validate(); !Validation)
			{ Error.ImportCause = std::make_shared<FAssetImportDataError>(Validation.error()); return Reject(EStaticMeshRebuildError::ImportValidation); }
			const auto Save = SaveOptions ? std::optional<FAssetBundleSaveOptions>(*SaveOptions) : std::nullopt;
			auto Result = std::make_shared<FStaticMeshCompilationResult>();
			if (const auto Submitted = Mesh.AsyncBuild({
				.Source = Source, .PreparedMaterialSlots = std::move(MaterialSlots), .Priority = EStaticMeshCompilationPriority::Interactive,
				.PreparePublication = [State](DStaticMesh& Target, DAssetImportData*& PreparedImportData) -> std::expected<void, FStaticMeshBuildFailure> {
					// The new inner is private until the mesh application boundary. Existing provenance is untouched on failure.
					auto* Data = NewObject<DStaticMeshImportData>(&Target, FName("AssetImportData_" + FGuid::NewGuid().ToString()));
					if (!Data) return std::unexpected(FStaticMeshBuildFailure{"Could not allocate DStaticMeshImportData.", EStaticMeshBuildStage::Application});
					Data->SetState(State);
					PreparedImportData = Data;
					return {};
				}}, [Owner, Save, Result, Completion = std::move(Completion)](const FStaticMeshCompilationResult& Value) {
				auto Final = Value;
				*Result = Value;
				if (Value.Status == EStaticMeshCompilationStatus::Succeeded && Save)
				{
					auto* Mesh = Cast<DStaticMesh>(ResolveObjectKey(Owner));
					DPackage* Package = Mesh ? Mesh->GetPackage() : nullptr;
					if (!Package)
					{
						Result->Status = EStaticMeshCompilationStatus::Failed;
						Result->Errors.push_back("StaticMesh package is unavailable for save.");
					}
					else if (const auto Saved = SavePackages(std::span<DPackage* const>(&Package, 1), *Save).Result; !Saved)
					{
						Result->Status = EStaticMeshCompilationStatus::Failed;
						Result->Errors.push_back(Saved.Message.substr(0, MaximumStaticMeshBuildDiagnosticBytes));
					}
				}
				if (Result->Status != Value.Status)
				{
					Final.Status = Result->Status;
					Final.Errors = Result->Errors;
				}
				if (Completion) Completion(Final);
			}); !Submitted)
			{
				Error.SubmissionErrors = Submitted.error();
				return Reject(EStaticMeshRebuildError::Submission);
			}
			if (bAsync) return {};
			FAssetCompilingManager::Get().FinishCompilationForObject(Mesh);
			if (Result->Status == EStaticMeshCompilationStatus::Succeeded) return {};
			Error.CompletionCause = *Result;
			return Reject(EStaticMeshRebuildError::Completion);
		}
	}

	auto FormatStaticMeshRebuildError(const FStaticMeshRebuildError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshRebuildError::None: return {};
		case EStaticMeshRebuildError::ObjectType: return "Reimport requires a StaticMesh object.";
		case EStaticMeshRebuildError::SourceCount: return std::format("StaticMesh reimport requires one source file; received {}.", Error.SourceCount);
		case EStaticMeshRebuildError::Path: return "Cannot resolve StaticMesh source path: " + Error.Filename + ": " + Error.SystemError.message();
		case EStaticMeshRebuildError::ObjectCreation: return "Cannot create StaticMesh object: " + Error.ObjectName;
		case EStaticMeshRebuildError::Settings: return Error.SettingsCause ? FormatStaticMeshImportSettingsError(*Error.SettingsCause) : "Invalid StaticMesh settings.";
		case EStaticMeshRebuildError::Package: return "StaticMesh has no owning package.";
		case EStaticMeshRebuildError::Mount: return "Cannot resolve StaticMesh package mount.";
		case EStaticMeshRebuildError::SourceHint: return Error.SourceHintCause ? FormatSourceHintError(*Error.SourceHintCause) : "Invalid StaticMesh source hint.";
		case EStaticMeshRebuildError::SourceFile: return "StaticMesh source is missing or uses an unsupported format: " + Error.Filename;
		case EStaticMeshRebuildError::Capture: return Error.CaptureCause ? FormatEncodedSourceError(*Error.CaptureCause) : "Cannot capture StaticMesh source.";
		case EStaticMeshRebuildError::Decode: return "Failed to decode StaticMesh source " + Error.Filename;
		case EStaticMeshRebuildError::Source: return Error.SourceCause ? FormatStaticMeshSourceError(*Error.SourceCause) : "Invalid StaticMesh source.";
		case EStaticMeshRebuildError::ImportValidation: return Error.ImportCause ? FormatAssetImportDataError(*Error.ImportCause) : "Invalid StaticMesh import data.";
		case EStaticMeshRebuildError::Submission: return !Error.SubmissionErrors.empty() ? FormatStaticMeshBuildMessages(Error.SubmissionErrors) : "StaticMesh compilation submission failed.";
		case EStaticMeshRebuildError::Completion: return Error.CompletionCause ? Error.CompletionCause->ToString() : "StaticMesh compilation failed.";
		case EStaticMeshRebuildError::ImportData: return "StaticMesh has no current family import data.";
		case EStaticMeshRebuildError::MissingSource: return "StaticMesh has no source filename to reimport.";
		}
		return {};
	}

	auto FStaticMeshFactoryError::Format() const -> std::string
	{
		if (const auto* Rebuild = std::get_if<FStaticMeshRebuildError>(&Cause)) return FormatStaticMeshRebuildError(*Rebuild);
		return std::get<FStaticMeshCompilationResult>(Cause).ToString();
	}

	DStaticMeshFactory::DStaticMeshFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DStaticMesh::StaticClass();
		Formats = {"obj", "fbx", "gltf", "glb", "dae", "3ds", "ply", "stl"};
	}

	auto DStaticMeshFactory::FactoryCreateFromFile(
		DClass* InClass,
		DObject* InParent,
		FName InName,
		EObjectFlags Flags,
		std::string_view Filename,
		DObject*,
		FFactoryDiagnostics* Diagnostics) const -> DObject*
	{
		auto Reject = [&](EFactoryError Code) -> DObject* {
			if (Diagnostics) Diagnostics->ReportFailure({
				.Code = Code,
				.ExpectedClass = DStaticMesh::StaticClass()->GetName(),
				.RequestedClass = InClass ? InClass->GetName() : std::string{},
				.Filename = std::string(Filename)});
			return nullptr;
		};
		if (InClass != DStaticMesh::StaticClass())
			return Reject(EFactoryError::ExactClass);
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Reject(EFactoryError::AssetPackageParent);
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Reject(EFactoryError::SourceMissing);
		if (!IsSupportedExtension(Input.extension().generic_string()))
			return Reject(EFactoryError::SourceFormat);
		if (const auto Validation = Settings.Validate(); !Validation)
		{
			if (Diagnostics) Diagnostics->ReportFailure({
				.Code = EFactoryError::StaticMeshSettings, .Filename = std::string(Filename),
				.StaticMeshSettingsCause = std::make_shared<FStaticMeshImportSettingsError>(Validation.error())});
			return nullptr;
		}
		auto* Mesh = NewObject<DStaticMesh>(InClass, Package, InName, Flags);
		if (!Mesh) return Reject(EFactoryError::ObjectCreation);
		if (const auto Rebuilt = RebuildFromFilename(
			*Mesh, Input.generic_string(), ESourceHintBase::AssetRelative,
			Settings, nullptr, Input, AsyncImportCompletion, bAsyncImport); !Rebuilt)
		{
			if (Diagnostics) Diagnostics->ReportDomainFailure(std::make_shared<FStaticMeshFactoryError>(Rebuilt.error()));
			return nullptr;
		}
		return Mesh;
	}

	auto DStaticMeshFactory::QueryReimportActions(std::string_view AssetClassName) const
		-> FReimportActions
	{
		if (AssetClassName != DStaticMesh::StaticClass()->GetQualifiedName().ToString())
			return {};
		return {.bSupportsReimport = true, .bSupportsReimportFromFile = true};
	}

	auto DStaticMeshFactory::GetSourceFileDialogs(const DObject& Object) const
		-> std::vector<FReimportSourceFileDialog>
	{
		if (!Cast<DStaticMesh>(&Object)) return {};
		return {{"Reimport StaticMesh From File", "Supported Geometry", "*.fbx;*.gltf;*.glb;*.obj;*.dae;*.3ds;*.ply;*.stl"}};
	}

	auto DStaticMeshFactory::GetReimportCapabilities(
		const DObject& Object) const -> FReimportCapabilities
	{
		const auto* Mesh = Cast<DStaticMesh>(&Object);
		const auto* Data = Mesh ? dynamic_cast<const DStaticMeshImportData*>(
			Mesh->GetAssetImportData()) : nullptr;
		if (!Mesh || !Mesh->GetPackage() || !Data)
			return {.Diagnostic = "StaticMesh has no current family import data."};
		const FStaticMeshImportDataState State = Data->GetStaticMeshState();
		const FSourceFile* Source = State.SourceData.FindByRole("source");
		const bool bHasSource = Source && !Source->Hint.empty();
		return {.bCanReimport = bHasSource, .bCanReimportFromFile = true,
			.Diagnostic = bHasSource ? std::string{}
				: "StaticMesh has no source hint to reimport."};
	}

	auto DStaticMeshFactory::Reimport(
		DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Mesh = Cast<DStaticMesh>(&Object);
		const auto* Data = Mesh ? dynamic_cast<const DStaticMeshImportData*>(
			Mesh->GetAssetImportData()) : nullptr;
		const FStaticMeshImportDataState State = Data
			? Data->GetStaticMeshState() : FStaticMeshImportDataState{};
		const FSourceFile* Source = Data ? State.SourceData.FindByRole("source") : nullptr;
		if (!Mesh || !Data || !Source || Source->Hint.empty())
		{
			const FStaticMeshRebuildError Error{
				.Code = !Mesh ? EStaticMeshRebuildError::ObjectType : !Data ? EStaticMeshRebuildError::ImportData : EStaticMeshRebuildError::MissingSource,
				.ObjectPath = Object.GetObjectPath()};
			if (Completion) Completion({EReimportStatus::MissingSource,
				FormatStaticMeshRebuildError(Error), std::make_shared<FStaticMeshFactoryError>(Error)});
			return;
		}
		const auto Submitted = RebuildFromFilename(*Mesh, Source->Hint,
			Source->HintBase, State.ImportSettings, nullptr, {},
			[Completion](const FStaticMeshCompilationResult& Result) {
				if (Completion) Completion(Result.Status == EStaticMeshCompilationStatus::Succeeded
					? FReimportResult{EReimportStatus::Succeeded, {}}
					: FReimportResult{EReimportStatus::SourceOrBuildFailure, Result.ToString(),
						std::make_shared<FStaticMeshFactoryError>(Result)});
			}, true);
		if (!Submitted && Completion) Completion({EReimportStatus::SourceOrBuildFailure, FormatStaticMeshRebuildError(Submitted.error()),
			std::make_shared<FStaticMeshFactoryError>(Submitted.error())});
	}

	auto DStaticMeshFactory::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, FReimportCompletion Completion) const
		-> void
	{
		auto* Mesh = Cast<DStaticMesh>(&Object);
		const auto* Data = Mesh ? dynamic_cast<const DStaticMeshImportData*>(
			Mesh->GetAssetImportData()) : nullptr;
		if (!Mesh || !Data || Filenames.size() != 1 || Filenames.front().empty())
		{
			const FStaticMeshRebuildError Error{
				.Code = !Mesh ? EStaticMeshRebuildError::ObjectType : !Data ? EStaticMeshRebuildError::ImportData
					: Filenames.size() != 1 ? EStaticMeshRebuildError::SourceCount : EStaticMeshRebuildError::MissingSource,
				.ObjectPath = Object.GetObjectPath(), .SourceCount = Filenames.size()};
			if (Completion) Completion({EReimportStatus::SourceOrBuildFailure,
				FormatStaticMeshRebuildError(Error), std::make_shared<FStaticMeshFactoryError>(Error)});
			return;
		}
		std::error_code SystemError;
		const std::filesystem::path Requested =
			std::filesystem::absolute(Filenames.front(), SystemError).lexically_normal();
		if (SystemError)
		{
			const FStaticMeshRebuildError Error{.Code = EStaticMeshRebuildError::Path,
				.ObjectPath = Object.GetObjectPath(), .Filename = Filenames.front(), .SystemError = SystemError};
			if (Completion) Completion({EReimportStatus::SourceOrBuildFailure,
				FormatStaticMeshRebuildError(Error), std::make_shared<FStaticMeshFactoryError>(Error)});
			return;
		}
		const auto Submitted = RebuildFromFilename(*Mesh, {}, ESourceHintBase::Absolute,
			Data->GetStaticMeshState().ImportSettings, nullptr, Requested,
			[Completion](const FStaticMeshCompilationResult& Result) {
				if (Completion) Completion(Result.Status == EStaticMeshCompilationStatus::Succeeded
					? FReimportResult{EReimportStatus::Succeeded, {}}
					: FReimportResult{EReimportStatus::SourceOrBuildFailure, Result.ToString(),
						std::make_shared<FStaticMeshFactoryError>(Result)});
			}, true);
		if (!Submitted && Completion) Completion({EReimportStatus::SourceOrBuildFailure, FormatStaticMeshRebuildError(Submitted.error()),
			std::make_shared<FStaticMeshFactoryError>(Submitted.error())});
	}

	auto ReimportStaticMesh(DStaticMesh& Mesh,
		const FAssetBundleSaveOptions& SaveOptions) -> FStaticMeshRebuildResult
	{
		const auto* Data = dynamic_cast<const DStaticMeshImportData*>(
			Mesh.GetAssetImportData());
		if (!Data)
		{
			return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::ImportData, .ObjectPath = Mesh.GetObjectPath()});
		}
		const FStaticMeshImportDataState State = Data->GetStaticMeshState();
		const FSourceFile* Source = State.SourceData.FindByRole("source");
		if (!Source)
		{
			return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::MissingSource, .ObjectPath = Mesh.GetObjectPath()});
		}
		return RebuildFromFilename(
			Mesh, Source->Hint, Source->HintBase,
			State.ImportSettings, &SaveOptions);
	}

	auto ReimportStaticMeshFromFile(DStaticMesh& Mesh, std::string_view FilePath,
		const FAssetBundleSaveOptions& SaveOptions) -> FStaticMeshRebuildResult
	{
		const auto* Data = dynamic_cast<const DStaticMeshImportData*>(
			Mesh.GetAssetImportData());
		if (!Data)
		{
			return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::ImportData, .ObjectPath = Mesh.GetObjectPath()});
		}
		std::error_code SystemError;
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath, SystemError).lexically_normal();
		if (SystemError) return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::Path,
			.ObjectPath = Mesh.GetObjectPath(), .Filename = std::string(FilePath), .SystemError = SystemError});
		if (!std::filesystem::is_regular_file(Requested, SystemError))
		{
			return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::SourceFile, .ObjectPath = Mesh.GetObjectPath(), .Filename = Requested.generic_string(), .SystemError = SystemError});
		}
		return RebuildFromFilename(Mesh, {}, ESourceHintBase::Absolute,
			Data->GetStaticMeshState().ImportSettings, &SaveOptions, Requested);
	}

	auto CreateTransientStaticMeshFromFile(std::string_view FilePath,
		DObject* Outer, std::string_view ObjectName,
		const FStaticMeshImportSettings& ImportSettings) -> std::expected<DStaticMesh*, FStaticMeshRebuildError>
	{
		std::error_code SystemError;
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath, SystemError).lexically_normal();
		if (SystemError) return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::Path,
			.Filename = std::string(FilePath), .ObjectName = std::string(ObjectName), .SystemError = SystemError});
		if (!std::filesystem::is_regular_file(Input, SystemError))
			return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::SourceFile, .Filename = Input.generic_string(),
				.ObjectName = std::string(ObjectName), .SystemError = SystemError});
		auto* Mesh = NewObject<DStaticMesh>(Outer, ObjectName);
		if (!Mesh) return std::unexpected(FStaticMeshRebuildError{.Code = EStaticMeshRebuildError::ObjectCreation,
			.Filename = Input.generic_string(), .ObjectName = std::string(ObjectName)});
		auto Rebuilt = RebuildFromFilename(*Mesh, {}, ESourceHintBase::Absolute, ImportSettings, nullptr, Input);
		if (!Rebuilt)
		{
			Rebuilt.error().Filename = Input.generic_string();
			Rebuilt.error().ObjectName = std::string(ObjectName);
			MarkAsGarbage(Mesh);
			return std::unexpected(std::move(Rebuilt.error()));
		}
		return Mesh;
	}

}
