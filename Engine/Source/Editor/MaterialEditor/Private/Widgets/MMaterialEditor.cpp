#include "Widgets/MMaterialEditor.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "Widgets/MMaterialFunctionEditor.h"
#include "Widgets/MaterialParameterPanelModel.h"
#include "Widgets/MaterialPreview.h"
#include "Widgets/MaterialEditingSession.h"
#include "Graph/MaterialGraphCanvas.h"
#include "MaterialGraphDocument.h"
#include "DObject/ObjectLifecycle.h"
#include "Settings/MaterialEditorSessionSettings.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Editor/AssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceUI.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Color.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "MonaImGuiWidgets.h"
#include "Texture/Texture2D.h"

namespace Durin::Editor::Material
{
	namespace
	{
		constexpr float MaximumMaterialVectorWidthInEm = 30.0f;

		struct FMaterialParameterGroup
		{
			std::string Label;
			std::string Path;
			std::vector<size_t> EntryIndices;
			std::vector<FMaterialParameterGroup> Children;
		};

		auto FindOrAddGroup(
			FMaterialParameterGroup& Parent,
			std::string_view Label,
			std::string Path
		) -> FMaterialParameterGroup&
		{
			const auto Existing = std::ranges::find(Parent.Children, Path, &FMaterialParameterGroup::Path);
			if (Existing != Parent.Children.end()) return *Existing;
			return Parent.Children.emplace_back(std::string(Label), std::move(Path));
		}

		auto AddParameterToGroupTree(
			FMaterialParameterGroup& Root,
			const FMaterialParameterPanelEntry& Entry,
			size_t EntryIndex
		) -> void
		{
			if (!Entry.Definition || Entry.Definition->GroupName.IsNone())
			{
				Root.EntryIndices.push_back(EntryIndex);
				return;
			}

			const std::string GroupName = Entry.Definition->GroupName.ToString();
			FMaterialParameterGroup* Group = &Root;
			std::string Path;
			for (size_t Begin = 0; Begin < GroupName.size();)
			{
				const size_t End = GroupName.find('/', Begin);
				const std::string_view Label(GroupName.data() + Begin,
					(End == std::string::npos ? GroupName.size() : End) - Begin);
				if (!Label.empty())
				{
					if (!Path.empty()) Path += '/';
					Path += Label;
					Group = &FindOrAddGroup(*Group, Label, Path);
				}
				if (End == std::string::npos) break;
				Begin = End + 1;
			}
			Group->EntryIndices.push_back(EntryIndex);
		}

		auto FormatParameterSource(const FMaterialParameterPanelEntry& Entry) -> std::string
		{
			if (Entry.bHasLocalOverride) return "Local override";
			if (!Entry.Source) return "Unresolved";
			return std::format("Inherited from {}", Entry.Source->GetName());
		}

		auto MakeMaterialPropertyTableConfig() -> MonaImGui::PropertyEdit::FTableConfig
		{
			return DetailsStyle::MakeTableConfig();
		}

		auto FindCompiledBase(DMaterialInterface* Material) -> DMaterial*
		{
			std::unordered_set<DMaterialInterface*> Visited;
			for (DMaterialInterface* Current = Material;
				Current && Visited.insert(Current).second;
				Current = Current->GetParent())
				if (auto* Base = Cast<DMaterial>(Current)) return Base;
			return nullptr;
		}

		auto FormatCompileState(EMaterialCompileState State) -> const char*
		{
			switch (State)
			{
			case EMaterialCompileState::NeverRequested: return "Not compiled";
			case EMaterialCompileState::NeedsCompile: return "Needs compile";
			case EMaterialCompileState::Scheduled: return "Waiting for edits to finish";
			case EMaterialCompileState::Deferred: return "Waiting for compiler capacity";
			case EMaterialCompileState::Pending: return "Compiling";
			case EMaterialCompileState::Running: return "Compiling";
			case EMaterialCompileState::Ready: return "Ready";
			case EMaterialCompileState::Failed: return "Failed";
			case EMaterialCompileState::Canceled: return "Canceled";
			case EMaterialCompileState::Superseded: return "Superseded";
			case EMaterialCompileState::Rejected: return "Rejected";
			case EMaterialCompileState::Shutdown: return "Unavailable";
			}
			return "Unknown";
		}

		auto FormatCacheOutcome(EMaterialCompileCacheOutcome Outcome) -> const char*
		{
			switch (Outcome)
			{
			case EMaterialCompileCacheOutcome::None: return "none";
			case EMaterialCompileCacheOutcome::RetainedHit: return "retained hit";
			case EMaterialCompileCacheOutcome::SingleFlight: return "shared flight";
			case EMaterialCompileCacheOutcome::Compiled: return "compiled";
			case EMaterialCompileCacheOutcome::Forced: return "forced";
			}
			return "unknown";
		}
	}

	class FMaterialParameterPanelCache
	{
	public:
		auto Synchronize(DMaterialInterface* Material) -> const FMaterialParameterPanelModel&
		{
			ObservedRevision.clear();
			for (DMaterialInterface* Current = Material;
				Current && std::ranges::find(ObservedRevision, Current,
					&FRevisionNode::Material) == ObservedRevision.end();
				Current = Current->GetParent())
			{
				DPackage* Package = Current->GetPackage();
				ObservedRevision.push_back({
					.Material = Current,
					.Package = Package,
					.PackageEditRevision = Package ? Package->GetEditRevision() : 0,
					.RenderStateRevision = Current->GetRenderStateVersion(),
				});
			}
			if (Model && ObservedRevision == Revision) return *Model;

			Revision = ObservedRevision;
			if (!Model || Model->GetMaterial() != Material)
				Model = std::make_unique<FMaterialParameterPanelModel>(Material);
			else
				Model->Refresh();
			std::vector<FSchemaNode> CurrentSchema;
			const std::span Entries = Model->GetEntries();
			CurrentSchema.reserve(Entries.size());
			for (const FMaterialParameterPanelEntry& Entry : Entries)
			{
				CurrentSchema.push_back({
					.ParameterId = Entry.ParameterId,
					.GroupPath = Entry.Definition
						? Entry.Definition->GroupName.ToString() : std::string{},
					.bOrphan = Entry.bOrphan,
				});
			}
			if (CurrentSchema != Schema)
			{
				Schema = std::move(CurrentSchema);
				Root = {};
				for (size_t Index = 0; Index < Entries.size(); ++Index)
				{
					AddParameterToGroupTree(Root, Entries[Index], Index);
				}
			}
			return *Model;
		}

		auto GetRoot() const -> const FMaterialParameterGroup& { return Root; }

	private:
		struct FRevisionNode
		{
			DMaterialInterface* Material = nullptr;
			DPackage* Package = nullptr;
			uint64 PackageEditRevision = 0;
			uint64 RenderStateRevision = 0;

			auto operator==(const FRevisionNode&) const -> bool = default;
		};

		struct FSchemaNode
		{
			FGuid ParameterId;
			std::string GroupPath;
			bool bOrphan = false;

			auto operator==(const FSchemaNode&) const -> bool = default;
		};

		std::vector<FRevisionNode> Revision;
		// Reused by the per-frame validation path so a stable parent depth allocates nothing.
		std::vector<FRevisionNode> ObservedRevision;
		std::vector<FSchemaNode> Schema;
		std::unique_ptr<FMaterialParameterPanelModel> Model;
		FMaterialParameterGroup Root;
	};

	class MMaterialEditor::FMaterialParameterRowScope
	{
	public:
		FMaterialParameterRowScope(
			MMaterialEditor& InEditor,
			const FMaterialParameterPanelModel& Model,
			const FMaterialParameterPanelEntry& InEntry
		)
			: Editor(InEditor)
			, Entry(InEntry)
			, Instance(Model.GetInstance())
			, bOverrideEnabled(!Instance || Entry.bHasLocalOverride)
		{
			const FMaterialParameterDefinition& Definition = *Entry.Definition;
			const std::string ParameterName = Definition.Name.ToString();
			ImGui::PushID(ParameterName.c_str());
			MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
			if (Instance)
			{
				if (ImGui::Checkbox("##Override", &bOverrideEnabled)
					&& !Model.SetOverrideEnabled(Editor.PropertyView,
						Editor.MakePropertyViewContext(), Entry, bOverrideEnabled))
					bOverrideEnabled = !bOverrideEnabled;
				ImGui::SameLine();
				if (bOverrideEnabled && ImGui::SmallButton("Reset")
					&& Model.SetOverrideEnabled(Editor.PropertyView,
						Editor.MakePropertyViewContext(), Entry, false))
					bOverrideEnabled = false;
			}
			if (!bOverrideEnabled)
			{
				ImGui::BeginDisabled();
				bDisabled = true;
			}
		}

		~FMaterialParameterRowScope()
		{
			if (bDisabled) ImGui::EndDisabled();
			if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
			MonaImGui::PropertyEdit::EndRow();
			ImGui::PopID();
		}

		auto IsOverrideEnabled() const -> bool { return bOverrideEnabled; }

		auto HandleContinuousEdit(bool bDeactivatedAfterEdit, bool bActive) -> void
		{
			if (bDeactivatedAfterEdit && Editor.PropertyView.IsEditing())
				Editor.FinishActivePropertyEdit(false);
			else if (bActive && ImGui::IsKeyPressed(ImGuiKey_Escape)
				&& Editor.PropertyView.IsEditing())
				Editor.FinishActivePropertyEdit(true);
		}

	private:
		MMaterialEditor& Editor;
		const FMaterialParameterPanelEntry& Entry;
		DMaterialInstance* Instance = nullptr;
		bool bOverrideEnabled = false;
		bool bDisabled = false;
	};

	MMaterialEditor::MMaterialEditor(
		::Durin::Editor::FWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
		, MaterialParameterPanelCache(std::make_unique<FMaterialParameterPanelCache>())
		, SessionSettings(std::make_unique<FMaterialEditorSessionSettings>())
	{
		SessionSettings->Load();
		MoveObserverHandle = RegisterAssetMoveObserver(
			this);
	}

	MMaterialEditor::~MMaterialEditor()
	{
		UnregisterAssetMoveObserver(MoveObserverHandle);
		FinishActivePropertyEdit(true);
		SessionSettings->Save();
		MaterialPreviews.clear();
		MaterialGraphCanvases.clear();
		EditingSessions.clear();
	}

	auto MMaterialEditor::GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId&
	{
		return Workspace::Type;
	}

	auto MMaterialEditor::OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return ::Durin::Editor::EDocumentOpenResult::Rejected;
		if (FindOpenMaterial(Document.ResourceId)) return ::Durin::Editor::EDocumentOpenResult::Opened;
		FObjectPath AssetPath;
		std::string PathError;
		if (!FObjectPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		DMaterialInterface* Material = nullptr;
		const FAssetResult Result = LoadObject(AssetPath, Material);
		if (!Result || !Material)
		{
			SetError(Result ? "The selected asset is not a material." : Result.Message);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		OpenMaterials.emplace(Document.ResourceId, Material);
		if (!ResetEditingSession(Document.ResourceId))
		{
			OpenMaterials.erase(Document.ResourceId);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		return ::Durin::Editor::EDocumentOpenResult::Opened;
	}

	auto MMaterialEditor::ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		if (const ::Durin::Editor::FDocumentTab* Active = WorkspaceManager.GetActiveDocument();
			Active && Active->Id != Document.Id)
			CancelCanvasInteraction(Active->Id.Value);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Material) && !FinishActivePropertyEdit(true)) return;
		Documents.Activate(Document, Material);
	}

	auto MMaterialEditor::RequestDeactivate() -> bool
	{
		if (const ::Durin::Editor::FDocumentTab* Active = WorkspaceManager.GetActiveDocument())
			CancelCanvasInteraction(Active->Id.Value);
		return FinishActivePropertyEdit(true);
	}

	auto MMaterialEditor::RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult
	{
		CancelCanvasInteraction(Document.Id.Value);
		const auto SessionIt = EditingSessions.find(Document.ResourceId);
		auto* EditingMaterial = SessionIt == EditingSessions.end()
			? FindOpenMaterial(Document.ResourceId) : SessionIt->second->GetWorkingMaterial();
		if (PropertyView.IsEditingObject(EditingMaterial) && !FinishActivePropertyEdit(true))
			return ::Durin::Editor::EDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return ::Durin::Editor::EDocumentCloseResult::PendingConfirmation;
		CaptureCanvasViewport(Document);
		SessionSettings->Save();
		OpenMaterials.erase(Document.ResourceId);
		MaterialPreviews.erase(Document.Id.Value);
		MaterialGraphCanvases.erase(Document.Id.Value);
		FunctionCallPickers.erase(Document.Id.Value);
		ParameterSearchTexts.erase(Document.Id.Value);
		EditingSessions.erase(Document.ResourceId);
		PendingLayoutResets.erase(Document.Id.Value);
		Documents.Close(Document.ResourceId);
		return ::Durin::Editor::EDocumentCloseResult::Closed;
	}

	auto MMaterialEditor::SaveDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		return SaveMaterial(FindOpenMaterial(Document.ResourceId));
	}

	auto MMaterialEditor::DiscardDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		CancelCanvasInteraction(Document.Id.Value);
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		if (!Material) return false;
		if (!FinishActivePropertyEdit(true)) return false;
		if (auto* Session = FindEditingSession(Material))
		{
			Session->CancelApply();
			Material = Session->GetSourceMaterial();
			if (!Material) return false;
			if (!Material->GetPackage()->IsDirty())
			{
				MaterialPreviews.erase(Document.Id.Value);
				MaterialGraphCanvases.erase(Document.Id.Value);
				return ResetEditingSession(Document.ResourceId);
			}
		}
		return Documents.Discard(Material, {},
			[this](DPackage* Previous, DPackage* Replacement) {
				WorkspaceManager.NotifyPackageReloaded(Previous, Replacement);
			}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto MMaterialEditor::OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void
	{
		FinishActivePropertyEdit(true);
		std::unordered_set<std::string> ReboundResources;
		for (auto& [ResourceId, Open] : OpenMaterials)
			if (Open.Get() && Open->GetPackage() == Previous)
			{
				Open = Cast<DMaterialInterface>(Replacement->FindTopLevelAsset(Open->GetFName()));
				ReboundResources.insert(ResourceId);
			}
		if (ReboundResources.contains(std::string(Documents.GetActiveResourceId())))
			MaterialParameterPanelCache = std::make_unique<FMaterialParameterPanelCache>();
		for (const auto& Document : WorkspaceManager.GetDocuments())
			if (Document.WorkspaceType == Workspace::Type
				&& ReboundResources.contains(Document.ResourceId))
			{
				CancelCanvasInteraction(Document.Id.Value);
				MaterialPreviews.erase(Document.Id.Value);
				MaterialGraphCanvases.erase(Document.Id.Value);
				ResetEditingSession(Document.ResourceId);
			}
	}

	auto MMaterialEditor::IsDocumentDirty(const ::Durin::Editor::FDocumentTab& Document) const -> bool
	{
		const auto* Material = FindOpenMaterial(Document.ResourceId);
		if (const auto* Session = FindEditingSession(Material))
			return Session->HasUnappliedChanges() || Documents.IsDirty(Session->GetSourceMaterial());
		return Documents.IsDirty(Material);
	}

	auto MMaterialEditor::CanSaveActiveDocument() const -> bool
	{
		return Documents.CanSave(GetActiveMaterial());
	}

	auto MMaterialEditor::SaveActiveDocument() -> bool
	{
		return SaveMaterial(GetActiveMaterial());
	}

	auto MMaterialEditor::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		for (auto& [ResourceId, Session] : EditingSessions)
		{
			std::string Error;
			Session->Tick(Error);
			if (!Error.empty()) SetError(std::move(Error));
		}
		std::vector<::Durin::Editor::FDocumentId> DeletedDocuments;
		for (const ::Durin::Editor::FDocumentTab& Document : WorkspaceManager.GetDocuments())
		{
			if (Document.WorkspaceType != Workspace::Type) continue;
			const auto It = OpenMaterials.find(Document.ResourceId);
			if (It != OpenMaterials.end() && !It->second.IsValid())
				DeletedDocuments.push_back(Document.Id);
		}
		for (const ::Durin::Editor::FDocumentId Id : DeletedDocuments)
			WorkspaceManager.RequestCloseDocument(Id);
		return Documents.GetDocumentHost().DrawDocuments(
			WorkspaceManager,
			Workspace::Type,
			Workspace::RootKey,
			[this](const ::Durin::Editor::FDocumentTab& Document) {
				return FindOpenMaterial(Document.ResourceId) != nullptr;
			},
			[this](const ::Durin::Editor::FDocumentTab& Document) {
				DrawDocument(Document, FindOpenMaterial(Document.ResourceId));
			},
			[this](const ::Durin::Editor::FDocumentTab& Document, bool bVisible) {
				if (!bVisible)
				{
					if (const auto PreviewIt = MaterialPreviews.find(Document.Id.Value); PreviewIt != MaterialPreviews.end())
						PreviewIt->second->SetVisible(false);
					const auto DockType = Workspace::MakeDocumentDockType(Document);
					if (ImGui::DockBuilderGetNode(::Durin::Editor::WorkspaceUI::MakeDockSpaceId(
						DockType, Workspace::LayoutVersion)))
						::Durin::Editor::WorkspaceUI::SubmitDockSpace(DockType, Workspace::LayoutVersion,
							{0.0f, 0.0f}, ImGuiDockNodeFlags_KeepAliveOnly);
				}
			}
		);
	}

	auto MMaterialEditor::ResetLayout() -> void
	{
		SessionSettings->bPreviewVisible = true;
		SessionSettings->bDetailsVisible = true;
		SessionSettings->bParametersVisible = true;
		SessionSettings->bDiagnosticsVisible = false;
		for (const auto& Document : WorkspaceManager.GetDocuments())
			if (Document.WorkspaceType == Workspace::Type)
				PendingLayoutResets.insert(Document.Id.Value);
	}

	auto MMaterialEditor::FindOpenMaterial(std::string_view ResourceId) const -> DMaterialInterface*
	{
		const auto It = OpenMaterials.find(std::string(ResourceId));
		if (It == OpenMaterials.end() || !It->second.IsValid()) return nullptr;
		const auto Session = EditingSessions.find(std::string(ResourceId));
		return Session == EditingSessions.end() ? It->second.Get()
			: Session->second->GetWorkingMaterial();
	}

	auto MMaterialEditor::FindEditingSession(const DMaterialInterface* Working) const
		-> FMaterialEditingSession*
	{
		if (!Working) return nullptr;
		for (const auto& [ResourceId, Session] : EditingSessions)
			if (Session->GetWorkingMaterial() == Working) return Session.get();
		return nullptr;
	}

	auto MMaterialEditor::ResetEditingSession(std::string_view ResourceId) -> bool
	{
		const std::string Key(ResourceId);
		EditingSessions.erase(Key);
		MaterialParameterPanelCache = std::make_unique<FMaterialParameterPanelCache>();
		const auto It = OpenMaterials.find(Key);
		if (It == OpenMaterials.end()) return false;
		auto* Source = Cast<DMaterial>(It->second.Get());
		if (!Source) return true;
		auto Session = std::make_unique<FMaterialEditingSession>();
		std::string Error;
		if (!Session->Initialize(*Source, SessionSettings->bAutoCompile
			? EMaterialEditCompileMode::Automatic : EMaterialEditCompileMode::Manual, Error,
			GEditor ? GEditor->GetTransactor() : nullptr))
		{
			SetError(std::move(Error));
			It->second = nullptr;
			return false;
		}
		EditingSessions.emplace(Key, std::move(Session));
		return true;
	}

	auto MMaterialEditor::GetActiveMaterial() const -> DMaterialInterface*
	{
		return FindOpenMaterial(Documents.GetActiveResourceId());
	}

	auto MMaterialEditor::SaveMaterial(DMaterialInterface* Material) -> bool
	{
		if (const auto* Active = WorkspaceManager.GetActiveDocument())
			CancelCanvasInteraction(Active->Id.Value);
		if (!FinishActivePropertyEdit(false)) return false;
		auto* Session = FindEditingSession(Material);
		if (Session)
		{
			std::string Error;
			if (!Session->FinishAndApply(Error))
			{
				SetError(std::move(Error));
				return false;
			}
			Material = Session->GetSourceMaterial();
		}
		if (!Documents.Save(Material, {}, [this](std::string Message) {
			SetError(std::move(Message));
		})) return false;
		if (Session) Session->MarkSaved();
		return true;
	}

	auto MMaterialEditor::CanUndo() const -> bool
	{
		return Documents.CanUndo();
	}

	auto MMaterialEditor::CanRedo() const -> bool
	{
		return Documents.CanRedo();
	}

	auto MMaterialEditor::GetUndoDescription() const -> std::string_view
	{
		return Documents.GetUndoDescription();
	}

	auto MMaterialEditor::GetRedoDescription() const -> std::string_view
	{
		return Documents.GetRedoDescription();
	}

	auto MMaterialEditor::Undo() -> bool
	{
		return Documents.Undo();
	}

	auto MMaterialEditor::Redo() -> bool
	{
		return Documents.Redo();
	}

	auto MMaterialEditor::DrawDocument(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		DrawToolbar(Material);
		ImGui::Spacing();

		DrawDockLayout(Document, Material);

		if (Documents.GetActiveResourceId() != Document.ResourceId) return;
		MonaImGui::ErrorDialog("Material Editor Error", ErrorMessage);
	}

	auto MMaterialEditor::DrawToolbar(DMaterialInterface* Material) -> void
	{
		if (ImGui::Button("Save")) SaveMaterial(Material);
		ImGui::SameLine();
		if (Material)
		{
			if (auto* Base = Cast<DMaterial>(Material))
			{
				auto* Session = FindEditingSession(Material);
				if (Session)
				{
					ImGui::BeginDisabled(!Session->HasUnappliedChanges() || Session->IsApplyPending());
					if (ImGui::Button(Session->IsApplyPending() ? "Applying..." : "Apply")
						&& FinishActivePropertyEdit(false))
					{
						if (const auto* Active = WorkspaceManager.GetActiveDocument())
							CancelCanvasInteraction(Active->Id.Value);
						std::string Error;
						if (!Session->RequestApply(Error)) SetError(std::move(Error));
					}
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply the preview changes to the source material and scene.");
					ImGui::SameLine();
				}
				if (ImGui::Button("Compile") && FinishActivePropertyEdit(false)) Base->CompileEdits();
				ImGui::SameLine();
				if (ImGui::Checkbox("Auto Compile", &SessionSettings->bAutoCompile))
				{
					for (const auto& [Resource, OpenSession] : EditingSessions)
						if (auto* OpenBase = OpenSession->GetWorkingMaterial())
							OpenBase->SetEditCompileMode(SessionSettings->bAutoCompile
								? EMaterialEditCompileMode::Automatic : EMaterialEditCompileMode::Manual);
					SessionSettings->Save();
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Compile after editing pauses. Disable to compile changes manually.");
				if (Session && Session->HasUnappliedChanges())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("Unapplied changes");
				}
			}
			else if (ImGui::Button("Compile")) RequestMaterialRecompile(*Material);
			const FMaterialCompileStatus& Status = Material->GetMaterialCompileStatus();
			const bool bPending = Status.State == EMaterialCompileState::Deferred
				|| Status.State == EMaterialCompileState::Pending
				|| Status.State == EMaterialCompileState::Scheduled
				|| Status.State == EMaterialCompileState::Running;
			if (bPending)
			{
				ImGui::SameLine();
				if (ImGui::Button("Cancel Compile"))
				{
					if (auto* Session = FindEditingSession(Material)) Session->CancelApply();
					for (const auto Handle : GetLoadedMaterialDependents(Material))
						if (auto* Owner = ResolveObjectHandle(Handle); IsValid(Owner))
							FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owner);
				}
			}
			ImGui::SameLine();
			auto* EditingSession = FindEditingSession(Material);
			ImGui::TextDisabled("%s: %s%s", EditingSession ? "Preview" : "Compile", FormatCompileState(Status.State),
				(Material->GetAcceptedCompiledProgram() && !Status.IsCurrent()) ? " (showing last known good)" : "");
			if (EditingSession && EditingSession->GetSourceMaterial())
			{
				const auto& SourceStatus = EditingSession->GetSourceMaterial()->GetMaterialCompileStatus();
				if (!SourceStatus.IsCurrent())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("Scene: %s", FormatCompileState(SourceStatus.State));
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Window")) ImGui::OpenPopup("MaterialWindows");
		if (ImGui::BeginPopup("MaterialWindows"))
		{
			ImGui::MenuItem("Preview", nullptr, &SessionSettings->bPreviewVisible);
			ImGui::MenuItem("Details", nullptr, &SessionSettings->bDetailsVisible);
			ImGui::MenuItem("Parameters", nullptr, &SessionSettings->bParametersVisible);
			ImGui::MenuItem("Diagnostics", nullptr, &SessionSettings->bDiagnosticsVisible);
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout")) ResetLayout();
			ImGui::EndPopup();
		}
	}

	auto MMaterialEditor::DrawDockLayout(
		const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		const auto DockType = Workspace::MakeDocumentDockType(Document);
		const ImGuiID DockSpaceId = ::Durin::Editor::WorkspaceUI::MakeDockSpaceId(DockType, Workspace::LayoutVersion);
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		if (Size.x <= 0.0f || Size.y <= 0.0f) return;
		const bool bReset = PendingLayoutResets.erase(Document.Id.Value) != 0;
		if (!ImGui::DockBuilderGetNode(DockSpaceId) || bReset)
			Workspace::BuildDefaultLayout(Document, Size);
		::Durin::Editor::WorkspaceUI::SubmitDockSpace(DockType, Workspace::LayoutVersion, Size);

		const auto BeginPanel = [&](const char* Label, const char* Key, bool* Open = nullptr) {
			const bool bVisible = ::Durin::Editor::WorkspaceUI::BeginDockablePanel(
				DockType, Label, Key, Open, ImGuiWindowFlags_NoCollapse);
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				const auto* Active = WorkspaceManager.GetActiveDocument();
				if (!Active || Active->Id != Document.Id)
					WorkspaceManager.ActivateDocument(Document.Id);
			}
			return bVisible;
		};
		if (BeginPanel("Material Graph", "Graph")) DrawGraphPanel(Document, Material, 0.0f);
		ImGui::End();
		if (SessionSettings->bPreviewVisible)
		{
			const bool bPreviewShown = BeginPanel("Preview", "Preview", &SessionSettings->bPreviewVisible);
			if (bPreviewShown) DrawPreviewPanel(Document, Material, 0.0f);
			else if (const auto It = MaterialPreviews.find(Document.Id.Value); It != MaterialPreviews.end())
				It->second->SetVisible(false);
			ImGui::End();
		}
		else if (const auto It = MaterialPreviews.find(Document.Id.Value); It != MaterialPreviews.end())
			It->second->SetVisible(false);
		if (SessionSettings->bDetailsVisible)
		{
			if (BeginPanel("Details", "Details", &SessionSettings->bDetailsVisible))
				DrawDetailsPanel(Document, Material);
			ImGui::End();
		}
		if (SessionSettings->bParametersVisible)
		{
			if (BeginPanel("Parameters", "Parameters", &SessionSettings->bParametersVisible))
				DrawParametersPanel(Document, Material);
			ImGui::End();
		}
		if (SessionSettings->bDiagnosticsVisible)
		{
			if (BeginPanel("Diagnostics", "Diagnostics", &SessionSettings->bDiagnosticsVisible))
				DrawCompileStatus(Document, Material);
			ImGui::End();
		}
	}

	auto MMaterialEditor::DrawPreviewPanel(
		const ::Durin::Editor::FDocumentTab& Document,
		DMaterialInterface* Material,
		float Height
	) -> void
	{
		std::unique_ptr<FMaterialPreview>& Preview = MaterialPreviews[Document.Id.Value];
		if (Preview == nullptr) Preview = std::make_unique<FMaterialPreview>(Document.Id.Value);
		Preview->Draw(Material, Height);
	}

	auto MMaterialEditor::DrawGraphPanel(
		const ::Durin::Editor::FDocumentTab& Document,
		DMaterialInterface* Material,
		float Height) -> void
	{
		DMaterial* Base = Cast<DMaterial>(Material);
		if (!Base)
		{
			if (ImGui::BeginChild("MaterialGraphInstance", ImVec2(0.0f, Height),
				ImGuiChildFlags_None))
			{
				ImGui::TextWrapped("Material instances inherit their graph from the root base material. Open the base material to author it.");
			}
			ImGui::EndChild();
			return;
		}
		if (!GEditor)
		{
			ImGui::TextDisabled("Material graph transactions are unavailable.");
			return;
		}
		FMaterialGraphCanvas& Canvas = GetOrCreateCanvas(Document);
		if (ImGui::Button("Insert Function Call")) ImGui::OpenPopup("InsertFunctionCall");
		ImGui::SetNextWindowSize(ImVec2(480.0f, 420.0f), ImGuiCond_Appearing);
		if (ImGui::BeginPopup("InsertFunctionCall"))
		{
			FunctionCallPickers[Document.Id.Value].Draw(*Base, *GEditor->GetTransactor(), ErrorMessage);
			ImGui::EndPopup();
		}
		Canvas.Draw(*Base, *GEditor->GetTransactor(), Height,
			[this](std::string Message) { SetError(std::move(Message)); });
		const auto [Zoom, Pan] = Canvas.GetViewport();
		SessionSettings->SetViewport(Document.ResourceId, {.Zoom = Zoom, .Pan = Pan});
	}

	auto MMaterialEditor::DrawSelectedFunction(const ::Durin::Editor::FDocumentTab& Document, DMaterial* Base) -> void
	{
		auto& Canvas = GetOrCreateCanvas(Document);
		for (const auto& Selected : Canvas.GetSelection())
			if (const auto* Id = std::get_if<FGuid>(&Selected))
				for (const auto& Expression : Base->GetExpressionCollection().Expressions)
					if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get());
						Call && Call->Id == *Id && Call->Function.IsValid())
					{
						ImGui::PushID(Id->ToString().c_str());
						if (ImGui::Button("Open Function")) WorkspaceManager.OpenAsset(Call->Function->GetObjectPath(),
							Call->Function->GetClass()->GetQualifiedName().ToString());
						ImGui::PopID();
					}
	}

	auto MMaterialEditor::DrawCompileStatus(
		const ::Durin::Editor::FDocumentTab& Document,
		DMaterialInterface* Material) -> void
	{
		DMaterial* Base = FindCompiledBase(Material);
		if (!Material)
		{
			ImGui::TextDisabled("Compiled program: unavailable");
			return;
		}
		const FMaterialCompileStatus& Status = Material->GetMaterialCompileStatus();
		const bool bCanNavigateGraph = Cast<DMaterial>(Material) != nullptr;
		ImGui::Text("Compile: %s", FormatCompileState(Status.State));
		ImGui::Text("Freshness: %s", Status.IsCurrent() ? "current" : "stale");
		ImGui::Text("Cache: %s", FormatCacheOutcome(Status.CacheOutcome));
		ImGui::Text("Target: %s", Status.Target.empty() ? "n/a" : Status.Target.c_str());
		ImGui::Text("Generation: %llu", static_cast<unsigned long long>(Status.RequestGeneration));
		if (Status.DurationMicroseconds != 0)
			ImGui::Text("Duration: %.2f ms",
				static_cast<double>(Status.DurationMicroseconds) / 1000.0);
		if (Material->GetAcceptedCompiledProgram() && !Status.IsCurrent())
			ImGui::TextDisabled("Preview uses the last known good program.");
		if (const auto* Session = FindEditingSession(Material); Session && Session->GetSourceMaterial())
		{
			const auto* Source = Session->GetSourceMaterial();
			ImGui::Text("Scene material: %s", FormatCompileState(Source->GetMaterialCompileStatus().State));
			for (const auto& Diagnostic : Source->GetMaterialCompileDiagnostics())
				ImGui::TextWrapped("Scene: %s", Diagnostic.Source.Message.c_str());
		}
		uint32 DiagnosticIndex = 0;
		for (const FMaterialCompileDiagnostic& Diagnostic
			: Material->GetMaterialCompileDiagnostics())
		{
			ImGui::PushID(static_cast<int>(DiagnosticIndex++));
			bool bLocated = false;
			switch (Diagnostic.Source.LocationKind)
			{
			case EMaterialProgramDiagnosticLocationKind::Node:
			case EMaterialProgramDiagnosticLocationKind::Input:
				bLocated = Base && std::ranges::any_of(Base->GetExpressionCollection().Expressions,
					[&](const auto& Expression) { return Expression->Id == Diagnostic.Source.NodeId; });
				break;
			case EMaterialProgramDiagnosticLocationKind::SurfaceOutput:
				bLocated = Diagnostic.Source.LocationIndex < 8;
				break;
			case EMaterialProgramDiagnosticLocationKind::Program:
				break;
			}
			const bool bStale = Diagnostic.Generation != Status.RequestGeneration;
			if (!bStale && !Diagnostic.Source.FunctionAssetPath.empty())
			{
				if (ImGui::SmallButton("Open Function"))
					if (const auto Editor = std::dynamic_pointer_cast<MMaterialFunctionEditor>(WorkspaceManager.FindWorkspace(MaterialFunctionWorkspaceType)))
						Editor->NavigateToNode(Diagnostic.Source.FunctionAssetPath, Diagnostic.Source.NodeId);
				ImGui::SameLine();
			}
			if (bCanNavigateGraph && bLocated && !bStale)
			{
				if (ImGui::SmallButton("Go"))
				{
					GetOrCreateCanvas(Document).SelectAndFrameDiagnostic(
						Diagnostic.Source);
					const auto GraphName = ::Durin::Editor::WorkspaceUI::MakePanelWindowName(
						"Material Graph", Workspace::MakeDocumentDockType(Document), "Graph");
					ImGui::SetWindowFocus(GraphName.c_str());
				}
				ImGui::SameLine();
			}
			ImGui::TextWrapped("%s%s", Diagnostic.Source.Message.c_str(),
				bStale || (Diagnostic.Source.LocationKind
					!= EMaterialProgramDiagnosticLocationKind::Program && !bLocated)
					? " (stale location)" : "");
			ImGui::PopID();
		}
		const std::string_view CookDiagnostic = Material->GetMaterialCookDiagnostic();
		if (!CookDiagnostic.empty())
			ImGui::TextWrapped("Cook: %.*s",
				static_cast<int>(CookDiagnostic.size()), CookDiagnostic.data());
	}

	auto MMaterialEditor::DrawParametersPanel(
		const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		if (auto* Instance = Cast<DMaterialInstance>(Material))
		{
			if (MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParameters", MakeMaterialPropertyTableConfig()))
			{
				DrawMaterialParameters(Instance);
				MonaImGui::PropertyEdit::EndTable();
			}
			return;
		}
		auto* Base = Cast<DMaterial>(Material);
		if (!Base || !GEditor || !GEditor->GetTransactor()) return;
		auto& Search = ParameterSearchTexts[Document.Id.Value];
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##ParameterSearch", "Search parameters...", Search.data(), Search.size());
		const ImGuiTextFilter Filter(Search.data());
		// Include disconnected owners; the instance model intentionally filters by reachability.
		std::vector<FMaterialParameterPanelEntry> Entries;
		for (const auto& Definition : Base->GetParameterDefinitions())
		{
			const auto Searchable = std::format("{} {} {}", Definition.Name.ToString(),
				Definition.DisplayName, Definition.GroupName.ToString());
			if (Filter.PassFilter(Searchable.c_str())) Entries.push_back({.Definition = Definition});
		}
		std::ranges::sort(Entries, [](const auto& A, const auto& B) {
			if (A.Definition->SortOrder != B.Definition->SortOrder)
				return A.Definition->SortOrder < B.Definition->SortOrder;
			return A.Definition->Name.ToString() < B.Definition->Name.ToString();
		});
		if (Entries.empty())
		{
			ImGui::TextDisabled(Base->GetParameterDefinitions().empty()
				? "Add parameter nodes in the graph to get started." : "No matching parameters.");
			return;
		}
		FMaterialParameterGroup Root;
		for (size_t Index = 0; Index < Entries.size(); ++Index)
			AddParameterToGroupTree(Root, Entries[Index], Index);
		auto& Canvas = GetOrCreateCanvas(Document);
		bool bSubmitted = false;
		const auto Submit = [&](const FMaterialGraphCommandResult& Result) {
			bSubmitted = true;
			if (!Result) SetError(Result.Message);
		};
		const auto EditText = [](const char* Label, std::string& Value) {
			std::array<char, MaterialProgramMaxDisplayNameBytes + 1> Buffer{};
			std::copy_n(Value.data(), std::min(Value.size(), Buffer.size() - 1), Buffer.data());
			if (!DetailsStyle::EditRow(Label, [&] { return ImGui::InputText("##Value", Buffer.data(), Buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue); })) return false;
			Value = Buffer.data();
			return true;
		};
		const auto DrawEntry = [&](FMaterialParameterDefinition Parameter) {
			std::vector<FGuid> NodeIds;
			for (const auto& Expression : Base->GetExpressionCollection().Expressions)
				if (const auto* Owner = Cast<DMaterialExpressionParameter>(Expression.Get());
					Owner && Owner->Metadata.Id == Parameter.Id) NodeIds.push_back(Owner->Id);
			if (NodeIds.empty()) return;
			ImGui::PushID(Parameter.Id.ToString().c_str());
			const bool bSelected = std::ranges::any_of(NodeIds,
				[&](const FGuid& Id) { return Canvas.GetSelection().contains(Id); });
			const auto Flags = ImGuiTreeNodeFlags_SpanAvailWidth
				| (bSelected ? ImGuiTreeNodeFlags_Selected : ImGuiTreeNodeFlags_None);
			const bool bOpen = ImGui::TreeNodeEx("Parameter", Flags, "%s", Parameter.Name.ToString().c_str());
			if (ImGui::IsItemClicked()) Canvas.SelectAndFrame(NodeIds.front());
			if (bOpen)
			{
				const auto Commit = [&]() {
					const auto& Expressions = Base->GetExpressionCollection().Expressions;
					const auto It = std::ranges::find(Expressions, NodeIds.front(), [](const auto& E) { return E->Id; });
					if (It == Expressions.end()) return;
					// Copy the concrete owner so texture-sample inputs and node identity survive metadata edits.
					TStrongObjectPtr<DMaterialExpressionParameter> Candidate(
						Cast<DMaterialExpressionParameter>(DuplicateObject(It->Get(), nullptr, NAME_None)));
					if (!Candidate || !Candidate->SetParameterDefinition(Parameter))
					{
						SetError("The parameter definition is invalid.");
						bSubmitted = true;
						return;
					}
					Submit(FMaterialGraphDocument(*Base).ReplaceExpression(*Candidate, GEditor->GetTransactor()));
				};
				if (MonaImGui::PropertyEdit::BeginTable("ParameterMetadata", MakeMaterialPropertyTableConfig()))
				{
					std::string Name = Parameter.Name.ToString();
					if (EditText("Name", Name))
						Submit(FMaterialGraphOperations::RenameParameter(*Base, Parameter.Id, FName(Name), GEditor->GetTransactor()));
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rename all references while preserving material instance overrides.");
					if (!bSubmitted && EditText("Display name", Parameter.DisplayName)) Commit();
					std::string Group = Parameter.GroupName.ToString();
					if (!bSubmitted && EditText("Group", Group)) { Parameter.GroupName = FName(Group); Commit(); }
					if (!bSubmitted && DetailsStyle::EditRow("Order", [&] { return ImGui::InputInt("##Value", &Parameter.SortOrder, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue); })) Commit();
					constexpr std::array Presentations{"Default", "Drag", "Integer", "Color", "Asset picker"};
					const auto PresentationIndex = static_cast<size_t>(Parameter.Presentation);
					MonaImGui::PropertyEdit::BeginRow("Presentation");
					if (!bSubmitted && ImGui::BeginCombo("##Presentation", PresentationIndex < Presentations.size()
						? Presentations[PresentationIndex] : "Unknown"))
					{
						for (size_t Index = 0; Index < Presentations.size(); ++Index)
						{
							auto Option = Parameter;
							Option.Presentation = static_cast<EMaterialParameterPresentation>(Index);
							if (FMaterialParameterPanelModel::SelectControl(Option) == EMaterialParameterControlKind::Unsupported) continue;
							if (ImGui::Selectable(Presentations[Index], Index == PresentationIndex))
							{
								Parameter.Presentation = Option.Presentation;
								Commit();
								break;
							}
						}
						ImGui::EndCombo();
					}
					MonaImGui::PropertyEdit::EndRow();
					if (!bSubmitted && Parameter.Type == EMaterialParameterType::Scalar)
					{
						if (DetailsStyle::EditRow("Range hint", [&] { return ImGui::Checkbox("##Value", &Parameter.bHasRange); })) Commit();
						if (!bSubmitted && Parameter.bHasRange)
						{
							float Range[]{Parameter.MinimumValue, Parameter.MaximumValue};
							if (DetailsStyle::EditRow("Min / Max", [&] { return ImGui::InputFloat2("##Value", Range, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue); }))
							{
								Parameter.MinimumValue = Range[0]; Parameter.MaximumValue = Range[1]; Commit();
							}
						}
					}
					MonaImGui::PropertyEdit::EndTable();
				}
				if (NodeIds.size() > 1)
				{
					ImGui::TextDisabled("Used by %zu nodes", NodeIds.size());
					for (size_t Index = 0; Index < NodeIds.size(); ++Index)
						if (ImGui::SmallButton(std::format("Locate node {}", Index + 1).c_str())) Canvas.SelectAndFrame(NodeIds[Index]);
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		};
		const auto DrawGroup = [&](const auto& Self, const FMaterialParameterGroup& Group) -> void {
			for (const auto& Child : Group.Children)
			{
				if (bSubmitted) break;
				if (ImGui::TreeNodeEx(Child.Path.c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s", Child.Label.c_str()))
				{
					Self(Self, Child);
					ImGui::TreePop();
				}
			}
			for (const size_t Index : Group.EntryIndices)
			{
				if (bSubmitted) break;
				DrawEntry(*Entries[Index].Definition);
			}
		};
		DrawGroup(DrawGroup, Root);
	}

	auto MMaterialEditor::DrawDetailsPanel(
		const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		auto* BaseMaterial = Cast<DMaterial>(Material);
		const bool bShowMaterialDetails = !BaseMaterial
			|| GetOrCreateCanvas(Document).GetSelection().empty()
			|| GetOrCreateCanvas(Document).GetSelection().contains(EMaterialGraphTerminal::MaterialOutput);
		if (bShowMaterialDetails)
		{
			ImGui::TextWrapped("%s", Material->GetName().c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Document.ResourceId.c_str());
			ImGui::TextDisabled("%s", Cast<DMaterialInstance>(Material) ? "Material Instance" : "Surface Material");
			ImGui::Spacing();
		}
		if (auto* Instance = Cast<DMaterialInstance>(Material))
		{
			DrawMaterialInstance(Instance);
		}
		else if (BaseMaterial)
		{
			if (bShowMaterialDetails
				&& MonaImGui::PropertyEdit::BeginTable("SurfaceProperties", MakeMaterialPropertyTableConfig()))
			{
				auto Properties = BaseMaterial->GetStaticProperties();
				int Shading = static_cast<int>(Properties.ShadingModel);
				int Blend = static_cast<int>(Properties.BlendMode);
				int Depth = static_cast<int>(Properties.DepthWritePolicy);
				bool bChanged = DetailsStyle::EditRow("Shading", [&] { return ImGui::Combo("##Value", &Shading, "Lit\0Unlit\0"); });
				bChanged |= DetailsStyle::EditRow("Blend", [&] { return ImGui::Combo("##Value", &Blend, "Opaque\0Masked\0Translucent\0"); });
				Properties.ShadingModel = static_cast<EMaterialShadingModel>(Shading);
				Properties.BlendMode = static_cast<EMaterialBlendMode>(Blend);
				if (Properties.BlendMode == EMaterialBlendMode::Masked)
					bChanged |= DetailsStyle::EditRow("Mask cutoff", [&] { return ImGui::InputFloat("##Value", &Properties.OpacityMaskThreshold,
						0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue); });
				bChanged |= DetailsStyle::EditRow("Two sided", [&] { return ImGui::Checkbox("##Value", &Properties.bTwoSided); });
				bChanged |= DetailsStyle::EditRow("Depth write", [&] { return ImGui::Combo("##Value", &Depth, "Automatic\0Enabled\0Disabled\0"); });
				Properties.DepthWritePolicy = static_cast<EMaterialDepthWritePolicy>(Depth);
				MonaImGui::PropertyEdit::EndTable();
				if (bChanged)
					if (auto* Property = BaseMaterial->GetClass()->FindPropertyByName(FName("StaticProperties")))
						PropertyView.SubmitPropertyValueEdit(MakePropertyViewContext(),
							::Durin::Editor::FPropertyEditTarget::ForMember(BaseMaterial, Property),
							[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
								*ScratchProperty->ContainerPtrToValuePtr<FMaterialStaticProperties>(ScratchContainer, ScratchArrayIndex) = Properties;
							}, false);
			}
			if (GEditor && GEditor->GetTransactor())
			{
				DrawSelectedFunction(Document, BaseMaterial);
				GetOrCreateCanvas(Document).DrawSelectionDetails(*BaseMaterial, *GEditor->GetTransactor(),
					[this](std::string Message) { SetError(std::move(Message)); });
			}
		}
	}

	auto MMaterialEditor::DrawMaterialInstance(DMaterialInstance* Instance) -> void
	{
		if (auto* Parent = Instance->GetParent(); Parent && ImGui::Button("Open Parent Material"))
			WorkspaceManager.OpenAsset(Parent->GetObjectPath(), Parent->GetClass()->GetQualifiedName().ToString());
		ImGui::SeparatorText("Inheritance");
		if (MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParent", MakeMaterialPropertyTableConfig()))
		{
			DrawParentPicker(Instance);
			MonaImGui::PropertyEdit::EndTable();
		}
		ImGui::SeparatorText("Rendering Overrides");
		auto Overrides = Instance->GetPropertyOverrides();
		const std::array<const char*, 5> Labels{"Blend mode", "Shading model", "Mask threshold", "Two sided", "Depth write"};
		const std::array<bool*, 5> Flags{&Overrides.bOverrideBlendMode, &Overrides.bOverrideShadingModel,
			&Overrides.bOverrideOpacityMaskThreshold, &Overrides.bOverrideTwoSided, &Overrides.bOverrideDepthWritePolicy};
		FResolvedMaterialProperties Resolved;
		std::string ResolveError;
		const bool bResolved = ResolveMaterialProperties(*Instance, Resolved, ResolveError);
		bool bChanged = false;
		if (MonaImGui::PropertyEdit::BeginTable("RenderingOverrides", MakeMaterialPropertyTableConfig()))
		{
			for (size_t Index = 0; Index < Labels.size(); ++Index)
			{
				ImGui::PushID(static_cast<int>(Index));
				MonaImGui::PropertyEdit::BeginRow(Labels[Index]);
				bChanged |= ImGui::Checkbox("##Override", Flags[Index]);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override this property; clear to inherit.");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-FLT_MIN);
				auto InheritedValues = bResolved ? Resolved.Properties : Overrides.Values;
				auto& DisplayValues = *Flags[Index] ? Overrides.Values : InheritedValues;
				ImGui::BeginDisabled(!*Flags[Index]);
				if (Index == 0 || Index == 1 || Index == 4)
				{
					int Value = Index == 0 ? static_cast<int>(DisplayValues.BlendMode)
						: Index == 1 ? static_cast<int>(DisplayValues.ShadingModel)
						: static_cast<int>(DisplayValues.DepthWritePolicy);
					const char* Items = Index == 0 ? "Opaque\0Masked\0Translucent\0"
						: Index == 1 ? "Lit\0Unlit\0" : "Automatic\0Enabled\0Disabled\0";
					if (ImGui::Combo("##Value", &Value, Items))
					{
						bChanged = true;
						if (Index == 0) DisplayValues.BlendMode = static_cast<EMaterialBlendMode>(Value);
						else if (Index == 1) DisplayValues.ShadingModel = static_cast<EMaterialShadingModel>(Value);
						else DisplayValues.DepthWritePolicy = static_cast<EMaterialDepthWritePolicy>(Value);
					}
				}
				else if (Index == 2)
					bChanged |= ImGui::InputFloat("##Value", &DisplayValues.OpacityMaskThreshold, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
				else bChanged |= ImGui::Checkbox("Enabled", &DisplayValues.bTwoSided);
				ImGui::EndDisabled();
				if (bResolved)
				{
					const auto* Source = ResolveObjectHandle(Resolved.Sources[Index]);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::TextWrapped("Source: %s", Source ? Source->GetObjectPath().c_str() : "unavailable");
					ImGui::PopStyleColor();
				}
				MonaImGui::PropertyEdit::EndRow();
				ImGui::PopID();
			}
			MonaImGui::PropertyEdit::EndTable();
		}
		ImGui::TextWrapped("Enable an override to edit; clear it to inherit.");
		if (!bResolved) ImGui::TextWrapped("%s", ResolveError.c_str());
		if (bChanged)
		{
			if (auto* Property = Instance->GetClass()->FindPropertyByName(FName("PropertyOverrides")))
				PropertyView.SubmitPropertyValueEdit(MakePropertyViewContext(),
					::Durin::Editor::FPropertyEditTarget::ForMember(Instance, Property),
					[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
						*ScratchProperty->ContainerPtrToValuePtr<FMaterialPropertyOverrides>(ScratchContainer, ScratchArrayIndex) = Overrides;
					}, false);
		}
	}

	auto MMaterialEditor::DrawParentPicker(DMaterialInstance* Instance) -> void
	{
		ImGui::PushID("MaterialParent");
		MonaImGui::PropertyEdit::BeginRow("Parent");
		DMaterialInterface* Current = Instance->GetParent();
		const ::Durin::Editor::FAssetPickerResult PickerResult = ::Durin::Editor::AssetPicker::Draw({
			.ComboId = "##Parent",
			.SearchId = "##ParentSearch",
			.SearchHint = "Search materials...",
			.RequiredClass = DMaterialInterface::StaticClass(),
			.ClassPolicy = ::Durin::Editor::EAssetClassPolicy::Derived,
			.CurrentSelection = Current,
			.SearchText = ParentSearchText,
			.bAllowNone = true,
			.AssignSelection = [this, Instance](DObject* Selection, std::string& OutError) {
				DMaterialInterface* Parent = Cast<DMaterialInterface>(Selection);
				if (Selection && !Parent)
				{
					OutError = "The selected asset is not a material.";
					return false;
				}
				FProperty* Property = Instance->GetClass()->FindPropertyByName(FName("Parent"));
				if (!Property)
				{
					OutError = "The reflected material parent property is unavailable.";
					return false;
				}
				const bool bAssigned = PropertyView.SubmitPropertyValueEdit(MakePropertyViewContext(),
					::Durin::Editor::FPropertyEditTarget::ForMember(Instance, Property), [&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					static_cast<FObjectProperty*>(ScratchProperty)->SetObjectPropertyValue(ScratchContainer, Parent, ScratchArrayIndex);
				}, false);
				if (!bAssigned && OutError.empty()) OutError = "Unable to assign the reflected material parent.";
				return bAssigned;
			},
		});
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawMaterialParameters(DMaterialInterface* Material) -> void
	{
		const FMaterialParameterPanelModel& Model = MaterialParameterPanelCache->Synchronize(Material);
		const auto DrawGroup = [this, &Model](const auto& Self,
			const FMaterialParameterGroup& Group, uint32 Depth) -> void {
			for (const FMaterialParameterGroup& Child : Group.Children)
			{
				const std::string Id = "MaterialParameterGroup/" + Child.Path;
				const ImGuiTreeNodeFlags Flags = Depth == 0
					? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
				if (MonaImGui::PropertyEdit::BeginGroup(Id.c_str(), Child.Label.c_str(), Flags))
				{
					Self(Self, Child, Depth + 1);
					MonaImGui::PropertyEdit::EndGroup();
				}
			}
			const std::span Entries = Model.GetEntries();
			for (size_t EntryIndex : Group.EntryIndices)
			{
				DrawMaterialParameter(Model, Entries[EntryIndex]);
			}
		};
		const auto* Root = &MaterialParameterPanelCache->GetRoot();
		// A sole container adds no grouping information; keep the actual parameter categories.
		while (Root->EntryIndices.empty() && Root->Children.size() == 1
			&& Root->Children.front().EntryIndices.empty())
			Root = &Root->Children.front();
		DrawGroup(DrawGroup, *Root, 0);
	}

	auto MMaterialEditor::DrawMaterialParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		if (Entry.bOrphan)
		{
			DrawOrphanParameter(Model, Entry);
			return;
		}
		switch (Entry.Control)
		{
		case EMaterialParameterControlKind::Scalar:
		case EMaterialParameterControlKind::RangedScalar:
			DrawScalarParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::IntegerScalar:
			DrawIntegerParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::Vector:
			DrawVectorParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::Color:
			DrawColorParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::AssetPicker:
			DrawTextureParameter(Model, Entry);
			break;
		case EMaterialParameterControlKind::Unsupported:
			MonaImGui::PropertyEdit::BeginRow(Entry.Definition->DisplayName.c_str(), true);
			ImGui::TextDisabled("<unsupported parameter presentation>");
			MonaImGui::PropertyEdit::EndRow(true);
			break;
		}
	}

	auto MMaterialEditor::DrawVectorParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		FMaterialParameterRowScope Row(*this, Model, Entry);
		MonaImGui::PropertyEdit::FWidgetState WidgetState;
		const MonaImGui::PropertyEdit::FValueWidgetConfig WidgetConfig{
			.MaximumWidthInEm = MaximumMaterialVectorWidthInEm,
			.bHasRange = Definition.bHasRange,
			.MinimumValue = Definition.MinimumValue,
			.MaximumValue = Definition.MaximumValue,
			.Format = "%.3f",
		};
		FMaterialParameterValue Edited = Entry.Value;
		bool bChanged = false;
		if (Definition.Type == EMaterialParameterType::Vector2)
		{
			FVector2 Value = Entry.Value.GetVector2();
			bChanged = MonaImGui::PropertyEdit::EditVectorValue(
				"##Value", Value, 0.01, &WidgetState, WidgetConfig);
			Edited.GetVector2() = Value;
		}
		else if (Definition.Type == EMaterialParameterType::Vector4)
		{
			FVector4 Value = Entry.Value.GetVector4();
			bChanged = MonaImGui::PropertyEdit::EditVectorValue(
				"##Value", Value, 0.01, &WidgetState, WidgetConfig);
			Edited.GetVector4() = Value;
		}
		else
		{
			FVector3 Value = Entry.Value.GetVector();
			bChanged = MonaImGui::PropertyEdit::EditVectorValue(
				"##Value", Value, 0.01, &WidgetState, WidgetConfig);
			Edited.GetVector() = Value;
		}
		if (bChanged && Row.IsOverrideEnabled()
			&& !Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
			SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		Row.HandleContinuousEdit(WidgetState.bDeactivatedAfterEdit, WidgetState.bActive);
	}

	auto MMaterialEditor::DrawColorParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		FVector3 Value = Entry.Value.GetVector();
		FMaterialParameterRowScope Row(*this, Model, Entry);
		float Color[3] = {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##Value", Color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB)
			&& Row.IsOverrideEnabled())
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.GetVector() = FVector3(Color[0], Color[1], Color[2]);
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		Row.HandleContinuousEdit(ImGui::IsItemDeactivatedAfterEdit(), ImGui::IsItemActive());
	}

	auto MMaterialEditor::DrawScalarParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		float Value = Entry.Value.GetScalar();
		FMaterialParameterRowScope Row(*this, Model, Entry);
		ImGui::SetNextItemWidth(-FLT_MIN);
		const float Minimum = Definition.bHasRange ? Definition.MinimumValue : 0.0f;
		const float Maximum = Definition.bHasRange ? Definition.MaximumValue : 0.0f;
		const ImGuiSliderFlags Flags = Definition.bHasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		if (ImGui::DragFloat("##Value", &Value, 0.01f, Minimum, Maximum, "%.3f", Flags)
			&& Row.IsOverrideEnabled())
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.GetScalar() = Value;
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		Row.HandleContinuousEdit(ImGui::IsItemDeactivatedAfterEdit(), ImGui::IsItemActive());
	}

	auto MMaterialEditor::DrawIntegerParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		FMaterialParameterRowScope Row(*this, Model, Entry);

		float Scalar = std::isfinite(Entry.Value.GetScalar())
			? Entry.Value.GetScalar() : Definition.Value.GetScalar();
		if (Definition.bHasRange)
			Scalar = std::clamp(Scalar, Definition.MinimumValue, Definition.MaximumValue);
		int Value = static_cast<int>(std::floor(Scalar + 0.5f));
		const int Minimum = Definition.bHasRange
			? static_cast<int>(std::ceil(Definition.MinimumValue)) : 0;
		const int Maximum = Definition.bHasRange
			? static_cast<int>(std::floor(Definition.MaximumValue)) : 0;
		ImGui::SetNextItemWidth(-FLT_MIN);
		const ImGuiSliderFlags Flags = Definition.bHasRange
			? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		if (ImGui::DragInt("##Value", &Value, 1.0f, Minimum, Maximum, "%d", Flags)
			&& Row.IsOverrideEnabled())
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.GetScalar() = static_cast<float>(Value);
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		Row.HandleContinuousEdit(ImGui::IsItemDeactivatedAfterEdit(), ImGui::IsItemActive());
	}

	auto MMaterialEditor::DrawTextureParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		DTexture2D* Texture = Entry.Value.GetTexture().Texture.Get();
		FMaterialParameterRowScope Row(*this, Model, Entry);
		const ::Durin::Editor::FAssetPickerResult PickerResult = ::Durin::Editor::AssetPicker::Draw({
			.ComboId = "##Texture",
			.SearchId = "##TextureSearch",
			.SearchHint = "Search textures...",
			.RequiredClass = DTexture2D::StaticClass(),
			.ClassPolicy = ::Durin::Editor::EAssetClassPolicy::Derived,
			.CurrentSelection = Texture,
			.SearchText = TextureSearchText,
			.bAllowNone = true,
			.AssignSelection = [this, &Model, Entry, Label = Definition.DisplayName](DObject* Selection, std::string& OutError) {
				DTexture2D* Selected = Cast<DTexture2D>(Selection);
				if (Selection && !Selected)
				{
					OutError = "The selected asset is not a texture.";
					return false;
				}
				FMaterialParameterValue Edited = Entry.Value;
				Edited.GetTexture().Texture = Selected;
				const bool bAssigned = Model.SubmitValueEdit(
					PropertyView, MakePropertyViewContext(), Entry, Edited, false);
				if (!bAssigned && OutError.empty()) OutError = "Unable to assign the reflected texture parameter.";
				return bAssigned;
			},
		});
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		if (ImGui::TreeNode("Sampling"))
		{
			FMaterialParameterValue Edited = Entry.Value;
			auto Combo = [](const char* Label, auto& Value, const char* Options) {
				int Selected = static_cast<int>(Value);
				ImGui::PushID(Label);
				ImGui::TextUnformatted(Label);
				ImGui::SetNextItemWidth(-FLT_MIN);
				const bool bChanged = ImGui::Combo("##Value", &Selected, Options);
				ImGui::PopID();
				if (!bChanged) return false;
				Value = static_cast<std::remove_reference_t<decltype(Value)>>(Selected);
				return true;
			};
			bool Changed = Combo("Minification", Edited.GetTexture().SamplerState.MinFilter,
				"Nearest\0Linear\0Nearest mip, nearest\0Nearest mip, linear\0Linear mip, nearest\0Linear mip, linear\0");
			Changed |= Combo("Magnification", Edited.GetTexture().SamplerState.MagFilter, "Nearest\0Linear\0");
			Changed |= Combo("Address U", Edited.GetTexture().SamplerState.AddressU, "Repeat\0Mirror\0Clamp\0");
			Changed |= Combo("Address V", Edited.GetTexture().SamplerState.AddressV, "Repeat\0Mirror\0Clamp\0");
			Changed |= Combo("Missing texture", Edited.GetTexture().TextureFallback, "White\0Black\0Flat normal (RG)\0");
			if (Changed) Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, false);
			ImGui::TreePop();
		}
	}

	auto MMaterialEditor::DrawOrphanParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const std::string Id = Entry.ParameterId.ToString();
		ImGui::PushID(Id.c_str());
		MonaImGui::PropertyEdit::BeginRow("Orphan Override");
		ImGui::TextDisabled("%s", Id.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Remove"))
			Model.RemoveOrphan(PropertyView, MakePropertyViewContext(), Entry);
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const ::Durin::Editor::FPropertyViewContext Context = MakePropertyViewContext();
		return PropertyView.FinishActiveEdit(&Context, bCancel);
	}

	auto MMaterialEditor::MakePropertyViewContext() -> ::Durin::Editor::FPropertyViewContext
	{
		return {
			.Transactor = GEditor ? GEditor->GetTransactor() : nullptr,
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
		};
	}

	auto MMaterialEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}

	auto MMaterialEditor::OnAssetsRelocated(
		std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		struct FMove
		{
			std::string Source;
			std::string Destination;
			TObjectPtr<DMaterialInterface> Material;
		};
		std::vector<FMove> Moves;
		for (const FAssetRelocationMapping& Mapping : Mappings)
		{
			const std::string Source = Mapping.SourcePath.ToString();
			const auto It = OpenMaterials.find(Source);
			if (It == OpenMaterials.end()) continue;
			Moves.push_back({Source, Mapping.DestinationPath.ToString(), It->second});
		}
		for (const FMove& Move : Moves) OpenMaterials.erase(Move.Source);
		for (const FMove& Move : Moves)
		{
			OpenMaterials[Move.Destination] = Move.Material;
			if (auto Node = EditingSessions.extract(Move.Source); !Node.empty())
			{
				Node.key() = Move.Destination;
				EditingSessions.insert(std::move(Node));
			}
			SessionSettings->MoveViewport(Move.Source, Move.Destination);
			WorkspaceManager.RemapResourceId(Move.Source, Move.Destination);
		}
	}

	auto MMaterialEditor::CancelCanvasInteraction(uint64 DocumentId) -> void
	{
		if (const auto It = MaterialGraphCanvases.find(DocumentId);
			It != MaterialGraphCanvases.end())
			It->second->CancelInteraction();
	}

	auto MMaterialEditor::GetOrCreateCanvas(
		const ::Durin::Editor::FDocumentTab& Document) -> FMaterialGraphCanvas&
	{
		std::unique_ptr<FMaterialGraphCanvas>& Canvas =
			MaterialGraphCanvases[Document.Id.Value];
		if (!Canvas)
		{
			Canvas = std::make_unique<FMaterialGraphCanvas>();
			if (const FMaterialGraphViewportState* State =
				SessionSettings->FindViewport(Document.ResourceId))
				Canvas->SetViewport(State->Zoom, State->Pan);
		}
		return *Canvas;
	}

	auto MMaterialEditor::CaptureCanvasViewport(
		const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		const auto It = MaterialGraphCanvases.find(Document.Id.Value);
		if (It == MaterialGraphCanvases.end()) return;
		const auto [Zoom, Pan] = It->second->GetViewport();
		SessionSettings->SetViewport(Document.ResourceId, {.Zoom = Zoom, .Pan = Pan});
	}
}
