#include "FMaterialEditor.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "MaterialEditorFunctionDocument.h"
#include "Toolkits/ToolkitManager.h"
#include "Misc/UnifiedError.h"
#include "Materials/Material.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "Settings/MaterialEditorProfileSettings.h"
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
#include "Transactions/Transactor.h"
#include "Documents/EditorDocumentManager.h"
#include "Misc/MountPaths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"

namespace Durin::MaterialEditor
{
	auto FMaterialEditor::FFunctionDocument::Material() const -> DMaterial*
	{ return Cast<DMaterial>(PreviewMaterial.Get()); }

	auto FMaterialEditor::FFunctionDocument::RetirePreview() -> void
	{
		if (Preview) Preview->SetVisible(false);
		Preview.reset();
		if (auto* Value = Material()) FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Value);
	}

	FMaterialEditor::FFunctionDocument::~FFunctionDocument()
	{
		RetirePreview();
		if (PreviewPackage.Get()) MarkObjectHierarchyAsGarbage(PreviewPackage.Get());
		if (WorkingPackage && Transactor.IsValid()) Transactor.Get()->ForgetPackage(*WorkingPackage);
		if (WorkingPackage) MarkObjectHierarchyAsGarbage(WorkingPackage.Get());
	}


	auto FMaterialEditor::SaveFunctionViewports() -> void
	{
		for (const auto& [Resource, Document] : FunctionDocuments)
		{
			const auto [Zoom, Pan] = Document->Canvas->GetViewport();
			ProfileSettings->SetViewport(Resource, {.Zoom = Zoom, .Pan = Pan});
		}
		ProfileSettings->Save();
	}
	auto FMaterialEditor::FindFunctionDocument(std::string_view Resource) const -> FFunctionDocument*
	{
		const auto It = FunctionDocuments.find(std::string(Resource));
		return It == FunctionDocuments.end() ? nullptr : It->second.get();
	}
	auto FMaterialEditor::FindOpenFunction(std::string_view Resource) const -> DMaterialFunction*
	{
		const auto* Document = FindFunctionDocument(Resource);
		return Document ? Document->Function() : nullptr;
	}
	auto FMaterialEditor::ApplyFunctionDocument(std::string_view Resource, bool bForce) -> bool
	{
		const auto It = FunctionDocuments.find(std::string(Resource));
		if (!IsEditorOpen() || It == FunctionDocuments.end()) return false;
		const auto Document = It->second;
		std::string Error;
		if (!Document->Publish(Error, bForce)) { SetError(std::move(Error)); return false; }
		const auto Current = FunctionDocuments.find(std::string(Resource));
		return IsEditorOpen() && Current != FunctionDocuments.end() && Current->second == Document;
	}

	auto FMaterialEditor::SaveFunction(DMaterialFunction* Function) -> bool
	{
		const auto It = std::ranges::find_if(FunctionDocuments, [&](const auto& Entry) { return Entry.second->Function() == Function; });
		if (It == FunctionDocuments.end()) return false;
		const auto Resource = It->first;
		const auto Document = It->second;
		if (!RequestDeactivate() || !ApplyFunctionDocument(Resource)) return false;
		return Documents.Save(Document->GetSource(), {}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto FMaterialEditor::IsFunctionDocumentDirty(std::string_view Resource) const -> bool
	{
		const auto It = FunctionDocuments.find(std::string(Resource));
		return It != FunctionDocuments.end() && It->second->IsDirty();
	}

	auto FMaterialEditor::IsFunctionPublishing(std::string_view Resource) const -> bool
	{
		const auto It = FunctionDocuments.find(std::string(Resource));
		return It != FunctionDocuments.end() && It->second->bPublishing;
	}

	auto FMaterialEditor::ReleaseFunctionDocuments() -> void
	{
		const auto Snapshot = FunctionDocuments;
		FunctionDocuments.clear();
		for (const auto& [Resource, Document] : Snapshot) Document->RetirePreview();
	}

	auto FMaterialEditor::RegisterFunctionUndo() -> void
	{
		if (FunctionUndoHandle || !Documents.GetEditor()) return;
		FunctionUndoHandle = Documents.GetEditor()->GetTransactor()->AddTransactionListener(
			[Weak = weak_from_this()](const FTransactionEvent& Event) {
				if (auto Owner = std::dynamic_pointer_cast<FMaterialEditor>(Weak.lock()); Owner && Owner->IsEditorOpen())
					Owner->HandleFunctionTransaction(Event);
			});
	}

	auto FMaterialEditor::UnregisterFunctionUndo() -> void
	{
		if (FunctionUndoHandle && Documents.GetEditor())
			Documents.GetEditor()->GetTransactor()->RemoveTransactionListener(FunctionUndoHandle);
		FunctionUndoHandle = 0;
	}

	auto FMaterialEditor::HandleFunctionTransaction(const FTransactionEvent& Event) -> void
	{
		if (Event.Type != ETransactionEventType::Undone && Event.Type != ETransactionEventType::Redone) return;
		auto* Transactions = Documents.GetEditor()->GetTransactor();
		std::vector<DPackage*> Packages;
		for (size_t Index = 0; Index < Transactions->GetQueueLength(); ++Index)
			if (const auto* Entry = Transactions->GetTransaction(Index); Entry && Entry->GetId() == Event.Id)
			{ Packages = Entry->GetAffectedPackages(); break; }
		const auto Snapshot = FunctionDocuments;
		for (const auto& [Resource, Document] : Snapshot)
		{
			if (!IsEditorOpen()) return;
			const auto Current = FunctionDocuments.find(Resource);
			if (Current != FunctionDocuments.end() && Current->second == Document
				&& std::ranges::contains(Packages, Document->WorkingPackage.Get())) ApplyFunctionDocument(Resource, true);
		}
	}

	auto FMaterialEditor::ResetFunctionDocument(std::string_view Resource) -> bool
	{
		const auto It = std::ranges::find(DocumentManager.GetDocuments(), Resource, &FEditorDocument::ResourceId);
		if (It == DocumentManager.GetDocuments().end()) return false;
		const auto Tab = *It;
		if (IsFunctionPublishing(Resource)) { SetError("Function publication is in progress."); return false; }
		if (const auto* Document = FindFunctionDocument(Resource))
		{
			Document->Canvas->CancelInteraction();
			const auto [Zoom, Pan] = Document->Canvas->GetViewport();
			ProfileSettings->SetViewport(Resource, {.Zoom = Zoom, .Pan = Pan});
		}
		const bool bActive = Documents.GetActiveResourceId() == Resource;
		if (const auto Previous = FunctionDocuments.find(std::string(Resource)); Previous != FunctionDocuments.end()) Previous->second->RetirePreview();
		FunctionDocuments.erase(std::string(Resource));
		if (OpenFunctionDocument(Tab) != EDocumentOpenResult::Opened) return false;
		if (bActive) Documents.Activate(Tab, FindOpenFunction(Resource));
		return true;
	}

	auto FMaterialEditor::DiscardFunctionDocument(const FEditorDocument& Tab) -> bool
	{
		if (IsFunctionPublishing(Tab.ResourceId)) { SetError("Function publication is in progress."); return false; }
		return RequestDeactivate() && ResetFunctionDocument(Tab.ResourceId);
	}

	auto FMaterialEditor::OpenFunctionDocument(const FEditorDocument& Tab) -> EDocumentOpenResult
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		if (FindFunctionDocument(Tab.ResourceId)) return EDocumentOpenResult::Opened;
		FSoftObjectPath Path;
		if (const auto PathValidation = FSoftObjectPath::TryCreateWithDiagnostic(Tab.ResourceId, Path); !PathValidation) { ErrorMessage = ToString(PathValidation.error()); return EDocumentOpenResult::Rejected; }
		DMaterialFunction* Function = nullptr;
		const auto Loaded = LoadObject<DMaterialFunction>(Path, nullptr, &LoadedError);
		Function = Loaded;
		if (!Loaded || !Function) { ErrorMessage = LoadedError ? LoadedError->ToString() : "The asset is not an editable function."; return EDocumentOpenResult::Rejected; }
		auto Document = std::make_shared<FFunctionDocument>();
		std::string Error;
		if (!Document->Initialize(*Function, Documents.GetEditor() ? Documents.GetEditor()->GetTransactor() : nullptr, Error))
		{ SetError(std::move(Error)); return EDocumentOpenResult::Rejected; }
		Function = Document->Function();
		Document->Canvas = std::make_unique<FMaterialGraphCanvas>(FMaterialGraphDocument(*Function),
			FMaterialGraphEditorServices{
				.ReportError = [this](std::string Message) { ErrorMessage = std::move(Message); },
				.OpenFunction = [this](std::string_view Path) { OpenRelatedAsset(Path, DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); }});
		if (const auto* State = ProfileSettings->FindViewport(Tab.ResourceId))
			Document->Canvas->SetViewport(State->Zoom, State->Pan);
		Document->PreviewInvalidation.SetFunction(Function);
		const auto Mount = FMountPaths::FindMountForVirtualPath(Function->GetPackage()->GetPackagePath());
		if (!Mount) { ErrorMessage = (Mount ? std::string{} : ToString(Mount.error())); return EDocumentOpenResult::Rejected; }
		FPackagePath PreviewPath;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(std::format("{}__FunctionPreview/{}", Mount->Mount->VirtualRoot, FGuid::NewGuid().ToString()), PreviewPath); !PathValidation)
		{
			ErrorMessage = ToString(PathValidation.error());
			return EDocumentOpenResult::Rejected;
		}
		auto* Package = NewObject<DPackage>(DPackage::StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		Package->InitializeAssetPackage(PreviewPath);
		Document->PreviewPackage = FStrongObjectPtr(Package);
		auto* Material = NewObject<DMaterial>(DMaterial::StaticClass(), Package, "Preview", EObjectFlags::Transient);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		Document->PreviewMaterial = FStrongObjectPtr(Material);
		Document->Preview = std::make_shared<FMaterialPreview>(Tab.Id.Value);
		if (!Function->GetFunctionSignature().Outputs.empty()) Document->Output = Function->GetFunctionSignature().Outputs.front().Id;
		FunctionDocuments.emplace(Tab.ResourceId, std::move(Document));
		return EDocumentOpenResult::Opened;
	}

	auto FMaterialEditor::CancelFunctionInteractions() -> bool
	{
		if (std::ranges::any_of(FunctionDocuments, [](const auto& Entry) { return Entry.second->bPublishing; })) return false;
		for (auto& [Resource, Document] : FunctionDocuments) Document->Canvas->CancelInteraction();
		return true;
	}
	auto FMaterialEditor::CloseFunctionDocument(const FEditorDocument& Tab) -> void
	{
		if (const auto* Document = FindFunctionDocument(Tab.ResourceId))
		{
			const auto [Zoom, Pan] = Document->Canvas->GetViewport();
			ProfileSettings->SetViewport(Tab.ResourceId, {.Zoom = Zoom, .Pan = Pan});
			ProfileSettings->Save();
		}
		FunctionDocuments.erase(Tab.ResourceId);
	}

	auto FMaterialEditor::RebindFunctionDocuments(DPackage* Previous, DPackage* Replacement) -> void
	{
		(void)Replacement;
		std::vector<std::string> Resources;
		for (const auto& [Resource, Document] : FunctionDocuments)
		{
			FSoftObjectPath Path;
			if (FSoftObjectPath::TryCreate(Resource, Path) && Path.GetPackagePath().ToString() == Previous->GetPackagePath()) Resources.push_back(Resource);
		}
		for (const auto& Resource : Resources) ResetFunctionDocument(Resource);
	}

	auto FMaterialEditor::ResetFunctionLayout() -> void
	{
		ProfileSettings->bPreviewVisible = true;
		ProfileSettings->bDetailsVisible = true;
		ProfileSettings->bParametersVisible = true;
		for (auto& [Resource, Document] : FunctionDocuments) Document->bResetLayout = true;
	}
	auto FMaterialEditor::NavigateToNode(std::string_view Resource, const FGuid& NodeId) -> bool
	{
		FFunctionDocument* Document = nullptr;
		if (auto* Owner = GetOwningAssetEditor())
		{
			if (!OpenRelatedAsset(Resource, DMaterialFunction::StaticClass()->GetQualifiedName().ToString())) return false;
			for (const auto& Entry : FToolkitManager::Get().GetToolkits(Owner->GetAssetEditorSubsystem()))
				if (auto Toolkit = std::dynamic_pointer_cast<FMaterialEditor>(Entry))
					if ((Document = Toolkit->FindFunctionDocument(Resource))) break;
		}
		if (!Document || !Document->Function()) return false;
		const auto& Nodes = Document->Function()->GetExpressionCollection().Expressions;
		return std::ranges::any_of(Nodes, [&](const auto& Node) { return Node->Id == NodeId; }) && Document->Canvas->SelectAndFrame(NodeId);
	}
	auto FMaterialEditor::DrawFunctionDocuments(bool bActive) -> bool
	{
		if (!bActive) RequestDeactivate();
		std::vector<FDocumentId> Deleted;
		for (const auto& Tab : DocumentManager.GetDocuments())
			if (Tab.ToolkitId == MaterialFunctionWorkspaceType)
				if (auto* Document = FindFunctionDocument(Tab.ResourceId); Document && !Document->Function()) Deleted.push_back(Tab.Id);
		for (const auto Id : Deleted) DocumentManager.RequestCloseDocument(Id);
		return Documents.GetTabManager().DrawDocuments(DocumentManager, MaterialFunctionWorkspaceType, GetPresentationRootKey("MaterialFunctionEditor"),
			[this](const FEditorDocument& Tab) { const auto* Document = FindFunctionDocument(Tab.ResourceId); return Document && Document->Function(); },
			[this](const FEditorDocument& Tab) {
				if (const auto It = FunctionDocuments.find(Tab.ResourceId); It != FunctionDocuments.end())
				{ const auto Lease = It->second; const auto TabSnapshot = Tab; DrawDocument(TabSnapshot, *Lease); }
			},
			[this](const FEditorDocument& Tab, bool Visible) {
				if (auto* Document = FindFunctionDocument(Tab.ResourceId); Document && Document->Preview) Document->Preview->SetVisible(Visible);
				if (!Visible)
				{
					const auto DockType = Workspace::MakeDocumentDockType(Tab);
					if (ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::FunctionLayoutVersion)))
						TabLayout::SubmitDockSpace(DockType, Workspace::FunctionLayoutVersion,
							{0.0f, 0.0f}, ImGuiDockNodeFlags_KeepAliveOnly);
				}
			});
	}

	auto FMaterialEditor::DrawInputs(FFunctionDocument& Document) -> void
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
			if (!Result) ErrorMessage = FormatMaterialGraphCommandResult(Result);
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

	auto FMaterialEditor::DrawInterface(FFunctionDocument& Document) -> void
	{
		auto& Function = *Document.Function();
		FMaterialGraphDocument Graph(Function);
		const auto Apply = [&](FMaterialGraphCommandResult Result) { if (!Result) ErrorMessage = FormatMaterialGraphCommandResult(Result); };
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
			DrawMaterialFunctionCallInputs(Function, *Id, *Documents.GetEditor()->GetTransactor(), ErrorMessage);
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
		if (ImGui::Combo("Type", &Type, "Float\0Float2\0Float3\0Float4\0Texture2D\0TextureCube\0Material Attributes\0"))
		{
			Document.PortDraft.Type = static_cast<EMaterialProgramValueType>(Type);
			Document.PortDraft.Default = {};
			if (!Document.bOutputPort) Document.PortDraft.Default.Kind = Type < 4 ? EMaterialFunctionDefaultKind::Numeric
				: Type <= 5 ? EMaterialFunctionDefaultKind::Texture : EMaterialFunctionDefaultKind::MaterialAttributes;
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
					if (ImGui::Combo("Fallback", &Fallback, Document.PortDraft.Type == EMaterialProgramValueType::TextureCube ? "White\0Black\0" : "White\0Black\0Flat RG Normal\0")) Document.PortDraft.Default.TextureFallback = static_cast<EMaterialTextureFallback>(Fallback);
					auto& Sampler = Document.PortDraft.Default.Sampler;
					int Min = static_cast<int>(Sampler.MinFilter), Mag = static_cast<int>(Sampler.MagFilter);
					int U = static_cast<int>(Sampler.AddressU), V = static_cast<int>(Sampler.AddressV), W = static_cast<int>(Sampler.AddressW);
					if (ImGui::Combo("Min Filter", &Min, "Nearest\0Linear\0Nearest Mipmap Nearest\0Linear Mipmap Nearest\0Nearest Mipmap Linear\0Linear Mipmap Linear\0")) Sampler.MinFilter = static_cast<EMaterialSamplerMinFilter>(Min);
					if (ImGui::Combo("Mag Filter", &Mag, "Nearest\0Linear\0")) Sampler.MagFilter = static_cast<EMaterialSamplerMagFilter>(Mag);
					if (ImGui::Combo("Address U", &U, "Repeat\0Mirrored Repeat\0Clamp To Edge\0")) Sampler.AddressU = static_cast<EMaterialSamplerAddressMode>(U);
					if (ImGui::Combo("Address V", &V, "Repeat\0Mirrored Repeat\0Clamp To Edge\0")) Sampler.AddressV = static_cast<EMaterialSamplerAddressMode>(V);
					if (ImGui::Combo("Address W", &W, "Repeat\0Mirrored Repeat\0Clamp To Edge\0")) Sampler.AddressW = static_cast<EMaterialSamplerAddressMode>(W);
				}
				else if (Document.PortDraft.Default.Kind == EMaterialFunctionDefaultKind::MaterialAttributes)
					for (uint32 Index = 0; Index < FMaterialAttributeDefinitionMap::GetDefinitions().size(); ++Index)
					{
						ImGui::PushID(static_cast<int>(Index));
						auto& Value = GetMaterialSurfaceOutputDefault(Document.PortDraft.Default.Surface, FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property);
						ImGui::InputScalarN(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Name.data(), ImGuiDataType_Float, &Value.X,
							static_cast<int>(GetMaterialSurfaceOutputType(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property)) + 1);
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

	auto FMaterialEditor::DrawDocument(const FEditorDocument& Tab, FFunctionDocument& Document) -> void
	{
		auto& Function = *Document.Function();
		const auto PreviewLease = Document.Preview;
		const auto IsCurrent = [&] { return IsEditorOpen() && FindFunctionDocument(Tab.ResourceId) == &Document; };
		if (!Documents.GetEditor()) { ImGui::TextDisabled("Editor transactions are unavailable."); return; }
		if (ImGui::Button("Apply"))
		{
			if (RequestDeactivate()) ApplyFunctionDocument(Tab.ResourceId);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save")) SaveDocument(Tab);
		if (!IsCurrent()) return;
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
			ImGui::MenuItem("Preview", nullptr, &ProfileSettings->bPreviewVisible);
			ImGui::MenuItem("Details", nullptr, &ProfileSettings->bDetailsVisible);
			ImGui::MenuItem("Inputs", nullptr, &ProfileSettings->bParametersVisible);
			if (ImGui::MenuItem("Reset Layout")) ResetFunctionLayout();
			ImGui::EndPopup();
		}
		if (!ErrorMessage.empty()) ImGui::TextWrapped("%s", ErrorMessage.c_str());
		Document.PreviewInvalidation.SetFunction(&Function);
		if (Document.PreviewInvalidation.ConsumeRefreshRequest())
		{
			auto Result = BuildMaterialFunctionPreview(Function, Document.Output, *Document.Material());
			if (!IsCurrent()) return;
			Document.Diagnostics = Result.Diagnostics;
			Document.bPreviewValid = static_cast<bool>(Result);
			if (!Result) ErrorMessage = FormatMaterialGraphCommandResult(Result);
			else Document.Material()->CompileEdits();
		}
		else if (Document.Material()->GetMaterialCompileStatus().HasUnsubmittedEdits())
			Document.Material()->CompileEdits();
		if (!IsCurrent()) return;
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
		const auto BeginPanel = [&](const char* Label, const char* Key, bool* bOpen = nullptr) {
			const bool Visible = TabLayout::BeginDockablePanel(DockType, Label, Key, bOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				const auto* Active = DocumentManager.GetActiveDocument();
				if (!Active || Active->Id != Tab.Id) DocumentManager.ActivateDocument(Tab.Id);
			}
			return Visible;
		};
		if (BeginPanel("Material Graph", "Graph"))
			Document.Canvas->Draw(*Documents.GetEditor()->GetTransactor(), 0);
		ImGui::End();
		if (!IsCurrent()) return;
		if (ProfileSettings->bDetailsVisible)
		{
			if (BeginPanel("Details", "Details", &ProfileSettings->bDetailsVisible))
			{
				Document.Canvas->DrawSelectionDetails(*Documents.GetEditor()->GetTransactor());
				if (IsCurrent()) DrawInterface(Document);
			}
			ImGui::End();
			if (!IsCurrent()) return;
		}
		if (ProfileSettings->bParametersVisible)
		{
			if (BeginPanel("Inputs", "Inputs", &ProfileSettings->bParametersVisible)) DrawInputs(Document);
			ImGui::End();
			if (!IsCurrent()) return;
		}
		const bool bPreviewPanel = ProfileSettings->bPreviewVisible;
		const bool PreviewVisible = bPreviewPanel
			&& BeginPanel("Preview", "Preview", &ProfileSettings->bPreviewVisible);
		if (PreviewLease) PreviewLease->SetVisible(PreviewVisible);
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
			if (Document.bPreviewValid) PreviewLease->Draw(Document.Material());
			else ImGui::TextDisabled("The selected output is unavailable.");
		}
		if (bPreviewPanel) ImGui::End();
		SaveDockLayout("Document", DockType, Workspace::FunctionLayoutVersion, PanelIds);
	}
	auto FMaterialEditor::RemapFunctionDocuments(std::string_view Source, std::string_view Destination) -> void
	{
		if (auto Node = FunctionDocuments.extract(std::string(Source)); !Node.empty())
		{ Node.key() = Destination; Node.mapped()->RefreshSourceRevision(); FunctionDocuments.insert(std::move(Node)); }
	}

}
