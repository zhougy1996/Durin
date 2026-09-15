#include "MaterialGraphCanvas.h"
#include "Widgets/MaterialDetailsStyle.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphExpressionState.h"
#include "MaterialExpressionParameters.h"
#include "MaterialExpressionInputs.h"
#include "MaterialGraphValueTypes.h"
#include "Editor/AssetPicker.h"
#include "Texture/Texture2D.h"
#include <cmath>

namespace Durin::Editor::Material
{
	auto FMaterialGraphCanvas::DrawSelectionDetails(DObject& Owner, DTransactor& Transactions,
		const FReportError& ReportError) -> void
	{
		const auto Selection = GetSelectedProgramNodes();
		if (Selection.size() != 1) return;
		FMaterialGraphDocument Document(Owner);
		const FMaterialExpressionCollection* Collection = nullptr;
		if (const auto* Material = Cast<DMaterial>(&Owner)) Collection = &Material->GetExpressionCollection();
		if (const auto* Function = Cast<DMaterialFunction>(&Owner)) Collection = &Function->GetExpressionCollection();
		if (!Collection) return;
		const auto Existing = std::ranges::find(Collection->Expressions, Selection.front(), [](const auto& E) { return E->Id; });
		if (Existing == Collection->Expressions.end()) return;
		const TStrongObjectPtr<DMaterialExpression> Expression(Existing->Get());
		GraphEditInternals::FOwnedGraphSnapshot State;
		const auto CaptureSelected = [&]() {
			State = {};
			if (!State.Capture(Owner)) return State.Expressions.end();
			return std::ranges::find(State.Expressions, Selection.front(), [](const auto& E) { return E->Id; });
		};
		const auto View = Document.Inspect();
		const auto Selected = std::ranges::find(View.Nodes, Selection.front(),
			[](const auto& Entry) { return Entry.Node.Id; });
		if (Selected == View.Nodes.end()) return;
		ImGui::PushID(Selection.front().ToString().c_str());
		ImGui::TextWrapped("%s", Selected->PrimaryLabel.c_str());
		if (SelectedDiagnostic && SelectedDiagnostic->NodeId == Selected->Node.Id)
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
			if (!Result) ReportError(Result.Message);
			Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
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
				auto Replacement = GraphEditInternals::MakeParameterExpression(Parameter);
				if (!Replacement) { ReportError("The parameter definition is invalid."); return; }
				Replacement->Id = Expression->Id;
				const auto ExpressionIt = CaptureSelected();
				if (ExpressionIt == State.Expressions.end()) return;
				if (auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(ExpressionIt->Get()); Sample && Parameter.Type == EMaterialParameterType::Texture)
				{
					const auto* Texture = Cast<DMaterialExpressionTextureParameter>(Replacement.Get());
					Sample->Metadata = Texture->Metadata; Sample->DefaultValue = Texture->DefaultValue; Sample->TextureUsage = Texture->TextureUsage;
				}
				else *ExpressionIt = Replacement.Get();
				Submit(Document.ReplaceExpression(**ExpressionIt, &Transactions));
			};
			std::string Name = Parameter.Name.ToString();
			if (!Changed && EditText("Parameter name", Name)) { Parameter.Name = FName(Name); CommitParameter(); }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Change this node's parameter binding. Existing names use the existing parameter's value.");
			if (!Changed && Parameter.Type == EMaterialParameterType::Texture)
			{
				int Usage = static_cast<int>(Parameter.TextureUsage);
				if (DetailsStyle::EditRow("Texture usage", [&] { return ImGui::Combo("##Value", &Usage, "Color\0Normal\0Data / Mask\0"); }))
				{
					Parameter.TextureUsage = static_cast<ETextureUsage>(Usage);
					CommitParameter();
				}
				MonaImGui::PropertyEdit::BeginRow("Texture");
				const auto Picker = AssetPicker::Draw({.RequiredClass = DTexture2D::StaticClass(),
					.CurrentSelection = Parameter.Value.GetTexture().Texture.Get(), .SearchText = NodeTextureSearch,
					.AssignSelection = [&](DObject* Object, std::string& Error) {
						auto Value = Parameter.Value;
						Value.GetTexture().Texture = Cast<DTexture2D>(Object);
						const auto Result = FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Value, &Transactions);
						Error = Result.Message; Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
						return static_cast<bool>(Result);
					}});
				MonaImGui::PropertyEdit::EndRow();
				if (!Picker.Error.empty()) ReportError(Picker.Error);
				if (MonaImGui::PropertyEdit::BeginGroup("Sampler", "Sampler settings", ImGuiTreeNodeFlags_None))
				{
					const auto Combo = [&](const char* Label, auto& Value, const char* Names) {
						int Index = static_cast<int>(Value);
						if (Changed || !DetailsStyle::EditRow(Label, [&] { return ImGui::Combo("##Value", &Index, Names); })) return;
						Value = static_cast<std::remove_reference_t<decltype(Value)>>(Index);
						Submit(FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Parameter.Value, &Transactions));
					};
					Combo("Missing texture", Parameter.Value.GetTexture().TextureFallback, "White\0Black\0Flat normal\0");
					Combo("Min filter", Parameter.Value.GetTexture().SamplerState.MinFilter, "Nearest\0Linear\0Nearest mip nearest\0Linear mip nearest\0Nearest mip linear\0Linear mip linear\0");
					Combo("Mag filter", Parameter.Value.GetTexture().SamplerState.MagFilter, "Nearest\0Linear\0");
					Combo("Address U", Parameter.Value.GetTexture().SamplerState.AddressU, "Repeat\0Mirrored repeat\0Clamp\0");
					Combo("Address V", Parameter.Value.GetTexture().SamplerState.AddressV, "Repeat\0Mirrored repeat\0Clamp\0");
					MonaImGui::PropertyEdit::EndGroup();
				}
			}
			else if (!Changed)
			{
				auto Literal = ReadParameterLiteral(GetProgramType(Parameter.Type), Parameter.Value);
				if (EditLiteral("Default value", GetProgramType(Parameter.Type), Literal))
				{
					if (Parameter.Presentation == EMaterialParameterPresentation::Integer)
					{
						Literal.X = std::round(Literal.X);
						if (Parameter.bHasRange) Literal.X = std::clamp(Literal.X, Parameter.MinimumValue, Parameter.MaximumValue);
					}
					Submit(FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id,
						MakeParameterValue(GetProgramType(Parameter.Type), Literal), &Transactions));
				}
			}
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
			{
				const auto Candidate = CaptureSelected();
				if (Candidate != State.Expressions.end())
				{
					if (auto* E = Cast<DMaterialExpressionScalarConstant>(Candidate->Get())) E->Value = Value.X;
					if (auto* E = Cast<DMaterialExpressionVector2Constant>(Candidate->Get())) E->Value = FVector2(Value.X, Value.Y);
					if (auto* E = Cast<DMaterialExpressionVector3Constant>(Candidate->Get())) E->Value = FVector3(Value.X, Value.Y, Value.Z);
					if (auto* E = Cast<DMaterialExpressionVector4Constant>(Candidate->Get())) E->Value = FVector4(Value.X, Value.Y, Value.Z, Value.W);
					Submit(GraphEditInternals::CommitOwnedExpressions(Owner, State, "Edit Constant Expression", &Transactions));
				}
			}
		}
		for (const auto& Pin : Selected->Inputs)
		{
			if (Changed) break;
			if (Selected->Node.IsSampleUVInput(Pin.InputIndex) || Pin.SourceType > EMaterialProgramValueType::Float4) continue;
			ImGui::PushID(static_cast<int>(Pin.InputIndex));
			auto Value = Pin.InlineDefault;
			if (Value.Kind == EMaterialInputDefaultKind::None) Value.Type = Pin.SourceType;
			const bool bConnected = Pin.Link.SourceNodeId.IsValid();
			const auto EditValue = [&]() {
				if (EditLiteralValue(Value.Type, Value.Literal))
				{
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Submit(Document.SetInputDefault(Selected->Node.Id, Pin.InputIndex, Value, Pin.PortId, &Transactions));
				}
			};
			MonaImGui::PropertyEdit::BeginRow(Pin.Name.c_str());
			const float MenuWidth = ImGui::GetFrameHeight();
			const float ValueWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - MenuWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(ValueWidth);
			if (bConnected)
			{
				const auto Source = std::ranges::find(View.Nodes, Pin.Link.SourceNodeId,
					[](const auto& Entry) { return Entry.Node.Id; });
				std::string SourceLabel = Source != View.Nodes.end() ? Source->PrimaryLabel : "Unavailable node";
				if (Source != View.Nodes.end())
				{
					const auto Output = std::ranges::find_if(Source->Outputs, [&](const auto& Port) {
						return Pin.Link.SourceOutputId.IsValid() ? Port.PortId == Pin.Link.SourceOutputId
							: Port.OutputIndex == Pin.Link.SourceOutputIndex;
					});
					if (Output != Source->Outputs.end()) SourceLabel += "." + Output->Name;
				}
				if (ImGui::Button((SourceLabel + "###Source").c_str(), {ValueWidth, 0.0f}) && Source != View.Nodes.end())
					SelectAndFrame(Pin.Link.SourceNodeId);
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
					if (ImGui::MenuItem("Inline constant")) Submit(Document.InlineInputNode(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
					if (!Changed && ImGui::MenuItem("Disconnect")) Submit(Pin.PortId.IsValid()
						? Document.DisconnectCallInput(Selected->Node.Id, Pin.PortId, &Transactions)
						: Document.ConnectInput(Selected->Node.Id, Pin.InputIndex, {}, true, &Transactions));
				}
				else if (!Changed)
				{
					if (ImGui::MenuItem("Extract constant")) Submit(Document.ExtractInputDefault(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
					if (!Changed && Material && ImGui::MenuItem("Promote to parameter"))
					{
						FMaterialParameterDefinition Definition;
						Definition.Id = FGuid::NewGuid(); Definition.Type = *GetParameterType(Value.Type);
						Definition.Value = MakeParameterValue(Value.Type, Value.Literal); Definition.Name = "Parameter";
						for (uint32 Suffix = 1; Material->FindParameterDefinition(Definition.Name); ++Suffix)
							Definition.Name = FName(std::format("Parameter{}", Suffix));
						Definition.DisplayName = Definition.Name.ToString();
						auto Parameter = GraphEditInternals::MakeParameterExpression(Definition);
						if (const auto Candidate = Parameter ? CaptureSelected() : State.Expressions.end(); Candidate != State.Expressions.end())
						{
							const FMaterialExpressionInput Link{Parameter->Id};
							if (Pin.PortId.IsValid())
							{
								auto* Call = Cast<DMaterialExpressionFunctionCall>(Candidate->Get());
								auto Input = std::ranges::find(Call->Inputs, Pin.PortId, &FMaterialExpressionFunctionInputBinding::InputId);
								if (Input == Call->Inputs.end()) Call->Inputs.push_back({Pin.PortId, Value.Type, Link});
								else Input->Input = Link;
							}
							else VisitMaterialExpressionInputs(**Candidate, [&](uint32 Index, FMaterialExpressionInput& Input) {
								if (Index == Pin.InputIndex) Input = Link;
							});
							const auto Position = std::ranges::find(State.Presentation.Nodes, Expression->Id, &FMaterialGraphNodePresentation::NodeId);
							const int32 X = Position == State.Presentation.Nodes.end() ? -320 : Position->X - 320;
							const int32 Y = Position == State.Presentation.Nodes.end() ? 0 : Position->Y;
							State.Presentation.Nodes.push_back({Parameter->Id, X, Y});
							State.Expressions.emplace_back(Parameter.Get());
							Submit(GraphEditInternals::CommitOwnedExpressions(Owner, State, "Promote Input Parameter", &Transactions));
						}
					}
				}
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
		}
		MonaImGui::PropertyEdit::EndTable();
		ImGui::PopID();
	}
}
