#include "Widgets/MMaterialFunctionEditor.h"
#include "Misc/UnifiedError.h"
#include "Materials/Material.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "Settings/MaterialEditorSessionSettings.h"
#include "Materials/MaterialExpressions.h"
#include "Widgets/MaterialPreview.h"
#include "Widgets/MaterialFunctionCallPicker.h"
#include "Graph/MaterialGraphCanvas.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "MaterialFunctionPreview.h"
#include "Workspace/MaterialEditorWorkspace.h"
#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Asset/AssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transaction.h"
#include "Documents/EditorDocumentManager.h"
#include "Misc/MountPaths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"

namespace Durin::MaterialEditor
{
	struct MMaterialFunctionEditor::FDocument
	{
		TObjectPtr<DMaterialFunction> Owner;
		FStrongObjectPtr PreviewPackage;
		FStrongObjectPtr PreviewMaterial;
		std::unique_ptr<FMaterialGraphCanvas> Canvas;
		std::unique_ptr<FMaterialPreview> Preview;
		FGuid Output;
		FMaterialFunctionPreviewInvalidation PreviewInvalidation;
		bool bPreviewValid = false;
		bool bResetLayout = false;
		bool bOutputPort = false;
		FGuid EditingPort;
		FGuid SelectedPortNode;
		FMaterialFunctionPort PortDraft;
		std::array<char, 129> PortName{};
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		auto Function() const -> DMaterialFunction* { return Cast<DMaterialFunction>(Owner.Get()); }
		auto Material() const -> DMaterial* { return Cast<DMaterial>(PreviewMaterial.Get()); }
		~FDocument()
		{
			Preview.reset();
			if (auto* Value = Material()) { FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Value); MarkAsGarbage(Value); }
			if (PreviewPackage.Get()) MarkAsGarbage(PreviewPackage.Get());
		}
	};

	MMaterialFunctionEditor::MMaterialFunctionEditor(FEditorDocumentManager& InManager)
		: SessionSettings(std::make_unique<FMaterialEditorSessionSettings>("MaterialFunctionPreferences.yaml")), Manager(InManager)
	{
		if (auto* Service = Manager.GetAssetEditorSubsystem()) Documents.BindEditor(Service->GetEditor());
	}
	MMaterialFunctionEditor::~MMaterialFunctionEditor()
	{
		for (const auto& [Resource, Document] : Open)
		{
			const auto [Zoom, Pan] = Document->Canvas->GetViewport();
			SessionSettings->SetViewport(Resource, {.Zoom = Zoom, .Pan = Pan});
		}
		SessionSettings->Save();
	}
	auto MMaterialFunctionEditor::Find(std::string_view Resource) const -> FDocument*
	{
		const auto It = Open.find(std::string(Resource));
		return It == Open.end() ? nullptr : It->second.get();
	}
	auto MMaterialFunctionEditor::OpenDocument(const FEditorDocument& Tab) -> EDocumentOpenResult
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		if (Find(Tab.ResourceId)) return EDocumentOpenResult::Opened;
		FSoftObjectPath Path;
		if (const auto PathValidation = FSoftObjectPath::TryCreateWithDiagnostic(Tab.ResourceId, Path); !PathValidation) { Error = ToString(PathValidation.error()); return EDocumentOpenResult::Rejected; }
		DMaterialFunction* Function = nullptr;
		const auto Loaded = LoadObject<DMaterialFunction>(Path, nullptr, &LoadedError);
		Function = Loaded;
		if (!Loaded || !Function) { Error = LoadedError ? LoadedError->ToString() : "The asset is not an editable function."; return EDocumentOpenResult::Rejected; }
		auto Document = std::make_unique<FDocument>();
		Document->Owner = Function;
		Document->Canvas = std::make_unique<FMaterialGraphCanvas>(FMaterialGraphDocument(*Function),
			FMaterialGraphEditorServices{
				.ReportError = [this](std::string Message) { Error = std::move(Message); },
				.OpenFunction = [this](std::string_view Path) { OpenRelatedAsset(Path, DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); }});
		if (const auto* State = SessionSettings->FindViewport(Tab.ResourceId))
			Document->Canvas->SetViewport(State->Zoom, State->Pan);
		Document->PreviewInvalidation.SetFunction(Function);
		const auto Mount = FMountPaths::FindMountForVirtualPath(Function->GetPackage()->GetPackagePath());
		if (!Mount) { Error = (Mount ? std::string{} : ToString(Mount.error())); return EDocumentOpenResult::Rejected; }
		FPackagePath PreviewPath;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(std::format("{}__FunctionPreview/{}", Mount->Mount->VirtualRoot, FGuid::NewGuid().ToString()), PreviewPath); !PathValidation)
		{
			Error = ToString(PathValidation.error());
			return EDocumentOpenResult::Rejected;
		}
		auto* Package = NewObject<DPackage>(DPackage::StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		Package->InitializeAssetPackage(PreviewPath);
		Document->PreviewPackage = FStrongObjectPtr(Package);
		auto* Material = NewObject<DMaterial>(DMaterial::StaticClass(), Package, "Preview", EObjectFlags::Transient);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		Document->PreviewMaterial = FStrongObjectPtr(Material);
		Document->Preview = std::make_unique<FMaterialPreview>(Tab.Id.Value);
		if (!Function->GetFunctionSignature().Outputs.empty()) Document->Output = Function->GetFunctionSignature().Outputs.front().Id;
		Open.emplace(Tab.ResourceId, std::move(Document));
		return EDocumentOpenResult::Opened;
	}
	auto MMaterialFunctionEditor::ActivateDocument(const FEditorDocument& Tab) -> void
	{
		RequestDeactivate();
		if (auto* Document = Find(Tab.ResourceId)) Documents.Activate(Tab, Document->Function());
	}
	auto MMaterialFunctionEditor::RequestDeactivate() -> bool
	{
		for (auto& [Resource, Document] : Open) Document->Canvas->CancelInteraction();
		return true;
	}
	auto MMaterialFunctionEditor::RequestCloseDocument(const FEditorDocument& Tab) -> EDocumentCloseResult
	{
		if (IsDocumentDirty(Tab)) return EDocumentCloseResult::PendingConfirmation;
		if (const auto* Document = Find(Tab.ResourceId))
		{
			const auto [Zoom, Pan] = Document->Canvas->GetViewport();
			SessionSettings->SetViewport(Tab.ResourceId, {.Zoom = Zoom, .Pan = Pan});
			SessionSettings->Save();
		}
		Open.erase(Tab.ResourceId); Documents.Close(Tab.ResourceId);
		return EDocumentCloseResult::Closed;
	}
	auto MMaterialFunctionEditor::OnSaveDocument(const FEditorDocument& Tab) -> bool
	{
		auto* Document = Find(Tab.ResourceId);
		return Document && Documents.Save(Document->Function(), {}, [this](std::string Message) { Error = std::move(Message); });
	}
	auto MMaterialFunctionEditor::OnDiscardDocument(const FEditorDocument& Tab) -> bool
	{
		auto* Document = Find(Tab.ResourceId);
		return Document && Documents.Discard(Document->Function(), [this] { RequestDeactivate(); },
			[this](DPackage* Old, DPackage* New) { Manager.NotifyPackageReloaded(Old, New); },
			[this](std::string Message) { Error = std::move(Message); }, &Manager);
	}
	auto MMaterialFunctionEditor::OnIsDocumentDirty(const FEditorDocument& Tab) const -> bool
	{
		const auto* Document = Find(Tab.ResourceId);
		return Document && Documents.IsDirty(Document->Function());
	}
	auto MMaterialFunctionEditor::OnCanSaveActiveDocument() const -> bool
	{
		const auto* Document = Find(Documents.GetActiveResourceId());
		return Document && Documents.CanSave(Document->Function());
	}
	auto MMaterialFunctionEditor::OnSaveActiveDocument() -> bool
	{
		auto* Document = Find(Documents.GetActiveResourceId());
		return Document && Documents.Save(Document->Function(), {}, [this](std::string Message) { Error = std::move(Message); });
	}
	auto MMaterialFunctionEditor::OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void
	{
		for (auto& [Resource, Document] : Open)
			if (auto* Function = Document->Function(); Function && Function->GetPackage() == Previous)
			{
				const auto [Zoom, Pan] = Document->Canvas->GetViewport();
				Document->Canvas->CancelInteraction();
				Document->Owner = Cast<DMaterialFunction>(Replacement->FindTopLevelAsset(Function->GetFName()));
				if (Document->Function())
				{
					Document->Canvas = std::make_unique<FMaterialGraphCanvas>(FMaterialGraphDocument(*Document->Function()),
						FMaterialGraphEditorServices{
							.ReportError = [this](std::string Message) { Error = std::move(Message); },
							.OpenFunction = [this](std::string_view Path) { OpenRelatedAsset(Path, DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); }});
					Document->Canvas->SetViewport(Zoom, Pan);
				}
				Document->PreviewInvalidation.RequestRefresh(); Document->EditingPort = {}; Document->SelectedPortNode = {};
			}
	}

	auto MMaterialFunctionEditor::ResetLayout() -> void
	{
		SessionSettings->bPreviewVisible = true;
		SessionSettings->bDetailsVisible = true;
		SessionSettings->bParametersVisible = true;
		for (auto& [Resource, Document] : Open) Document->bResetLayout = true;
	}
	auto MMaterialFunctionEditor::NavigateToNode(std::string_view Resource, const FGuid& NodeId) -> bool
	{
		FDocument* Document = nullptr;
		if (auto* Owner = GetOwningAssetEditor())
		{
			if (!OpenRelatedAsset(Resource, DMaterialFunction::StaticClass()->GetQualifiedName().ToString())) return false;
			for (const auto& Session : Owner->GetAssetEditorSubsystem()->GetOwnedAssetEditors())
				if (auto Toolkit = std::dynamic_pointer_cast<MMaterialFunctionEditor>(Session->GetToolkit()))
					if ((Document = Toolkit->Find(Resource))) break;
		}
		if (!Document || !Document->Function()) return false;
		const auto& Nodes = Document->Function()->GetExpressionCollection().Expressions;
		return std::ranges::any_of(Nodes, [&](const auto& Node) { return Node->Id == NodeId; }) && Document->Canvas->SelectAndFrame(NodeId);
	}
	auto MMaterialFunctionEditor::OnDrawToolkit(bool bActive) -> bool
	{
		if (!bActive) RequestDeactivate();
		std::vector<FDocumentId> Deleted;
		for (const auto& Tab : Manager.GetDocuments())
			if (Tab.ToolkitId == MaterialFunctionWorkspaceType)
				if (auto* Document = Find(Tab.ResourceId); Document && !Document->Function()) Deleted.push_back(Tab.Id);
		for (const auto Id : Deleted) Manager.RequestCloseDocument(Id);
		return Documents.GetTabManager().DrawDocuments(Manager, MaterialFunctionWorkspaceType, GetPresentationRootKey("MaterialFunctionEditor"),
			[this](const FEditorDocument& Tab) { const auto* Document = Find(Tab.ResourceId); return Document && Document->Function(); },
			[this](const FEditorDocument& Tab) { DrawDocument(Tab, *Find(Tab.ResourceId)); },
			[this](const FEditorDocument& Tab, bool Visible) {
				if (auto* Document = Find(Tab.ResourceId)) Document->Preview->SetVisible(Visible);
				if (!Visible)
				{
					const auto DockType = Workspace::MakeDocumentDockType(Tab);
					if (ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::FunctionLayoutVersion)))
						TabLayout::SubmitDockSpace(DockType, Workspace::FunctionLayoutVersion,
							{0.0f, 0.0f}, ImGuiDockNodeFlags_KeepAliveOnly);
				}
			});
	}

	auto MMaterialFunctionEditor::DrawInputs(FDocument& Document) -> void
	{
		auto& Function = *Document.Function();
		auto Signature = Function.GetFunctionSignature();
		std::ranges::stable_sort(Signature.Inputs, {}, &FMaterialFunctionPort::DisplayOrder);
		if (ImGui::Button("New Input"))
		{
			FMaterialFunctionPort Port;
			Port.Name = "Input";
			for (uint32 Suffix = 1; std::ranges::any_of(Signature.Inputs,
				[&](const auto& Input) { return Input.Name == Port.Name; }); ++Suffix)
				Port.Name = std::format("Input{}", Suffix);
			Port.Default.Kind = EMaterialFunctionDefaultKind::Numeric;
			FMaterialGraphDocument Graph(Function);
			const auto Result = Graph.AddPort(false, Port, {}, 0, static_cast<int32>(Signature.Inputs.size()) * 120);
			if (!Result) Error = FormatMaterialGraphCommandResult(Result);
			else if (!Result.GeneratedNodeIds.empty()) Document.Canvas->SelectAndFrame(Result.GeneratedNodeIds.front());
		}
		if (Signature.Inputs.empty()) ImGui::TextDisabled("No function inputs.");
		for (const auto& Port : Signature.Inputs)
		{
			const auto Label = std::format("{} ({})##{}", Port.Name, GetProgramTypeName(Port.Type), Port.Id.ToString());
			for (const auto& Expression : Function.GetExpressionCollection().Expressions)
				if (const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get()); Input && Input->Port.Id == Port.Id)
				{
					if (ImGui::Selectable(Label.c_str(), Document.Canvas->GetSelection().contains(Input->Id)))
						Document.Canvas->SelectAndFrame(Input->Id);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", Port.bRequired ? "Required input" : DescribeFunctionDefault(Port.Default).c_str());
					break;
				}
		}
	}

	auto MMaterialFunctionEditor::DrawInterface(FDocument& Document) -> void
	{
		auto& Function = *Document.Function();
		FMaterialGraphDocument Graph(Function);
		const auto Apply = [&](FMaterialGraphCommandResult Result) { if (!Result) Error = FormatMaterialGraphCommandResult(Result); };
		if (Document.Canvas->GetSelection().size() != 1)
		{
			Document.SelectedPortNode = {};
			return;
		}
		const auto* Id = std::get_if<FGuid>(&*Document.Canvas->GetSelection().begin());
		if (!Id) return;
		const auto& Expressions = Function.GetExpressionCollection().Expressions;
		const auto Selected = std::ranges::find(Expressions, *Id, [](const auto& Node) { return Node->Id; });
		if (Selected == Expressions.end()) return;
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Selected->Get()); Call && Call->Function.IsValid())
		{
			if (ImGui::Button("Open Function")) OpenRelatedAsset(Call->Function->GetObjectPath(),
				Call->Function->GetClass()->GetQualifiedName().ToString());
			DrawMaterialFunctionCallInputs(Function, *Id, *Documents.GetEditor()->GetTransactor(), Error);
		}
		const auto* Input = Cast<DMaterialExpressionFunctionInput>(Selected->Get());
		const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Selected->Get());
		const auto* Port = Input ? &Input->Port : Output ? &Output->Port : nullptr;
		if (!Port) { Document.SelectedPortNode = {}; return; }
		if (Document.SelectedPortNode != *Id)
		{
			Document.SelectedPortNode = *Id;
			Document.EditingPort = Port->Id;
			Document.bOutputPort = Output != nullptr;
			Document.PortDraft = *Port;
			std::snprintf(Document.PortName.data(), Document.PortName.size(), "%s", Port->Name.c_str());
		}
		ImGui::InputText("Name", Document.PortName.data(), Document.PortName.size());
		int Type = static_cast<int>(Document.PortDraft.Type);
		if (ImGui::Combo("Type", &Type, "Float\0Float2\0Float3\0Float4\0Texture2D\0Surface\0"))
		{
			Document.PortDraft.Type = static_cast<EMaterialProgramValueType>(Type);
			Document.PortDraft.Default = {};
			if (!Document.bOutputPort) Document.PortDraft.Default.Kind = Type < 4 ? EMaterialFunctionDefaultKind::Numeric
				: Type == 4 ? EMaterialFunctionDefaultKind::Texture : EMaterialFunctionDefaultKind::Surface;
		}
		ImGui::InputInt("Order", &Document.PortDraft.DisplayOrder);
		ImGui::Checkbox("Advanced", &Document.PortDraft.bAdvanced);
		if (!Document.bOutputPort)
		{
			ImGui::Checkbox("Required", &Document.PortDraft.bRequired);
			if (!Document.PortDraft.bRequired)
			{
				int Kind = static_cast<int>(Document.PortDraft.Default.Kind);
				if (ImGui::Combo("Default Kind", &Kind, "None\0Numeric\0Texture\0Surface\0Other Input\0UV0\0"))
					Document.PortDraft.Default.Kind = static_cast<EMaterialFunctionDefaultKind>(Kind);
				if (Document.PortDraft.Default.Kind == EMaterialFunctionDefaultKind::Numeric)
				{
					const int Components = static_cast<int>(Document.PortDraft.Type) + 1;
					if (Components >= 1 && Components <= 4)
						ImGui::InputScalarN("Default", ImGuiDataType_Float, &Document.PortDraft.Default.Numeric.X, Components);
				}
				else if (Document.PortDraft.Default.Kind == EMaterialFunctionDefaultKind::Input)
				{
					if (ImGui::BeginCombo("Default Input", "Select input"))
					{
						for (const auto& Port : Function.GetFunctionSignature().Inputs)
							if (Port.Type == Document.PortDraft.Type && Port.Id != Document.EditingPort && ImGui::Selectable(Port.Name.c_str()))
								Document.PortDraft.Default.InputId = Port.Id;
						ImGui::EndCombo();
					}
				}
				else if (Document.PortDraft.Default.Kind == EMaterialFunctionDefaultKind::Texture)
				{
					int Fallback = static_cast<int>(Document.PortDraft.Default.TextureFallback);
					if (ImGui::Combo("Fallback", &Fallback, "White\0Black\0Flat RG Normal\0")) Document.PortDraft.Default.TextureFallback = static_cast<EMaterialTextureFallback>(Fallback);
					auto& Sampler = Document.PortDraft.Default.Sampler;
					int Min = static_cast<int>(Sampler.MinFilter), Mag = static_cast<int>(Sampler.MagFilter);
					int U = static_cast<int>(Sampler.AddressU), V = static_cast<int>(Sampler.AddressV);
					if (ImGui::Combo("Min Filter", &Min, "Nearest\0Linear\0Nearest Mipmap Nearest\0Linear Mipmap Nearest\0Nearest Mipmap Linear\0Linear Mipmap Linear\0")) Sampler.MinFilter = static_cast<EMaterialSamplerMinFilter>(Min);
					if (ImGui::Combo("Mag Filter", &Mag, "Nearest\0Linear\0")) Sampler.MagFilter = static_cast<EMaterialSamplerMagFilter>(Mag);
					if (ImGui::Combo("Address U", &U, "Repeat\0Mirrored Repeat\0Clamp To Edge\0")) Sampler.AddressU = static_cast<EMaterialSamplerAddressMode>(U);
					if (ImGui::Combo("Address V", &V, "Repeat\0Mirrored Repeat\0Clamp To Edge\0")) Sampler.AddressV = static_cast<EMaterialSamplerAddressMode>(V);
				}
				else if (Document.PortDraft.Default.Kind == EMaterialFunctionDefaultKind::Surface)
					for (uint32 Index = 0; Index < 8; ++Index)
					{
						ImGui::PushID(static_cast<int>(Index));
						auto& Value = GetMaterialSurfaceOutputDefault(Document.PortDraft.Default.Surface, static_cast<EMaterialSurfaceOutput>(Index));
						ImGui::InputScalarN(MaterialSurfaceNames[Index], ImGuiDataType_Float, &Value.X,
							static_cast<int>(GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index))) + 1);
						ImGui::PopID();
					}
			}
			else Document.PortDraft.Default = {};
		}
		if (ImGui::Button("Apply Port"))
		{
			Document.PortDraft.Name = Document.PortName.data();
			if (Document.bOutputPort) { Document.PortDraft.bRequired = false; Document.PortDraft.Default = {}; }
			Apply(Graph.SetPort(Document.bOutputPort, Document.PortDraft));
		}
		if (Document.EditingPort.IsValid())
		{
			ImGui::SameLine();
			if (ImGui::Button("Remove Port")) Apply(Graph.RemovePort(Document.bOutputPort, Document.EditingPort));
		}
	}

	auto MMaterialFunctionEditor::DrawDocument(const FEditorDocument& Tab, FDocument& Document) -> void
	{
		auto& Function = *Document.Function();
		if (!Documents.GetEditor()) { ImGui::TextDisabled("Editor transactions are unavailable."); return; }
		if (ImGui::Button("Save")) SaveDocument(Tab);
		ImGui::SameLine();
		if (ImGui::Button("Compile Preview")) Document.PreviewInvalidation.RequestRefresh();
		ImGui::SameLine();
		if (ImGui::BeginCombo("Output", Document.Output.IsValid() ? "Selected output" : "Choose output"))
		{
			for (const auto& Port : Function.GetFunctionSignature().Outputs)
				if (ImGui::Selectable(Port.Name.c_str(), Port.Id == Document.Output)) { Document.Output = Port.Id; Document.PreviewInvalidation.RequestRefresh(); }
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("Window")) ImGui::OpenPopup("FunctionWindows");
		if (ImGui::BeginPopup("FunctionWindows"))
		{
			ImGui::MenuItem("Preview", nullptr, &SessionSettings->bPreviewVisible);
			ImGui::MenuItem("Details", nullptr, &SessionSettings->bDetailsVisible);
			ImGui::MenuItem("Inputs", nullptr, &SessionSettings->bParametersVisible);
			if (ImGui::MenuItem("Reset Layout")) ResetLayout();
			ImGui::EndPopup();
		}
		if (!Error.empty()) ImGui::TextWrapped("%s", Error.c_str());
		Document.PreviewInvalidation.SetFunction(&Function);
		if (Document.PreviewInvalidation.ConsumeRefreshRequest())
		{
			auto Result = BuildMaterialFunctionPreview(Function, Document.Output, *Document.Material());
			Document.Diagnostics = Result.Diagnostics;
			Document.bPreviewValid = static_cast<bool>(Result);
			if (!Result) Error = FormatMaterialGraphCommandResult(Result);
			else Document.Material()->CompileEdits();
		}
		else if (Document.Material()->GetMaterialCompileStatus().HasUnsubmittedEdits())
			Document.Material()->CompileEdits();
		const auto DockType = Workspace::MakeDocumentDockType(Tab);
		static constexpr std::string_view PanelIds[]{"Graph", "Preview", "Details", "Inputs"};
		RestoreDockLayout("Document", DockType, Workspace::FunctionLayoutVersion, PanelIds);
		const auto Size = ImGui::GetContentRegionAvail();
		if (Size.x <= 0.0f || Size.y <= 0.0f) return;
		if (Document.bResetLayout || !ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::FunctionLayoutVersion)))
		{
			Workspace::BuildDefaultLayout(Tab, Size, true);
			Document.bResetLayout = false;
		}
		TabLayout::SubmitDockSpace(DockType, Workspace::FunctionLayoutVersion, Size);
		const auto BeginPanel = [&](const char* Label, const char* Key, bool* Open = nullptr) {
			const bool Visible = TabLayout::BeginDockablePanel(DockType, Label, Key, Open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				const auto* Active = Manager.GetActiveDocument();
				if (!Active || Active->Id != Tab.Id) Manager.ActivateDocument(Tab.Id);
			}
			return Visible;
		};
		if (BeginPanel("Material Graph", "Graph"))
			Document.Canvas->Draw(*Documents.GetEditor()->GetTransactor(), 0);
		ImGui::End();
		if (SessionSettings->bDetailsVisible)
		{
			if (BeginPanel("Details", "Details", &SessionSettings->bDetailsVisible))
			{
				Document.Canvas->DrawSelectionDetails(*Documents.GetEditor()->GetTransactor());
				DrawInterface(Document);
			}
			ImGui::End();
		}
		if (SessionSettings->bParametersVisible)
		{
			if (BeginPanel("Inputs", "Inputs", &SessionSettings->bParametersVisible)) DrawInputs(Document);
			ImGui::End();
		}
		const bool bPreviewPanel = SessionSettings->bPreviewVisible;
		const bool PreviewVisible = bPreviewPanel
			&& BeginPanel("Preview", "Preview", &SessionSettings->bPreviewVisible);
		Document.Preview->SetVisible(PreviewVisible);
		if (PreviewVisible)
		{
			const auto DrawDiagnostic = [&](const FMaterialProgramDiagnostic& Diagnostic) {
				if (ImGui::Selectable(Diagnostic.Message.c_str()))
				{
					if (Diagnostic.FunctionAssetPath.empty() || Diagnostic.FunctionAssetPath == Function.GetObjectPath()) Document.Canvas->SelectAndFrame(Diagnostic.NodeId);
					else NavigateToNode(Diagnostic.FunctionAssetPath, Diagnostic.NodeId);
				}
			};
			for (const auto& Diagnostic : Document.Diagnostics) DrawDiagnostic(Diagnostic);
			for (const auto& Diagnostic : Document.Material()->GetMaterialCompileDiagnostics()) DrawDiagnostic(Diagnostic);
			if (Document.bPreviewValid) Document.Preview->Draw(Document.Material());
			else ImGui::TextDisabled("The selected output is unavailable.");
		}
		if (bPreviewPanel) ImGui::End();
		SaveDockLayout("Document", DockType, Workspace::FunctionLayoutVersion, PanelIds);
	}
	auto MMaterialFunctionEditor::OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void
	{
		if (auto Node = Open.extract(std::string(Source)); !Node.empty()) { Node.key() = Destination; Open.insert(std::move(Node)); }
		SessionSettings->MoveViewport(Source, Destination);
		SessionSettings->Save();
		Documents.RemapResourceId(Source, Destination);
	}

	auto MMaterialFunctionEditor::GetLayoutVersion() const -> uint32
	{ return Workspace::FunctionLayoutVersion; }
	auto MMaterialFunctionEditor::PostInitAssetEditor() -> void
	{
		FBaseAssetToolkit::PostInitAssetEditor();
		if (const auto* Owner = GetOwningAssetEditor())
			SessionSettings->SelectStorage("MaterialEditor/Profiles/" + FXxHash128::HashBuffer(GetLayoutProfileKey()).ToString() + ".yaml");
	}
	auto MMaterialFunctionEditor::OnHostChanged() -> void
	{
		SessionSettings->Save();
		FBaseAssetToolkit::OnHostChanged();
		PostInitAssetEditor();
	}

}
