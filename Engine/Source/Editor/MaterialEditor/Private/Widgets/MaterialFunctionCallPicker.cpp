#include "Widgets/MaterialFunctionCallPicker.h"
#include "Editor/AssetPicker.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "MonaImGui.h"

namespace Durin::Editor::Material
{
	auto DrawMaterialFunctionCallInputs(DObject& Owner, const FGuid& CallId,
		DTransactor& Transactions, std::string& Error) -> void
	{
		FMaterialGraphDocument Document(Owner);
		const auto View = Document.Inspect();
		const auto Node = std::ranges::find(View.Nodes, CallId, [](const auto& Item) { return Item.Node.Id; });
		if (Node == View.Nodes.end() || Node->Node.Opcode != EMaterialProgramOpcode::FunctionCall) return;
		ImGui::PushID(CallId.ToString().c_str());
		for (const auto& Pin : Node->Inputs)
		{
			ImGui::PushID(Pin.PortId.ToString().c_str());
			ImGui::Text("%s%s", Pin.Name.c_str(), Pin.bRequired ? " (required)" : "");
			ImGui::TextDisabled("%s", DescribeFunctionDefault(Pin.Default).c_str());
			if (Pin.Link.SourceNodeId.IsValid() && !Pin.bRequired && ImGui::SmallButton("Use Default"))
			{
				const auto Result = Document.DisconnectCallInput(CallId, Pin.PortId, &Transactions);
				if (!Result) Error = Result.Message;
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	auto FMaterialFunctionCallPicker::Draw(DObject& Owner, DTransactor& Transactions, std::string& Error) -> void
	{
		if (!ImGui::CollapsingHeader("Insert Function Call")) return;
		const auto Picked = AssetPicker::Draw({.RequiredClass = DMaterialFunctionInterface::StaticClass(),
			.CurrentSelection = Function.Get(), .SearchText = Search, .AssignSelection = [&](DObject* Object, std::string&) {
				Function = Cast<DMaterialFunctionInterface>(Object); Inputs.clear(); return true;
			}});
		if (!Picked.Error.empty()) Error = Picked.Error;
		auto* Callee = Cast<DMaterialFunctionInterface>(Function.Get());
		if (!Callee) return;
		FMaterialGraphDocument Document(Owner);
		const auto View = Document.Inspect();
		for (const auto& Port : Callee->GetFunctionSignature().Inputs)
		{
			ImGui::PushID(Port.Id.ToString().c_str());
			const auto Binding = std::ranges::find(Inputs, Port.Id, &FMaterialFunctionInputBinding::InputId);
			const bool HasBinding = Binding != Inputs.end();
			const auto Label = std::format("{} ({}){}", Port.Name, GetProgramTypeName(Port.Type), Port.bRequired ? " *" : "");
			if (ImGui::BeginCombo(Label.c_str(), Binding == Inputs.end() ? (Port.bRequired ? "Required connection" : "Use default") : "Connected"))
			{
				if (!Port.bRequired && ImGui::Selectable("Use default")) std::erase_if(Inputs, [&](const auto& Input) { return Input.InputId == Port.Id; });
				for (const auto& Node : View.Nodes)
					for (const auto& Pin : Node.Outputs)
						if (!Pin.bMissing && Pin.Type == Port.Type && ImGui::Selectable(std::format("{}: {}##{}{}{}", Node.PrimaryLabel,
							Pin.Name, Node.Node.Id.ToString(), Pin.PortId.ToString(), Pin.OutputIndex).c_str()))
						{
							std::erase_if(Inputs, [&](const auto& Input) { return Input.InputId == Port.Id; });
							Inputs.push_back({Port.Id, Port.Type, {Node.Node.Id, Pin.OutputIndex, Pin.PortId}});
						}
				ImGui::EndCombo();
			}
			if (!Port.bRequired && !HasBinding) ImGui::TextDisabled("%s", DescribeFunctionDefault(Port.Default).c_str());
			ImGui::PopID();
		}
		if (ImGui::Button("Insert Call"))
		{
			const auto Result = Document.InsertFunctionCall(*Callee, 0, 480, Inputs, &Transactions);
			if (!Result) Error = Result.Message;
			else { Function = nullptr; Inputs.clear(); }
		}
	}
}
