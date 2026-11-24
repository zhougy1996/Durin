#include "FMaterialEditor.h"
#include "MaterialEditorUI.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "MaterialFunctionEditingSession.h"
#include "Widgets/MaterialEditingSession.h"
#include "MaterialGraph.h"
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
#include "Misc/MountPaths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"

namespace Durin::MaterialEditor
{
	FMaterialFunctionEditingSession::FMaterialFunctionEditingSession() = default;

	auto FMaterialFunctionEditingSession::Material() const -> DMaterial*
	{ return Cast<DMaterial>(PreviewMaterial.Get()); }

	auto FMaterialFunctionEditingSession::RetirePreview() -> void
	{
		if (Preview) Preview->SetVisible(false);
		Preview.reset();
		if (auto* Value = Material()) FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Value);
	}

	FMaterialFunctionEditingSession::~FMaterialFunctionEditingSession()
	{
		RetirePreview();
		if (PreviewPackage.Get()) MarkObjectHierarchyAsGarbage(PreviewPackage.Get());
		if (WorkingPackage && Transactor.IsValid()) Transactor.Get()->ForgetPackage(*WorkingPackage);
		if (WorkingPackage) MarkObjectHierarchyAsGarbage(WorkingPackage.Get());
	}


	auto FMaterialEditor::SaveFunctionViewport() -> void
	{
		if (const auto* Source = FunctionSession ? FunctionSession->GetSource() : nullptr)
		{
			const auto [Zoom, Pan] = FunctionSession->Canvas->GetViewport();
			ProfileSettings->SetViewport(Source->GetObjectPath(), {.Zoom = Zoom, .Pan = Pan});
		}
		ProfileSettings->Save();
	}

	auto FMaterialEditor::ApplyFunction(bool bForce) -> bool
	{
		const auto Session = FunctionSession;
		if (!IsEditorOpen() || !Session) return false;
		std::string Error;
		if (!Session->Publish(Error, bForce)) { SetError(std::move(Error)); return false; }
		return IsEditorOpen() && FunctionSession == Session;
	}

	auto FMaterialEditor::SaveFunction() -> bool
	{
		const auto Session = FunctionSession;
		if (!Session || !Session->Canvas->FinishDetailsEdit(false) || !RequestDeactivate()
			|| !ApplyFunction() || FunctionSession != Session) return false;
		return EditorSession.Save(Session->GetSource(), {}, [this](std::string Message) { SetError(std::move(Message)); });
	}

	auto FMaterialEditor::ReleaseFunctionSession() -> void
	{
		if (auto Session = std::exchange(FunctionSession, {})) Session->RetirePreview();
	}

	auto FMaterialEditor::RegisterUndo() -> void
	{
		if (UndoHandle.IsValid() || !EditorSession.GetEditor()) return;
		auto* Transactions = EditorSession.GetEditor()->GetTransactor();
		auto Listener = [Weak = weak_from_this()](bool bUndo) {
			return [Weak, bUndo](const FTransactionContext& Context, bool bSucceeded) {
				if (auto Owner = std::dynamic_pointer_cast<FMaterialEditor>(Weak.lock()); bSucceeded && Owner && Owner->IsEditorOpen())
					Owner->HandleTransaction(Context, bUndo);
			};
		};
		UndoHandle = Transactions->OnUndo().AddLambda(Listener(true));
		RedoHandle = Transactions->OnRedo().AddLambda(Listener(false));
	}

	auto FMaterialEditor::UnregisterUndo() -> void
	{
		if (UndoHandle.IsValid() && EditorSession.GetEditor())
		{
			auto* Transactions = EditorSession.GetEditor()->GetTransactor();
			Transactions->OnUndo().Remove(UndoHandle);
			Transactions->OnRedo().Remove(RedoHandle);
		}
		UndoHandle.Reset();
		RedoHandle.Reset();
	}

	auto FMaterialEditor::HandleTransaction(const FTransactionContext& Context, bool bUndo) -> void
	{
		auto* Transactions = EditorSession.GetEditor()->GetTransactor();
		std::vector<DPackage*> Packages;
		for (size_t Index = 0; Index < Transactions->GetQueueLength(); ++Index)
			if (const auto* Entry = Transactions->GetTransaction(Index); Entry && Entry->GetId() == Context.TransactionId)
			{ Packages = Entry->GetAffectedPackages(); break; }
		auto* Working = FunctionSession ? static_cast<DObject*>(FunctionSession->Function())
			: EditingSession ? EditingSession->GetWorkingMaterial() : nullptr;
		if (!Working || !std::ranges::contains(Packages, Working->GetPackage())) return;
		if (bUndo) PostUndo(true);
		else PostRedo(true);
	}

	auto FMaterialEditor::PostUndo(bool bSuccess) -> void
	{
		if (!bSuccess || !IsEditorOpen()) return;
		if (const auto Session = FunctionSession)
		{
			if (Session->Graph) Session->Graph->LinkGraphNodesFromMaterial();
			ApplyFunction(true);
		}
		else if (EditingSession && EditingSession->Graph)
			EditingSession->Graph->LinkGraphNodesFromMaterial();
	}

	auto FMaterialEditor::ResetFunctionSession(DMaterialFunction& Source) -> bool
	{
		const auto Previous = FunctionSession;
		if (Previous && Previous->bPublishing) { SetError("Function publication is in progress."); return false; }
		if (!RequestDeactivate() || FunctionSession != Previous) return false;
		if (Previous)
		{
			const auto [Zoom, Pan] = Previous->Canvas->GetViewport();
			ProfileSettings->SetViewport(Source.GetObjectPath(), {.Zoom = Zoom, .Pan = Pan});
		}
		auto Document = std::make_shared<FMaterialFunctionEditingSession>();
		std::string Error;
		if (!Document->Initialize(Source, EditorSession.GetEditor() ? EditorSession.GetEditor()->GetTransactor() : nullptr, Error))
		{ SetError(std::move(Error)); return false; }
		auto* Function = Document->Function();
		Document->Canvas = std::make_unique<FMaterialGraphCanvas>(*Document->Graph,
			FMaterialGraphEditorServices{
				.ReportError = [this](std::string Message) { ErrorMessage = std::move(Message); },
				.OpenFunction = [this](std::string_view Path) { OpenReferencedMaterialAsset(*this, Path, DMaterialFunction::StaticClass()->GetQualifiedName().ToString()); }});
		if (const auto* State = ProfileSettings->FindViewport(Source.GetObjectPath()))
			Document->Canvas->SetViewport(State->Zoom, State->Pan);
		Document->PreviewInvalidation.SetFunction(Function);
		const auto Mount = FMountPaths::FindMountForVirtualPath(Function->GetPackage()->GetPackagePath());
		if (!Mount) { ErrorMessage = (Mount ? std::string{} : ToString(Mount.error())); return false; }
		FPackagePath PreviewPath;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(std::format("{}__FunctionPreview/{}", Mount->Mount->VirtualRoot, FGuid::NewGuid().ToString()), PreviewPath); !PathValidation)
		{
			ErrorMessage = ToString(PathValidation.error());
			return false;
		}
		auto* Package = NewObject<DPackage>(DPackage::StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		Package->InitializeAssetPackage(PreviewPath);
		Document->PreviewPackage = FStrongObjectPtr(Package);
		auto* Material = NewObject<DMaterial>(DMaterial::StaticClass(), Package, "Preview", EObjectFlags::Transient);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		Document->PreviewMaterial = FStrongObjectPtr(Material);
		Document->Preview = std::make_shared<FMaterialPreview>(GetWindowId());
		if (!Function->GetFunctionSignature().Outputs.empty()) Document->Output = Function->GetFunctionSignature().Outputs.front().Id;
		if (FunctionSession != Previous || IsClosed()) return false;
		ReleaseFunctionSession();
		FunctionSession = std::move(Document);
		return true;
	}

	auto FMaterialEditor::CancelFunctionInteractions() -> bool
	{
		const auto Session = FunctionSession;
		return !Session || (!Session->bPublishing && Session->Canvas->CancelInteraction());
	}

	auto FMaterialEditor::ResetFunctionLayout() -> void
	{
		ProfileSettings->bPreviewVisible = true;
		ProfileSettings->bDetailsVisible = true;
		ProfileSettings->bParametersVisible = true;
		bPendingLayoutReset = true;
	}

	auto FMaterialEditor::NavigateToNode(const FGuid& NodeId) -> bool
	{
		const auto Session = FunctionSession;
		if (!IsEditorOpen() || !Session || !Session->Function()) return false;
		const auto& Nodes = Session->Function()->GetExpressionCollection().Expressions;
		return std::ranges::any_of(Nodes, [&](const auto& Node) { return Node->Id == NodeId; })
			&& Session->Canvas->SelectAndFrame(NodeId);
	}

	auto FMaterialEditor::DrawFunction(bool bActive) -> bool
	{
		if (!bActive) RequestDeactivate();
		const auto Session = FunctionSession;
		if (!Session) return false;
		if (!IsValid(Session->GetSource()))
		{
			CloseWindow(EAssetEditorCloseReason::AssetUnloadingOrInvalid);
			return false;
		}
		return DrawAssetTabs("MaterialFunctionEditor",
			[&](DObject& Source) {
				if (FunctionSession == Session && Session->Function()) DrawDocument(Source.GetObjectPath(), *Session);
			},
			[&](DObject&, bool Visible) {
				if (Session->Preview) Session->Preview->SetVisible(Visible);
				if (!Visible)
				{
					const auto DockType = Workspace::MakeDockType(GetPresentationRootKey("Document"));
					if (ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::FunctionLayoutVersion)))
						TabLayout::SubmitDockSpace(DockType, Workspace::FunctionLayoutVersion,
							{0.0f, 0.0f}, ImGuiDockNodeFlags_KeepAliveOnly);
				}
			});
	}

	auto FMaterialEditor::DrawInputs(FMaterialFunctionEditingSession& Document) -> void
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
			const auto Result = Document.Graph->AddPort(false, Port, {}, 0, static_cast<int32>(Signature.Inputs.size()) * 120);
			if (!Result) ErrorMessage = FormatMaterialGraphCommandResult(Result);
			else Document.Canvas->SelectAndFrame((*Result)->GetView().Node.Id);
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

	auto FMaterialEditor::DrawInterface(FMaterialFunctionEditingSession& Document) -> void
	{
		auto& Function = *Document.Function();
		const auto Apply = [&](FMaterialGraphCommandResult Result) { if (!Result) ErrorMessage = FormatMaterialGraphCommandResult(Result); };
		if (Document.Canvas->GetSelection().size() != 1)
		{
			Document.SelectedPortNode = {};
			return;
		}
		const auto* Id = std::get_if<FGuid>(&*Document.Canvas->GetSelection().begin());
		if (!Id) return;
		auto* Node = Document.Graph->FindNode(*Id);
		if (!Node) return;
		auto* Selected = Node->GetMaterialExpression();
		if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Selected); Call && Call->Function.IsValid())
		{
			if (ImGui::Button("Open Function")) OpenReferencedMaterialAsset(*this, Call->Function->GetObjectPath(),
				Call->Function->GetClass()->GetQualifiedName().ToString());
			DrawMaterialFunctionCallInputs(*Document.Graph, *Id, ErrorMessage);
		}
		const auto* Input = Cast<DMaterialExpressionFunctionInput>(Selected);
		const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Selected);
		const auto* Port = Input ? &Input->Port : Output ? &Output->Port : nullptr;
		if (!Port) { Document.SelectedPortNode = {}; return; }
		if (Document.SelectedPortNode != *Id || Document.PortBaseline != *Port)
		{
			Document.SelectedPortNode = *Id;
			Document.EditingPort = Port->Id;
			Document.bOutputPort = Output != nullptr;
			Document.PortBaseline = Document.PortDraft = *Port;
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
			const auto Result = Node->SetFunctionPort(Document.PortDraft);
			if (Result) Document.PortBaseline = Document.PortDraft;
			else ErrorMessage = FormatMaterialGraphCommandResult(Result);
		}
		if (Document.EditingPort.IsValid())
		{
			ImGui::SameLine();
			if (ImGui::Button("Remove Port")) Apply(Document.Graph->RemovePort(Document.bOutputPort, Document.EditingPort));
		}
	}

	auto FMaterialEditor::DrawDocument(std::string_view Resource, FMaterialFunctionEditingSession& Document) -> void
	{
		auto& Function = *Document.Function();
		const auto PreviewLease = Document.Preview;
		const auto IsCurrent = [&] { return IsEditorOpen() && FunctionSession.get() == &Document; };
		if (!EditorSession.GetEditor()) { ImGui::TextDisabled("Editor transactions are unavailable."); return; }
		if (ImGui::Button("Apply"))
		{
			if (Document.Canvas->FinishDetailsEdit(false) && RequestDeactivate()) ApplyFunction();
		}
		ImGui::SameLine();
		if (ImGui::Button("Save")) SaveActiveDocument();
		if (!IsCurrent()) return;
		ImGui::SameLine();
		if (ImGui::Button("Compile Preview") && Document.Canvas->FinishDetailsEdit(false))
			Document.PreviewInvalidation.RequestRefresh();
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
		const auto DockType = Workspace::MakeDockType(GetPresentationRootKey("Document"));
		static constexpr std::string_view PanelIds[]{"Graph", "Preview", "Details", "Inputs"};
		RestoreDockLayout("Document", DockType, Workspace::FunctionLayoutVersion, PanelIds);
		const auto Size = ImGui::GetContentRegionAvail();
		if (Size.x <= 0.0f || Size.y <= 0.0f) return;
		if (bPendingLayoutReset || !ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Workspace::FunctionLayoutVersion)))
		{
			Workspace::BuildDefaultLayout(DockType, Size, true);
			bPendingLayoutReset = false;
		}
		TabLayout::SubmitDockSpace(DockType, Workspace::FunctionLayoutVersion, Size);
		const auto BeginPanel = [&](const char* Label, const char* Key, bool* bOpen = nullptr) {
			return TabLayout::BeginDockablePanel(DockType, Label, Key, bOpen,
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
		};
		if (BeginPanel("Material Graph", "Graph"))
			Document.Canvas->Draw(*EditorSession.GetEditor()->GetTransactor(), 0);
		ImGui::End();
		if (!IsCurrent()) return;
		if (ProfileSettings->bDetailsVisible)
		{
			if (BeginPanel("Details", "Details", &ProfileSettings->bDetailsVisible))
			{
				Document.Canvas->DrawSelectionDetails();
				if (IsCurrent()) DrawInterface(Document);
			}
			ImGui::End();
			if (!IsCurrent()) return;
		}
		Document.Canvas->EndNodeLiteralFrame();
		if (!IsCurrent()) return;
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
					else NavigateToFunctionNode(*this, Diagnostic.FunctionAssetPath, Diagnostic.NodeId);
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
}
