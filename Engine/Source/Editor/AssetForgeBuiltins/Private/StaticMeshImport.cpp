#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshImportSettings.h"
#include "AssetForge/Builtins/SceneImportData.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"

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
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"

namespace Durin::AssetForge::Builtins
{
	auto ReconcileStaticMeshMaterialSlots(
		std::span<const FMeshMaterialSlotDefinition> PreviousMaterialSlots,
		std::span<const FMeshPolygonGroup> ImportedSlots) -> std::vector<FMeshMaterialSlotDefinition>
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
		for (const FMeshPolygonGroup& Slot : ImportedSlots) ++NewNameCounts[Slot.SourceName];
		for (const FMeshMaterialSlotDefinition& Slot : PreviousSlots) ++OldSourceIndexCounts[Slot.SourceMaterialIndex];
		for (const FMeshPolygonGroup& Slot : ImportedSlots) ++NewSourceIndexCounts[Slot.SourceMaterialIndex];

		auto PreserveSlot = [&](size_t ImportedIndex, size_t OldIndex) {
			const FMeshPolygonGroup& Imported = ImportedSlots[ImportedIndex];
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
			const FMeshPolygonGroup& Imported = ImportedSlots[NewIndex];
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

		auto MakeUniqueSlotName = [&](const FMeshPolygonGroup& Imported) {
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
			const FMeshPolygonGroup& Imported = ImportedSlots[NewIndex];
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
		for (const FMeshPolygonGroup& Imported : ImportedSlots)
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

		auto ImportFromFilename(DStaticMesh& Mesh, std::string Filename, ESourceHintBase HintBase,
			const FStaticMeshImportSettings& Settings,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}, uint32 LODIndex = 0) -> std::expected<void, std::string>
		{
			if (const auto Valid = Settings.Validate(); !Valid)
				return std::unexpected(FormatStaticMeshImportSettingsError(Valid.error()));
			std::filesystem::path PackagePath;
			if (auto* Package = Mesh.GetPackage())
			{
				auto Resolved = FMountPaths::ResolveAssetPath(Package->GetPackagePath(), EMountPathExistence::AllowMissing);
				if (!Resolved) return std::unexpected(std::string("Cannot resolve StaticMesh package mount."));
				PackagePath = Resolved->PhysicalPath; PackagePath += ".dasset";
			}
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath) PhysicalPath = *SelectedPhysicalPath;
			else
			{
				if (PackagePath.empty()) return std::unexpected(std::string("StaticMesh has no owning package."));
				auto Resolved = ResolveSourceHint(HintBase, Filename, PackagePath.generic_string());
				if (!Resolved) return std::unexpected(FormatSourceHintError(Resolved.error()));
				PhysicalPath = *Resolved;
			}
			std::error_code Error;
			if (!std::filesystem::is_regular_file(PhysicalPath, Error)
				|| !IsSupportedExtension(PhysicalPath.extension().generic_string()))
				return std::unexpected("StaticMesh source is missing or unsupported: " + PhysicalPath.generic_string()
					+ (Error ? ": " + Error.message() : std::string{}));
			if (SelectedPhysicalPath)
			{
				if (PackagePath.empty()) { HintBase = ESourceHintBase::Absolute; Filename = PhysicalPath.generic_string(); }
				else
				{
					auto Hint = MakeSourceHint(PhysicalPath.generic_string(), PackagePath.generic_string());
					if (!Hint) return std::unexpected(FormatSourceHintError(Hint.error()));
					HintBase = Hint->Base; Filename = std::move(Hint->Hint);
				}
			}
			auto Captured = CaptureEncodedSource(Filename, PhysicalPath, MaximumStaticMeshEncodedBytes);
			if (!Captured) return std::unexpected(FormatEncodedSourceError(Captured.error()));
			auto Snapshot = std::move(*Captured);
			FImportedSceneData Scene;
			if (!ImportGeometryFromMemory(Snapshot.GetBytes(), PhysicalPath.extension().generic_string(), Scene, MakeOptions(Settings, {})))
			{
				std::string Diagnostic = "Failed to decode StaticMesh source: " + PhysicalPath.generic_string();
				for (const auto& Entry : Scene.Diagnostics) Diagnostic += "\n" + Entry.Message;
				return std::unexpected(std::move(Diagnostic));
			}
			auto Geometry = MakeMeshDescription(Scene);
			auto State = MakeImportDataState(Filename, HintBase, PhysicalPath, Snapshot, Settings);
			std::vector<FMeshDescription> LODs;
			const auto* Previous = dynamic_cast<const DStaticMeshImportData*>(Mesh.GetAssetImportData());
			const auto Count = Mesh.GetSource().IsValid() ? Mesh.GetSource().GetNumLODs()
				: Previous ? std::max(Mesh.GetSource().GetNumLODs(), static_cast<uint32>(Previous->GetSourceData().Sources.size())) : 0;
			if (Count > MaximumStaticMeshSourceLODs) return std::unexpected("StaticMesh source LOD count is invalid.");
			bool bReloadAll = LODIndex == 0 && Previous && Count > 1;
			for (uint32 LOD = 1; bReloadAll && LOD < Count; ++LOD)
				bReloadAll = Previous->GetSourceData().FindByRole(FName(std::format("lod{}", LOD))) != nullptr;
			if (LODIndex == 0 && Count > 1 && !bReloadAll && !Mesh.GetSource().IsValid())
				return std::unexpected("Unrecorded authored LODs require readable canonical source geometry.");
			if (bReloadAll) LODs.resize(Count);
			else if (Mesh.GetSource().IsValid() && (LODIndex != 0 || Count > 1))
			{
				const auto Existing = Mesh.GetSource().AcquireLODs();
				if (!Existing) return std::unexpected(FormatStaticMeshSourceError(Existing.error()));
				for (const auto& LOD : *Existing) LODs.push_back(*LOD);
			}
			if (LODIndex > LODs.size() || LODIndex >= MaximumStaticMeshSourceLODs)
				return std::unexpected("LOD import requires contiguous levels within the eight-LOD limit.");
			if (Previous)
			{
				const auto Prior = Previous->GetStaticMeshState();
				if (LODIndex != 0)
				{
					auto File = State.SourceData.Sources.front();
					File.Role = FName(std::format("lod{}", LODIndex));
					State = Prior;
					std::erase_if(State.SourceData.Sources, [&](const auto& Entry) { return Entry.Role == File.Role; });
					State.SourceData.Sources.push_back(std::move(File));
				}
				else
				{
					for (const auto& Entry : Prior.SourceData.Sources)
						if (Entry.Role != FName("source")) State.SourceData.Sources.push_back(Entry);
					// Decode every recorded lower source before accepting any new authored state.
					for (uint32 LOD = 1; LOD < LODs.size(); ++LOD)
					{
						auto Found = std::ranges::find(State.SourceData.Sources, FName(std::format("lod{}", LOD)), &FSourceFile::Role);
						if (Found == State.SourceData.Sources.end()) continue;
						const auto Path = ResolveSourceHint(Found->HintBase, Found->Hint, PackagePath.generic_string());
						if (!Path) return std::unexpected(FormatSourceHintError(Path.error()));
						auto Input = CaptureEncodedSource(Found->Hint, *Path, MaximumStaticMeshEncodedBytes);
						if (!Input) return std::unexpected(FormatEncodedSourceError(Input.error()));
						FImportedSceneData LowerScene;
						if (!ImportGeometryFromMemory(Input->GetBytes(), Path->extension().generic_string(), LowerScene, MakeOptions(Settings, {})))
							return std::unexpected("Failed to decode StaticMesh LOD source: " + Path->generic_string());
						LODs[LOD] = MakeMeshDescription(LowerScene);
						Found->ContentHashLow = Input->ContentHash.HashLow;
						Found->ContentHashHigh = Input->ContentHash.HashHigh;
						Found->ByteCount = Input->FileSize;
					}
				}
			}
			if (LODIndex == LODs.size()) LODs.push_back(std::move(Geometry));
			else LODs[LODIndex] = std::move(Geometry);
			// Separate files have independent material indices. Reconcile them into the
			// primary namespace by source name before producing immutable source data.
			auto& Groups = LODs.front().PolygonGroups;
			for (size_t LOD = 1; LOD < LODs.size(); ++LOD)
			{
				std::unordered_map<uint32, uint32> Remap;
				for (auto& Group : LODs[LOD].PolygonGroups)
				{
					auto Match = std::ranges::find(Groups, Group.SourceName, &FMeshPolygonGroup::SourceName);
					uint32 Index = 0;
					if (Match != Groups.end()) Index = Match->SourceMaterialIndex;
					else
					{
						while (std::ranges::find(Groups, Index, &FMeshPolygonGroup::SourceMaterialIndex) != Groups.end()) ++Index;
						auto Added = Group; Added.SourceMaterialIndex = Index; Groups.push_back(std::move(Added));
					}
					Remap.emplace(Group.SourceMaterialIndex, Index);
					Group.SourceMaterialIndex = Index;
				}
				for (auto& Section : LODs[LOD].Sections) Section.SourceMaterialIndex = Remap.at(Section.SourceMaterialIndex);
			}
			auto ImportedSlots = Groups;
			FStaticMeshSource Source;
			if (const auto Initialized = Source.InitializeLODs(std::move(LODs)); !Initialized)
				return std::unexpected(FormatStaticMeshSourceError(Initialized.error()));
			State.SourceData.Normalize();
			if (const auto Valid = State.Validate(); !Valid)
				return std::unexpected(FormatAssetImportDataError(Valid.error()));
			Mesh.BlockOnAnyAsyncBuild();
			auto Slots = ReconcileStaticMeshMaterialSlots(Mesh.GetMaterialSlots(), ImportedSlots);
			auto* Data = NewObject<DStaticMeshImportData>(&Mesh, FName("AssetImportData_" + FGuid::NewGuid().ToString()));
			if (!Data) return std::unexpected(std::string("Could not allocate StaticMesh import metadata."));
			Data->SetState(State);
			if (auto Committed = Mesh.SetImportedSource(std::move(Source), std::move(Slots), *Data); !Committed)
			{
				MarkAsGarbage(Data);
				return Committed;
			}
			Mesh.PostEditChange();
			return {};
		}

		auto MakeReimportResult(std::expected<void, std::string> Result) -> FReimportResult
		{
			if (Result) return {EReimportResult::Succeeded, {}};
			auto Cause = std::make_shared<FStaticMeshFactoryError>(Result.error());
			return {EReimportResult::Failed, std::move(Result.error()), std::move(Cause)};
		}
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
		if (const auto Rebuilt = ImportFromFilename(
			*Mesh, Input.generic_string(), ESourceHintBase::AssetRelative, Settings, Input); !Rebuilt)
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
		if (!Cast<DStaticMesh>(&Object) || IsOneTimeModelImportedAsset(Object)) return {};
		return {{"Reimport StaticMesh From File", "Supported Geometry", "*.obj;*.dae;*.3ds;*.ply;*.stl"}};
	}

	auto DStaticMeshFactory::GetReimportCapabilities(
		const DObject& Object) const -> FReimportCapabilities
	{
		if (IsOneTimeModelImportedAsset(Object)) return {.Diagnostic = std::string(ModelOneTimeImportDiagnostic)};
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

	auto DStaticMeshFactory::Reimport(DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Mesh = Cast<DStaticMesh>(&Object);
		auto Result = Mesh ? ReimportStaticMesh(*Mesh)
			: FReimportResult{EReimportResult::Failed, "Reimport requires a StaticMesh object."};
		if (Completion) Completion(std::move(Result));
	}

	auto DStaticMeshFactory::ReimportFromFiles(DObject& Object, std::span<const std::string> Filenames,
		FReimportCompletion Completion) const -> void
	{
		auto* Mesh = Cast<DStaticMesh>(&Object);
		if (!Mesh || Filenames.size() != 1)
		{
			if (Completion) Completion({EReimportResult::Failed, "StaticMesh reimport requires one source file."});
			return;
		}
		auto Result = ReimportStaticMeshFromFile(*Mesh, Filenames.front());
		if (Completion) Completion(std::move(Result));
	}

	auto ReimportStaticMesh(DStaticMesh& Mesh) -> FReimportResult
	{
		if (IsOneTimeModelImportedAsset(Mesh)) return MakeReimportResult(std::unexpected(std::string(ModelOneTimeImportDiagnostic)));
		const auto* Data = dynamic_cast<const DStaticMeshImportData*>(Mesh.GetAssetImportData());
		if (!Data) return MakeReimportResult(std::unexpected(std::string("StaticMesh has no current family import data.")));
		const auto State = Data->GetStaticMeshState();
		const auto* Source = State.SourceData.FindByRole("source");
		if (!Source || Source->Hint.empty()) return MakeReimportResult(std::unexpected(std::string("StaticMesh has no source filename to reimport.")));
		return MakeReimportResult(ImportFromFilename(Mesh, Source->Hint, Source->HintBase, State.ImportSettings));
	}

	auto ImportStaticMeshLODFromFile(DStaticMesh& Mesh, uint32 LODIndex, std::string_view FilePath) -> FReimportResult
	{
		const auto* Data = dynamic_cast<const DStaticMeshImportData*>(Mesh.GetAssetImportData());
		if (!Data || LODIndex == 0 || LODIndex >= MaximumStaticMeshSourceLODs)
			return MakeReimportResult(std::unexpected(std::string("LOD import requires an authored StaticMesh and a level from 1 to 7.")));
		if (IsOneTimeModelImportedAsset(Mesh) || IsOneTimeModelSource(FilePath))
			return MakeReimportResult(std::unexpected(std::string(ModelOneTimeImportDiagnostic)));
		std::error_code Error;
		const auto Path = std::filesystem::absolute(FilePath, Error).lexically_normal();
		if (Error) return MakeReimportResult(std::unexpected(Error.message()));
		return MakeReimportResult(ImportFromFilename(Mesh, Path.generic_string(), ESourceHintBase::Absolute,
			Data->GetImportSettings(), Path, LODIndex));
	}

	auto ReimportStaticMeshFromFile(DStaticMesh& Mesh, std::string_view FilePath) -> FReimportResult
	{
		if (IsOneTimeModelImportedAsset(Mesh) || IsOneTimeModelSource(FilePath))
			return MakeReimportResult(std::unexpected(std::string(ModelOneTimeImportDiagnostic)));
		const auto* Data = dynamic_cast<const DStaticMeshImportData*>(Mesh.GetAssetImportData());
		if (!Data) return MakeReimportResult(std::unexpected(std::string("StaticMesh has no current family import data.")));
		std::error_code Error;
		const auto Path = std::filesystem::absolute(FilePath, Error).lexically_normal();
		if (Error) return MakeReimportResult(std::unexpected("Cannot resolve StaticMesh source path: " + Error.message()));
		return MakeReimportResult(ImportFromFilename(Mesh, {}, ESourceHintBase::Absolute,
			Data->GetStaticMeshState().ImportSettings, Path));
	}

	auto CreateTransientStaticMeshFromFile(std::string_view FilePath, DObject* Outer,
		std::string_view ObjectName, const FStaticMeshImportSettings& Settings) -> std::expected<DStaticMesh*, std::string>
	{
		std::error_code Error;
		const auto Path = std::filesystem::absolute(FilePath, Error).lexically_normal();
		if (Error) return std::unexpected("Cannot resolve StaticMesh source path: " + Error.message());
		auto* Mesh = NewObject<DStaticMesh>(Outer, ObjectName);
		if (!Mesh) return std::unexpected(std::string("Cannot create StaticMesh object."));
		if (auto Imported = ImportFromFilename(*Mesh, {}, ESourceHintBase::Absolute, Settings, Path); !Imported)
		{
			MarkAsGarbage(Mesh);
			return std::unexpected(std::move(Imported.error()));
		}
		return Mesh;
	}

}
