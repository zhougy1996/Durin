#include "Widgets/MaterialFunctionCallPicker.h"
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
				const auto Result = Document.Disconnect(FMaterialGraphPinAddress::Input(CallId, Pin.InputIndex, Pin.PortId), &Transactions);
				if (!Result) Error = Result.Message;
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}

}
