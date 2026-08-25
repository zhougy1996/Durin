#include "Widgets/MMaterialEditor.h"
#include "Widgets/MaterialParameterPanelModel.h"
#include "Widgets/MaterialPreview.h"

#include "AssetAuthoring.h"
#include "DObject/Package.h"
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
		constexpr float DefaultSidebarRatio = 0.40f;
		constexpr float DefaultPreviewPaneRatio = 0.68f;
		constexpr float WideLayoutMinimumWidth = 760.0f;
		constexpr float MinimumSidebarWidth = 280.0f;
		constexpr float MinimumDetailsWidth = 360.0f;
		constexpr float MinimumPreferredPreviewWidth = 420.0f;
		constexpr float MaximumPreferredPreviewWidth = 640.0f;
		constexpr float MinimumPreviewHeight = 260.0f;
		constexpr float MinimumOverviewHeight = 110.0f;
		constexpr float NarrowPreviewHeight = 340.0f;
		constexpr float MaximumMaterialValueColumnWidthInEm = 34.0f;
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
			MonaImGui::PropertyEdit::FTableConfig Config;
			Config.MaximumValueColumnWidthInEm = MaximumMaterialValueColumnWidthInEm;
			return Config;
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
			Model = std::make_unique<FMaterialParameterPanelModel>(Material);
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

	MMaterialEditor::MMaterialEditor(::Durin::Editor::FWorkspaceManager& InWorkspaceManager)
		: WorkspaceManager(InWorkspaceManager)
		, MaterialParameterPanelCache(std::make_unique<FMaterialParameterPanelCache>())
	{
	}

	MMaterialEditor::~MMaterialEditor()
	{
		FinishActivePropertyEdit(true);
		MaterialPreviews.clear();
	}

	auto MMaterialEditor::GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId&
	{
		return Workspace::Type;
	}

	auto MMaterialEditor::OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return ::Durin::Editor::EDocumentOpenResult::Rejected;
		if (FindOpenMaterial(Document.ResourceId)) return ::Durin::Editor::EDocumentOpenResult::Opened;
		FAssetPath AssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Document.ResourceId, AssetPath, &PathError))
		{
			SetError(std::move(PathError));
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		DMaterialInterface* Material = nullptr;
		const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Material);
		if (!Result || !Material)
		{
			SetError(Result ? "The selected asset is not a material." : Result.Message);
			return ::Durin::Editor::EDocumentOpenResult::Rejected;
		}
		OpenMaterials.emplace(Document.ResourceId, Material);
		return ::Durin::Editor::EDocumentOpenResult::Opened;
	}

	auto MMaterialEditor::ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void
	{
		DMaterialInterface* Material = FindOpenMaterial(Document.ResourceId);
		if (PropertyView.IsEditing() && !PropertyView.IsEditingObject(Material) && !FinishActivePropertyEdit(true)) return;
		Documents.Activate(Document, Material);
	}

	auto MMaterialEditor::RequestDeactivate() -> bool
	{
		return FinishActivePropertyEdit(true);
	}

	auto MMaterialEditor::RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult
	{
		if (PropertyView.IsEditingObject(FindOpenMaterial(Document.ResourceId)) && !FinishActivePropertyEdit(true))
			return ::Durin::Editor::EDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return ::Durin::Editor::EDocumentCloseResult::PendingConfirmation;
		OpenMaterials.erase(Document.ResourceId);
		MaterialPreviews.erase(Document.Id.Value);
		Documents.Close(Document.ResourceId);
		return ::Durin::Editor::EDocumentCloseResult::Closed;
	}

	auto MMaterialEditor::SaveDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		return SaveMaterial(FindOpenMaterial(Document.ResourceId));
	}

	auto MMaterialEditor::DiscardDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool
	{
		return Documents.Discard(FindOpenMaterial(Document.ResourceId));
	}

	auto MMaterialEditor::IsDocumentDirty(const ::Durin::Editor::FDocumentTab& Document) const -> bool
	{
		return Documents.IsDirty(FindOpenMaterial(Document.ResourceId));
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
			[this](const ::Durin::Editor::FDocumentTab& Document) {
				if (const auto PreviewIt = MaterialPreviews.find(Document.Id.Value); PreviewIt != MaterialPreviews.end())
					PreviewIt->second->SetVisible(false);
			}
		);
	}

	auto MMaterialEditor::ResetLayout() -> void
	{
		SidebarRatio = DefaultSidebarRatio;
		PreviewPaneRatio = DefaultPreviewPaneRatio;
		bUsePreferredPreviewWidth = true;
	}

	auto MMaterialEditor::FindOpenMaterial(std::string_view ResourceId) const -> DMaterialInterface*
	{
		const auto It = OpenMaterials.find(std::string(ResourceId));
		return It == OpenMaterials.end() ? nullptr : It->second.Get();
	}

	auto MMaterialEditor::GetActiveMaterial() const -> DMaterialInterface*
	{
		return FindOpenMaterial(Documents.GetActiveResourceId());
	}

	auto MMaterialEditor::SaveMaterial(DMaterialInterface* Material) -> bool
	{
		return Documents.Save(Material, {}, [this](std::string Message) {
			SetError(std::move(Message));
		});
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
		DrawToolbar(Document, Material);
		ImGui::Spacing();

		if (ImGui::GetContentRegionAvail().x >= MonaImGui::ScaleUI(WideLayoutMinimumWidth))
			DrawWideLayout(Document, Material);
		else
			DrawNarrowLayout(Document, Material);

		if (Documents.GetActiveResourceId() != Document.ResourceId) return;
		MonaImGui::ErrorDialog("Material Editor Error", ErrorMessage);
	}

	auto MMaterialEditor::DrawToolbar(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		if (ImGui::Button("Save")) SaveMaterial(Material);
		ImGui::SameLine();
		if (DMaterial* Base = FindCompiledBase(Material))
		{
			const FMaterialCompileStatus& Status = Base->GetMaterialCompileStatus();
			const bool bPending = Status.State == EMaterialCompileState::Pending
				|| Status.State == EMaterialCompileState::Running;
			if (bPending)
			{
				if (ImGui::Button("Cancel Compile")) CancelMaterialCompile(*Base);
			}
			else if (ImGui::Button(Status.State == EMaterialCompileState::Failed
				|| Status.State == EMaterialCompileState::Rejected
					? "Retry Compile" : "Recompile"))
			{
				RequestMaterialRecompile(*Base, true);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%s%s", FormatCompileState(Status.State),
				Status.bLastKnownGoodDisplayed ? " (showing last known good)" : "");
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Material");
		ImGui::SameLine();
		ImGui::TextUnformatted(Document.ResourceId.c_str());
	}

	auto MMaterialEditor::DrawWideLayout(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		const ImVec2 Available = ImGui::GetContentRegionAvail();
		const float UsableHeight = std::max(Available.y - Metrics.SplitterThickness, 0.0f);
		const float ScaledMinimumSidebarWidth = MonaImGui::ScaleUI(MinimumSidebarWidth);
		const float ScaledMinimumDetailsWidth = MonaImGui::ScaleUI(MinimumDetailsWidth);
		const float MaximumSidebarWidth = std::max(
			ScaledMinimumSidebarWidth,
			Available.x - Metrics.SplitterThickness - ScaledMinimumDetailsWidth);
		const float PreferredPreviewWidth = std::clamp(
			UsableHeight * PreviewPaneRatio,
			MonaImGui::ScaleUI(MinimumPreferredPreviewWidth),
			MonaImGui::ScaleUI(MaximumPreferredPreviewWidth));
		const float DesiredSidebarWidth = bUsePreferredPreviewWidth
			? PreferredPreviewWidth
			: Available.x * SidebarRatio;
		const float SidebarWidth = std::clamp(
			DesiredSidebarWidth, ScaledMinimumSidebarWidth, MaximumSidebarWidth);
		if (bUsePreferredPreviewWidth)
		{
			SidebarRatio = SidebarWidth / Available.x;
			bUsePreferredPreviewWidth = false;
		}

		if (ImGui::BeginChild("MaterialEditorSidebar", ImVec2(SidebarWidth, Available.y)))
		{
			const float ScaledMinimumPreviewHeight = MonaImGui::ScaleUI(MinimumPreviewHeight);
			const float ScaledMinimumOverviewHeight = MonaImGui::ScaleUI(MinimumOverviewHeight);
			if (UsableHeight >= ScaledMinimumPreviewHeight + ScaledMinimumOverviewHeight)
			{
				const float PreviewHeight = std::clamp(
					UsableHeight * PreviewPaneRatio,
					ScaledMinimumPreviewHeight,
					UsableHeight - ScaledMinimumOverviewHeight);

				DrawPreviewPanel(Document, Material, PreviewHeight);
				MonaImGui::DrawSplitter(
					"MaterialEditorPreviewSplitter",
					MonaImGui::EUISplitterAxis::Y,
					ImGui::GetContentRegionAvail().x,
					UsableHeight,
					ScaledMinimumPreviewHeight,
					ScaledMinimumOverviewHeight,
					PreviewPaneRatio);
				DrawOverviewPanel(Document, Material, 0.0f);
			}
			else
			{
				DrawPreviewPanel(Document, Material, 0.0f);
			}
		}
		ImGui::EndChild();
		ImGui::SameLine();
		MonaImGui::DrawSplitter(
			"MaterialEditorSidebarSplitter",
			MonaImGui::EUISplitterAxis::X,
			Available.y,
			Available.x,
			ScaledMinimumSidebarWidth,
			ScaledMinimumDetailsWidth,
			SidebarRatio);
		ImGui::SameLine();
		DrawDetailsPanel(Material, Available.y);
	}

	auto MMaterialEditor::DrawNarrowLayout(const ::Durin::Editor::FDocumentTab& Document, DMaterialInterface* Material) -> void
	{
		DrawPreviewPanel(
			Document,
			Material,
			MonaImGui::ScaleUI(NarrowPreviewHeight));
		ImGui::Spacing();
		DrawOverviewPanel(
			Document,
			Material,
			MonaImGui::ScaleUI(MinimumOverviewHeight));
		ImGui::Spacing();
		DrawDetailsPanel(Material, 0.0f);
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

	auto MMaterialEditor::DrawOverviewPanel(
		const ::Durin::Editor::FDocumentTab& Document,
		DMaterialInterface* Material,
		float Height
	) -> void
	{
		if (ImGui::BeginChild("MaterialOverview", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
		{
			ImGui::SeparatorText("Material Overview");
			ImGui::TextDisabled("Asset");
			ImGui::TextWrapped("%s", Document.ResourceId.c_str());
			ImGui::Spacing();
			ImGui::TextDisabled("Type");
			ImGui::TextUnformatted(Material->GetClass()->GetQualifiedName().ToString().c_str());
			DrawCompileStatus(Material);
		}
		ImGui::EndChild();
	}

	auto MMaterialEditor::DrawCompileStatus(DMaterialInterface* Material) -> void
	{
		DMaterial* Base = FindCompiledBase(Material);
		if (!Base)
		{
			ImGui::TextDisabled("Compiled program: unavailable");
			return;
		}
		const FMaterialCompileStatus& Status = Base->GetMaterialCompileStatus();
		ImGui::Text("Compile: %s", FormatCompileState(Status.State));
		ImGui::Text("Freshness: %s", Status.IsCurrent() ? "current" : "stale");
		ImGui::Text("Cache: %s", FormatCacheOutcome(Status.CacheOutcome));
		ImGui::Text("Target: %s", Status.Target.empty() ? "n/a" : Status.Target.c_str());
		ImGui::Text("Generation: %llu", static_cast<unsigned long long>(Status.RequestGeneration));
		if (Status.DurationMicroseconds != 0)
			ImGui::Text("Duration: %.2f ms",
				static_cast<double>(Status.DurationMicroseconds) / 1000.0);
		if (Status.bLastKnownGoodDisplayed)
			ImGui::TextDisabled("Preview uses the last known good program.");
		for (const FMaterialCompileDiagnostic& Diagnostic
			: Base->GetMaterialCompileDiagnostics())
			ImGui::TextWrapped("%s", Diagnostic.Source.Message.c_str());
		const std::string_view CookDiagnostic = Base->GetMaterialCookDiagnostic();
		if (!CookDiagnostic.empty())
			ImGui::TextWrapped("Cook: %.*s",
				static_cast<int>(CookDiagnostic.size()), CookDiagnostic.data());
	}

	auto MMaterialEditor::DrawDetailsPanel(DMaterialInterface* Material, float Height) -> void
	{
		if (ImGui::BeginChild("MaterialDetails", ImVec2(0.0f, Height), ImGuiChildFlags_Borders))
		{
			ImGui::SeparatorText("Details");
			if (auto* Instance = Cast<DMaterialInstance>(Material)) DrawMaterialInstance(Instance);
			else if (auto* BaseMaterial = Cast<DMaterial>(Material)) DrawMaterial(BaseMaterial);
		}
		ImGui::EndChild();
	}

	auto MMaterialEditor::DrawMaterial(DMaterial* Material) -> void
	{
		ImGui::SeparatorText("Surface Parameters");
		if (!MonaImGui::PropertyEdit::BeginTable("MaterialParameters", MakeMaterialPropertyTableConfig())) return;
		DrawMaterialParameters(Material);
		MonaImGui::PropertyEdit::EndTable();
	}

	auto MMaterialEditor::DrawMaterialInstance(DMaterialInstance* Instance) -> void
	{
		ImGui::SeparatorText("Inheritance");
		if (MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParent", MakeMaterialPropertyTableConfig()))
		{
			DrawParentPicker(Instance);
			MonaImGui::PropertyEdit::EndTable();
		}
		ImGui::SeparatorText("Parameter Overrides");
		if (!MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParameters", MakeMaterialPropertyTableConfig())) return;
		DrawMaterialParameters(Instance);
		MonaImGui::PropertyEdit::EndTable();
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
		DrawGroup(DrawGroup, MaterialParameterPanelCache->GetRoot(), 0);
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
		DMaterialInstance* Instance = Model.GetInstance();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		ImGui::PushID(Definition.Name.ToString().c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride)
				&& !Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride)) bOverride = !bOverride;
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset")
				&& Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false)) bOverride = false;
			if (!bOverride) ImGui::BeginDisabled();
		}
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
			FVector2 Value = Entry.Value.Vector2Value;
			bChanged = MonaImGui::PropertyEdit::EditVectorValue(
				"##Value", Value, 0.01, &WidgetState, WidgetConfig);
			Edited.Vector2Value = Value;
		}
		else
		{
			FVector3 Value = Entry.Value.VectorValue;
			bChanged = MonaImGui::PropertyEdit::EditVectorValue(
				"##Value", Value, 0.01, &WidgetState, WidgetConfig);
			Edited.VectorValue = Value;
		}
		if (bChanged && bOverride
			&& !Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
			SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		if (WidgetState.bDeactivatedAfterEdit && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (WidgetState.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance && !bOverride) ImGui::EndDisabled();
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawColorParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		FVector3 Value = Entry.Value.VectorValue;
		DMaterialInstance* Instance = Model.GetInstance();
		const std::string ParameterName = Definition.Name.ToString();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		ImGui::PushID(ParameterName.c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride))
					bOverride = !bOverride;
			}
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset"))
			{
				if (Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false))
					bOverride = false;
			}
			if (!bOverride) ImGui::BeginDisabled();
		}
		float Color[3] = {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##Value", Color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_InputRGB) && bOverride)
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.VectorValue = FVector3(Color[0], Color[1], Color[2]);
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance && !bOverride) ImGui::EndDisabled();
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawScalarParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		float Value = Entry.Value.ScalarValue;
		DMaterialInstance* Instance = Model.GetInstance();
		const std::string ParameterName = Definition.Name.ToString();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		// The visible label belongs to the property-table column, while the actual controls use
		// hidden labels. Scope the complete row by parameter name so base materials and instances
		// both receive stable, distinct ImGui IDs.
		ImGui::PushID(ParameterName.c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride))
					bOverride = !bOverride;
			}
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset"))
			{
				if (Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false))
					bOverride = false;
			}
			if (!bOverride) ImGui::BeginDisabled();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		const float Minimum = Definition.bHasRange ? Definition.MinimumValue : 0.0f;
		const float Maximum = Definition.bHasRange ? Definition.MaximumValue : 0.0f;
		const ImGuiSliderFlags Flags = Definition.bHasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		if (ImGui::DragFloat("##Value", &Value, 0.01f, Minimum, Maximum, "%.3f", Flags) && bOverride)
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.ScalarValue = Value;
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance && !bOverride) ImGui::EndDisabled();
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawIntegerParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		DMaterialInstance* Instance = Model.GetInstance();
		const std::string ParameterName = Definition.Name.ToString();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		ImGui::PushID(ParameterName.c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride))
					bOverride = !bOverride;
			}
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset"))
			{
				if (Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false))
					bOverride = false;
			}
			if (!bOverride) ImGui::BeginDisabled();
		}

		float Scalar = std::isfinite(Entry.Value.ScalarValue)
			? Entry.Value.ScalarValue : Definition.Value.ScalarValue;
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
		if (ImGui::DragInt("##Value", &Value, 1.0f, Minimum, Maximum, "%d", Flags) && bOverride)
		{
			FMaterialParameterValue Edited = Entry.Value;
			Edited.ScalarValue = static_cast<float>(Value);
			if (!Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, true))
				SetError(std::format("The reflected {} parameter is unavailable.", Definition.DisplayName));
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && PropertyView.IsEditing()) FinishActivePropertyEdit(false);
		else if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditing()) FinishActivePropertyEdit(true);
		if (Instance && !bOverride) ImGui::EndDisabled();
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto MMaterialEditor::DrawTextureParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		DTexture2D* Texture = Entry.Value.TextureValue.Get();
		DMaterialInstance* Instance = Model.GetInstance();
		const std::string ParameterName = Definition.Name.ToString();
		bool bOverride = !Instance || Entry.bHasLocalOverride;
		ImGui::PushID(ParameterName.c_str());
		MonaImGui::PropertyEdit::BeginRow(Definition.DisplayName.c_str());
		if (Instance)
		{
			if (ImGui::Checkbox("##Override", &bOverride))
			{
				if (!Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, bOverride))
					bOverride = !bOverride;
			}
			ImGui::SameLine();
			if (bOverride && ImGui::SmallButton("Reset"))
			{
				if (Model.SetOverrideEnabled(PropertyView, MakePropertyViewContext(), Entry, false))
					bOverride = false;
			}
		}
		if (!bOverride) ImGui::BeginDisabled();
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
				Edited.TextureValue = Selected;
				const bool bAssigned = Model.SubmitValueEdit(
					PropertyView, MakePropertyViewContext(), Entry, Edited, false);
				if (!bAssigned && OutError.empty()) OutError = "Unable to assign the reflected texture parameter.";
				return bAssigned;
			},
		});
		if (!bOverride) ImGui::EndDisabled();
		if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
		if (Instance) ImGui::TextDisabled("%s", FormatParameterSource(Entry).c_str());
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
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
			.Transactions = GEditor ? &GEditor->GetTransactionManager() : nullptr,
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
		};
	}

	auto MMaterialEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}
}
