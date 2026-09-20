#include "Widgets/MMaterialFunctionEditor.h"
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
#include "Editor/AssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transaction.h"
#include "Editor/WorkspaceManager.h"
#include "Misc/MountPaths.h"

namespace Durin::Editor::Material
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

	MMaterialFunctionEditor::MMaterialFunctionEditor(FWorkspaceManager& InManager)
		: SessionSettings(std::make_unique<FMaterialEditorSessionSettings>("MaterialFunctionEditorSession.yaml")), Manager(InManager)
	{
		SessionSettings->Load();
		MoveObserver = RegisterAssetMoveObserver(this);
	}
	MMaterialFunctionEditor::~MMaterialFunctionEditor()
	{
		for (const auto& [Resource, Document] : Open)
		{
			const auto [Zoom, Pan] = Document->Canvas->GetViewport();
			SessionSettings->SetViewport(Resource, {.Zoom = Zoom, .Pan = Pan});
		}
		SessionSettings->Save();
		UnregisterAssetMoveObserver(MoveObserver);
	}
	auto MMaterialFunctionEditor::Find(std::string_view Resource) const -> FDocument*
	{
		const auto It = Open.find(std::string(Resource));
		return It == Open.end() ? nullptr : It->second.get();
	}
	auto MMaterialFunctionEditor::OpenDocument(const FDocumentTab& Tab) -> EDocumentOpenResult
	{
		if (Find(Tab.ResourceId)) return EDocumentOpenResult::Opened;
		FObjectPath Path;
		if (const auto PathValidation = FObjectPath::TryCreateWithDiagnostic(Tab.ResourceId, Path); !PathValidation) { Error = Durin::FormatObjectError(PathValidation.Error); return EDocumentOpenResult::Rejected; }
		DMaterialFunction* Function = nullptr;
		const auto Loaded = LoadObject(Path, Function);
		if (!Loaded || !Function) { Error = Loaded ? "The asset is not an editable function." : Loaded.Message; return EDocumentOpenResult::Rejected; }
		auto Document = std::make_unique<FDocument>();
		Document->Owner = Function;
		Document->Canvas = std::make_unique<FMaterialGraphCanvas>(FMaterialGraphDocument(*Function),
			FMaterialGraphEditorServices{
				.ReportError = [this](std::string Message) { Error = std::move(Message); },
				.OpenFunction = [this](std::string_view Path) { Manager.OpenAsset(std::string(Path), DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); }});
		if (const auto* State = SessionSettings->FindViewport(Tab.ResourceId))
			Document->Canvas->SetViewport(State->Zoom, State->Pan);
		Document->PreviewInvalidation.SetFunction(Function);
		const auto Mount = FMountPaths::FindMountForVirtualPath(Function->GetPackage()->GetPackagePath());
		if (!Mount) { Error = Mount.Message; return EDocumentOpenResult::Rejected; }
		FPackagePath PreviewPath;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(std::format("{}__FunctionPreview/{}", Mount.Mount->VirtualRoot, FGuid::NewGuid().ToString()), PreviewPath); !PathValidation)
		{
			Error = Durin::FormatObjectError(PathValidation.Error);
			return EDocumentOpenResult::Rejected;
		}
		auto* Package = NewObject<DPackage>(DPackage::StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		Package->InitializeAssetPackage(PreviewPath);
		Package->SetStandaloneResidency(false);
		Document->PreviewPackage = FStrongObjectPtr(Package);
		auto* Material = NewObject<DMaterial>(DMaterial::StaticClass(), Package, "Preview", EObjectFlags::Transient);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		Document->PreviewMaterial = FStrongObjectPtr(Material);
		Document->Preview = std::make_unique<FMaterialPreview>(Tab.Id.Value);
		if (!Function->GetFunctionSignature().Outputs.empty()) Document->Output = Function->GetFunctionSignature().Outputs.front().Id;
		Open.emplace(Tab.ResourceId, std::move(Document));
		return EDocumentOpenResult::Opened;
	}
	auto MMaterialFunctionEditor::ActivateDocument(const FDocumentTab& Tab) -> void
	{
		RequestDeactivate();
		if (auto* Document = Find(Tab.ResourceId)) Documents.Activate(Tab, Document->Function());
	}
	auto MMaterialFunctionEditor::RequestDeactivate() -> bool
	{
		for (auto& [Resource, Document] : Open) Document->Canvas->CancelInteraction();
		return true;
	}
	auto MMaterialFunctionEditor::RequestCloseDocument(const FDocumentTab& Tab) -> EDocumentCloseResult
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
	auto MMaterialFunctionEditor::SaveDocument(const FDocumentTab& Tab) -> bool
	{
		auto* Document = Find(Tab.ResourceId);
		return Document && Documents.Save(Document->Function(), {}, [this](std::string Message) { Error = std::move(Message); });
	}
	auto MMaterialFunctionEditor::DiscardDocument(const FDocumentTab& Tab) -> bool
	{
		auto* Document = Find(Tab.ResourceId);
		return Document && Documents.Discard(Document->Function(), [this] { RequestDeactivate(); },
			[this](DPackage* Old, DPackage* New) { Manager.NotifyPackageReloaded(Old, New); },
			[this](std::string Message) { Error = std::move(Message); });
	}
	auto MMaterialFunctionEditor::IsDocumentDirty(const FDocumentTab& Tab) const -> bool
	{
		const auto* Document = Find(Tab.ResourceId);
		return Document && Documents.IsDirty(Document->Function());
	}
	auto MMaterialFunctionEditor::CanSaveActiveDocument() const -> bool
	{
		const auto* Document = Find(Documents.GetActiveResourceId());
		return Document && Documents.CanSave(Document->Function());
	}
	auto MMaterialFunctionEditor::SaveActiveDocument() -> bool
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
							.OpenFunction = [this](std::string_view Path) { Manager.OpenAsset(std::string(Path), DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); }});
					Document->Canvas->SetViewport(Zoom, Pan);
				}
				Document->PreviewInvalidation.RequestRefresh(); Document->EditingPort = {}; Document->SelectedPortNode = {};
			}
	}
	auto MMaterialFunctionEditor::OnAssetsRelocated(std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		for (const auto& Mapping : Mappings)
			if (auto Node = Open.extract(Mapping.SourcePath.ToString()); !Node.empty())
			{
				SessionSettings->MoveViewport(Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
				Node.key() = Mapping.DestinationPath.ToString(); Open.insert(std::move(Node));
				Manager.RemapResourceId(Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
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
		if (!Manager.OpenAsset(std::string(Resource), DMaterialFunction::StaticClass()->GetQualifiedName().ToString())) return false;
		auto* Document = Find(Resource);
		if (!Document || !Document->Function()) return false;
		const auto& Nodes = Document->Function()->GetExpressionCollection().Expressions;
		return std::ranges::any_of(Nodes, [&](const auto& Node) { return Node->Id == NodeId; }) && Document->Canvas->SelectAndFrame(NodeId);
	}
	auto MMaterialFunctionEditor::DrawWorkspace(bool bActive) -> bool
	{
		if (!bActive) RequestDeactivate();
		std::vector<FDocumentId> Deleted;
		for (const auto& Tab : Manager.GetDocuments())
			if (Tab.WorkspaceType == MaterialFunctionWorkspaceType)
				if (auto* Document = Find(Tab.ResourceId); Document && !Document->Function()) Deleted.push_back(Tab.Id);
		for (const auto Id : Deleted) Manager.RequestCloseDocument(Id);
		return Documents.GetDocumentHost().DrawDocuments(Manager, MaterialFunctionWorkspaceType, "MaterialFunctionEditor",
			[this](const FDocumentTab& Tab) { const auto* Document = Find(Tab.ResourceId); return Document && Document->Function(); },
			[this](const FDocumentTab& Tab) { DrawDocument(Tab, *Find(Tab.ResourceId)); },
			[this](const FDocumentTab& Tab, bool Visible) {
				if (auto* Document = Find(Tab.ResourceId)) Document->Preview->SetVisible(Visible);
				if (!Visible)
				{
					const auto DockType = Workspace::MakeDocumentDockType(Tab);
					if (ImGui::DockBuilderGetNode(WorkspaceUI::MakeDockSpaceId(DockType, Workspace::FunctionLayoutVersion)))
						WorkspaceUI::SubmitDockSpace(DockType, Workspace::FunctionLayoutVersion,
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
			const auto Result = Graph.AddPort(false, Port, {}, 0,
				static_cast<int32>(Signature.Inputs.size()) * 120, GEditor->GetTransactor());
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
			if (ImGui::Button("Open Function")) Manager.OpenAsset(Call->Function->GetObjectPath(),
				Call->Function->GetClass()->GetQualifiedName().ToString());
			DrawMaterialFunctionCallInputs(Function, *Id, *GEditor->GetTransactor(), Error);
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
			Apply(Graph.SetPort(Document.bOutputPort, Document.PortDraft, GEditor->GetTransactor()));
		}
		if (Document.EditingPort.IsValid())
		{
			ImGui::SameLine();
			if (ImGui::Button("Remove Port")) Apply(Graph.RemovePort(Document.bOutputPort, Document.EditingPort, GEditor->GetTransactor()));
		}
	}

	auto MMaterialFunctionEditor::DrawDocument(const FDocumentTab& Tab, FDocument& Document) -> void
	{
		auto& Function = *Document.Function();
		if (!GEditor) { ImGui::TextDisabled("Editor transactions are unavailable."); return; }
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
		else if (Document.Material()->GetMaterialCompileStatus().State == EMaterialCompileState::NeedsCompile)
			Document.Material()->CompileEdits();
		const auto DockType = Workspace::MakeDocumentDockType(Tab);
		const auto Size = ImGui::GetContentRegionAvail();
		if (Size.x <= 0.0f || Size.y <= 0.0f) return;
		if (Document.bResetLayout || !ImGui::DockBuilderGetNode(WorkspaceUI::MakeDockSpaceId(DockType, Workspace::FunctionLayoutVersion)))
		{
			Workspace::BuildDefaultLayout(Tab, Size, true);
			Document.bResetLayout = false;
		}
		WorkspaceUI::SubmitDockSpace(DockType, Workspace::FunctionLayoutVersion, Size);
		const auto BeginPanel = [&](const char* Label, const char* Key, bool* Open = nullptr) {
			const bool Visible = WorkspaceUI::BeginDockablePanel(DockType, Label, Key, Open, ImGuiWindowFlags_NoCollapse);
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				const auto* Active = Manager.GetActiveDocument();
				if (!Active || Active->Id != Tab.Id) Manager.ActivateDocument(Tab.Id);
			}
			return Visible;
		};
		if (BeginPanel("Material Graph", "Graph"))
			Document.Canvas->Draw(*GEditor->GetTransactor(), 0);
		ImGui::End();
		if (SessionSettings->bDetailsVisible)
		{
			if (BeginPanel("Details", "Details", &SessionSettings->bDetailsVisible))
			{
				Document.Canvas->DrawSelectionDetails(*GEditor->GetTransactor());
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
				if (ImGui::Selectable(FormatMaterialError(Diagnostic.Error).c_str()))
				{
					if (Diagnostic.FunctionAssetPath.empty() || Diagnostic.FunctionAssetPath == Function.GetObjectPath()) Document.Canvas->SelectAndFrame(Diagnostic.NodeId);
					else if (Manager.OpenAsset(Diagnostic.FunctionAssetPath, DMaterialFunction::StaticClass()->GetQualifiedName().ToString()))
						if (auto* Nested = Find(Diagnostic.FunctionAssetPath)) Nested->Canvas->SelectAndFrame(Diagnostic.NodeId);
				}
			};
			for (const auto& Diagnostic : Document.Diagnostics) DrawDiagnostic(Diagnostic);
			for (const auto& Diagnostic : Document.Material()->GetMaterialCompileDiagnostics()) DrawDiagnostic(Diagnostic.Source);
			if (Document.bPreviewValid) Document.Preview->Draw(Document.Material());
			else ImGui::TextDisabled("The selected output is unavailable.");
		}
		if (bPreviewPanel) ImGui::End();
	}
}
