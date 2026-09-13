#include "MaterialGraphCanvas.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphValueTypes.h"
#include "Editor/AssetPicker.h"
#include "Texture/Texture2D.h"

namespace Durin::Editor::Material
{
	auto FMaterialGraphCanvas::DrawSelectionDetails(DObject& Owner, DTransactor& Transactions,
		const FReportError& ReportError) -> void
	{
		const auto Selection = GetSelectedProgramNodes();
		if (Selection.size() != 1) return;
		FMaterialGraphDocument Document(Owner);
		FMaterialGraphDocumentState State;
		if (!Document.Capture(State)) return;
		const auto View = Document.Inspect();
		const auto Selected = std::ranges::find(View.Nodes, Selection.front(),
			[](const auto& Entry) { return Entry.Node.Id; });
		if (Selected == View.Nodes.end() || !ImGui::CollapsingHeader("Selected Node", ImGuiTreeNodeFlags_DefaultOpen)) return;
		ImGui::PushID(Selection.front().ToString().c_str());
		ImGui::TextUnformatted(Selected->PrimaryLabel.c_str());
		if (SelectedDiagnostic && SelectedDiagnostic->NodeId == Selected->Node.Id)
		{
			constexpr std::array UVNames{"Channel", "Scale", "Offset", "Rotation"};
			if (SelectedDiagnostic->UVFieldIndex && *SelectedDiagnostic->UVFieldIndex < UVNames.size())
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic UV field: %s", UVNames[*SelectedDiagnostic->UVFieldIndex]);
			else if (SelectedDiagnostic->InputIndex)
				ImGui::TextColored({1, .65f, .25f, 1}, "Diagnostic input: %u", *SelectedDiagnostic->InputIndex);
		}
		bool Changed = false;
		auto Submit = [&](FMaterialGraphCommandResult Result)
		{
			if (!Result) ReportError(Result.Message);
			Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
		};
		auto* Material = Cast<DMaterial>(&Owner);
		const auto Definition = std::ranges::find(State.Definitions, Selected->Node.ParameterId, &FMaterialParameterDefinition::Id);
		if (Definition != State.Definitions.end() && Definition->Type == EMaterialParameterType::Texture && Material)
		{
			ImGui::Text("Parameter: %s", Definition->Name.ToString().c_str());
			const auto Picker = AssetPicker::Draw({.RequiredClass = DTexture2D::StaticClass(),
				.CurrentSelection = Definition->Value.TextureValue.Get(), .SearchText = NodeTextureSearch,
				.AssignSelection = [&](DObject* Object, std::string& Error)
				{
					auto Value = Definition->Value;
					Value.TextureValue = Cast<DTexture2D>(Object);
					auto Candidate = State;
					std::ranges::find(Candidate.Definitions, Definition->Id, &FMaterialParameterDefinition::Id)->Value = Value;
					const auto Result = Document.Commit(std::move(Candidate), "Assign Texture", &Transactions);
					Error = Result.Message;
					Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
					return static_cast<bool>(Result);
				}});
			if (!Picker.Error.empty()) ReportError(Picker.Error);
			if (ImGui::BeginCombo("Rebind parameter", Definition->Name.ToString().c_str()))
			{
				for (const auto& Candidate : State.Definitions)
					if (Candidate.Type == EMaterialParameterType::Texture && ImGui::Selectable(Candidate.Name.ToString().c_str(), Candidate.Id == Definition->Id))
					{
						auto Node = Selected->Node;
						Node.ParameterId = Candidate.Id;
						Submit(Document.ReplaceNode(std::move(Node), &Transactions));
					}
				ImGui::EndCombo();
			}
		}
		auto DrawValue = [&](const char* Label, FMaterialInputDefault Value, bool Connected,
			const std::function<void(FMaterialInputDefault, std::optional<FMaterialParameterDefinition>)>& Assign)
		{
			ImGui::TextUnformatted(Label);
			if (Connected) ImGui::TextDisabled("Connected; retained value is inactive");
			const auto Parameter = std::ranges::find(State.Definitions, Value.ParameterId, &FMaterialParameterDefinition::Id);
			if (Value.Kind == EMaterialInputDefaultKind::Parameter && Parameter != State.Definitions.end())
			{
				ImGui::Text("Parameter: %s", Parameter->Name.ToString().c_str());
				auto Literal = ReadParameterLiteral(Value.Type, Parameter->Value);
				std::array<float, 4> Components{Literal.X, Literal.Y, Literal.Z, Literal.W};
				const int Count = Value.Type == EMaterialProgramValueType::Float ? 1 : Value.Type == EMaterialProgramValueType::Float2 ? 2
					: Value.Type == EMaterialProgramValueType::Float3 ? 3 : 4;
				if (ImGui::InputScalarN("Parameter default", ImGuiDataType_Float, Components.data(), Count, nullptr, nullptr, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue))
				{
					auto Candidate = State;
					std::ranges::find(Candidate.Definitions, Parameter->Id, &FMaterialParameterDefinition::Id)->Value =
						MakeParameterValue(Value.Type, {Components[0], Components[1], Components[2], Components[3]});
					Submit(Document.Commit(std::move(Candidate), "Edit Parameter Default", &Transactions));
				}
			}
			if (Value.Kind == EMaterialInputDefaultKind::Literal)
			{
				std::array<float, 4> Components{Value.Literal.X, Value.Literal.Y, Value.Literal.Z, Value.Literal.W};
				const int Count = Value.Type == EMaterialProgramValueType::Float ? 1 : Value.Type == EMaterialProgramValueType::Float2 ? 2
					: Value.Type == EMaterialProgramValueType::Float3 ? 3 : 4;
				if (ImGui::InputScalarN("Value", ImGuiDataType_Float, Components.data(), Count, nullptr, nullptr, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue))
				{
					Value.Literal = {Components[0], Components[1], Components[2], Components[3]};
					Assign(Value, std::nullopt);
				}
			}
			else if (Value.Kind == EMaterialInputDefaultKind::None) ImGui::TextDisabled("Function default / required connection");
			if (ImGui::BeginMenu("Binding actions"))
			{
				if (ImGui::MenuItem("Use literal"))
				{
					if (Parameter != State.Definitions.end()) Value.Literal = ReadParameterLiteral(Value.Type, Parameter->Value);
					Value.Kind = EMaterialInputDefaultKind::Literal;
					Value.ParameterId = {};
					Assign(Value, std::nullopt);
				}
				if (ImGui::MenuItem("Remove retained binding")) Assign({}, std::nullopt);
				if (Material && ImGui::BeginMenu("Bind existing parameter"))
				{
					for (const auto& Candidate : State.Definitions)
						if (GetProgramType(Candidate.Type) == Value.Type && ImGui::MenuItem(Candidate.Name.ToString().c_str()))
							Assign({.Kind = EMaterialInputDefaultKind::Parameter, .Type = Value.Type, .ParameterId = Candidate.Id}, std::nullopt);
					ImGui::EndMenu();
				}
				if (Material && ImGui::BeginMenu("Create parameter"))
				{
					ImGui::InputTextWithHint("Name", "Parameter name", PromotionNameDraft.data(), PromotionNameDraft.size());
					if (ImGui::Button("Create and bind"))
					{
						FMaterialParameterDefinition NewDefinition;
						NewDefinition.Id = FGuid::NewGuid();
						NewDefinition.Name = FName(PromotionNameDraft.data());
						NewDefinition.DisplayName = PromotionNameDraft.data();
						NewDefinition.Type = *GetParameterType(Value.Type);
						NewDefinition.Value = MakeParameterValue(Value.Type, Value.Literal);
						Assign({.Kind = EMaterialInputDefaultKind::Parameter, .Type = Value.Type, .ParameterId = NewDefinition.Id}, NewDefinition);
					}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
		};
		for (const auto& Pin : Selected->Inputs)
		{
			if (Changed) break;
			if (IsMaterialSampleUVInput(Selected->Node, Pin.InputIndex) || Pin.SourceType > EMaterialProgramValueType::Float4) continue;
			ImGui::PushID(static_cast<int>(Pin.InputIndex));
			auto Value = Pin.InlineDefault;
			if (Value.Kind == EMaterialInputDefaultKind::None) Value.Type = Pin.SourceType;
			DrawValue(Pin.Name.c_str(), Value, Pin.Link.SourceNodeId.IsValid(), [&](auto NewValue, auto NewDefinition)
			{
				if (!NewDefinition) { Submit(Document.SetInputDefault(Selected->Node.Id, Pin.InputIndex, NewValue, Pin.PortId, &Transactions)); return; }
				auto Candidate = State;
				Candidate.Definitions.push_back(*NewDefinition);
				auto& Node = *std::ranges::find(Candidate.Program.Nodes, Selected->Node.Id, &FMaterialProgramNode::Id);
				if (Pin.PortId.IsValid())
				{
					auto& Call = *std::ranges::find(Candidate.Calls, Node.Id, &FMaterialFunctionCall::NodeId);
					auto Input = std::ranges::find(Call.Inputs, Pin.PortId, &FMaterialFunctionInputBinding::InputId);
					if (Input == Call.Inputs.end()) Call.Inputs.push_back({Pin.PortId, NewValue.Type, {}, NewValue});
					else Input->Default = NewValue;
				}
				else if (Node.Opcode == EMaterialProgramOpcode::TextureCoordinates)
					const_cast<FMaterialInputDefault&>(GetMaterialUVSetting(Node.UVSettings, Pin.InputIndex)) = NewValue;
				else { Node.InputDefaults.resize(Node.Inputs.size()); Node.InputDefaults[Pin.InputIndex] = NewValue; }
				Submit(Document.Commit(std::move(Candidate), "Create Input Parameter", &Transactions));
			});
			if (Pin.Link.SourceNodeId.IsValid())
			{
				if (ImGui::SmallButton("Inline source")) Submit(Document.InlineInputNode(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
				ImGui::SameLine();
				if (ImGui::SmallButton("Disconnect")) Submit(Pin.PortId.IsValid()
					? Document.DisconnectCallInput(Selected->Node.Id, Pin.PortId, &Transactions)
					: Document.ConnectInput(Selected->Node.Id, Pin.InputIndex, {}, true, &Transactions));
			}
			else if (ImGui::SmallButton("Extract input node")) Submit(Document.ExtractInputDefault(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
			ImGui::Separator();
			ImGui::PopID();
		}
		if (!Changed && IsMaterialSamplingNode(Selected->Node.Opcode))
		{
			const uint32 UVIndex = Selected->Node.Opcode == EMaterialProgramOpcode::TextureSample2D ? 1 : 0;
			const bool Connected = Selected->Node.Inputs[UVIndex].SourceNodeId.IsValid();
			ImGui::TextUnformatted("UV Settings (rotation in radians)");
			if (Connected && ImGui::SmallButton("Disconnect external UV")) Submit(Document.ConnectInput(Selected->Node.Id, UVIndex, {}, true, &Transactions));
			constexpr std::array Names{"Channel", "Scale", "Offset", "Rotation"};
			for (uint32 Index = 0; Index < 4 && !Changed; ++Index)
			{
				ImGui::PushID(static_cast<int>(Index + 100));
				DrawValue(Names[Index], GetMaterialUVSetting(Selected->Node.UVSettings, Index), Connected, [&](auto Value, auto DefinitionToAdd)
				{
					auto Candidate = State;
					if (DefinitionToAdd) Candidate.Definitions.push_back(*DefinitionToAdd);
					auto& Node = *std::ranges::find(Candidate.Program.Nodes, Selected->Node.Id, &FMaterialProgramNode::Id);
					const_cast<FMaterialInputDefault&>(GetMaterialUVSetting(Node.UVSettings, Index)) = Value;
					Submit(Document.Commit(std::move(Candidate), "Edit UV Setting", &Transactions));
				});
				ImGui::PopID();
			}
			if (!Connected && !Changed && ImGui::Button("Extract Texture Coordinates")) Submit(Document.ExtractUVSettings(Selected->Node.Id, &Transactions));
		}
		ImGui::PopID();
	}
}
