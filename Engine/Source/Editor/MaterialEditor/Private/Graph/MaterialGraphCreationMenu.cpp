#include "Graph/MaterialGraphCanvas.h"
#include "MaterialGraphDocument.h"
#include "Graph/MaterialGraphControls.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "Editor/Transaction.h"
#include "Graph/MaterialGraphCreationShortcuts.h"
#include "Asset/Asset.h"
#include "DObject/Class.h"
#include "Editor/AssetPicker.h"
#include "Misc/StringHelper.h"

namespace Durin::Editor::Material
{
	namespace
	{
		auto FormatInputSignature(const FMaterialGraphCatalogEntry& Entry) -> std::string
		{
			std::string Result;
			for (size_t Index = 0; Index < Entry.AcceptedInputTypes.size(); ++Index)
			{
				if (!Result.empty()) Result += ", ";
				Result += Index < Entry.InputNames.size()
					? Entry.InputNames[Index] : std::format("Input {}", Index + 1);
				Result += ": ";
				if (IsMaterialAdaptiveNumeric(Entry.Opcode) && !(Entry.Opcode == EMaterialProgramOpcode::Lerp && Index == 2))
				{
					Result += Entry.Opcode == EMaterialProgramOpcode::Normalize ? "Vector (automatic)" : "Numeric (automatic)";
					continue;
				}
				for (size_t TypeIndex = 0;
					TypeIndex < Entry.AcceptedInputTypes[Index].size(); ++TypeIndex)
				{
					if (TypeIndex != 0) Result += '/';
					Result += GetProgramTypeName(Entry.AcceptedInputTypes[Index][TypeIndex]);
				}
			}
			return Result.empty() ? "No inputs" : Result;
		}
	}
	auto FMaterialGraphCanvas::RememberCreation(const FMaterialGraphCreationAction& Node) -> void
	{
		const std::string Key = Node.Id;
		std::erase(RecentCreationMenuEntries, Key);
		RecentCreationMenuEntries.insert(RecentCreationMenuEntries.begin(), Key);
		if (RecentCreationMenuEntries.size() > 8) RecentCreationMenuEntries.resize(8);
		bCreationMenuResultsDirty = true;
	}

	auto FMaterialGraphCanvas::DrawCreationMenu(
		DObject& Owner,
		DTransactor& Transactions,
		const FMaterialGraphView& View,
		const FReportError& ReportError) -> void
	{
		auto* CreationMenu = std::get_if<FNodeCreationMenuInteraction>(&Interaction);
		if (CreationMenu && CreationMenu->bOpenRequested)
		{
			CreationMenu->Search.fill('\0');
			CreationMenu->Selection = 0;
			CreationMenu->FunctionPaths.clear();
			for (const auto& [PackagePath, Data] : CaptureAssetCatalogSnapshot().Assets)
				for (const auto& Asset : Data.TopLevelAssets)
					if (!Asset.IsRedirector() && AssetPicker::MatchesClass(FindClassByQualifiedName(Asset.AssetClassName),
						DMaterialFunctionInterface::StaticClass(), EAssetClassPolicy::Derived))
						CreationMenu->FunctionPaths.push_back(Asset.AssetPath.ToString());
			std::ranges::sort(CreationMenu->FunctionPaths);
			bCreationMenuResultsDirty = true;
			ImGui::OpenPopup("MaterialNodeCreationMenu");
			CreationMenu->bOpenRequested = false;
		}
		const ImGuiViewport* MainViewport = ImGui::GetWindowViewport();
		// BeginPopup adds AlwaysAutoResize, so constrain both axes every frame.
		const float Margin = MonaImGui::ScaleUI(16.0f);
		ImGui::SetNextWindowSize({
			std::min(MonaImGui::ScaleUI(420.0f), std::max(1.0f, MainViewport->WorkSize.x - Margin)),
			std::min(MonaImGui::ScaleUI(480.0f), std::max(1.0f, MainViewport->WorkSize.y - Margin))});
		if (!ImGui::BeginPopup("MaterialNodeCreationMenu", ImGuiWindowFlags_NoSavedSettings))
		{
			if (CreationMenu) ResetInteraction();
			return;
		}
		CreationMenu = std::get_if<FNodeCreationMenuInteraction>(&Interaction);
		if (!CreationMenu)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(-FLT_MIN);
		const bool bSearchSubmitted = ImGui::InputTextWithHint(
			"##NodeCreationMenuSearch", "Search nodes or categories...",
			CreationMenu->Search.data(), CreationMenu->Search.size(),
			ImGuiInputTextFlags_EnterReturnsTrue);
		if (ImGui::IsItemEdited()) CreationMenu->Selection = 0;

		std::optional<EMaterialProgramValueType> SourceType;
		const auto Source = std::ranges::find(View.Nodes, CreationMenu->SourceNode,
			[](const FMaterialGraphNodeView& Node) { return Node.Node.Id; });
		if (CreationMenu->SourceNode.IsValid() && Source != View.Nodes.end())
			for (const auto& Pin : Source->Outputs)
				if (Pin.PortId == CreationMenu->SourceOutputId && Pin.OutputIndex == CreationMenu->SourceOutputIndex) SourceType = Pin.Type;
		const bool bCreationMenuResultsStale =
			bCreationMenuResultsDirty
			|| CachedCreationMenuQuery != CreationMenu->Search.data()
			|| CachedCreationMenuSourceType != SourceType;
		if (bCreationMenuResultsStale)
		{
			CachedCreationActions.clear();
			const auto& Document = GraphDocument;
			for (const auto Index : FMaterialGraphOperations::SearchCatalogIndices(Catalog, "", SourceType))
				CachedCreationActions.push_back(MakeCreationAction(Catalog[Index]));
			for (bool bOutput : {false, true})
				CachedCreationActions.push_back(MakePortCreationAction(bOutput, SourceType.value_or(EMaterialProgramValueType::Float)));
			for (const auto& Path : CreationMenu->FunctionPaths)
				CachedCreationActions.push_back(MakeFunctionCreationAction(Path));
			CachedCreationMenuResults.clear();
			const std::string Query = CreationMenu->Search.data();
			const auto Needle = StringUtils::FoldAscii(Query);
			std::vector<uint8> Ranks(CachedCreationActions.size(), 4);
			for (size_t Index = 0; Index < CachedCreationActions.size(); ++Index)
			{
				const auto& Action = CachedCreationActions[Index];
				auto& Match = Ranks[Index];
				if (Needle.empty()) Match = 3;
				else
				{
					const auto Test = [&](const std::string& Field) {
						const auto Text = StringUtils::FoldAscii(Field);
						if (Text == Needle) Match = std::min<uint8>(Match, 0);
						else if (Text.starts_with(Needle)) Match = std::min<uint8>(Match, 1);
						else if (Text.find(Needle) != std::string::npos) Match = std::min<uint8>(Match, 2);
					};
					Test(Action.Name); Test(Action.Category); Test(Action.Keywords);
					if (const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Action.Payload))
						for (const auto& Field : Entry->NormalizedSearchFields) Test(Field);
				}
				// Resolve live function signatures only for matching rows.
				if (Match < 4 && Document.CanCreate(Action, SourceType)) CachedCreationMenuResults.push_back(Index);
			}
			std::stable_sort(CachedCreationMenuResults.begin(), CachedCreationMenuResults.end(),
				[&](size_t A, size_t B) { return Ranks[A] < Ranks[B]; });
			CachedCreationMenuRecentCount = 0;
			if (Query.empty())
			{
				std::vector<size_t> Recent;
				for (const auto& Key : RecentCreationMenuEntries)
				{
					const auto Found = std::ranges::find_if(CachedCreationMenuResults, [&](size_t Index) {
						return CachedCreationActions[Index].Id == Key;
					});
					if (Found != CachedCreationMenuResults.end()) Recent.push_back(*Found);
				}
				CachedCreationMenuRecentCount = Recent.size();
				CachedCreationMenuResults.insert(CachedCreationMenuResults.begin(), Recent.begin(), Recent.end());
			}
			bCreationMenuResultsDirty = false;
			CachedCreationMenuQuery = CreationMenu->Search.data();
			CachedCreationMenuSourceType = SourceType;
		}
		const std::vector<size_t>& Results = CachedCreationMenuResults;
		const auto ResultCount = static_cast<int32>(Results.size());
		const int32 PreviousSelection = CreationMenu->Selection;
		CreationMenu->Selection = std::clamp(CreationMenu->Selection, 0,
			std::max(ResultCount - 1, 0));
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && ResultCount > 0)
			CreationMenu->Selection = std::min(CreationMenu->Selection + 1,
				ResultCount - 1);
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && ResultCount > 0)
			CreationMenu->Selection = std::max(CreationMenu->Selection - 1, 0);
		if (SourceType)
			ImGui::TextDisabled("Compatible with %s output", GetProgramTypeName(*SourceType));
		else ImGui::TextDisabled("Add Node");
		ImGui::Separator();

		bool bActivateSelection = bSearchSubmitted;
		if (ImGui::BeginChild("NodeCreationMenuResults",
			{0.0f, -2.0f * ImGui::GetFrameHeightWithSpacing()}))
		{
			if (ResultCount == 0) ImGui::TextDisabled("No matching nodes.");
			std::string PreviousGroup;
			for (size_t EntryIndex = 0; EntryIndex < Results.size(); ++EntryIndex)
			{
				const auto& Action = CachedCreationActions[Results[EntryIndex]];
				const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Action.Payload);
				const std::string Group = CreationMenu->Search.front() != '\0' ? "Results"
					: EntryIndex < CachedCreationMenuRecentCount ? "Recently Used" : Action.Category;
				if (Group != PreviousGroup)
				{
					ImGui::SeparatorText(Group.c_str());
					PreviousGroup = Group;
				}
				ImGui::PushID(static_cast<int>(EntryIndex));
				const std::string Label = !Entry ? Action.Name
					: IsMaterialAdaptiveNumeric(Entry->Opcode) || Entry->Opcode == EMaterialProgramOpcode::AppendVector || Entry->Opcode == EMaterialProgramOpcode::Swizzle
					? std::format("{}  {}", Action.Name, GetCreationShortcutHint(*Entry))
					: std::format("{}  ({})  {}", Action.Name, GetProgramTypeName(Entry->ResultType), GetCreationShortcutHint(*Entry));
				if (ImGui::Selectable(Label.c_str(),
					CreationMenu->Selection == static_cast<int32>(EntryIndex),
					ImGuiSelectableFlags_NoAutoClosePopups))
				{
					CreationMenu->Selection = static_cast<int32>(EntryIndex);
					bActivateSelection = true;
				}
				if (ImGui::IsItemHovered())
				{
					if (const auto* FunctionPath = std::get_if<std::string>(&Action.Payload))
						ImGui::SetTooltip("%s", FunctionPath->c_str());
					else
						ImGui::SetTooltip("%s\n%s", Action.Description.c_str(), Entry ? FormatInputSignature(*Entry).c_str() : Action.Name.c_str());
				}
				if (CreationMenu->Selection == static_cast<int32>(EntryIndex)
					&& PreviousSelection != CreationMenu->Selection)
					ImGui::SetScrollHereY();
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		if (!SourceType)
		{
			if (HasClipboard() && ImGui::Button("Paste"))
			{
				const ImVec2 Position = CreationMenu->GraphPosition;
				PasteNodes(Owner, Transactions, Position, ReportError);
				ResetInteraction();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
			if (HasClipboard()) ImGui::SameLine();
			if (auto* Base = Cast<DMaterial>(&Owner); Base && ImGui::Button("Auto Layout"))
			{
				const auto Layout = FMaterialGraphDocument(*Base).Layout({}, &Transactions);
				ReportCommand(Layout, ReportError);
				ResetInteraction();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
		}
		if (bActivateSelection && !Results.empty())
		{
			const auto& Action = CachedCreationActions[Results[static_cast<size_t>(CreationMenu->Selection)]];
			FMaterialGraphCreationRequest Request{Action,
				static_cast<int32>(std::round(CreationMenu->GraphPosition.x)), static_cast<int32>(std::round(CreationMenu->GraphPosition.y))};
			if (CreationMenu->SourceNode.IsValid()) Request.Source = FMaterialGraphPinAddress::Output(
				{CreationMenu->SourceNode, CreationMenu->SourceOutputIndex, CreationMenu->SourceOutputId});
			const auto Created = GraphDocument.Create(Request, &Transactions);
			ReportCommand(Created, ReportError);
			if (Created)
			{
				SelectedSurfaceOutput.reset();
				if (!Created.GeneratedNodeIds.empty()) SelectedNodes = {Created.GeneratedNodeIds.front()};
				RememberCreation(Action);
				ResetInteraction();
				ImGui::CloseCurrentPopup();
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			ResetInteraction();
			ImGui::CloseCurrentPopup();
		}
		ImGui::TextDisabled("Up/Down navigate   Enter create   Esc close");
		ImGui::EndPopup();
	}

}
