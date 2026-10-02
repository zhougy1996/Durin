#include "GltfAssetImportDialog.h"

#include "Import/AssetDestinationValidation.h"
#include "Asset/Asset.h"
#include "Asset/AssetPicker.h"
#include "Materials/Material.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin::Editor
{
	FGltfAssetImportDialog::FGltfAssetImportDialog(FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks)) {}

	auto FGltfAssetImportDialog::Open(std::string_view InDestinationDirectory) -> void
	{
		if (bImporting) return;
		if (Session && Session->GetProgress().Phase != AssetForge::Builtins::ESceneImportPhase::Ready
			&& Session->GetProgress().Phase != AssetForge::Builtins::ESceneImportPhase::Completed) return;
		Session.reset();
		bImporting = bReportedCompletion = false;
		SourcePathBuffer.fill(0);
		Coordinates.Reset();
		Coordinates.SetPreset(FMeshCoordinateImportModel::EPreset::YUpNegativeZForward);
		MaterialOptions = {};
		ImportOptions = {};
		SourceMeshes.clear();
		SourceScenes.clear();
		MaterialPreview = {};
		bPreviewDirty = true;
		DestinationDirectory.Reset(InDestinationDirectory);
		ModalState.RequestOpen();
	}

	auto FGltfAssetImportDialog::Draw(bool bAllowAssetMutation) -> void
	{
		using AssetForge::Builtins::ESceneImportPhase;
		if (Session) Session->Tick();
		ModalState.OpenPopupIfRequested("Import glTF Assets");
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, std::min(MonaImGui::ScaleUI(780), ImGui::GetMainViewport()->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import glTF Assets", nullptr,
			ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;
		if (Session && Session->GetProgress().Phase == ESceneImportPhase::Completed && !bReportedCompletion)
		{
			bReportedCompletion = true;
			bImporting = false;
			const auto& Result = Session->GetResult();
			if (!Result.SavedPackages.empty()) Callbacks.NotifyImportedDirectory(DestinationDirectory.GetPath());
			if (!Result)
				SetError(Result.SavedPackages.empty() ? Result.Message
					: std::format("{} package(s) saved before import stopped. {}", Result.SavedPackages.size(), Result.Message));

			bImporting = false;
		}
		if (Session && Session->GetProgress().Phase == ESceneImportPhase::Completed)
		{
			const auto& Result = Session->GetResult();
			ImGui::SeparatorText(Result ? "Import completed" : "Import stopped");
			ImGui::TextWrapped("%s", Result.Message.c_str());
			for (const auto& Diagnostic : Result.Diagnostics) ImGui::TextWrapped("%s", Diagnostic.Message.c_str());
			constexpr const char* States[] = {"Unexecuted", "Saved", "Preserved", "Failed"};
			for (const auto& Output : Result.Outputs)
			{
				ImGui::TextWrapped("%s: %s", States[static_cast<size_t>(Output.State)], Output.AssetPath.ToString().c_str());
				if (Output.State == AssetForge::EImportOutputState::Saved || Output.State == AssetForge::EImportOutputState::Preserved)
				{
					ImGui::PushID(Output.StableIdentity.c_str());
					if (ImGui::SmallButton("Show asset")) Callbacks.NotifyAssetCreated(Output.AssetPath.ToString());
					ImGui::PopID();
				}
			}
			if (ImGui::Button("Close")) { Session.reset(); ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if (ImGui::Button("Prepare again")) { Session.reset(); bPreviewDirty = true; }
			ImGui::EndPopup();
			return;
		}
		if (Session && Session->GetProgress().Phase != ESceneImportPhase::Ready
			&& Session->GetProgress().Phase != ESceneImportPhase::Completed)
		{
			const auto& Progress = Session->GetProgress();
			constexpr const char* PhaseNames[] = {"Reading source", "Configure outputs", "Building assets",
				"Preparing assets", "Compiling materials", "Saving assets", "Finishing", "Completed"};
			ImGui::TextUnformatted(PhaseNames[static_cast<size_t>(Progress.Phase)]);
			ImGui::TextUnformatted(Progress.Activity.c_str());
			if (Progress.Total)
			{
				ImGui::ProgressBar(static_cast<float>(Progress.Completed) / static_cast<float>(Progress.Total));
				ImGui::Text("%zu / %zu", Progress.Completed, Progress.Total);
			}
			else ImGui::TextDisabled("Working%s", static_cast<int>(ImGui::GetTime() * 3) % 3 == 0 ? "." : static_cast<int>(ImGui::GetTime() * 3) % 3 == 1 ? ".." : "...");
			ImGui::BeginDisabled(Progress.bCancellationRequested);
			if (ImGui::Button("Cancel")) { Session->Cancel(); }
			ImGui::EndDisabled();
			if (Progress.bCancellationRequested) ImGui::TextDisabled("Canceling; finishing the current safe step...");
			ImGui::EndPopup();
			return;
		}

		ImGui::TextUnformatted("Import meshes, materials and textures from a glTF or GLB source.");
		ImGui::TextWrapped("One-time import. Reimport is unavailable; use a new destination for later imports.");
		ImGui::TextDisabled("Outputs are peer assets grouped by type inside one destination directory.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source model");
		ImGui::TextDisabled("The selected source and its relative dependencies remain in place.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
			- BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##SceneImportSource",
			"Choose a glTF or GLB source...", SourcePathBuffer.data(),
			SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		const std::string Extension = StringUtils::FoldAscii(
			SourcePath.extension().generic_string());
		const bool bSupportedSource = Extension == ".gltf" || Extension == ".glb";
		if (bHasSource) ImGui::TextDisabled("%s", SourcePath.filename().generic_string().c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		ImGui::BeginGroup();
		Coordinates.Draw();
		ImGui::EndGroup();
		if (ImGui::IsItemEdited()) { bPreviewDirty = true; Session.reset(); }
		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		const std::string PreviousDirectory(DestinationDirectory.GetPath());
		if (DestinationDirectory.DrawRow("Output directory", "##SceneImportDirectory",
			"/Project/Imported/ModelName", "Choose...", BrowseButtonWidth))
			BrowseDestinationDirectory();
		if (PreviousDirectory != DestinationDirectory.GetPath()) bPreviewDirty = true;
		const FContentDirectoryValidation DestinationValidation = DestinationDirectory.Inspect();
		const auto SettingsValidation = Coordinates.GetSettings().Validate();
		const bool bImportSettingsValid = SettingsValidation.has_value();
		const std::string ImportSettingsError = SettingsValidation ? std::string{} : FormatStaticMeshImportSettingsError(SettingsValidation.error());
		if (!Session && bSourceExists && bSupportedSource && bImportSettingsValid && DestinationValidation)
		{
			Session = std::make_unique<AssetForge::Builtins::FAssetImportSession>(
				SourcePathBuffer.data(), DestinationValidation.DirectoryPath, Coordinates.GetSettings(), ImportOptions);
			bReportedCompletion = bImporting = false;
		}
		DrawOutputs();
		DrawMaterials(DestinationValidation.DirectoryPath,
			bAllowAssetMutation && DestinationValidation && bSourceExists && bSupportedSource && bImportSettingsValid);

		if (!bPreviewDirty && !MaterialPreview.Outputs.empty())
		{
			ImGui::SeparatorText("Planned assets");
			constexpr const char* Dispositions[] = {"Create", "Update", "Preserve", "Conflict"};
			ImGui::BeginChild("GltfOutputPreview", ImVec2(0, MonaImGui::ScaleUI(180)), ImGuiChildFlags_Borders);
			for (const auto& Output : MaterialPreview.Outputs)
				ImGui::TextWrapped("%s %s: %s", Dispositions[static_cast<size_t>(Output.Disposition)],
					Output.Role.c_str(), Output.AssetPath.ToString().c_str());
			for (const auto& Diagnostic : MaterialPreview.Diagnostics) ImGui::TextWrapped("%s", Diagnostic.Message.c_str());
			ImGui::EndChild();
		}

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source model to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource) ValidationMessage = "This importer supports glTF and GLB files.";
		else if (!bImportSettingsValid) ValidationMessage = ImportSettingsError;
		else if (!DestinationValidation) ValidationMessage = FormatContentDirectoryValidation(DestinationValidation);
		else if (!Session || Session->GetProgress().Phase != ESceneImportPhase::Ready) ValidationMessage = "Source preparation is not ready.";
		else if (bPreviewDirty) ValidationMessage = "Validating output selections...";
		else if (!MaterialPreview.bSucceeded) ValidationMessage = MaterialPreview.Message;
		DrawImportDialogWarning(ValidationMessage);
		ImGui::TextWrapped("Reimport updates meshes and textures. Existing materials and mesh material bindings are preserved unless Rebuild materials is enabled.");

		ImGui::Spacing();
		ImGui::Separator();
		if (!bAllowAssetMutation) DrawImportDialogWarning("Asset imports are unavailable during Play.");
		ImGui::BeginDisabled(!bAllowAssetMutation || !ValidationMessage.empty());
		if (ImGui::Button("Import glTF Assets", ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f))
			&& !bImporting) (void)Import();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true))
		{
			if (Session) Session->Cancel();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	auto FGltfAssetImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select a glTF or GLB source";
		Request.Filters = {{"glTF and GLB", "*.gltf;*.glb"}, {"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
		if (SourcePathBuffer[0] != '\0')
			Request.InitialDirectory = std::filesystem::path(
				SourcePathBuffer.data()).parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		if (Result.FilePath.size() >= SourcePathBuffer.size())
		{
			SetError("The selected file path is too long for the import form.");
			return;
		}
		SourcePathBuffer.fill(0);
		Session.reset();
		bImporting = bReportedCompletion = false;
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(),
			std::min(Result.FilePath.size(), SourcePathBuffer.size() - 1));
		Coordinates.Reset();
		Coordinates.SetPreset(FMeshCoordinateImportModel::EPreset::YUpNegativeZForward);
		MaterialOptions.Overrides.clear();
		ImportOptions.SelectedMeshes.clear();
		ImportOptions.SelectedScenes.clear();
		MaterialPreview = {};
		bPreviewDirty = true;
		const std::string SceneName = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(), "Scene");
		const FProjectInfo* Project = GetCurrentProject();
		DestinationDirectory.SuggestPath(DestinationDirectory.MakeSuggestedPath(
			SceneName, (Project ? Project->MountRoot : "/") + std::string("Imported/")));
	}

	auto FGltfAssetImportDialog::BrowseDestinationDirectory() -> void
	{
		(void)DestinationDirectory.Browse("Choose an Asset Output Directory",
			"The selected directory path is too long for the import form.",
			"Asset outputs must be saved inside a package-enabled mount.", Callbacks);
		bPreviewDirty = true;
	}

	auto FGltfAssetImportDialog::Import() -> bool
	{
		const FContentDirectoryValidation DestinationValidation =
			DestinationDirectory.Inspect();
		if (!DestinationValidation)
		{
			SetError(FormatContentDirectoryValidation(DestinationValidation));
			return false;
		}
		const FPackagePath& OutputDirectory = DestinationValidation.DirectoryPath;
		bImporting = Session && Session->BeginImport(OutputDirectory, MaterialOptions);
		return bImporting;
	}

	auto FGltfAssetImportDialog::DrawOutputs() -> void
	{
		using namespace AssetForge::Builtins;
		if (!Session || Session->GetProgress().Phase != ESceneImportPhase::Ready) return;
		if (SourceMeshes.empty() || bPreviewDirty)
		{
			SourceMeshes = Session->GetSourceMeshes();
			SourceScenes = Session->GetSourceScenes();
		}
		ImGui::SeparatorText("Outputs");
		bool bChanged = false;
		int Mode = static_cast<int>(ImportOptions.MeshMode);
		if (ImGui::Combo("Mesh layout", &Mode, "One asset per source mesh\0Combine scene instances\0"))
		{
			ImportOptions.MeshMode = static_cast<EAssetImportMeshMode>(Mode);
			bChanged = true;
		}
		bChanged |= ImGui::Checkbox("Create mesh assets", &ImportOptions.bCreateMeshes);
		bChanged |= ImGui::Checkbox("Create or map materials", &ImportOptions.bCreateMaterials);
		bChanged |= ImGui::Checkbox("Create texture assets", &ImportOptions.bCreateTextures);
		if (ImGui::Checkbox("Include unused resources", &ImportOptions.bIncludeUnusedResources))
		{
			ImportOptions.SelectedMeshes.clear();
			bChanged = true;
		}
		if (!SourceScenes.empty())
		{
			uint32 Selected = ImportOptions.SelectedScenes.empty()
				? std::ranges::find(SourceScenes, true, &FAssetImportSourceScene::bDefault)->Index
				: ImportOptions.SelectedScenes.front();
			const auto Label = SourceScenes[Selected].Name.empty() ? std::format("Scene {}", Selected) : SourceScenes[Selected].Name;
			if (ImGui::BeginCombo("Source scene", Label.c_str()))
			{
				for (const auto& Scene : SourceScenes)
					if (ImGui::Selectable((Scene.Name.empty() ? std::format("Scene {}", Scene.Index) : Scene.Name).c_str(), Scene.Index == Selected))
					{
						ImportOptions.SelectedScenes = {Scene.Index};
						ImportOptions.SelectedMeshes.clear();
						bChanged = true;
					}
				ImGui::EndCombo();
			}
		}
		if (ImGui::TreeNodeEx("Source mesh resources", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::BeginChild("GltfMeshResources", ImVec2(0, MonaImGui::ScaleUI(140)), ImGuiChildFlags_Borders);
			for (auto& Mesh : SourceMeshes)
			{
				ImGui::PushID(static_cast<int>(Mesh.Index));
				const auto Name = Mesh.Name.empty() ? std::format("Mesh {}", Mesh.Index) : Mesh.Name;
				if (ImGui::Checkbox(Name.c_str(), &Mesh.bSelected))
				{
					std::vector<uint32> Selection;
					for (const auto& Row : SourceMeshes) if (Row.bSelected) Selection.push_back(Row.Index);
					if (Selection.empty()) Mesh.bSelected = true;
					else { ImportOptions.SelectedMeshes = std::move(Selection); bChanged = true; }
				}
				ImGui::SameLine(); ImGui::TextDisabled("%u primitive(s)", Mesh.PrimitiveCount);
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::TreePop();
		}
		if (!ImportOptions.bCreateMaterials) ImGui::TextWrapped("Mesh material slots will use the unassigned default fallback.");
		if (!ImportOptions.bCreateTextures) ImGui::TextWrapped("Materials will retain source factors with untextured fallbacks.");
		if (bChanged)
		{
			MaterialOptions.Overrides.clear();
			bPreviewDirty = true;
			(void)Session->SetOptions(ImportOptions);
		}
	}

	auto FGltfAssetImportDialog::DrawMaterials(const FPackagePath& Directory, bool bCanPreview) -> void
	{
		using namespace AssetForge::Builtins;
		ImGui::SeparatorText("Materials");
		const auto DrawSelection = [&](FSceneMaterialSelection& Selection) {
			int Mode = static_cast<int>(Selection.Mode);
			if (ImGui::Combo("Mode", &Mode, "Create Materials\0Create Material Instances\0Use Existing Material\0"))
			{
				Selection.Mode = static_cast<ESceneMaterialImportMode>(Mode);
				bPreviewDirty = true;
			}
			if (Selection.Mode == ESceneMaterialImportMode::UseExisting)
			{
				const auto Picker = AssetPicker::Draw({.RequiredClass = DMaterialInterface::StaticClass(),
					.ClassPolicy = EAssetClassPolicy::Derived, .AssignmentMode = EAssetAssignmentMode::AssetPath,
					.CurrentSelectionPath = Selection.ExistingMaterialPath, .SearchText = ParentSearch,
					.AssignPathSelection = [&](std::string_view Path, std::string&) {
						Selection.ExistingMaterialPath = Path; bPreviewDirty = true; return true;
					}});
				if (!Picker.Error.empty()) SetError(Picker.Error);
			}
			if (Selection.Mode == ESceneMaterialImportMode::CreateInstances)
			{
				ImGui::TextUnformatted("Parent material");
				const auto Picker = AssetPicker::Draw({
					.RequiredClass = DMaterial::StaticClass(),
					.ClassPolicy = EAssetClassPolicy::Exact,
					.AssignmentMode = EAssetAssignmentMode::AssetPath,
					.CurrentSelectionPath = Selection.ParentMaterialPath,
					.SearchText = ParentSearch,
					.AssignPathSelection = [&](std::string_view Path, std::string&) {
						Selection.ParentMaterialPath = Path;
						bPreviewDirty = true;
						return true;
					}});
				if (!Picker.Error.empty()) SetError(Picker.Error);
				ImGui::TextDisabled("Mapping: standard PBR or compatible imported PBR (automatic)");
			}
		};
		DrawSelection(MaterialOptions.Default);
		if (!MaterialPreview.Materials.empty())
		{
			ImGui::BeginChild("SceneMaterials", ImVec2(0, MonaImGui::ScaleUI(240)), ImGuiChildFlags_Borders);
			for (const auto& Row : MaterialPreview.Materials)
			{
				ImGui::PushID(Row.StableIdentity.c_str());
				ImGui::SeparatorText(Row.SourceName.empty() ? "Unnamed material" : Row.SourceName.c_str());
				ImGui::TextWrapped("%s", Row.AssetPath.ToString().c_str());
				ImGui::TextWrapped("%s", bPreviewDirty ? "Selections changed; refresh preview." : Row.Message.c_str());
				if (Row.bPreserved)
				{
					ImGui::TextDisabled("Existing %s", Row.Selection.Mode == ESceneMaterialImportMode::CreateInstances ? "Material Instance" : "Material");
					if (!Row.Selection.ParentMaterialPath.empty()) ImGui::TextWrapped("Parent: %s", Row.Selection.ParentMaterialPath.c_str());
				}
				else
				{
					auto Override = std::ranges::find(MaterialOptions.Overrides, Row.StableIdentity, &FSceneMaterialOverride::StableIdentity);
					bool bOverride = Override != MaterialOptions.Overrides.end();
					if (ImGui::Checkbox("Override default", &bOverride))
					{
						if (bOverride)
						{
							MaterialOptions.Overrides.push_back({Row.StableIdentity, MaterialOptions.Default});
							Override = std::prev(MaterialOptions.Overrides.end());
						}
						else MaterialOptions.Overrides.erase(Override);
						bPreviewDirty = true;
					}
					if (bOverride) DrawSelection(Override->Selection);
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
		}
		if (bCanPreview && bPreviewDirty && Session
			&& Session->GetProgress().Phase == ESceneImportPhase::Ready)
		{
			MaterialPreview = Session->PreviewOutputs(Directory, MaterialOptions);
			bPreviewDirty = false;
		}
		if (Session && Session->GetProgress().Phase == ESceneImportPhase::Completed && ImGui::Button("Retry preparation"))
		{
			Session.reset();
			bPreviewDirty = true;
		}
	}

	auto FGltfAssetImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::Editor
