#include "Widgets/MaterialFunctionCallPicker.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "MonaImGui.h"

namespace Durin::MaterialEditor
{
	auto DrawMaterialFunctionCallInputs(DMaterialGraph& Graph, const FGuid& CallId,
		std::string& Error) -> void
	{
		const auto* Node = Graph.FindNode(CallId);
		if (!Node || Node->GetView().Node.Opcode != EMaterialProgramOpcode::FunctionCall) return;
		ImGui::PushID(CallId.ToString().c_str());
		const FMaterialGraphPin* Disconnect = nullptr;
		for (const auto& Pin : Node->GetView().Inputs)
		{
			ImGui::PushID(Pin.PortId.ToString().c_str());
			ImGui::Text("%s%s", Pin.Name.c_str(), Pin.bRequired ? " (required)" : "");
			ImGui::TextDisabled("%s", DescribeFunctionDefault(Pin.Default).c_str());
			if (Pin.Link.SourceNodeId.IsValid() && !Pin.bRequired && ImGui::SmallButton("Use Default"))
			{
				Disconnect = Node->FindInputPin(Pin);
			}
			ImGui::PopID();
		}
		ImGui::PopID();
		if (Disconnect)
		{
			Graph.GetSchema().BreakPinLinks(Disconnect, true, &Error);
		}
	}

}
