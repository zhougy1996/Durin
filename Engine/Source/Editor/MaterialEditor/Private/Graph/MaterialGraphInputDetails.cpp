#include "MaterialGraphCanvas.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScene.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/Material.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "Graph/MaterialEditingOperations.h"
#include "MaterialExpressionParameters.h"
#include "Materials/MaterialExpressionInputs.h"
#include "MaterialGraphValueTypes.h"
#include "Asset/AssetPicker.h"
#include "Math/Color.h"
#include "Texture/Texture2D.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "Transactions/Transactor.h"

namespace Durin::MaterialEditor
{
	auto FMaterialGraphCanvas::DrawParameterValue(
		const FMaterialParameterDefinition& Parameter, DTransactor& Transactions) -> void
	{
		if (!Cast<DMaterial>(EditorGraph->GetMaterialOrFunction())) { CancelInteraction(); return; }
		if (Parameter.Type == EMaterialParameterType::Texture) return;
		ImGui::PushID(Parameter.Id.ToString().c_str());
		MonaImGui::PropertyEdit::BeginRow("Default value");
		const ImGuiID Widget = ImGui::GetID("##Value");
		using namespace MonaImGui::PropertyEdit;
		FPropertyInteractionState Interaction;
		auto Value = Parameter.Value;
		bool Edited = false;
		const float Minimum = Parameter.bHasRange ? Parameter.MinimumValue : 0.0f;
		const float Maximum = Parameter.bHasRange ? Parameter.MaximumValue : 0.0f;
		const auto Flags = Parameter.bHasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
		if (Parameter.Type == EMaterialParameterType::Scalar)
		{
			Edited = ImGui::DragFloat("##Value", &Value.GetScalar(),
				Parameter.Presentation == EMaterialParameterPresentation::Integer ? 1.0f : 0.01f,
				Minimum, Maximum, Parameter.Presentation == EMaterialParameterPresentation::Integer ? "%.0f" : "%.4g", Flags);
			if (Parameter.Presentation == EMaterialParameterPresentation::Integer)
			{
				Value.GetScalar() = std::round(Value.GetScalar());
				if (Parameter.bHasRange) Value.GetScalar() = std::clamp(Value.GetScalar(), Minimum, Maximum);
			}
		}
		else if (Parameter.Type == EMaterialParameterType::Vector4)
		{
			const auto V = Value.GetVector4();
			float Components[]{static_cast<float>(V.x), static_cast<float>(V.y), static_cast<float>(V.z), static_cast<float>(V.w)};
			if (Parameter.Presentation == EMaterialParameterPresentation::Color)
			{
				FLinearColor Color(Components[0], Components[1], Components[2], Components[3]);
				Edited = DrawColorPicker(Color, EColorPickerMode::LinearHDR, true, false, &Interaction);
				if (Interaction.End != EPropertyInteractionEnd::Cancel)
					Interaction = FPropertyInteractionState::FromPopup(Interaction.Id,
						ImGui::IsPopupOpen(Interaction.Id, ImGuiPopupFlags_AnyPopupLevel));
				if (Edited) Value.GetVector4() = FVector4(Color.R, Color.G, Color.B, Color.A);
			}
			else
			{
				Edited = ImGui::DragFloat4("##Value", Components, 0.01f, Minimum, Maximum, "%.4g", Flags);
				Value.GetVector4() = FVector4(Components[0], Components[1], Components[2], Components[3]);
			}
		}
		const bool Active = ImGui::IsItemActive();
		const bool Deactivated = ImGui::IsItemDeactivatedAfterEdit();
		if (Interaction.Id == 0) Interaction = FPropertyInteractionState::FromWidget(Widget, {Active, Deactivated});
		ObserveParameterValue(Parameter.Id, Transactions, Interaction, Edited ? std::optional(std::move(Value)) : std::nullopt);
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
	}

	auto FMaterialGraphCanvas::ObserveParameterValue(const FGuid& Id, DTransactor& Transactions,
		const MonaImGui::PropertyEdit::FPropertyInteractionState& State, std::optional<FMaterialParameterValue> Value) -> void
	{
		ParameterFrame.Observe({Id, State.Id}, TWeakObjectPtr<DTransactor>(&Transactions), State.End, std::move(Value));
	}

	auto FMaterialGraphCanvas::EndParameterFrame() -> void
	{
		ParameterFrame.EndFrame(ImGui::IsKeyPressed(ImGuiKey_Escape, false),
			[this](const FParameterControlId& Id, const TWeakObjectPtr<DTransactor>& Transactions) {
				auto* Material = Cast<DMaterial>(EditorGraph->GetMaterialOrFunction());
				auto* Transactor = Transactions.Get();
				if (!Material || !Transactor)
				{
					ReportError(!Material ? "The material graph owner is no longer available."
						: "The editor transactor is no longer available.");
					return false;
				}
				const auto Result = ParameterSession.Begin(*Material, Id.first, Transactor);
				if (!Result) ReportError(Result.error());
				return static_cast<bool>(Result);
			},
			[this](const FMaterialParameterValue& Value) {
				const auto Result = ParameterSession.Apply(Value);
				if (!ParameterSession.IsActive()) FinishParameterEdit(false);
				if (!Result) ReportError(Result.error());
				return static_cast<bool>(Result);
			},
			[this](bool bCancel) { return FinishParameterSession(bCancel); });
	}

	auto FMaterialGraphCanvas::FinishParameterEdit(bool bCancel) -> bool
	{
		return ParameterFrame.Finish(bCancel, [this](bool bRestore) { return FinishParameterSession(bRestore); });
	}

	auto FMaterialGraphCanvas::FinishParameterSession(bool bCancel) -> bool
	{
		if (!ParameterSession.IsActive()) return true;
		const int Frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
		if (Frame >= 0 && FailedParameterFinishFrame == Frame) return false;
		const auto Result = bCancel ? ParameterSession.Cancel() : ParameterSession.Commit();
		if (!Result)
		{
			FailedParameterFinishFrame = Frame;
			auto Error = Result.error();
			if (LastParameterFinishError != Error)
			{
				LastParameterFinishError = Error;
				ReportError(std::move(Error));
			}
			return false;
		}
		FailedParameterFinishFrame = -1;
		LastParameterFinishError.clear();
		return true;
	}

	auto FMaterialGraphCanvas::PrepareDetailsView(DObject& Owner) -> const FMaterialGraphView&
	{
		// Refresh here too: Details can be drawn before the canvas or while it is hidden.
		if (auto* Material = Cast<DMaterial>(&Owner)) PrepareView(*Material);
		else if (auto* Function = Cast<DMaterialFunction>(&Owner)) PrepareFunctionView(*Function);
		return ReadModel.GetView();
	}

	auto FMaterialGraphCanvas::DrawSelectionDetails(DTransactor& Transactions) -> void
	{
		if (!EditorGraph->GetMaterialOrFunction()) { CancelInteraction(); return; }
		auto& Owner = *EditorGraph->GetMaterialOrFunction();
		if (SelectedNodes.empty()) return;
		const auto& View = PrepareDetailsView(Owner);
		const auto Selection = GetSelectedProgramNodes();
		if (Selection.size() != 1) return;
		auto* SelectedNode = EditorGraph->FindNode(Selection.front());
		if (!SelectedNode) return;
		const TStrongObjectPtr<DMaterialExpression> Expression(SelectedNode->GetMaterialExpression());
		if (!Expression && !SelectedNode->GetView().Node.bMaterialOutput) return;
		const auto& Selected = SelectedNode->GetView();
		ImGui::PushID(Selection.front().ToString().c_str());
		ImGui::TextWrapped("%s", Selected.PrimaryLabel.c_str());
		if (SelectedDiagnostic && SelectedDiagnostic->NodeId == Selected.Node.Id)
		{
			constexpr std::array UVNames{"Channel", "Scale", "Offset", "Rotation"};
			if (SelectedDiagnostic->UVFieldIndex && *SelectedDiagnostic->UVFieldIndex < UVNames.size())
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic UV field: %s", UVNames[*SelectedDiagnostic->UVFieldIndex]);
			else if (SelectedDiagnostic->InputIndex)
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic input: %u", *SelectedDiagnostic->InputIndex);
		}
		if (!MonaImGui::PropertyEdit::BeginTable("NodeProperties", DetailsStyle::MakeTableConfig()))
		{
			ImGui::PopID();
			return;
		}
		bool Changed = false;
		const auto Submit = [&](FMaterialGraphCommandResult Result) {
			CheckCommand(Result);
			Changed = Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded;
		};
		const auto EditText = [](const char* Label, std::string& Value) {
			std::array<char, MaterialProgramMaxDisplayNameBytes + 1> Buffer{};
			std::copy_n(Value.data(), std::min(Value.size(), Buffer.size() - 1), Buffer.data());
			if (!DetailsStyle::EditRow(Label, [&] { return ImGui::InputText("##Value", Buffer.data(), Buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue); })) return false;
			Value = Buffer.data();
			return true;
		};
		const auto EditLiteralValue = [](EMaterialProgramValueType Type, FMaterialProgramLiteral& Value) {
			std::array<float, 4> Components{Value.X, Value.Y, Value.Z, Value.W};
			const int Count = static_cast<int>(Type) + 1;
			if (Count < 1 || Count > 4 || !ImGui::InputScalarN("##Value", ImGuiDataType_Float, Components.data(), Count,
				nullptr, nullptr, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) return false;
			Value = {Components[0], Components[1], Components[2], Components[3]};
			return true;
		};
		const auto EditLiteral = [&](const char* Label, EMaterialProgramValueType Type, FMaterialProgramLiteral& Value) {
			return DetailsStyle::EditRow(Label, [&] { return EditLiteralValue(Type, Value); });
		};
		auto* Material = Cast<DMaterial>(&Owner);
		if (const auto* ParameterExpression = Cast<DMaterialExpressionParameter>(Expression.Get()); Material && ParameterExpression)
		{
			auto Parameter = ParameterExpression->GetParameterDefinition();
			const auto CommitParameter = [&]() {
				auto Created = GraphEditInternals::MakeParameterExpression(Parameter);
				if (!Created) { ReportError(Created.Message); return; }
				auto Replacement = std::move(Created.Expression);
				Replacement->Id = Expression->Id;
				if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()); Sample && Parameter.Type == EMaterialParameterType::Texture)
				{
					TStrongObjectPtr<DMaterialExpressionTextureSampleParameter2D> Copy(DuplicateObject(Sample, nullptr, NAME_None).value());
					if (!Copy) { ReportError("Unable to copy the parameter expression."); return; }
					if (const auto Applied = Copy->SetParameterDefinition(Parameter); !Applied) { ReportError(Applied.error()); return; }
					Submit(EditorGraph->ReplaceExpression(*Copy.Get()));
				}
				else Submit(EditorGraph->ReplaceExpression(*Replacement.Get()));
			};
			std::string Name = Parameter.Name.ToString();
			if (!Changed && EditText("Parameter name", Name)) { Parameter.Name = FName(Name); CommitParameter(); }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Change this node's parameter binding. Existing names use the existing parameter's value.");
			if (!Changed && Parameter.Type == EMaterialParameterType::Texture)
			{
				const bool Cube = Parameter.Value.GetTexture().Dimension == EMaterialTextureDimension::TextureCube;
				int Usage = Cube ? (Parameter.TextureUsage == ETextureUsage::DataMask ? 1 : 0) : static_cast<int>(Parameter.TextureUsage);
				if (DetailsStyle::EditRow("Texture usage", [&] { return ImGui::Combo("##Value", &Usage, Cube ? "Color\0Data / Mask\0" : "Color\0Normal\0Data / Mask\0"); }))
				{
					Parameter.TextureUsage = Cube ? (Usage == 1 ? ETextureUsage::DataMask : ETextureUsage::Color) : static_cast<ETextureUsage>(Usage);
					CommitParameter();
				}
				MonaImGui::PropertyEdit::BeginRow("Texture");
				const auto Interaction = AssetPicker::Draw({.RequiredClass = Parameter.Value.GetTexture().Dimension == EMaterialTextureDimension::TextureCube ? DTextureCube::StaticClass() : DTexture2D::StaticClass(),
					.CurrentSelection = Parameter.Value.GetTexture().Texture.Get(), .SearchText = NodeTextureSearch,
					.AssignSelection = [&](DObject* Object, std::string& Error) {
						auto Value = Parameter.Value;
						Value.GetTexture().Texture = Cast<DTexture>(Object);
						const auto Result = MaterialEditing::SetParameterValue(*Material, Parameter.Id, Value);
						Error = FormatMaterialGraphCommandResult(Result); Changed = Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded;
						return static_cast<bool>(Result);
					}});
				MonaImGui::PropertyEdit::EndRow();
				if (!Interaction.Error.empty()) ReportError(Interaction.Error);
				if (MonaImGui::PropertyEdit::BeginGroup("Sampler", "Sampler settings", ImGuiTreeNodeFlags_None))
				{
					const auto Combo = [&](const char* Label, auto& Value, const char* Names) {
						int Index = static_cast<int>(Value);
						if (Changed || !DetailsStyle::EditRow(Label, [&] { return ImGui::Combo("##Value", &Index, Names); })) return;
						Value = static_cast<std::remove_reference_t<decltype(Value)>>(Index);
						Submit(MaterialEditing::SetParameterValue(*Material, Parameter.Id, Parameter.Value));
					};
					Combo("Missing texture", Parameter.Value.GetTexture().TextureFallback, Parameter.Value.GetTexture().Dimension == EMaterialTextureDimension::TextureCube ? "White\0Black\0" : "White\0Black\0Flat normal\0");
					Combo("Min filter", Parameter.Value.GetTexture().SamplerState.MinFilter, "Nearest\0Linear\0Nearest mip nearest\0Linear mip nearest\0Nearest mip linear\0Linear mip linear\0");
					Combo("Mag filter", Parameter.Value.GetTexture().SamplerState.MagFilter, "Nearest\0Linear\0");
					Combo("Address U", Parameter.Value.GetTexture().SamplerState.AddressU, "Repeat\0Mirrored repeat\0Clamp\0");
					Combo("Address V", Parameter.Value.GetTexture().SamplerState.AddressV, "Repeat\0Mirrored repeat\0Clamp\0");
					Combo("Address W", Parameter.Value.GetTexture().SamplerState.AddressW, "Repeat\0Mirrored repeat\0Clamp\0");
					MonaImGui::PropertyEdit::EndGroup();
				}
			}
			else if (!Changed) DrawParameterValue(Parameter, Transactions);
		}
		else
		{
			FMaterialProgramLiteral Value;
			EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
			bool bConstant = true;
			if (const auto* E = Cast<DMaterialExpressionScalarConstant>(Expression.Get())) Value = {E->Value};
			else if (const auto* E = Cast<DMaterialExpressionVector2Constant>(Expression.Get())) { Type = EMaterialProgramValueType::Float2; Value = {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y)}; }
			else if (const auto* E = Cast<DMaterialExpressionVector3Constant>(Expression.Get())) { Type = EMaterialProgramValueType::Float3; Value = {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z)}; }
			else if (const auto* E = Cast<DMaterialExpressionVector4Constant>(Expression.Get())) { Type = EMaterialProgramValueType::Float4; Value = {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z), static_cast<float>(E->Value.w)}; }
			else bConstant = false;
			if (bConstant && EditLiteral("Value", Type, Value))
				Submit(SelectedNode->SetConstantValue(MakeParameterValue(Type, Value)));
		}
		std::optional<EMaterialTextureReadMode> SamplingMode;
		const auto EditSamplingMode = [&](const auto* Sample) {
			if (!Sample || Changed) return;
			SamplingMode = Sample->ReadMode;
			int Mode = static_cast<int>(*SamplingMode);
			if (!DetailsStyle::EditRow("Sampling", [&] { return ImGui::Combo("##Value", &Mode, "Implicit\0Level\0Bias\0Grad\0"); })) return;
			using ExpressionType = std::remove_cv_t<std::remove_pointer_t<decltype(Sample)>>;
			TStrongObjectPtr<ExpressionType> Copy(DuplicateObject(Sample, nullptr, NAME_None).value());
			Copy->ReadMode = static_cast<EMaterialTextureReadMode>(Mode);
			Submit(EditorGraph->ReplaceExpression(*Copy));
		};
		EditSamplingMode(Cast<DMaterialExpressionTextureSampleCube>(Expression.Get()));
		EditSamplingMode(Cast<DMaterialExpressionTextureSampleParameterCube>(Expression.Get()));
		EditSamplingMode(Cast<DMaterialExpressionTextureSample2D>(Expression.Get()));
		EditSamplingMode(Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()));
		if (const auto* Scene = Cast<DMaterialExpressionSceneTexture>(Expression.Get()); Scene && !Changed)
		{
			int Id = static_cast<int>(Scene->SceneTextureId);
			bool Filtered = Scene->bFiltered;
			const bool IdChanged = DetailsStyle::EditRow("Scene input", [&] { return ImGui::Combo("##Value", &Id, "PostProcessInput0\0Scene Depth\0"); });
			if (Id == static_cast<int>(EMaterialSceneTextureId::SceneDepth)) Filtered = false;
			const bool FilterChanged = Id == static_cast<int>(EMaterialSceneTextureId::PostProcessInput0)
				&& DetailsStyle::EditRow("Linear filtering", [&] { return ImGui::Checkbox("##Value", &Filtered); });
			if (IdChanged || FilterChanged)
			{
				TStrongObjectPtr<DMaterialExpressionSceneTexture> Copy(DuplicateObject(Scene, nullptr, NAME_None).value());
				Copy->SceneTextureId = static_cast<EMaterialSceneTextureId>(Id); Copy->bFiltered = Filtered;
				Submit(EditorGraph->ReplaceExpression(*Copy));
			}
		}
		if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()); Swizzle && !Changed)
		{
			DetailsStyle::EditRow("Channels", [&] {
				constexpr std::array Labels{"R", "G", "B", "A"};
				for (uint8 Channel = 0; Channel < 4; ++Channel)
				{
					if (Channel) ImGui::SameLine();
					bool Enabled = std::ranges::contains(Swizzle->Components, Channel);
					ImGui::BeginDisabled(Enabled && Swizzle->Components.size() == 1);
					const bool Edited = ImGui::Checkbox(Labels[Channel], &Enabled);
					ImGui::EndDisabled();
					if (!Edited) continue;
					std::vector<uint8> Mask;
					for (uint8 Candidate = 0; Candidate < 4; ++Candidate)
						if (Candidate == Channel ? Enabled : std::ranges::contains(Swizzle->Components, Candidate))
							Mask.push_back(Candidate);
					Submit(SelectedNode->SetSwizzleComponents(Mask));
					return true;
				}
				return false;
			});
		}
		if (const auto* Surface = Cast<DMaterialExpressionGetMaterialAttributes>(Expression.Get()); Surface && !Changed)
		{
			for (uint32 Index = 0; Index < FMaterialAttributeDefinitionMap::GetDefinitions().size() && !Changed; ++Index)
			{
				const auto ID = FMaterialAttributeDefinitionMap::GetDefinitions()[Index].AttributeID;
				bool Enabled = std::ranges::contains(Surface->AttributeIDs, ID);
				if (!DetailsStyle::EditRow(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Name.data(), [&] { return ImGui::Checkbox("##Value", &Enabled); })) continue;
				Submit(SelectedNode->SetAttributeOutputEnabled(ID, Enabled));
			}
		}
		if (const auto* Surface = Cast<DMaterialExpressionSetMaterialAttributes>(Expression.Get()); Surface && !Changed)
		{
			for (uint32 Index = 0; Index < FMaterialAttributeDefinitionMap::GetDefinitions().size() && !Changed; ++Index)
			{
				const auto Attribute = FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Property;
				const auto Binding = std::ranges::find(Surface->Attributes, Attribute, &FMaterialExpressionAttributeBinding::GetProperty);
				const bool bKeepBase = Binding == Surface->Attributes.end()
					|| !Binding->Input.IsConnected();
				DetailsStyle::EditRow(FMaterialAttributeDefinitionMap::GetDefinitions()[Index].Name.data(), [&] {
					if (!ImGui::BeginCombo("##Value", bKeepBase ? "Keep base value" : "Connected")) return false;
					if (ImGui::Selectable("Keep base value", bKeepBase)) Submit(SelectedNode->ClearAttributeInput(Attribute));
					for (const auto& Source : View.Nodes)
					{
						if (Changed) break;
						for (const auto& Pin : Source.Outputs)
						{
							const auto* Output = EditorGraph->FindPin(FMaterialGraphPinAddress::Output({Source.Node.Id, Pin.OutputIndex, Pin.PortId}));
							if (!SelectedNode->CanSetAttributeInput(Attribute, Output).Allows(true)) continue;
							if (ImGui::Selectable(std::format("{}: {}##{}:{}:{}", Source.PrimaryLabel, Pin.Name,
								Source.Node.Id.ToString(), Pin.OutputIndex, Pin.PortId.ToString()).c_str()))
							{
								std::string Error;
								Changed = SelectedNode->SetAttributeInput(Attribute, *Output, ETransactionMode::Record, &Error);
								if (!Error.empty()) ReportError(std::move(Error));
								break;
							}
						}
					}
					ImGui::EndCombo();
					return Changed;
				});
			}
		}
		const auto DrawInput = [&](size_t Index) {
			// A property edit can synchronously reconstruct the node during this row.
			const auto Pin = Selected.Inputs[Index];
			if (Pin.SourceType > EMaterialProgramValueType::Float4) return;
			ImGui::PushID(static_cast<int>(Pin.InputIndex));
			auto Value = Pin.InlineDefault;
			if (Value.Kind == EMaterialInputDefaultKind::None)
			{
				Value.Type = Pin.SourceType;
				if (Pin.PortId.IsValid() && !Pin.bRequired && Pin.Default.Kind == EMaterialFunctionDefaultKind::Numeric)
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Value.Literal = Pin.Default.Numeric;
				}
			}
			const bool bInheritedDynamic = Pin.PortId.IsValid() && !Pin.bRequired
				&& Value.Kind == EMaterialInputDefaultKind::None && Pin.Default.Kind != EMaterialFunctionDefaultKind::None;
			const auto* InputPin = SelectedNode->GetInputPin(static_cast<int32>(Index));
			const auto* Upstream = InputPin->LinkedTo.empty() ? nullptr : InputPin->LinkedTo.front();
			const bool bConnected = Upstream != nullptr;
			const bool bInheritedContext = Pin.bSupportsConstant && !Pin.bUseConstant
				&& (Selected.Node.IsSampleUVInput(Pin.InputIndex)
					|| (Selected.Node.Opcode == EMaterialProgramOpcode::SetMaterialAttributes && Pin.InputIndex != 0));
			const bool bRootNormal = Selected.Node.bMaterialOutput && Pin.MaterialProperty == EMaterialProperty::MP_Normal;
			const auto EditValue = [&]() {
				if (bRootNormal)
				{
					ImGui::TextUnformatted("Default tangent-space normal (0, 0, 1)");
					return;
				}
				if (bInheritedContext)
				{
					ImGui::TextUnformatted(Selected.Node.IsSampleUVInput(Pin.InputIndex) ? "Mesh UV0" : "Base Surface value");
					return;
				}
				if (bInheritedDynamic)
				{
					ImGui::TextWrapped("%s", DescribeFunctionDefault(Pin.Default).c_str());
					return;
				}
				if (EditLiteralValue(Value.Type, Value.Literal))
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Submit(SelectedNode->PinDefaultValueChanged(Selected.InputAddress(Pin), Value));
				}
			};
			auto Label = Pin.Name;
			if (SamplingMode)
			{
				if (Pin.bActive && Pin.Name == "Mip Value")
					Label = *SamplingMode == EMaterialTextureReadMode::Level ? "Mip Level" : "Mip Bias";
			}
			else if (Pin.bSupportsConstant) Label += Pin.bUseConstant ? " (constant)" : " (inherited)";
			MonaImGui::PropertyEdit::BeginRow(Label.c_str());
			const float MenuWidth = ImGui::GetFrameHeight();
			const float ValueWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - MenuWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(ValueWidth);
			if (Upstream)
			{
				const auto& Source = Upstream->GetOwningNode()->GetView();
				const auto& Output = Source.Outputs[Upstream->DisplayIndex];
				const auto SourceLabel = std::format("{}.{}", Source.PrimaryLabel, Output.Name);
				if (ImGui::Button((SourceLabel + "###Source").c_str(), {ValueWidth, 0.0f}))
					SelectAndFrame(Source.Node.Id);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connected to %s. Click to locate.", SourceLabel.c_str());
			}
			else EditValue();
			ImGui::SameLine();
			if (ImGui::Button("...", {MenuWidth, 0.0f})) ImGui::OpenPopup("InputActions");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Input actions");
			MonaImGui::PropertyEdit::EndRow();
			if (ImGui::BeginPopup("InputActions"))
			{
				if (!Changed && bConnected)
				{
					if (ImGui::MenuItem("Inline constant", nullptr, false, !bRootNormal)) Submit(SelectedNode->InlineInputNode(Selected.InputAddress(Pin)));
					if (!Changed && ImGui::MenuItem("Disconnect")) Changed = BreakPinLinks(Selected.InputAddress(Pin));
				}
				else if (!Changed)
				{
					if (ImGui::MenuItem("Extract constant", nullptr, false, Pin.InlineDefault.Kind == EMaterialInputDefaultKind::Literal)) Changed = CheckCommand(SelectedNode->ExtractInputDefault(Selected.InputAddress(Pin)));
					if (!Changed && Material && ImGui::MenuItem("Promote to parameter", nullptr, false, !bInheritedDynamic && !bInheritedContext
						&& (!Selected.InputAddress(Pin).IsMaterialRoot() || MaterialEditing::CanPromoteSurfaceOutput(*Material, Selected.InputAddress(Pin).Property))))
						Changed = CheckCommand(EditorGraph->PromoteInputToParameter(Selected.InputAddress(Pin)));
				}
				if (!Changed && Pin.bSupportsConstant && ImGui::MenuItem(Pin.bUseConstant ? "Use definition default" : "Use retained constant"))
					Submit(SelectedNode->SetPinUseConstant(Selected.InputAddress(Pin), !Pin.bUseConstant));
				if (!Changed && bInheritedDynamic && ImGui::MenuItem("Override with constant"))
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Submit(SelectedNode->PinDefaultValueChanged(Selected.InputAddress(Pin), Value));
				}
				if (!Changed && Pin.PortId.IsValid() && !Pin.bRequired
					&& Pin.InlineDefault.Kind != EMaterialInputDefaultKind::None && ImGui::MenuItem("Use function default"))
					Submit(SelectedNode->PinDefaultValueChanged(Selected.InputAddress(Pin), {}));
				if (!Changed && bConnected && ImGui::BeginMenu("When disconnected"))
				{
					ImGui::TextUnformatted("Value used after disconnecting");
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
					EditValue();
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		};
		std::vector<size_t> UnusedConnectedInputs;
		for (size_t Index = 0; !Changed && Index < Selected.Inputs.size(); ++Index)
		{
			if (SamplingMode && !Selected.Inputs[Index].bActive)
			{
				if (!SelectedNode->GetInputPin(static_cast<int32>(Index))->LinkedTo.empty())
					UnusedConnectedInputs.push_back(Index);
				continue;
			}
			DrawInput(Index);
		}
		if (!Changed && !UnusedConnectedInputs.empty()
			&& MonaImGui::PropertyEdit::BeginGroup("UnusedSamplingInputs", "Unused inputs", ImGuiTreeNodeFlags_None))
		{
			for (const size_t Index : UnusedConnectedInputs)
			{
				if (Changed) break;
				DrawInput(Index);
			}
			MonaImGui::PropertyEdit::EndGroup();
		}
		MonaImGui::PropertyEdit::EndTable();
		ImGui::PopID();
	}
}
