#include "MaterialGraphCanvas.h"
#include "MaterialGraphDocument.h"
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
		const auto Submit = [&](FMaterialGraphCommandResult Result) {
			if (!Result) ReportError(Result.Message);
			Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
		};
		const auto EditText = [](const char* Label, std::string& Value) {
			std::array<char, MaterialProgramMaxDisplayNameBytes + 1> Buffer{};
			std::copy_n(Value.data(), std::min(Value.size(), Buffer.size() - 1), Buffer.data());
			if (!ImGui::InputText(Label, Buffer.data(), Buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) return false;
			Value = Buffer.data();
			return true;
		};
		const auto EditLiteral = [](const char* Label, EMaterialProgramValueType Type, FMaterialProgramLiteral& Value) {
			std::array<float, 4> Components{Value.X, Value.Y, Value.Z, Value.W};
			const int Count = static_cast<int>(Type) + 1;
			if (Count < 1 || Count > 4 || !ImGui::InputScalarN(Label, ImGuiDataType_Float, Components.data(), Count,
				nullptr, nullptr, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) return false;
			Value = {Components[0], Components[1], Components[2], Components[3]};
			return true;
		};
		auto* Material = Cast<DMaterial>(&Owner);
		if (Material && Selected->Node.Parameter.Id.IsValid())
		{
			auto Node = Selected->Node;
			auto& Parameter = Node.Parameter;
			int Type = static_cast<int>(Parameter.Type);
			if (ImGui::Combo("Type", &Type, "Scalar\0Vector3\0Texture\0Vector2\0Vector4\0"))
			{
				const auto Previous = ReadParameterLiteral(Node.ResultType, Parameter.Value);
				Parameter.Type = static_cast<EMaterialParameterType>(Type);
				Node.ResultType = GetProgramType(Parameter.Type);
				Node.Opcode = Parameter.Type == EMaterialParameterType::Texture
					? EMaterialProgramOpcode::TextureParameter : EMaterialProgramOpcode::Parameter;
				Node.Inputs.clear(); Node.InputDefaults.clear(); Node.UVSettings = {};
				Parameter.Value = MakeParameterValue(Node.ResultType, Previous);
				Parameter.Presentation = EMaterialParameterPresentation::Default;
				Parameter.bHasRange = false;
				Submit(Document.ReplaceNode(Node, &Transactions));
			}
			int Presentation = static_cast<int>(Parameter.Presentation);
			if (!Changed && ImGui::Combo("Presentation", &Presentation, "Default\0Drag\0Integer\0Color\0Asset picker\0"))
			{
				Parameter.Presentation = static_cast<EMaterialParameterPresentation>(Presentation);
				Submit(Document.ReplaceNode(Node, &Transactions));
			}
			std::string Name = Parameter.Name.ToString();
			if (!Changed && EditText("Name", Name)) { Parameter.Name = FName(Name); Submit(Document.ReplaceNode(Node, &Transactions)); }
			if (!Changed && EditText("Display name", Parameter.DisplayName)) Submit(Document.ReplaceNode(Node, &Transactions));
			std::string Group = Parameter.GroupName.ToString();
			if (!Changed && EditText("Group", Group)) { Parameter.GroupName = FName(Group); Submit(Document.ReplaceNode(Node, &Transactions)); }
			if (!Changed && ImGui::InputInt("Order", &Parameter.SortOrder, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) Submit(Document.ReplaceNode(Node, &Transactions));
			if (!Changed && ImGui::Checkbox("Range hint", &Parameter.bHasRange)) Submit(Document.ReplaceNode(Node, &Transactions));
			if (!Changed && Parameter.bHasRange)
			{
				float Range[2]{Parameter.MinimumValue, Parameter.MaximumValue};
				if (ImGui::InputFloat2("Min / Max", Range, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue))
				{
					Parameter.MinimumValue = Range[0]; Parameter.MaximumValue = Range[1];
					Submit(Document.ReplaceNode(Node, &Transactions));
				}
			}
			if (!Changed && Parameter.Type == EMaterialParameterType::Texture)
			{
				int Usage = static_cast<int>(Parameter.TextureUsage);
				if (ImGui::Combo("Texture usage", &Usage, "Color\0Normal\0Data / Mask\0"))
				{
					Parameter.TextureUsage = static_cast<ETextureUsage>(Usage);
					Submit(Document.ReplaceNode(Node, &Transactions));
				}
				const auto Picker = AssetPicker::Draw({.RequiredClass = DTexture2D::StaticClass(),
					.CurrentSelection = Parameter.Value.TextureValue.Get(), .SearchText = NodeTextureSearch,
					.AssignSelection = [&](DObject* Object, std::string& Error) {
						auto Value = Parameter.Value;
						Value.TextureValue = Cast<DTexture2D>(Object);
						const auto Result = FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Value, &Transactions);
						Error = Result.Message; Changed = Result.Status == EMaterialGraphCommandStatus::Succeeded;
						return static_cast<bool>(Result);
					}});
				if (!Picker.Error.empty()) ReportError(Picker.Error);
				const auto Combo = [&](const char* Label, auto& Value, const char* Names) {
					int Index = static_cast<int>(Value);
					if (Changed || !ImGui::Combo(Label, &Index, Names)) return;
					Value = static_cast<std::remove_reference_t<decltype(Value)>>(Index);
					Submit(FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id, Parameter.Value, &Transactions));
				};
				Combo("Fallback", Parameter.Value.TextureFallback, "White\0Black\0Flat normal\0");
				Combo("Min filter", Parameter.Value.SamplerState.MinFilter, "Nearest\0Linear\0Nearest mip nearest\0Linear mip nearest\0Nearest mip linear\0Linear mip linear\0");
				Combo("Mag filter", Parameter.Value.SamplerState.MagFilter, "Nearest\0Linear\0");
				Combo("Address U", Parameter.Value.SamplerState.AddressU, "Repeat\0Mirrored repeat\0Clamp\0");
				Combo("Address V", Parameter.Value.SamplerState.AddressV, "Repeat\0Mirrored repeat\0Clamp\0");
			}
			else if (!Changed)
			{
				auto Literal = ReadParameterLiteral(Node.ResultType, Parameter.Value);
				if (EditLiteral("Default", Node.ResultType, Literal))
				{
					if (Parameter.Presentation == EMaterialParameterPresentation::Integer)
					{
						Literal.X = std::round(Literal.X);
						if (Parameter.bHasRange) Literal.X = std::clamp(Literal.X, Parameter.MinimumValue, Parameter.MaximumValue);
					}
					Submit(FMaterialGraphOperations::SetParameterValue(*Material, Parameter.Id,
						MakeParameterValue(Node.ResultType, Literal), &Transactions));
				}
			}
		}
		else if (Selected->Node.Opcode == EMaterialProgramOpcode::Constant)
		{
			auto Node = Selected->Node;
			if (EditLiteral("Value", Node.ResultType, Node.Literal)) Submit(Document.ReplaceNode(Node, &Transactions));
		}
		for (const auto& Pin : Selected->Inputs)
		{
			if (Changed) break;
			if (IsMaterialSampleUVInput(Selected->Node, Pin.InputIndex) || Pin.SourceType > EMaterialProgramValueType::Float4) continue;
			ImGui::PushID(static_cast<int>(Pin.InputIndex));
			ImGui::TextUnformatted(Pin.Name.c_str());
			auto Value = Pin.InlineDefault;
			if (Value.Kind == EMaterialInputDefaultKind::None) Value.Type = Pin.SourceType;
			if (Pin.Link.SourceNodeId.IsValid()) ImGui::TextDisabled("Connected; literal fallback is retained");
			if (EditLiteral("Fallback", Value.Type, Value.Literal))
			{
				Value.Kind = EMaterialInputDefaultKind::Literal;
				Submit(Document.SetInputDefault(Selected->Node.Id, Pin.InputIndex, Value, Pin.PortId, &Transactions));
			}
			if (!Changed && Pin.Link.SourceNodeId.IsValid())
			{
				if (ImGui::SmallButton("Inline constant")) Submit(Document.InlineInputNode(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
				if (!Changed && ImGui::SmallButton("Disconnect")) Submit(Pin.PortId.IsValid()
					? Document.DisconnectCallInput(Selected->Node.Id, Pin.PortId, &Transactions)
					: Document.ConnectInput(Selected->Node.Id, Pin.InputIndex, {}, true, &Transactions));
			}
			else if (!Changed)
			{
				if (ImGui::SmallButton("Extract constant")) Submit(Document.ExtractInputDefault(Selected->Node.Id, Pin.InputIndex, Pin.PortId, &Transactions));
				if (!Changed && Material && ImGui::SmallButton("Promote to parameter"))
				{
					auto Candidate = State;
					FMaterialProgramNode ParameterNode{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::Parameter, .ResultType = Value.Type};
					ParameterNode.Parameter.Id = FGuid::NewGuid();
					ParameterNode.Parameter.Type = *GetParameterType(Value.Type);
					ParameterNode.Parameter.Value = MakeParameterValue(Value.Type, Value.Literal);
					ParameterNode.Parameter.Name = FName("Parameter");
					for (uint32 Suffix = 1; Material->FindParameterDefinition(ParameterNode.Parameter.Name); ++Suffix)
						ParameterNode.Parameter.Name = FName(std::format("Parameter{}", Suffix));
					ParameterNode.Parameter.DisplayName = ParameterNode.Parameter.Name.ToString();
					const FMaterialProgramLink Link{ParameterNode.Id};
					if (Pin.PortId.IsValid())
					{
						auto& Call = *std::ranges::find(Candidate.Calls, Selected->Node.Id, &FMaterialFunctionCall::NodeId);
						auto Input = std::ranges::find(Call.Inputs, Pin.PortId, &FMaterialFunctionInputBinding::InputId);
						if (Input == Call.Inputs.end()) Call.Inputs.push_back({Pin.PortId, Value.Type, Link});
						else Input->Source = Link;
					}
					else std::ranges::find(Candidate.Program.Nodes, Selected->Node.Id, &FMaterialProgramNode::Id)->Inputs[Pin.InputIndex] = Link;
					const auto Position = std::ranges::find(Candidate.Presentation.Nodes, Selected->Node.Id, &FMaterialGraphNodePresentation::NodeId);
					const int32 X = Position == Candidate.Presentation.Nodes.end() ? -320 : Position->X - 320;
					const int32 Y = Position == Candidate.Presentation.Nodes.end() ? 0 : Position->Y;
					Candidate.Presentation.Nodes.push_back({ParameterNode.Id, X, Y});
					Candidate.Program.Nodes.push_back(std::move(ParameterNode));
					Submit(Document.Commit(std::move(Candidate), "Promote Input Parameter", &Transactions));
				}
			}
			ImGui::Separator(); ImGui::PopID();
		}
		if (!Changed && IsMaterialSamplingNode(Selected->Node.Opcode))
		{
			const uint32 UVIndex = Selected->Node.Opcode == EMaterialProgramOpcode::TextureSample2D ? 1 : 0;
			const bool Connected = Selected->Node.Inputs[UVIndex].SourceNodeId.IsValid();
			ImGui::TextUnformatted("UV Settings (rotation in radians)");
			if (Connected && ImGui::SmallButton("Disconnect UV")) Submit(Document.ConnectInput(Selected->Node.Id, UVIndex, {}, true, &Transactions));
			constexpr std::array Names{"Channel", "Scale", "Offset", "Rotation"};
			for (uint32 Index = 0; Index < 4 && !Changed; ++Index)
			{
				auto Value = GetMaterialUVSetting(Selected->Node.UVSettings, Index);
				if (EditLiteral(Names[Index], Value.Type, Value.Literal))
				{
					auto Node = Selected->Node;
					const_cast<FMaterialInputDefault&>(GetMaterialUVSetting(Node.UVSettings, Index)) = Value;
					Submit(Document.ReplaceNode(Node, &Transactions));
				}
			}
			if (!Connected && !Changed && ImGui::Button("Extract Texture Coordinates")) Submit(Document.ExtractUVSettings(Selected->Node.Id, &Transactions));
		}
		ImGui::PopID();
	}
}
