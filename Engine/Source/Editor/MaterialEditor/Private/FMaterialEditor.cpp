#include "Materials/ObjectCacheContext.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Misc/UnifiedError.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "FMaterialEditor.h"
#include "MaterialEditorUI.h"
#include "DObject/ObjectGraphReplacement.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "Widgets/MaterialParameterPanelModel.h"
#include "Widgets/MaterialPreview.h"
#include "Widgets/MaterialEditingSession.h"
#include "MaterialFunctionEditingSession.h"
#include "Graph/MaterialGraphCanvas.h"
#include "Graph/MaterialEditingOperations.h"
#include "DObject/ObjectLifecycle.h"
#include "Settings/MaterialEditorProfileSettings.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transactor.h"
#include "Toolkits/AssetEditor.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Asset/AssetPicker.h"
#include "Framework/TabLayout.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Math/Color.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"
#include "Widgets/MonaImGuiWidgets.h"
#include "Texture/Texture2D.h"

namespace Durin::MaterialEditor
{
	class FMaterialEditorReloadParticipant final : public IObjectReplacementParticipant
	{
		std::shared_ptr<FMaterialEditor> Owner;
		std::shared_ptr<FMaterialEditingSession> MaterialSession;
		std::shared_ptr<FMaterialFunctionEditingSession> FunctionSession;
		FObjectKey Previous;
		TStrongObjectPtr<DMaterial> Replacement;
		TStrongObjectPtr<DMaterialFunction> FunctionReplacement;
		uint64 WorkingRevision = 0;
		uint64 ReplacementRevision = 0;
		bool bCommitted = false;
	public:
		explicit FMaterialEditorReloadParticipant(std::shared_ptr<FMaterialEditor> InOwner) : Owner(std::move(InOwner)) {}
		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			Abort();
			bCommitted = false;
			const auto Function = Owner->FunctionSession;
			const auto Material = Owner->EditingSession;
			auto* Source = Function ? static_cast<DObject*>(Function->GetSource())
				: Material ? Material->GetSourceMaterial() : nullptr;
			const auto* Entry = Map.Find(Source);
			if (!Entry) return {};
			if (!Owner->RequestDeactivate() || !Owner->IsEditorOpen()
				|| Material != Owner->EditingSession || Function != Owner->FunctionSession)
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy});
			Previous = FObjectKey(Entry->Previous);
			if (Function)
			{
				if (Function->bPublishing || Function->IsDirty())
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy});
				FunctionReplacement = Cast<DMaterialFunction>(Entry->Replacement);
				if (!FunctionReplacement)
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::UnmappedReference});
				FunctionSession = Function;
				WorkingRevision = Function->Working->GetGraphChanges().GetEditRevision();
			}
			if (Material)
			{
				if (Material->bPublishing) return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy});
				Replacement = Cast<DMaterial>(Entry->Replacement);
				if (!Replacement) return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::UnmappedReference});
				MaterialSession = Material;
				WorkingRevision = Material->Working->GetGraphChanges().GetEditRevision();
				ReplacementRevision = Replacement->GetGraphChanges().GetEditRevision();
			}
			return {};
		}
		auto Validate() const -> bool override
		{
			return Owner->IsEditorOpen()
				&& (!FunctionSession || (Owner->FunctionSession == FunctionSession
					&& FObjectKey(FunctionSession->GetSource()) == Previous
					&& !FunctionSession->bPublishing && !FunctionSession->IsDirty()
					&& FunctionSession->Working->GetGraphChanges().GetEditRevision() == WorkingRevision))
				&& (!MaterialSession || (Owner->EditingSession == MaterialSession
					&& FObjectKey(MaterialSession->GetSourceMaterial()) == Previous
					&& !MaterialSession->bPublishing
					&& MaterialSession->Working->GetGraphChanges().GetEditRevision() == WorkingRevision
					&& Replacement->GetGraphChanges().GetEditRevision() == ReplacementRevision));
		}
		auto CoversNativeReferences(const DObject&) const -> bool override { return false; }
		auto GetStrongReferenceCount(const DObject& Object) const -> uint32 override
		{ return (MaterialSession || FunctionSession) && FObjectKey(&Object) == Previous ? 1 : 0; }
		auto Commit() noexcept -> void override
		{
			if (MaterialSession)
			{
				MaterialSession->Source = std::move(Replacement);
				MaterialSession->SourceRevision = ReplacementRevision;
			}
			if (FunctionSession)
			{
				FunctionSession->Source = std::move(FunctionReplacement);
				FunctionSession->RefreshSourceRevision();
			}
			bCommitted = true;
		}
		auto Abort() noexcept -> void override
		{
			MaterialSession.reset();
			FunctionSession.reset();
			Replacement.Reset();
			FunctionReplacement.Reset();
			Previous = {};
		}
		auto CanRetire() const -> bool override { return bCommitted; }
	};
	auto FMaterialEditor::GetOwnedEditingObjects() const -> std::optional<std::vector<DObject*>>
	{
		auto* Source = FunctionSession ? static_cast<DObject*>(FunctionSession->GetSource())
			: EditingSession ? EditingSession->GetSourceMaterial() : nullptr;
		return IsValid(Source) ? std::vector<DObject*>{Source} : std::vector<DObject*>{};
	}

	auto FMaterialEditor::CreateReloadParticipant() -> std::shared_ptr<IObjectReplacementParticipant>
	{
		const auto Owner = std::dynamic_pointer_cast<FMaterialEditor>(weak_from_this().lock());
		return Owner ? std::make_shared<FMaterialEditorReloadParticipant>(Owner) : nullptr;
	}

	FMaterialEditor::FMaterialEditor(
		EMaterialEditorMode InMode)
		: Mode(InMode)
		, ProfileSettings(InMode == EMaterialEditorMode::Function
			? std::make_unique<FMaterialEditorProfileSettings>("MaterialFunctionPreferences.yaml")
			: std::make_unique<FMaterialEditorProfileSettings>())
	{
	}

	FMaterialEditor::~FMaterialEditor()
	{
		SaveFunctionViewport();
		UnregisterUndo();
		ReleaseFunctionSession();
		SettingsPage = {};
		FinishActivePropertyEdit(true);
		ProfileSettings->Save();
		ReleaseMaterialSession();
	}

	auto FMaterialEditor::GetToolkitId() const -> const FAssetEditorToolkitId&
	{
		return Mode == EMaterialEditorMode::Function ? MaterialFunctionWorkspaceType : Workspace::Type;
	}

	auto FMaterialEditor::OnOpenAsset(DObject& Object) -> bool
	{
		if (EditingSession || FunctionSession) return false;
		if (Mode == EMaterialEditorMode::Function)
		{
			auto* Function = Cast<DMaterialFunction>(&Object);
			return Function && ResetFunctionSession(*Function);
		}
		auto* Material = Cast<DMaterial>(&Object);
		return Material && ResetEditingSession(*Material);
	}

	auto FMaterialEditor::OnActivateAsset(DObject&) -> void
	{
		auto* EditingObject = FunctionSession
			? static_cast<DObject*>(FunctionSession->Function()) : GetActiveMaterial();
		EditorSession.Activate(EditingObject);
	}

	auto FMaterialEditor::RequestDeactivate() -> bool
	{
		if (Mode == EMaterialEditorMode::Function) return CancelFunctionInteractions();
		return CancelCanvasInteraction() && FinishActivePropertyEdit(true);
	}

	auto FMaterialEditor::OnCloseAsset(DObject&) -> void
	{
		CaptureCanvasViewport();
		if (FunctionSession) SaveFunctionViewport();
		ReleaseFunctionSession();
		ReleaseMaterialSession();
		ProfileSettings->Save();
	}

	auto FMaterialEditor::OnSaveAsset(DObject&) -> bool
	{
		return OnSaveActiveDocument();
	}

	auto FMaterialEditor::OnDiscardAsset(DObject& Object) -> bool
	{
		if (!RequestDeactivate()) return false;
		if (Mode == EMaterialEditorMode::Function)
		{
			auto* Source = Cast<DMaterialFunction>(&Object);
			if (!Source || !ResetFunctionSession(*Source)) return false;
			OnActivateAsset(*Source);
			return true;
		}
		const auto Session = EditingSession;
		auto* Source = Cast<DMaterial>(&Object);
		if (!Session || !Source || Session->GetSourceMaterial() != Source) return false;
		Session->CancelApply();
		if (!Source->GetPackage()->IsDirty() || Source->GetPackage()->IsNewlyCreated())
		{
			if (!ResetEditingSession(*Source)) return false;
			OnActivateAsset(*Source);
			return true;
		}
		return EditorSession.Discard(Source, {}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto FMaterialEditor::OnPackageReloaded(DPackage*, DPackage* Replacement) -> void
	{
		auto* Source = GetActiveAsset();
		if (!Source || Source->GetPackage() != Replacement || !RequestDeactivate()) return;
		if (auto* Function = Cast<DMaterialFunction>(Source))
		{
			if (!ResetFunctionSession(*Function)) return;
		}
		else if (!ResetEditingSession(*Cast<DMaterial>(Source))) return;
		OnActivateAsset(*Source);
	}

	auto FMaterialEditor::OnRequestCloseAsset(DObject& Asset) -> EAssetEditorCloseResult
	{
		const auto Result = FBaseAssetToolkit::OnRequestCloseAsset(Asset);
		if (Result != EAssetEditorCloseResult::Closed || IsReadOnly()) return Result;
		const bool HasDraft = Mode == EMaterialEditorMode::Function ? FunctionSession && FunctionSession->IsDirty()
			: EditingSession && EditingSession->HasUnappliedChanges();
		return HasDraft ? EAssetEditorCloseResult::PendingConfirmation : EAssetEditorCloseResult::Closed;
	}

	auto FMaterialEditor::OnIsAssetDirty(const DObject&) const -> bool
	{
		if (Mode == EMaterialEditorMode::Function) return FunctionSession && FunctionSession->IsDirty();
		return EditingSession && EditingSession->GetSourceMaterial()
			&& (EditingSession->HasUnappliedChanges() || EditorSession.IsDirty(EditingSession->GetSourceMaterial()));
	}

	auto FMaterialEditor::OnCanSaveActiveDocument() const -> bool
	{
		return EditorSession.CanSave(Mode == EMaterialEditorMode::Function
			? static_cast<DObject*>(FunctionSession ? FunctionSession->GetSource() : nullptr)
			: EditingSession ? EditingSession->GetSourceMaterial() : nullptr);
	}

	auto FMaterialEditor::OnSaveActiveDocument() -> bool
	{
		return Mode == EMaterialEditorMode::Function ? SaveFunction() : SaveMaterial();
	}

	auto FMaterialEditor::OnDrawToolkit(bool bActive) -> bool
	{
		if (Mode == EMaterialEditorMode::Function) return DrawFunction(bActive);
		if (!bActive) RequestDeactivate();
		const auto Session = EditingSession;
		if (Session && !IsValid(Session->GetSourceMaterial()))
		{
			CloseWindow(EAssetEditorCloseReason::AssetUnloadingOrInvalid);
			return false;
		}
		if (Session)
		{
			std::string Error;
			Session->Tick(Error);
			if (!Error.empty()) SetError(std::move(Error));
		}
		if (!IsEditorOpen() || !Session || Session != EditingSession || !GetActiveMaterial()) return false;
		return DrawAssetTabs(Workspace::RootKey,
			[&](DObject& Source) {
				if (Session == EditingSession) DrawDocument(Source.GetObjectPath(), Session->GetWorkingMaterial());
			},
			[&](DObject&, bool bVisible) {
				if (bVisible) return;
				CancelCanvasInteraction();
				if (Session->Preview) Session->Preview->SetVisible(false);
				const auto DockType = Workspace::MakeDockType(GetPresentationRootKey("Document"));
				if (ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::LayoutVersion)))
					TabLayout::SubmitDockSpace(DockType, Workspace::LayoutVersion,
						{0.0f, 0.0f}, ImGuiDockNodeFlags_KeepAliveOnly);
			});
	}

	auto FMaterialEditor::ResetLayout() -> void
	{
		if (Mode == EMaterialEditorMode::Function) { ResetFunctionLayout(); return; }
		ProfileSettings->bPreviewVisible = true;
		ProfileSettings->bDetailsVisible = true;
		ProfileSettings->bParametersVisible = true;
		ProfileSettings->bDiagnosticsVisible = false;
		bPendingLayoutReset = true;
	}

	auto FMaterialEditor::ResetEditingSession(DMaterial& Source) -> bool
	{
		const auto Previous = EditingSession;
		if (!RequestDeactivate() || Previous != EditingSession) return false;
		CaptureCanvasViewport();
		auto Session = std::make_shared<FMaterialEditingSession>();
		std::string Error;
		if (!Session->Initialize(Source, ProfileSettings->bAutoCompile
			? EMaterialEditCompileMode::Automatic : EMaterialEditCompileMode::Manual, Error,
			EditorSession.GetEditor() ? EditorSession.GetEditor()->GetTransactor() : nullptr))
		{
			SetError(std::move(Error));
			return false;
		}
		if (Previous != EditingSession || IsClosed()) return false;
		ReleaseMaterialSession();
		EditingSession = std::move(Session);
		if (Previous && Previous->Canvas && Previous->GetSourceMaterial() == &Source)
			GetOrCreateCanvas().RestoreSelection(Previous->Canvas->GetSelection());
		return true;
	}

	auto FMaterialEditor::ReleaseMaterialSession() -> void
	{
		if (auto Session = std::exchange(EditingSession, {}))
		{
			Session->CancelApply();
			if (Session->Preview) Session->Preview->SetVisible(false);
		}
	}

	auto FMaterialEditor::GetActiveMaterial() const -> DMaterialInterface*
	{
		return EditingSession ? EditingSession->GetWorkingMaterial() : nullptr;
	}

	auto FMaterialEditor::SaveMaterial() -> bool
	{
		const auto Session = EditingSession;
		if (!FinishActivePropertyEdit(false) || !CancelCanvasInteraction()
			|| !IsEditorOpen() || EditingSession != Session) return false;
		auto* Source = Session ? Session->GetSourceMaterial() : nullptr;
		if (Session)
		{
			std::string Error;
			if (!Session->FinishAndApply(Error))
			{
				SetError(std::move(Error));
				return false;
			}
			if (!IsEditorOpen() || EditingSession != Session || Session->GetSourceMaterial() != Source) return false;
		}
		return EditorSession.Save(Source, {}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto FMaterialEditor::OnCanUndo() const -> bool
	{
		return EditorSession.CanUndo();
	}

	auto FMaterialEditor::OnCanRedo() const -> bool
	{
		return EditorSession.CanRedo();
	}

	auto FMaterialEditor::GetUndoDescription() const -> std::string
	{
		return EditorSession.GetUndoDescription();
	}

	auto FMaterialEditor::GetRedoDescription() const -> std::string
	{
		return EditorSession.GetRedoDescription();
	}

	auto FMaterialEditor::OnUndo() -> bool
	{
		if (Mode == EMaterialEditorMode::Function && !RequestDeactivate()) return false;
		const bool Result = EditorSession.Undo();
		if (auto* Editor = EditorSession.GetEditor()) Editor->GetTransactor()->DispatchEvents();
		return Result;
	}

	auto FMaterialEditor::OnRedo() -> bool
	{
		if (Mode == EMaterialEditorMode::Function && !RequestDeactivate()) return false;
		const bool Result = EditorSession.Redo();
		if (auto* Editor = EditorSession.GetEditor()) Editor->GetTransactor()->DispatchEvents();
		return Result;
	}

	auto FMaterialEditor::DrawDocument(std::string_view Resource, DMaterialInterface* Material) -> void
	{
		const auto Session = EditingSession;
		DrawToolbar(Material);
		if (!IsEditorOpen() || EditingSession != Session) return;
		ImGui::Spacing();

		DrawDockLayout(Resource, Material);
		if (!IsEditorOpen() || EditingSession != Session) return;
		PropertyView.EndInteractionFrame(MakePropertyViewContext());
		if (Session && Session->Canvas && EditingSession == Session) Session->Canvas->EndParameterFrame();

		MonaImGui::ErrorDialog("Material Editor Error", ErrorMessage);
	}

	auto FMaterialEditor::DrawToolbar(DMaterialInterface* Material) -> void
	{
		if (ImGui::Button("Save")) SaveMaterial();
		if (!IsEditorOpen()) return;
		ImGui::SameLine();
		if (Material)
		{
			if (auto* Base = Cast<DMaterial>(Material))
			{
				const auto Session = EditingSession;
				if (Session)
				{
					ImGui::BeginDisabled(!Session->HasUnappliedChanges() || Session->IsApplyPending());
					if (ImGui::Button(Session->IsApplyPending() ? "Applying..." : "Apply")
						&& FinishActivePropertyEdit(false))
					{
						if (CancelCanvasInteraction() && IsEditorOpen() && EditingSession == Session)
						{
							std::string Error;
							if (!Session->RequestApply(Error)) SetError(std::move(Error));
						}
					}
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply the preview changes to the source material and scene.");
					ImGui::SameLine();
				}
				if (ImGui::Button("Compile") && FinishActivePropertyEdit(false)) Base->CompileEdits();
				if (Session && Session->HasUnappliedChanges())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("Unapplied changes");
				}
			}
			const FMaterialCompileStatus& Status = Material->GetMaterialCompileStatus();
			const bool bPending = Material->IsCompiling() || Status.HasScheduledEdits();
			if (bPending)
			{
				ImGui::SameLine();
				if (ImGui::Button("Cancel Compile"))
				{
					if (const auto Session = EditingSession) Session->CancelApply();
					FObjectCacheContext Context;
				for (auto* Owner : Context.GetMaterialsAffectedByMaterial(Material))
						if (IsValid(Owner))
							FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owner);
				}
			}
			if (Status.State == EMaterialCompileState::Running)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("Compiling material...");
			}
			const auto* Source = EditingSession ? EditingSession->GetSourceMaterial() : nullptr;
			if (Source && Source != Material && Source->GetMaterialCompileStatus().State == EMaterialCompileState::Running)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("Compiling scene material...");
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Window")) ImGui::OpenPopup("MaterialWindows");
		if (ImGui::BeginPopup("MaterialWindows"))
		{
			ImGui::MenuItem("Preview", nullptr, &ProfileSettings->bPreviewVisible);
			ImGui::MenuItem("Details", nullptr, &ProfileSettings->bDetailsVisible);
			ImGui::MenuItem("Parameters", nullptr, &ProfileSettings->bParametersVisible);
			ImGui::MenuItem("Diagnostics", nullptr, &ProfileSettings->bDiagnosticsVisible);
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout")) ResetLayout();
			ImGui::Separator();
			if (ImGui::MenuItem("Material Editor Preferences..."))
				FEditorSettingsRegistry::Get().RequestOpen(GetPreferencesPageId());
			ImGui::EndPopup();
		}
	}

	auto FMaterialEditor::DrawDockLayout(
		std::string_view Resource, DMaterialInterface* Material) -> void
	{
		const auto Session = EditingSession;
		const auto IsCurrent = [&] { return IsEditorOpen() && EditingSession == Session; };
		const auto DockType = Workspace::MakeDockType(GetPresentationRootKey("Document"));
		static constexpr std::string_view PanelIds[]{"Graph", "Preview", "Details", "Parameters", "Diagnostics"};
		RestoreDockLayout("Document", DockType, Workspace::LayoutVersion, PanelIds);
		const ImGuiID DockSpaceId = TabLayout::MakeDockSpaceId(DockType, Workspace::LayoutVersion);
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		if (Size.x <= 0.0f || Size.y <= 0.0f) return;
		const bool bReset = std::exchange(bPendingLayoutReset, false);
		if (!ImGui::DockBuilderGetNode(DockSpaceId) || bReset)
			Workspace::BuildDefaultLayout(DockType, Size);
		TabLayout::SubmitDockSpace(DockType, Workspace::LayoutVersion, Size);

		const auto BeginPanel = [&](const char* Label, const char* Key, bool* Open = nullptr) {
			return TabLayout::BeginDockablePanel(
				DockType, Label, Key, Open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
		};
		if (BeginPanel("Material Graph", "Graph")) DrawGraphPanel(Resource, 0.0f);
		ImGui::End();
		if (!IsCurrent()) return;
		if (ProfileSettings->bPreviewVisible)
		{
			const bool bPreviewShown = BeginPanel("Preview", "Preview", &ProfileSettings->bPreviewVisible);
			if (bPreviewShown) DrawPreviewPanel(Material, 0.0f);
			else if (const auto Preview = Session->Preview)
				Preview->SetVisible(false);
			ImGui::End();
		}
		else if (const auto Preview = Session->Preview)
			Preview->SetVisible(false);
		if (!IsCurrent()) return;
		if (ProfileSettings->bDetailsVisible)
		{
			if (BeginPanel("Details", "Details", &ProfileSettings->bDetailsVisible))
				DrawDetailsPanel(Resource, Material);
			ImGui::End();
		}
		if (!IsCurrent()) return;
		if (ProfileSettings->bParametersVisible)
		{
			if (BeginPanel("Parameters", "Parameters", &ProfileSettings->bParametersVisible))
				DrawParametersPanel(Resource, Material);
			ImGui::End();
		}
		if (!IsCurrent()) return;
		if (ProfileSettings->bDiagnosticsVisible)
		{
			if (BeginPanel("Diagnostics", "Diagnostics", &ProfileSettings->bDiagnosticsVisible))
				DrawMaterialCompileStatus(Material, Session->GetSourceMaterial(),
					[this](const auto& Diagnostic) { NavigateToFunctionNode(*this, Diagnostic.FunctionAssetPath, Diagnostic.NodeId); },
					[&](const auto& Diagnostic) {
						if (!IsCurrent()) return;
						GetOrCreateCanvas().SelectAndFrameDiagnostic(Diagnostic);
						const auto GraphName = TabLayout::MakePanelWindowName("Material Graph", DockType, "Graph");
						ImGui::SetWindowFocus(GraphName.c_str());
					});
			ImGui::End();
		}
		if (IsCurrent()) SaveDockLayout("Document", DockType, Workspace::LayoutVersion, PanelIds);
	}

	auto FMaterialEditor::DrawPreviewPanel(DMaterialInterface* Material, float Height) -> void
	{
		auto& Preview = EditingSession->Preview;
		if (Preview == nullptr) Preview = std::make_shared<FMaterialPreview>(GetWindowId());
		// A close callback may release the toolkit's preview while this draw unwinds.
		const auto DrawingPreview = Preview;
		const auto* Canvas = EditingSession->Canvas.get();
		if (Canvas && Canvas->GetSelection().contains(EMaterialGraphEndpoint::MaterialRoot))
			DrawingPreview->DrawRootInputs(Material, Height, Canvas->GetRootInputDraft());
		else
			DrawingPreview->Draw(Material, Height);
	}

	auto FMaterialEditor::DrawGraphPanel(std::string_view Resource, float Height) -> void
	{
		if (!EditorSession.GetEditor())
		{
			ImGui::TextDisabled("Material graph transactions are unavailable.");
			return;
		}
		FMaterialGraphCanvas& Canvas = GetOrCreateCanvas();
		Canvas.Draw(*EditorSession.GetEditor()->GetTransactor(), Height);
		const auto [Zoom, Pan] = Canvas.GetViewport();
		ProfileSettings->SetViewport(Resource, {.Zoom = Zoom, .Pan = Pan});
	}

	auto FMaterialEditor::DrawSelectedFunction(std::string_view Resource, DMaterial* Base) -> void
	{
		auto& Canvas = GetOrCreateCanvas();
		for (const auto& Selected : Canvas.GetSelection())
			if (const auto* Id = std::get_if<FGuid>(&Selected))
				for (const auto& Expression : Base->GetExpressionCollection().Expressions)
					if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get());
						Call && Call->Id == *Id && Call->Function.IsValid())
					{
						ImGui::PushID(Id->ToString().c_str());
						if (ImGui::Button("Open Function")) OpenReferencedMaterialAsset(*this, Call->Function->GetObjectPath(),
							Call->Function->GetClass()->GetQualifiedName().ToString());
						ImGui::PopID();
					}
	}

	auto FMaterialEditor::DrawParametersPanel(
		std::string_view Resource, DMaterialInterface* Material) -> void
	{
		auto* Base = Cast<DMaterial>(Material);
		if (!Base || !EditorSession.GetEditor() || !EditorSession.GetEditor()->GetTransactor()) return;
		auto& Search = ParameterSearchText;
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##ParameterSearch", "Search parameters...", Search.data(), Search.size());
		const ImGuiTextFilter Filter(Search.data());
		// Include disconnected owners; the instance model intentionally filters by reachability.
		std::vector<FMaterialParameterPanelEntry> Entries;
		for (const auto& Definition : Base->GetParameterDefinitions())
		{
			const auto Searchable = std::format("{} {} {}", Definition.Name,
				Definition.DisplayName, Definition.GroupName);
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
		auto& Canvas = GetOrCreateCanvas();
		bool bSubmitted = false;
		const auto Submit = [&](const FMaterialGraphCommandResult& Result) {
			bSubmitted = true;
			if (!Result) SetError(FormatMaterialGraphCommandResult(Result));
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
			const auto Flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow
				| (bSelected ? ImGuiTreeNodeFlags_Selected : ImGuiTreeNodeFlags_None);
			const bool bOpen = ImGui::TreeNodeEx("Parameter", Flags, "%s", Parameter.Name.ToString().c_str());
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				Canvas.SelectAndFrame(NodeIds.front());
			if (bOpen)
			{
				const auto Commit = [&]() {
					const auto& Expressions = Base->GetExpressionCollection().Expressions;
					const auto It = std::ranges::find(Expressions, NodeIds.front(), [](const auto& E) { return E->Id; });
					if (It == Expressions.end()) return;
					if (const auto* Expression = Cast<DMaterialExpressionParameter>(It->Get()))
						Submit(Canvas.ReplaceParameterDefinition(*Expression, Parameter));
				};
				if (MonaImGui::PropertyEdit::BeginTable("ParameterMetadata", DetailsStyle::MakeTableConfig()))
				{
					using EField = FMaterialGraphCanvas::EParameterTextField;
					Canvas.DrawParameterText("Name", EField::Name, Parameter.Id, Parameter.Name.ToString());
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rename all references while preserving material instance overrides.");
					Canvas.DrawParameterText("Display name", EField::DisplayName, Parameter.Id, Parameter.DisplayName);
					Canvas.DrawParameterText("Group", EField::Group, Parameter.Id, Parameter.GroupName.ToString());
					int SortOrder = Parameter.SortOrder;
					if (!bSubmitted && DetailsStyle::EditRow("Order", [&] {
						return ImGui::InputInt("##Value", &SortOrder, 0, 0) && DetailsStyle::NumericInputFinished();
					}))
					{
						Parameter.SortOrder = SortOrder;
						Commit();
					}
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
							if (DetailsStyle::EditRow("Min / Max", [&] {
								return ImGui::InputFloat2("##Value", Range, "%.4g") && DetailsStyle::NumericInputFinished();
							}))
							{
								Parameter.MinimumValue = Range[0]; Parameter.MaximumValue = Range[1]; Commit();
							}
						}
					}
					if (!bSubmitted) Canvas.DrawParameterValue(Parameter);
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

	auto FMaterialEditor::DrawDetailsPanel(
		std::string_view Resource, DMaterialInterface* Material) -> void
	{
		PropertyView.DrawInteractionError();
		auto* BaseMaterial = Cast<DMaterial>(Material);
		if (!BaseMaterial) return;
		auto& Canvas = GetOrCreateCanvas();
		const auto& Selection = Canvas.GetSelection();
		const bool bShowMaterialDetails = Selection.empty() || Selection.contains(EMaterialGraphEndpoint::MaterialRoot);
		if (bShowMaterialDetails)
		{
			ImGui::PushTextWrapPos(0.0f);
			MonaImGui::TextName(Material->GetFName());
			ImGui::PopTextWrapPos();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Resource.data());
			ImGui::TextDisabled("%s", BaseMaterial->GetDomain() == EMaterialDomain::PostProcess ? "PostProcess Material" : "Surface Material");
			ImGui::Spacing();
		}
		if (bShowMaterialDetails
			&& MonaImGui::PropertyEdit::BeginTable("SurfaceProperties", DetailsStyle::MakeTableConfig()))
		{
			ImGui::BeginDisabled(!BaseMaterial->CanEditMaterialAttributesMode());
			bool bAttributes = BaseMaterial->UsesMaterialAttributes();
			if (DetailsStyle::EditRow("Use material attributes", [&] { return ImGui::Checkbox("##Value", &bAttributes); }))
			{
				const auto Result = MaterialEditing::SetUseMaterialAttributes(*BaseMaterial, bAttributes);
				if (!Result) SetError(FormatMaterialGraphCommandResult(Result));
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Switch output inputs. Both modes retain their connections. Only the selected mode is used.");
			ImGui::EndDisabled();
			FMaterialConfiguration Configuration{BaseMaterial->GetDomain(), BaseMaterial->GetStaticProperties()};
			int Domain = static_cast<int>(Configuration.Domain);
			bool bChanged = DetailsStyle::EditRow("Domain", [&] { return ImGui::Combo("##Value", &Domain, "Surface\0PostProcess\0"); });
			Configuration.Domain = static_cast<EMaterialDomain>(Domain);
			if (bChanged && Configuration.Domain == EMaterialDomain::PostProcess)
			{
				Configuration.Properties.ShadingModel = EMaterialShadingModel::Unlit;
				Configuration.Properties.BlendMode = EMaterialBlendMode::Opaque;
				Configuration.Properties.DepthWritePolicy = EMaterialDepthWritePolicy::Disabled;
			}
			auto& Properties = Configuration.Properties;
			int Shading = static_cast<int>(Properties.ShadingModel);
			int Blend = static_cast<int>(Properties.BlendMode);
			int Depth = static_cast<int>(Properties.DepthWritePolicy);
			bChanged |= DetailsStyle::EditRow("Shading", [&] { return ImGui::Combo("##Value", &Shading, "DefaultLit\0Unlit\0ClearCoat\0Single Layer Water\0"); });
			bChanged |= DetailsStyle::EditRow("Blend", [&] { return ImGui::Combo("##Value", &Blend, "Opaque\0Masked\0Translucent\0"); });
			Properties.ShadingModel = static_cast<EMaterialShadingModel>(Shading);
			Properties.BlendMode = static_cast<EMaterialBlendMode>(Blend);
			if (Properties.ShadingModel == EMaterialShadingModel::SingleLayerWater)
			{
				int Refraction = static_cast<int>(Properties.RefractionMethod);
				bChanged |= DetailsStyle::EditRow("Refraction", [&] {
					return ImGui::Combo("##Value", &Refraction, "None\0Index of Refraction\0Index of Refraction from F0\0Pixel Normal Offset\0");
				});
				Properties.RefractionMethod = static_cast<EMaterialRefractionMethod>(Refraction);
				if (Properties.RefractionMethod != EMaterialRefractionMethod::None)
					bChanged |= DetailsStyle::EditRow("Refraction depth bias (cm)", [&] {
						return ImGui::InputFloat("##Value", &Properties.RefractionDepthBias, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
					});
			}
			if (Properties.BlendMode == EMaterialBlendMode::Masked)
			{
				float Threshold = Properties.OpacityMaskThreshold;
				if (DetailsStyle::EditRow("Mask cutoff", [&] {
					return ImGui::InputFloat("##Value", &Threshold, 0, 0, "%.3f") && DetailsStyle::NumericInputFinished();
				}))
				{
					Properties.OpacityMaskThreshold = Threshold;
					bChanged = true;
				}
			}
			bChanged |= DetailsStyle::EditRow("Two sided", [&] { return ImGui::Checkbox("##Value", &Properties.bTwoSided); });
			bChanged |= DetailsStyle::EditRow("Depth write", [&] { return ImGui::Combo("##Value", &Depth, "Automatic\0Enabled\0Disabled\0"); });
			Properties.DepthWritePolicy = static_cast<EMaterialDepthWritePolicy>(Depth);
			MonaImGui::PropertyEdit::EndTable();
			if (bChanged)
			{
				const auto Result = MaterialEditing::SetConfiguration(*BaseMaterial, Configuration);
				if (!Result) SetError(FormatMaterialGraphCommandResult(Result));
			}
		}
		if (EditorSession.GetEditor() && EditorSession.GetEditor()->GetTransactor())
		{
			DrawSelectedFunction(Resource, BaseMaterial);
			if (GetActiveMaterial() == Material && IsEditorOpen())
				Canvas.DrawSelectionDetails();
		}
	}

	auto FMaterialEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const FPropertyViewContext Context = MakePropertyViewContext();
		if (!PropertyView.FinishActiveEdit(&Context, bCancel)) return false;
		const auto Session = EditingSession;
		return !Session || !Session->Canvas || Session->Canvas->FinishDetailsEdit(bCancel);
	}

	auto FMaterialEditor::MakePropertyViewContext() -> FPropertyViewContext
	{
		return {
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
		};
	}

	auto FMaterialEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}



	auto FMaterialEditor::CancelCanvasInteraction() -> bool
	{
		const auto Session = EditingSession;
		return !Session || !Session->Canvas || Session->Canvas->CancelInteraction();
	}

	auto FMaterialEditor::GetOrCreateCanvas() -> FMaterialGraphCanvas&
	{
		auto& Canvas = EditingSession->Canvas;
		if (!Canvas)
		{
			Canvas = std::make_unique<FMaterialGraphCanvas>(*EditingSession->Graph,
				FMaterialGraphEditorServices{
					.ReportError = [this](std::string Message) { SetError(std::move(Message)); },
					.OpenFunction = [this](std::string_view Path) { OpenReferencedMaterialAsset(*this, Path, DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); },
					.RootInputCommitted = [Session = std::weak_ptr(EditingSession)](uint64 PreviousRevision) {
						if (const auto Document = Session.lock(); Document && Document->Preview)
							Document->Preview->OnRootInputCommitted(PreviousRevision, Document->GetWorkingMaterial()->GetMaterialContentRevision());
					}});
			if (const auto* State = ProfileSettings->FindViewport(EditingSession->GetSourceMaterial()->GetObjectPath()))
				Canvas->SetViewport(State->Zoom, State->Pan);
		}
		return *Canvas;
	}

	auto FMaterialEditor::CaptureCanvasViewport() -> void
	{
		const auto* Source = EditingSession ? EditingSession->GetSourceMaterial() : nullptr;
		if (!Source || !EditingSession || !EditingSession->Canvas) return;
		const auto [Zoom, Pan] = EditingSession->Canvas->GetViewport();
		ProfileSettings->SetViewport(Source->GetObjectPath(), {.Zoom = Zoom, .Pan = Pan});
	}
	auto FMaterialEditor::OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void
	{
		if (FunctionSession && FunctionSession->GetSource()
			&& FunctionSession->GetSource()->GetObjectPath() == Destination) FunctionSession->RefreshSourceRevision();
		ProfileSettings->MoveViewport(Source, Destination);
		ProfileSettings->Save();
	}

	auto FMaterialEditor::GetLayoutVersion() const -> uint32
	{ return Mode == EMaterialEditorMode::Function ? Workspace::FunctionLayoutVersion : Workspace::LayoutVersion; }
	auto FMaterialEditor::PostInitAssetEditor() -> void
	{
		FBaseAssetToolkit::PostInitAssetEditor();
		EditorSession.BindEditor(GetAssetEditorSubsystem()->GetEditor());
		if (const auto* Owner = GetOwningAssetEditor())
			ProfileSettings->SelectStorage("MaterialEditor/Profiles/" + FXxHash128::HashBuffer(GetLayoutProfileKey()).ToString() + ".yaml");
		RegisterUndo();
		if (Mode == EMaterialEditorMode::Function) return;
		SettingsPage = FEditorSettingsPageRegistration({
			.Id = GetPreferencesPageId(), .Label = "Material Editor" + (GetOwningAssetEditor()->GetInstanceKey().empty() ? std::string{} : " [" + std::string(GetOwningAssetEditor()->GetInstanceKey()) + "]"),
			.Keywords = "auto compile preview details parameters diagnostics panels",
			.Description = "Compilation and panel preferences for this editing session.",
			.Draw = [Weak = weak_from_this()](std::string& Error) {
				if (auto Toolkit = std::dynamic_pointer_cast<FMaterialEditor>(Weak.lock()); Toolkit && Toolkit->IsEditorOpen()) Toolkit->DrawPreferences(Error);
			},
			.Reset = [Weak = weak_from_this()] {
				const auto Toolkit = std::dynamic_pointer_cast<FMaterialEditor>(Weak.lock());
				return Toolkit && Toolkit->IsEditorOpen() && Toolkit->ResetPreferences();
			}});
	}
	auto FMaterialEditor::OnClose() -> void
	{
		RequestDeactivate();
		SaveFunctionViewport();
		UnregisterUndo();
		ReleaseFunctionSession();
		SettingsPage = {};
		CaptureCanvasViewport();
		ReleaseMaterialSession();
		FBaseAssetToolkit::OnClose();
	}
	auto FMaterialEditor::GetPreferencesPageId() const -> std::string
	{
		const auto* Owner = GetOwningAssetEditor();
		return Owner ? "editor.material." + FXxHash128::HashBuffer(GetPresentationRootKey("MaterialEditor")).ToString() : "editor.material";
	}

}
