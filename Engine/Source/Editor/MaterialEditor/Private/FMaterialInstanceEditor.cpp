#include "FMaterialInstanceEditor.h"

#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetPicker.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/Package.h"
#include "MaterialEditorUI.h"
#include "Materials/MaterialInstance.h"
#include "Materials/ObjectCacheContext.h"
#include "MonaImGui.h"
#include "Settings/MaterialEditorProfileSettings.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "Widgets/MaterialParameterPanelModel.h"
#include "Widgets/MaterialPreview.h"
#include "Widgets/MonaImGuiPropertyTable.h"
#include "Workspace/MaterialEditorWorkspace.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		constexpr float MaximumMaterialVectorWidthInEm = 30.0f;
		auto FormatParameterSource(const FMaterialParameterPanelEntry& Entry) -> std::string
		{
			if (Entry.bHasLocalOverride) return "Local override";
			if (!Entry.Source) return "Unresolved";
			return std::format("Inherited from {}", Entry.Source->GetName());
		}

	}

	class FMaterialParameterPanelCache
	{
	public:
		auto Synchronize(DMaterialInterface* Material) -> const FMaterialParameterPanelModel&
		{
			if (Model && Model->GetMaterial() == Material && !Model->NeedsRefresh()) return *Model;

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
		struct FSchemaNode
		{
			FGuid ParameterId;
			std::string GroupPath;
			bool bOrphan = false;

			auto operator==(const FSchemaNode&) const -> bool = default;
		};

		std::vector<FSchemaNode> Schema;
		std::unique_ptr<FMaterialParameterPanelModel> Model;
		FMaterialParameterGroup Root;
	};

	class FMaterialInstanceEditor::FMaterialParameterRowScope
	{
	public:
		FMaterialParameterRowScope(
			FMaterialInstanceEditor& InEditor,
			const FMaterialParameterPanelModel& InModel,
			const FMaterialParameterPanelEntry& InEntry
		)
			: Editor(InEditor)
			, Model(InModel)
			, Entry(InEntry)
			, Instance(InModel.GetInstance())
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

		auto SubmitContinuousEdit(bool bChanged, const FMaterialParameterValue& Value,
			bool bDeactivatedAfterEdit, bool bActive) -> void
		{
			const auto Interaction = MonaImGui::PropertyEdit::FPropertyInteractionState::FromWidget(
				ImGui::GetID("##ParameterInteraction"), {bActive, bDeactivatedAfterEdit});
			if (bOverrideEnabled)
				Model.UpdateValueEdit(Editor.PropertyView, Editor.MakePropertyViewContext(), Entry, Interaction, bChanged ? &Value : nullptr);
		}

		auto SubmitColorEdit(bool bChanged, const FMaterialParameterValue& Value,
			const MonaImGui::PropertyEdit::FPropertyInteractionState& Picker) -> void
		{
			if (bOverrideEnabled)
				Model.UpdateValueEdit(Editor.PropertyView, Editor.MakePropertyViewContext(), Entry, Picker, bChanged ? &Value : nullptr);
		}

		auto IsReadOnly() const -> bool { return bDisabled; }

	private:
		FMaterialInstanceEditor& Editor;
		const FMaterialParameterPanelModel& Model;
		const FMaterialParameterPanelEntry& Entry;
		DMaterialInstance* Instance = nullptr;
		bool bOverrideEnabled = false;
		bool bDisabled = false;
	};

	FMaterialInstanceEditor::FMaterialInstanceEditor()
		: ProfileSettings(std::make_unique<FMaterialEditorProfileSettings>())
		, MaterialParameterPanelCache(std::make_unique<FMaterialParameterPanelCache>())
	{
	}

	FMaterialInstanceEditor::~FMaterialInstanceEditor()
	{
		ReleasePreview();
	}

	auto FMaterialInstanceEditor::GetToolkitId() const -> const FAssetEditorToolkitId&
	{
		return Workspace::Type;
	}

	auto FMaterialInstanceEditor::GetLayoutVersion() const -> uint32
	{
		return Workspace::LayoutVersion;
	}

	auto FMaterialInstanceEditor::OnOpenAsset(DObject& Object) -> bool
	{
		if (Source) return false;
		Source = Cast<DMaterialInstance>(&Object);
		return IsValid(Source.Get());
	}

	auto FMaterialInstanceEditor::OnActivateAsset(DObject& Object) -> void
	{
		EditorSession.Activate(&Object);
	}

	auto FMaterialInstanceEditor::RequestDeactivate() -> bool
	{
		return FinishActivePropertyEdit(true);
	}

	auto FMaterialInstanceEditor::OnCloseAsset(DObject&) -> void
	{
		Source.Reset();
		ReleasePreview();
	}

	auto FMaterialInstanceEditor::OnIsAssetDirty(const DObject& Object) const -> bool
	{
		return EditorSession.IsDirty(&Object);
	}

	auto FMaterialInstanceEditor::OnCanSaveActiveDocument() const -> bool
	{
		return EditorSession.CanSave(Source.Get());
	}

	auto FMaterialInstanceEditor::OnSaveActiveDocument() -> bool
	{
		return FinishActivePropertyEdit(false) && IsEditorOpen()
			&& EditorSession.Save(Source.Get(), {}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto FMaterialInstanceEditor::OnDiscardAsset(DObject& Object) -> bool
	{
		return RequestDeactivate() && EditorSession.Discard(&Object, {},
			[this](std::string Message) { SetError(std::move(Message)); });
	}

	auto FMaterialInstanceEditor::GetOwnedEditingObjects() const -> std::optional<std::vector<DObject*>>
	{
		auto* Instance = Source.Get();
		return IsValid(Instance) ? std::vector<DObject*>{Instance} : std::vector<DObject*>{};
	}

	class FMaterialInstanceEditor::FReloadParticipant final : public IObjectReplacementParticipant
	{
	public:
		explicit FReloadParticipant(std::shared_ptr<FMaterialInstanceEditor> InOwner) : Owner(std::move(InOwner)) {}

		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			Abort();
			const auto* Entry = Map.Find(Owner->Source.Get());
			if (!Entry) return {};
			if (!Owner->RequestDeactivate() || !Owner->IsEditorOpen()
				|| Owner->Source.Get() != Entry->Previous)
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy});
			Replacement = Cast<DMaterialInstance>(Entry->Replacement);
			if (!Replacement)
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::UnmappedReference});
			Previous = FObjectKey(Entry->Previous);
			return {};
		}
		auto Validate() const -> bool override
		{
			return Owner->IsEditorOpen() && (!Replacement || Owner->Source.GetKey() == Previous);
		}
		auto CoversNativeReferences(const DObject&) const -> bool override { return false; }
		auto GetStrongReferenceCount(const DObject& Object) const -> uint32 override
		{
			return Previous == FObjectKey(&Object) ? 1 : 0;
		}
		auto Commit() noexcept -> void override
		{
			if (Replacement) Owner->Source = std::move(Replacement);
		}
		auto Abort() noexcept -> void override
		{
			Replacement.Reset();
			Previous = {};
		}
		auto CanRetire() const -> bool override { return true; }
	private:
		std::shared_ptr<FMaterialInstanceEditor> Owner;
		FObjectKey Previous;
		TStrongObjectPtr<DMaterialInstance> Replacement;
	};

	auto FMaterialInstanceEditor::CreateReloadParticipant() -> std::shared_ptr<IObjectReplacementParticipant>
	{
		auto Owner = std::dynamic_pointer_cast<FMaterialInstanceEditor>(weak_from_this().lock());
		return Owner ? std::make_shared<FReloadParticipant>(std::move(Owner)) : nullptr;
	}

	auto FMaterialInstanceEditor::OnPackageReloaded(DPackage*, DPackage* Replacement) -> void
	{
		auto* Instance = Source.Get();
		if (!Instance || Instance->GetPackage() != Replacement || !RequestDeactivate()) return;
		MaterialParameterPanelCache = std::make_unique<FMaterialParameterPanelCache>();
		ReleasePreview();
		OnActivateAsset(*Instance);
	}

	auto FMaterialInstanceEditor::PostInitAssetEditor() -> void
	{
		FBaseAssetToolkit::PostInitAssetEditor();
		if (auto* Editors = GetAssetEditorSubsystem()) EditorSession.BindEditor(Editors->GetEditor());
		ProfileSettings->SelectStorage("MaterialEditor/Profiles/" + FXxHash128::HashBuffer(GetLayoutProfileKey()).ToString() + ".yaml");
		SettingsPage = FEditorSettingsPageRegistration({
			.Id = GetPreferencesPageId(),
			.Label = "Material Instance Editor" + (GetInstanceKey().empty() ? std::string{} : " [" + std::string(GetInstanceKey()) + "]"),
			.Keywords = "preview details parameters diagnostics panels",
			.Description = "Panel preferences for this material instance editor.",
			.Draw = [Weak = weak_from_this()](std::string& Error) {
				if (const auto Toolkit = std::dynamic_pointer_cast<FMaterialInstanceEditor>(Weak.lock()); Toolkit && Toolkit->IsEditorOpen())
					DrawMaterialEditorPreferences(*Toolkit->ProfileSettings, Error);
			},
			.Reset = [Weak = weak_from_this()] {
				const auto Toolkit = std::dynamic_pointer_cast<FMaterialInstanceEditor>(Weak.lock());
				return Toolkit && Toolkit->IsEditorOpen() && ResetMaterialEditorPreferences(*Toolkit->ProfileSettings);
			}});
	}

	auto FMaterialInstanceEditor::OnHostChanged() -> void
	{
		ProfileSettings->Save();
		SettingsPage = {};
		FBaseAssetToolkit::OnHostChanged();
		PostInitAssetEditor();
	}

	auto FMaterialInstanceEditor::OnClose() -> void
	{
		RequestDeactivate();
		Source = nullptr;
		ReleasePreview();
		ProfileSettings->Save();
		SettingsPage = {};
		FBaseAssetToolkit::OnClose();
	}

	auto FMaterialInstanceEditor::GetPreferencesPageId() const -> std::string
	{
		return "editor.material." + FXxHash128::HashBuffer(GetPresentationRootKey(Workspace::RootKey)).ToString();
	}

	auto FMaterialInstanceEditor::ReleasePreview() -> void
	{
		if (auto Current = std::exchange(Preview, {})) Current->SetVisible(false);
	}

	auto FMaterialInstanceEditor::ResetLayout() -> void
	{
		ProfileSettings->bPreviewVisible = true;
		ProfileSettings->bDetailsVisible = true;
		ProfileSettings->bParametersVisible = true;
		ProfileSettings->bDiagnosticsVisible = false;
		bPendingLayoutReset = true;
	}

	auto FMaterialInstanceEditor::OnDrawToolkit(bool bActive) -> bool
	{
		if (!bActive) RequestDeactivate();
		if (!IsValid(Source.Get()))
		{
			CloseWindow(EAssetEditorCloseReason::AssetUnloadingOrInvalid);
			return false;
		}
		return DrawAssetTabs(Workspace::RootKey,
			[this](DObject& Object) {
				const TStrongObjectPtr<DMaterialInstance> DrawingSource(Cast<DMaterialInstance>(&Object));
				if (DrawingSource) DrawDocument(*DrawingSource);
			},
			[this](DObject&, bool bVisible) {
				if (bVisible) return;
				if (Preview) Preview->SetVisible(false);
				const auto DockType = Workspace::MakeDockType(GetPresentationRootKey("Document"));
				if (ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::LayoutVersion)))
					TabLayout::SubmitDockSpace(DockType, Workspace::LayoutVersion, {0.0f, 0.0f}, ImGuiDockNodeFlags_KeepAliveOnly);
			});
	}

	auto FMaterialInstanceEditor::DrawToolbar(DMaterialInstance& Instance) -> void
	{
		if (ImGui::Button("Save")) SaveActiveDocument();
		if (!IsEditorOpen() || Source.Get() != &Instance) return;
		ImGui::SameLine();
		if (ImGui::Button("Compile")) RequestMaterialRecompile(Instance);
		const auto& Status = Instance.GetMaterialCompileStatus();
		if (Instance.IsCompiling() || Status.HasScheduledEdits())
		{
			ImGui::SameLine();
			if (ImGui::Button("Cancel Compile"))
			{
				FObjectCacheContext Context;
				for (auto* Owner : Context.GetMaterialsAffectedByMaterial(&Instance))
					if (IsValid(Owner)) FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owner);
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Compile: %s%s", FormatCompileState(Status),
			(Instance.GetAcceptedCompiledProgram() && !Status.IsCurrent()) ? " (showing last known good)" : "");
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

	auto FMaterialInstanceEditor::DrawDocument(DMaterialInstance& Instance) -> void
	{
		DrawToolbar(Instance);
		if (!IsEditorOpen() || Source.Get() != &Instance) return;
		ImGui::Spacing();
		const auto DockType = Workspace::MakeDockType(GetPresentationRootKey("Document"));
		static constexpr std::string_view PanelIds[]{"Graph", "Preview", "Details", "Parameters", "Diagnostics"};
		RestoreDockLayout("Document", DockType, Workspace::LayoutVersion, PanelIds);
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		if (Size.x <= 0.0f || Size.y <= 0.0f) return;
		const bool bReset = std::exchange(bPendingLayoutReset, false);
		if (bReset || !ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::LayoutVersion)))
			Workspace::BuildDefaultLayout(DockType, Size);
		TabLayout::SubmitDockSpace(DockType, Workspace::LayoutVersion, Size);

		const auto Panel = [&](const char* Label, const char* Id, bool* Open, auto Draw) {
			if (Open && !*Open) return false;
			const bool bVisible = TabLayout::BeginDockablePanel(DockType, Label, Id, Open,
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
			if (bVisible && IsEditorOpen() && Source.Get() == &Instance) Draw();
			ImGui::End();
			return bVisible;
		};
		Panel("Material Graph", "Graph", nullptr, [] {
			ImGui::TextWrapped("Material instances inherit their graph from the root base material. Open the base material to author it.");
		});
		if (!Panel("Preview", "Preview", &ProfileSettings->bPreviewVisible, [&] {
			if (!Preview) Preview = std::make_shared<FMaterialPreview>(GetWindowId());
			const auto DrawingPreview = Preview;
			DrawingPreview->Draw(&Instance);
		}) && Preview) Preview->SetVisible(false);
		Panel("Details", "Details", &ProfileSettings->bDetailsVisible, [&] {
			PropertyView.DrawInteractionError();
			MonaImGui::TextName(Instance.GetFName());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Instance.GetObjectPath().c_str());
			ImGui::TextDisabled("Material Instance");
			ImGui::Spacing();
			DrawMaterialInstance(&Instance);
		});
		Panel("Parameters", "Parameters", &ProfileSettings->bParametersVisible, [&] {
			PropertyView.DrawInteractionError();
			if (MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParameters", DetailsStyle::MakeTableConfig()))
			{
				DrawMaterialParameters(&Instance);
				MonaImGui::PropertyEdit::EndTable();
			}
		});
		Panel("Diagnostics", "Diagnostics", &ProfileSettings->bDiagnosticsVisible, [&] {
			DrawMaterialCompileStatus(&Instance, nullptr,
				[this](const auto& Diagnostic) { NavigateToFunctionNode(*this, Diagnostic.FunctionAssetPath, Diagnostic.NodeId); });
		});
		SaveDockLayout("Document", DockType, Workspace::LayoutVersion, PanelIds);
		PropertyView.EndInteractionFrame(MakePropertyViewContext());
		MonaImGui::ErrorDialog("Material Editor Error", ErrorMessage);
	}

	auto FMaterialInstanceEditor::DrawMaterialInstance(DMaterialInstance* Instance) -> void
	{
		if (auto* Parent = Instance->GetParent(); Parent && ImGui::Button("Open Parent Material"))
			OpenReferencedMaterialAsset(*this, Parent->GetObjectPath(), Parent->GetClass()->GetQualifiedName().ToString());
		ImGui::SeparatorText("Inheritance");
		if (MonaImGui::PropertyEdit::BeginTable("MaterialInstanceParent", DetailsStyle::MakeTableConfig()))
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
		std::expected<void, std::string> ResolveError;
		const bool bResolved = static_cast<bool>(ResolveError = ResolveMaterialProperties(*Instance, Resolved));
		bool bChanged = false;
		if (MonaImGui::PropertyEdit::BeginTable("RenderingOverrides", DetailsStyle::MakeTableConfig()))
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
						: Index == 1 ? "DefaultLit\0Unlit\0ClearCoat\0" : "Automatic\0Enabled\0Disabled\0";
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
					const auto* Source = ResolveObjectKey(Resolved.Sources[Index]);
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
		if (!bResolved) ImGui::TextWrapped("%s", ResolveError.error().c_str());
		if (bChanged)
		{
			if (auto* Property = Instance->GetClass()->FindPropertyByName(FName("PropertyOverrides")))
				PropertyView.SubmitPropertyValueEdit(MakePropertyViewContext(),
					FPropertyEditTarget::ForMember(Instance, Property),
					[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
						*ScratchProperty->ContainerPtrToValuePtr<FMaterialPropertyOverrides>(ScratchContainer, ScratchArrayIndex) = Overrides;
					}, false);
		}
	}

	auto FMaterialInstanceEditor::DrawParentPicker(DMaterialInstance* Instance) -> void
	{
		ImGui::PushID("MaterialParent");
		MonaImGui::PropertyEdit::BeginRow("Parent");
		DMaterialInterface* Current = Instance->GetParent();
		const FAssetPickerResult PickerResult = AssetPicker::Draw({
			.ComboId = "##Parent",
			.SearchId = "##ParentSearch",
			.SearchHint = "Search materials...",
			.RequiredClass = DMaterialInterface::StaticClass(),
			.ClassPolicy = EAssetClassPolicy::Derived,
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
				auto Context = MakePropertyViewContext();
				Context.ReportPropertyError = [&](std::string Error) { OutError = std::move(Error); };
				return PropertyView.SubmitPropertyValueEdit(Context,
					FPropertyEditTarget::ForMember(Instance, Property), [&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
					static_cast<FObjectProperty*>(ScratchProperty)->SetObjectPropertyValue(ScratchContainer, Parent, ScratchArrayIndex);
				}, false);
			},
		});
		if (!PickerResult.Error.empty()) PropertyView.ReportEditError(MakePropertyViewContext(), PickerResult.Error);
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto FMaterialInstanceEditor::DrawMaterialParameters(DMaterialInterface* Material) -> void
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

	auto FMaterialInstanceEditor::DrawMaterialParameter(
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

	auto FMaterialInstanceEditor::DrawVectorParameter(
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
		FVector4& Value = Edited.GetVector4();
		const bool bChanged = MonaImGui::PropertyEdit::EditVectorValue("##Value", Value, 0.01, &WidgetState, WidgetConfig);
		Row.SubmitContinuousEdit(bChanged, Edited,
			WidgetState.bDeactivatedAfterEdit, WidgetState.bActive);
	}

	auto FMaterialInstanceEditor::DrawColorParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		FMaterialParameterValue Edited = Entry.Value;
		FVector4& Value = Edited.GetVector4();
		FMaterialParameterRowScope Row(*this, Model, Entry);
		using namespace MonaImGui::PropertyEdit;
		FPropertyInteractionState Picker;
		FLinearColor Color(static_cast<float>(Value.x), static_cast<float>(Value.y),
			static_cast<float>(Value.z), static_cast<float>(Value.w));
		const bool bChanged = DrawColorPicker(Color, EColorPickerMode::Linear, true, Row.IsReadOnly(), &Picker);
		if (bChanged) Value = FVector4(Color.R, Color.G, Color.B, Color.A);
		Row.SubmitColorEdit(bChanged, Edited, Picker);
	}

	auto FMaterialInstanceEditor::DrawScalarParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		FMaterialParameterValue Edited = Entry.Value;
		float& Value = Edited.GetScalar();
		FMaterialParameterRowScope Row(*this, Model, Entry);
		ImGui::SetNextItemWidth(-FLT_MIN);
		const float Minimum = Definition.bHasRange ? Definition.MinimumValue : 0.0f;
		const float Maximum = Definition.bHasRange ? Definition.MaximumValue : 0.0f;
		const ImGuiSliderFlags Flags = Definition.bHasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		const bool bChanged = ImGui::DragFloat("##Value", &Value, 0.01f,
			Minimum, Maximum, "%.3f", Flags);
		Row.SubmitContinuousEdit(bChanged, Edited,
			ImGui::IsItemDeactivatedAfterEdit(), ImGui::IsItemActive());
	}

	auto FMaterialInstanceEditor::DrawIntegerParameter(
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
		const bool bChanged = ImGui::DragInt("##Value", &Value, 1.0f,
			Minimum, Maximum, "%d", Flags);
		FMaterialParameterValue Edited = Entry.Value;
		if (bChanged) Edited.GetScalar() = static_cast<float>(Value);
		Row.SubmitContinuousEdit(bChanged, Edited,
			ImGui::IsItemDeactivatedAfterEdit(), ImGui::IsItemActive());
	}

	auto FMaterialInstanceEditor::DrawTextureParameter(
		const FMaterialParameterPanelModel& Model,
		const FMaterialParameterPanelEntry& Entry
	) -> void
	{
		const FMaterialParameterDefinition& Definition = *Entry.Definition;
		DTexture* Texture = Entry.Value.GetTexture().Texture.Get();
		FMaterialParameterRowScope Row(*this, Model, Entry);
		const FAssetPickerResult PickerResult = AssetPicker::Draw({
			.ComboId = "##Texture",
			.SearchId = "##TextureSearch",
			.SearchHint = "Search textures...",
			.RequiredClass = Entry.Value.GetTexture().Dimension == EMaterialTextureDimension::TextureCube ? DTextureCube::StaticClass() : DTexture2D::StaticClass(),
			.ClassPolicy = EAssetClassPolicy::Derived,
			.CurrentSelection = Texture,
			.SearchText = TextureSearchText,
			.bAllowNone = true,
			.AssignSelection = [this, &Model, Entry, Label = Definition.DisplayName](DObject* Selection, std::string& OutError) {
				DTexture* Selected = Cast<DTexture>(Selection);
				if (Selection && !Selected)
				{
					OutError = "The selected asset is not a texture.";
					return false;
				}
				FMaterialParameterValue Edited = Entry.Value;
				Edited.GetTexture().Texture = Selected;
				auto Context = MakePropertyViewContext();
				Context.ReportPropertyError = [&](std::string Error) { OutError = std::move(Error); };
				return Model.SubmitValueEdit(PropertyView, Context, Entry, Edited, false);
			},
		});
		if (!PickerResult.Error.empty()) PropertyView.ReportEditError(MakePropertyViewContext(), PickerResult.Error);
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
			Changed |= Combo("Address W", Edited.GetTexture().SamplerState.AddressW, "Repeat\0Mirror\0Clamp\0");
			Changed |= Combo("Missing texture", Edited.GetTexture().TextureFallback, Edited.GetTexture().Dimension == EMaterialTextureDimension::TextureCube ? "White\0Black\0" : "White\0Black\0Flat normal (RG)\0");
			if (Changed) Model.SubmitValueEdit(PropertyView, MakePropertyViewContext(), Entry, Edited, false);
			ImGui::TreePop();
		}
	}

	auto FMaterialInstanceEditor::DrawOrphanParameter(
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

	auto FMaterialInstanceEditor::FinishActivePropertyEdit(bool bCancel) -> bool
	{
		const FPropertyViewContext Context = MakePropertyViewContext();
		return PropertyView.FinishActiveEdit(&Context, bCancel);
	}

	auto FMaterialInstanceEditor::MakePropertyViewContext() -> FPropertyViewContext
	{
		return {
			.ReportError = [this](std::string Error) { SetError(std::move(Error)); },
		};
	}

	auto FMaterialInstanceEditor::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
		DURIN_ERROR("Material editor: {}", ErrorMessage);
	}
}
