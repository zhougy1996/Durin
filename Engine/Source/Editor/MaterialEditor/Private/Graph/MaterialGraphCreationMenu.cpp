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
		auto CreationMenuEntryKey(const FMaterialGraphCatalogEntry& Entry) -> std::string
		{
			return std::format("{}|{}", static_cast<uint32>(Entry.Opcode),
				IsMaterialAdaptiveNumeric(Entry.Opcode)
					? 0u : static_cast<uint32>(Entry.ResultType));
		}
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
	auto FMaterialGraphCanvas::RememberCreation(const FMaterialGraphCatalogEntry& Node) -> void
	{
		const std::string Key = CreationMenuEntryKey(Node);
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
			CachedCreationMenuResults = FMaterialGraphOperations::SearchCatalogIndices(
				Catalog, CreationMenu->Search.data(), SourceType);
			CachedCreationMenuRecentCount = 0;
			if (CreationMenu->Search.front() == '\0')
			{
				// Recent rows are shortcuts to catalog entries, not a replacement category.
				std::vector<size_t> Recent;
				for (const auto& Key : RecentCreationMenuEntries)
				{
					const auto Found = std::ranges::find_if(CachedCreationMenuResults, [&](size_t Index) {
						return CreationMenuEntryKey(Catalog[Index]) == Key;
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
		struct FFunctionEntry { std::string Label; std::string Path; bool bOutput = false; };
		std::vector<FFunctionEntry> Functions;
		if (Cast<DMaterialFunction>(&Owner))
			for (bool bOutput : {false, true})
			{
				const std::string Label = bOutput ? "Function Output" : "Function Input";
				if ((!SourceType || bOutput) && StringUtils::ContainsInsensitive(Label, CreationMenu->Search.data()))
					Functions.push_back({Label, {}, bOutput});
			}
		for (const auto& Path : CreationMenu->FunctionPaths)
			if (StringUtils::ContainsInsensitive(Path, CreationMenu->Search.data()))
				Functions.push_back({Path, Path});
		const auto ResultCount = static_cast<int32>(Results.size() + Functions.size());
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
				const FMaterialGraphCatalogEntry& Entry = Catalog[Results[EntryIndex]];
				const std::string Group = CreationMenu->Search.front() != '\0' ? "Results"
					: EntryIndex < CachedCreationMenuRecentCount ? "Recently Used" : Entry.Category;
				if (Group != PreviousGroup)
				{
					ImGui::SeparatorText(Group.c_str());
					PreviousGroup = Group;
				}
				ImGui::PushID(static_cast<int>(EntryIndex));
				const std::string Label = IsMaterialAdaptiveNumeric(Entry.Opcode) || Entry.Opcode == EMaterialProgramOpcode::AppendVector || Entry.Opcode == EMaterialProgramOpcode::Swizzle
					? std::format("{}  {}", Entry.OperationName, GetCreationShortcutHint(Entry))
					: std::format("{}  ({})  {}", Entry.OperationName,
						GetProgramTypeName(Entry.ResultType), GetCreationShortcutHint(Entry));
				if (ImGui::Selectable(Label.c_str(),
					CreationMenu->Selection == static_cast<int32>(EntryIndex),
					ImGuiSelectableFlags_NoAutoClosePopups))
				{
					CreationMenu->Selection = static_cast<int32>(EntryIndex);
					bActivateSelection = true;
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s\n%s", Entry.Description.c_str(), FormatInputSignature(Entry).c_str());
				if (CreationMenu->Selection == static_cast<int32>(EntryIndex)
					&& PreviousSelection != CreationMenu->Selection)
					ImGui::SetScrollHereY();
				ImGui::PopID();
			}
			if (!Functions.empty()) ImGui::SeparatorText("Material Functions");
			for (size_t Index = 0; Index < Functions.size(); ++Index)
			{
				const auto Selection = static_cast<int32>(Results.size() + Index);
				if (ImGui::Selectable(Functions[Index].Label.c_str(), CreationMenu->Selection == Selection,
					ImGuiSelectableFlags_NoAutoClosePopups))
				{
					CreationMenu->Selection = Selection;
					bActivateSelection = true;
				}
				if (CreationMenu->Selection == Selection && PreviousSelection != Selection) ImGui::SetScrollHereY();
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
				const auto Layout = FMaterialGraphOperations::Layout(*Base, {}, &Transactions);
				ReportCommand(Layout, ReportError);
				ResetInteraction();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
		}
		if (bActivateSelection && ResultCount > 0 && CreationMenu->Selection >= static_cast<int32>(Results.size()))
		{
			const auto& Entry = Functions[CreationMenu->Selection - Results.size()];
			FMaterialGraphDocument Document(Owner);
			const auto X = static_cast<int32>(std::round(CreationMenu->GraphPosition.x));
			const auto Y = static_cast<int32>(std::round(CreationMenu->GraphPosition.y));
			FMaterialGraphCommandResult Created;
			if (Entry.Path.empty())
			{
				const auto& Signature = Cast<DMaterialFunction>(&Owner)->GetFunctionSignature();
				const auto& Ports = Entry.bOutput ? Signature.Outputs : Signature.Inputs;
				FMaterialFunctionPort Port;
				Port.Type = SourceType.value_or(EMaterialProgramValueType::Float);
				for (uint32 Index = 1;; ++Index)
				{
					Port.Name = std::format("{} {}", Entry.bOutput ? "Output" : "Input", Index);
					if (std::ranges::none_of(Ports, [&](const auto& Existing) { return Existing.Name == Port.Name; })) break;
				}
				if (!Entry.bOutput) Port.Default.Kind = EMaterialFunctionDefaultKind::Numeric;
				Created = Document.AddPort(Entry.bOutput, Port,
					{CreationMenu->SourceNode, CreationMenu->SourceOutputIndex, CreationMenu->SourceOutputId}, X, Y, &Transactions);
			}
			else
			{
				FTopLevelAssetPath Path;
				DMaterialFunctionInterface* Function = nullptr;
				if (FTopLevelAssetPath::TryCreate(Entry.Path, Path) && LoadObject(Path, Function))
				{
					std::vector<FMaterialFunctionInputBinding> Inputs;
					if (SourceType)
						for (const auto& Port : Function->GetFunctionSignature().Inputs)
							if (Port.Type == *SourceType)
							{
								Inputs.push_back({Port.Id, Port.Type, {CreationMenu->SourceNode, CreationMenu->SourceOutputIndex, CreationMenu->SourceOutputId}});
								break;
							}
					if (SourceType && Inputs.empty()) Created.Message = "This function has no compatible input.";
					else Created = Document.InsertFunctionCall(*Function, X, Y, Inputs, &Transactions);
				}
				else Created.Message = "Unable to load the material function.";
			}
			ReportCommand(Created, ReportError);
			if (Created)
			{
				SelectedSurfaceOutput.reset();
				SelectedNodes = {Created.GeneratedNodeIds.front()};
				ResetInteraction();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
		}
		else if (bActivateSelection && !Results.empty())
		{
			const FMaterialGraphCatalogEntry& Entry = Catalog[
				Results[static_cast<size_t>(CreationMenu->Selection)]];
			const FMaterialExpressionInput Source = SourceType
				? FMaterialExpressionInput{CreationMenu->SourceNode, CreationMenu->SourceOutputIndex, CreationMenu->SourceOutputId} : FMaterialExpressionInput{};
			const auto Created = FMaterialGraphDocument(Owner).CreateCatalogNode(Entry,
				static_cast<int32>(std::round(CreationMenu->GraphPosition.x)), static_cast<int32>(std::round(CreationMenu->GraphPosition.y)),
				Source, &Transactions);
			ReportCommand(Created, ReportError);
			if (Created)
			{
				SelectedSurfaceOutput.reset();
				if (!Created.GeneratedNodeIds.empty())
					SelectedNodes = {Created.GeneratedNodeIds.front()};
				RememberCreation(Entry);
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
