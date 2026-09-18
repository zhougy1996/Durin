#include "Widgets/MMaterialFunctionEditor.h"
#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "DObject/Property.h"
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
		FMaterialGraphCanvas Canvas;
		std::unique_ptr<FMaterialPreview> Preview;
		FGuid Output;
		FMaterialFunctionPreviewInvalidation PreviewInvalidation;
		bool bPreviewValid = false;
		bool bLayout = false;
		bool bOutputPort = false;
		FGuid EditingPort;
		FGuid SelectedPortNode;
		FMaterialFunctionPort PortDraft;
		std::array<char, 129> PortName{};
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		FGuid EditingNode;
		TStrongObjectPtr<DMaterialExpression> NodeDraft;
		auto Function() const -> DMaterialFunction* { return Cast<DMaterialFunction>(Owner.Get()); }
		auto Material() const -> DMaterial* { return Cast<DMaterial>(PreviewMaterial.Get()); }
		~FDocument()
		{
			Preview.reset();
			if (auto* Value = Material()) { FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Value); MarkAsGarbage(Value); }
			if (PreviewPackage.Get()) MarkAsGarbage(PreviewPackage.Get());
		}
	};

	MMaterialFunctionEditor::MMaterialFunctionEditor(FWorkspaceManager& InManager) : Manager(InManager)
	{
		MoveObserver = RegisterAssetMoveObserver(this);
	}
	MMaterialFunctionEditor::~MMaterialFunctionEditor() { UnregisterAssetMoveObserver(MoveObserver); }
	auto MMaterialFunctionEditor::Find(std::string_view Resource) const -> FDocument*
	{
		const auto It = Open.find(std::string(Resource));
		return It == Open.end() ? nullptr : It->second.get();
	}
	auto MMaterialFunctionEditor::OpenDocument(const FDocumentTab& Tab) -> EDocumentOpenResult
	{
		if (Find(Tab.ResourceId)) return EDocumentOpenResult::Opened;
		FObjectPath Path;
		if (const auto PathValidation = FObjectPath::TryCreate(Tab.ResourceId, Path); !PathValidation) { Error = Durin::FormatObjectError(PathValidation.Error); return EDocumentOpenResult::Rejected; }
		DMaterialFunction* Function = nullptr;
		const auto Loaded = LoadObject(Path, Function);
		if (!Loaded || !Function) { Error = Loaded ? "The asset is not an editable function." : Loaded.Message; return EDocumentOpenResult::Rejected; }
		auto Document = std::make_unique<FDocument>();
		Document->Owner = Function;
		Document->PreviewInvalidation.SetFunction(Function);
		const auto Mount = FMountPaths::FindMountForVirtualPath(Function->GetPackage()->GetPackagePath());
		if (!Mount) { Error = Mount.Message; return EDocumentOpenResult::Rejected; }
		FPackagePath PreviewPath;
		if (const auto PathValidation = FPackagePath::TryCreate(std::format("{}__FunctionPreview/{}", Mount.Mount->VirtualRoot, FGuid::NewGuid().ToString()), PreviewPath); !PathValidation)
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
		for (auto& [Resource, Document] : Open) Document->Canvas.CancelInteraction();
		return true;
	}
	auto MMaterialFunctionEditor::RequestCloseDocument(const FDocumentTab& Tab) -> EDocumentCloseResult
	{
		if (IsDocumentDirty(Tab)) return EDocumentCloseResult::PendingConfirmation;
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
				Document->Owner = Cast<DMaterialFunction>(Replacement->FindTopLevelAsset(Function->GetFName()));
				Document->Canvas.CancelInteraction(); Document->PreviewInvalidation.RequestRefresh(); Document->EditingPort = {}; Document->SelectedPortNode = {}; Document->EditingNode = {};
			}
	}
	auto MMaterialFunctionEditor::OnAssetsRelocated(std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		for (const auto& Mapping : Mappings)
			if (auto Node = Open.extract(Mapping.SourcePath.ToString()); !Node.empty())
			{
				Node.key() = Mapping.DestinationPath.ToString(); Open.insert(std::move(Node));
				Manager.RemapResourceId(Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
			}
	}
	auto MMaterialFunctionEditor::ResetLayout() -> void { for (auto& [Resource, Document] : Open) Document->bLayout = false; }
	auto MMaterialFunctionEditor::NavigateToNode(std::string_view Resource, const FGuid& NodeId) -> bool
	{
		if (!Manager.OpenAsset(std::string(Resource), DMaterialFunction::StaticClass()->GetQualifiedName().ToString())) return false;
		auto* Document = Find(Resource);
		if (!Document || !Document->Function()) return false;
		const auto& Nodes = Document->Function()->GetExpressionCollection().Expressions;
		return std::ranges::any_of(Nodes, [&](const auto& Node) { return Node->Id == NodeId; }) && Document->Canvas.SelectAndFrame(NodeId);
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
			[this](const FDocumentTab& Tab, bool Visible) { if (auto* Document = Find(Tab.ResourceId)) Document->Preview->SetVisible(Visible); });
	}

	auto MMaterialFunctionEditor::DrawInterface(FDocument& Document) -> void
	{
		auto& Function = *Document.Function();
		FMaterialGraphDocument Graph(Function);
		const auto Apply = [&](FMaterialGraphCommandResult Result) { if (!Result) Error = FormatMaterialGraphCommandResult(Result); };
		ImGui::SeparatorText("Interface");
		if (Document.Canvas.GetSelection().size() != 1) Document.SelectedPortNode = {};
		for (const auto& Selection : Document.Canvas.GetSelection())
			if (const auto* Id = std::get_if<FGuid>(&Selection); Id && Document.Canvas.GetSelection().size() == 1)
				for (const auto& Expression : Function.GetExpressionCollection().Expressions)
					if (Expression->Id == *Id)
					{
						const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get());
						const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get());
						const auto* Port = Input ? &Input->Port : Output ? &Output->Port : nullptr;
						if (!Port) Document.SelectedPortNode = {};
						if (Port && Document.SelectedPortNode != *Id)
						{
							Document.SelectedPortNode = *Id;
							Document.EditingPort = Port->Id;
							Document.bOutputPort = Output != nullptr;
							Document.PortDraft = *Port;
							std::snprintf(Document.PortName.data(), Document.PortName.size(), "%s", Port->Name.c_str());
						}
					}
		for (bool Output : {false, true})
		{
			ImGui::PushID(Output ? "Outputs" : "Inputs");
			for (const auto& Port : Output ? Function.GetFunctionSignature().Outputs : Function.GetFunctionSignature().Inputs)
				if (ImGui::Selectable(std::format("{} ({})##{}", Port.Name, GetProgramTypeName(Port.Type), Port.Id.ToString()).c_str(), Document.EditingPort == Port.Id))
				{
					Document.EditingPort = Port.Id; Document.bOutputPort = Output; Document.PortDraft = Port;
					std::snprintf(Document.PortName.data(), Document.PortName.size(), "%s", Port.Name.c_str());
				}
			ImGui::PopID();
		}
		const bool NewInput = ImGui::Button("New Input");
		ImGui::SameLine();
		const bool NewOutput = ImGui::Button("New Output");
		if (NewInput || NewOutput)
		{
			Document.bOutputPort = NewOutput;
			Document.EditingPort = {}; Document.PortDraft = {};
			Document.PortDraft.Default.Kind = NewOutput ? EMaterialFunctionDefaultKind::None : EMaterialFunctionDefaultKind::Numeric;
			Document.PortName.fill(0);
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
					ImGui::InputFloat4("Default", &Document.PortDraft.Default.Numeric.X);
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
						ImGui::InputFloat4(MaterialSurfaceNames[Index], &Value.X);
						ImGui::PopID();
					}
			}
			else Document.PortDraft.Default = {};
		}
		if (ImGui::Button(Document.EditingPort.IsValid() ? "Apply Port" : "Add Port"))
		{
			Document.PortDraft.Name = Document.PortName.data();
			if (Document.bOutputPort) { Document.PortDraft.bRequired = false; Document.PortDraft.Default = {}; }
			if (Document.EditingPort.IsValid())
			{
				Apply(Graph.SetPort(Document.bOutputPort, Document.PortDraft, GEditor->GetTransactor()));
			}
			else Apply(Graph.AddPort(Document.bOutputPort, Document.PortDraft, {}, 0, 240, GEditor->GetTransactor()));
		}
		if (Document.EditingPort.IsValid())
		{
			ImGui::SameLine();
			if (ImGui::Button("Remove Port")) Apply(Graph.RemovePort(Document.bOutputPort, Document.EditingPort, GEditor->GetTransactor()));
		}
		ImGui::Spacing();
		for (const auto& Selection : Document.Canvas.GetSelection())
			if (const auto* Id = std::get_if<FGuid>(&Selection))
			{
				const auto& Nodes = Function.GetExpressionCollection().Expressions;
				const auto Node = std::ranges::find_if(Nodes, [&](const auto& Expression) { return Expression->Id == *Id; });
				if (Node == Nodes.end()) continue;
				if (Document.EditingNode != *Id || !Document.NodeDraft.Get() || Document.NodeDraft->GetClass() != (*Node)->GetClass())
				{
					Document.EditingNode = *Id;
					Document.NodeDraft = TStrongObjectPtr<DMaterialExpression>(DuplicateObject(Node->Get(), nullptr, NAME_None).Object);
				}
				if (!Document.NodeDraft.Get()) continue;
				auto* Draft = Document.NodeDraft.Get();
				// Preserve unapplied fields while refreshing links/defaults changed by shared commands.
				Draft->GetClass()->ForEachProperty([&](FProperty* Property) {
					if (Property->NamePrivate == FName("Value") || Property->NamePrivate == FName("Components")
						|| Property->NamePrivate == FName("AttributeMask") || Property->NamePrivate == FName("Attributes")) return;
					for (uint32 Element = 0; Element < Property->GetArrayDim(); ++Element)
						Property->CopyAssignValue(Property->GetValuePtr(Draft, Element), Property->GetValuePtr(Node->Get(), Element));
				});
				if (auto* Constant = Cast<DMaterialExpressionScalarConstant>(Draft)) ImGui::InputFloat("Value", &Constant->Value);
				else if (auto* Constant = Cast<DMaterialExpressionVector2Constant>(Draft))
				{
					std::array<float, 2> Value{static_cast<float>(Constant->Value.x), static_cast<float>(Constant->Value.y)};
					if (ImGui::InputFloat2("Value", Value.data())) Constant->Value = {Value[0], Value[1]};
				}
				else if (auto* Constant = Cast<DMaterialExpressionVector3Constant>(Draft))
				{
					std::array<float, 3> Value{static_cast<float>(Constant->Value.x), static_cast<float>(Constant->Value.y), static_cast<float>(Constant->Value.z)};
					if (ImGui::InputFloat3("Value", Value.data())) Constant->Value = {Value[0], Value[1], Value[2]};
				}
				else if (auto* Constant = Cast<DMaterialExpressionVector4Constant>(Draft))
				{
					std::array<float, 4> Value{static_cast<float>(Constant->Value.x), static_cast<float>(Constant->Value.y), static_cast<float>(Constant->Value.z), static_cast<float>(Constant->Value.w)};
					if (ImGui::InputFloat4("Value", Value.data())) Constant->Value = {Value[0], Value[1], Value[2], Value[3]};
				}
				if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Draft))
					for (size_t Index = 0; Index < Swizzle->Components.size(); ++Index)
					{
						ImGui::PushID(static_cast<int>(Index));
						int Component = Swizzle->Components[Index];
						if (ImGui::Combo("Channel", &Component, "R\0G\0B\0A\0")) Swizzle->Components[Index] = static_cast<uint8>(std::clamp(Component, 0, 3));
						ImGui::PopID();
					}
				DrawMaterialFunctionCallInputs(Function, *Id, *GEditor->GetTransactor(), Error);
				if (auto* GetSurface = Cast<DMaterialExpressionGetSurfaceAttributes>(Draft))
					for (uint32 Index = 0; Index < 8; ++Index)
					{
						bool Enabled = (GetSurface->AttributeMask & (1 << Index)) != 0;
						if (ImGui::Checkbox(MaterialSurfaceNames[Index], &Enabled))
							GetSurface->AttributeMask = Enabled ? GetSurface->AttributeMask | (1 << Index) : GetSurface->AttributeMask & ~(1 << Index);
					}
				if (auto* SetSurface = Cast<DMaterialExpressionSetSurfaceAttributes>(Draft))
					for (uint32 Index = 0; Index < 8; ++Index)
					{
						const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
						ImGui::PushID(static_cast<int>(Index));
						if (ImGui::BeginCombo(MaterialSurfaceNames[Index], "Override source"))
						{
							if (ImGui::Selectable("Keep base value")) std::erase_if(SetSurface->Attributes, [&](const auto& Binding) { return Binding.Attribute == Attribute; });
							for (const auto& Source : Graph.Inspect().Nodes)
								for (const auto& Pin : Source.Outputs)
									if (Pin.Type == GetMaterialSurfaceOutputType(Attribute) && ImGui::Selectable(std::format("{}: {}##{}{}", Source.PrimaryLabel, Pin.Name, Source.Node.Id.ToString(), Pin.PortId.ToString()).c_str()))
									{
										std::erase_if(SetSurface->Attributes, [&](const auto& Binding) { return Binding.Attribute == Attribute; });
										SetSurface->Attributes.push_back({Attribute, {Source.Node.Id, Pin.OutputIndex, Pin.PortId}});
									}
							ImGui::EndCombo();
						}
						ImGui::PopID();
					}
				if (ImGui::Button("Apply Node")) Apply(Graph.ReplaceExpression(*Draft, GEditor->GetTransactor()));
				break;
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
		if (!Document.bLayout) { Workspace::BuildDefaultLayout(Tab, Size); Document.bLayout = true; }
		WorkspaceUI::SubmitDockSpace(DockType, Workspace::LayoutVersion, Size);
		if (WorkspaceUI::BeginDockablePanel(DockType, "Material Graph", "Graph"))
			Document.Canvas.DrawFunction(Function, *GEditor->GetTransactor(), 0, [this](std::string Message) { Error = std::move(Message); },
				[this](std::string_view Path) { Manager.OpenAsset(std::string(Path), DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); });
		ImGui::End();
		if (WorkspaceUI::BeginDockablePanel(DockType, "Details", "Details"))
		{
			Document.Canvas.DrawSelectionDetails(Function, *GEditor->GetTransactor(),
				[this](std::string Message) { Error = std::move(Message); });
			DrawInterface(Document);
		}
		ImGui::End();
		const bool PreviewVisible = WorkspaceUI::BeginDockablePanel(DockType, "Preview", "Preview");
		Document.Preview->SetVisible(PreviewVisible);
		if (PreviewVisible)
		{
			ImGui::TextDisabled("Required inputs use neutral preview values; optional inputs use their defaults.");
			if (Document.bPreviewValid) Document.Preview->Draw(Document.Material());
			else ImGui::TextDisabled("The selected output is unavailable. See diagnostics.");
		}
		ImGui::End();
		if (WorkspaceUI::BeginDockablePanel(DockType, "Diagnostics", "Diagnostics"))
		{
			const auto DrawDiagnostic = [&](const FMaterialProgramDiagnostic& Diagnostic) {
				if (ImGui::Selectable(FormatMaterialError(Diagnostic.Error).c_str()))
				{
					if (Diagnostic.FunctionAssetPath.empty() || Diagnostic.FunctionAssetPath == Function.GetObjectPath()) Document.Canvas.SelectAndFrame(Diagnostic.NodeId);
					else if (Manager.OpenAsset(Diagnostic.FunctionAssetPath, DMaterialFunction::StaticClass()->GetQualifiedName().ToString()))
						if (auto* Nested = Find(Diagnostic.FunctionAssetPath)) Nested->Canvas.SelectAndFrame(Diagnostic.NodeId);
				}
			};
			for (const auto& Diagnostic : Document.Diagnostics) DrawDiagnostic(Diagnostic);
			for (const auto& Diagnostic : Document.Material()->GetMaterialCompileDiagnostics()) DrawDiagnostic(Diagnostic.Source);
		}
		ImGui::End();
	}
}
