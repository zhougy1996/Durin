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
		std::optional<FMaterialGraphPinAddress> Disconnect;
		for (const auto& Pin : Node->GetView().Inputs)
		{
			ImGui::PushID(Pin.PortId.ToString().c_str());
			ImGui::Text("%s%s", Pin.Name.c_str(), Pin.bRequired ? " (required)" : "");
			ImGui::TextDisabled("%s", DescribeFunctionDefault(Pin.Default).c_str());
			if (Pin.Link.SourceNodeId.IsValid() && !Pin.bRequired && ImGui::SmallButton("Use Default"))
			{
				Disconnect = Node->GetView().InputAddress(Pin);
			}
			ImGui::PopID();
		}
		ImGui::PopID();
		if (Disconnect)
		{
			const auto Result = Graph.GetSchema().BreakPinLinks(Graph, *Disconnect);
			if (!Result) Error = FormatMaterialGraphCommandResult(Result);
		}
	}

}
