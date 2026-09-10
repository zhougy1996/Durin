#include "Graph/MaterialGraphCanvas.h"
#include "Graph/MaterialGraphControls.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "Editor/Transaction.h"
#include <cctype>

namespace Durin::Editor::Material
{
	namespace
	{
		auto CreationMenuEntryKey(const FMaterialGraphCatalogEntry& Entry) -> std::string
		{
			return std::format("{}|{}", static_cast<uint32>(Entry.NodeTemplate.Opcode),
				static_cast<uint32>(Entry.NodeTemplate.ResultType));
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
	auto FMaterialGraphCanvas::RememberCreation(const FMaterialProgramNode& Node) -> void
	{
		const std::string Key = std::format("{}|{}", static_cast<uint32>(Node.Opcode),
			static_cast<uint32>(Node.ResultType));
		std::erase(RecentCreationMenuEntries, Key);
		RecentCreationMenuEntries.insert(RecentCreationMenuEntries.begin(), Key);
		if (RecentCreationMenuEntries.size() > 8) RecentCreationMenuEntries.resize(8);
		++RecentCreationMenuRevision;
	}

	auto FMaterialGraphCanvas::DrawCreationMenu(
		DMaterial& Material,
		DTransactor& Transactions,
		const FMaterialGraphView& View,
		const FReportError& ReportError) -> void
	{
		auto* CreationMenu = std::get_if<FNodeCreationMenuInteraction>(&Interaction);
		if (CreationMenu && CreationMenu->bOpenRequested)
		{
			CreationMenu->Search.fill('\0');
			CreationMenu->Selection = 0;
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

		if (CreationMenu->PendingParameter)
		{
			const FMaterialProgramNode Template = *CreationMenu->PendingParameter;
			if (ImGui::Button("Back"))
			{
				CreationMenu->PendingParameter.reset();
				ImGui::EndPopup();
				return;
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(GetProgramTypeName(Template.ResultType));
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##ParameterFilter", "Filter existing parameters...",
				CreationMenu->ParameterFilter.data(), CreationMenu->ParameterFilter.size());
			std::optional<FMaterialProgramNode> Chosen;
			if (ImGui::Button("Create New Parameter")) Chosen = Template;
			ImGui::SeparatorText("Use Existing Parameter");
			if (ImGui::BeginChild("ParameterBindings", {0.0f, -ImGui::GetFrameHeightWithSpacing()}))
			{
				std::string Filter = CreationMenu->ParameterFilter.data();
				std::ranges::transform(Filter, Filter.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
				bool bHasMatch = false;
				for (const auto& Definition : Material.GetParameterDefinitions())
				{
					if (GetProgramType(Definition.Type) != Template.ResultType) continue;
					const std::string Name = Definition.Name.ToString();
					std::string SearchName = Name;
					std::ranges::transform(SearchName, SearchName.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
					if (SearchName.find(Filter) == std::string::npos) continue;
					bHasMatch = true;
					if (ImGui::Selectable(Name.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups))
					{
						Chosen = Template;
						Chosen->ParameterId = Definition.Id;
						Chosen->DisplayName = Definition.DisplayName;
					}
				}
				if (!bHasMatch) ImGui::TextDisabled("No matching parameters.");
			}
			ImGui::EndChild();
			if (Chosen)
			{
				const auto Created = FMaterialGraphOperations::CreateNodeWithDefaultInputs(Material,
					{.Node = *Chosen,
						.X = static_cast<int32>(std::round(CreationMenu->GraphPosition.x)),
						.Y = static_cast<int32>(std::round(CreationMenu->GraphPosition.y))}, {}, &Transactions);
				ReportCommand(Created, ReportError);
				if (Created)
				{
					SelectedNodes = {Created.GeneratedNodeIds.front()};
					RememberCreation(Template);
					ResetInteraction();
					ImGui::CloseCurrentPopup();
				}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				ResetInteraction();
				ImGui::CloseCurrentPopup();
			}
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
			SourceType = Source->Node.ResultType;
		const auto EntryGroup = [this, CreationMenu](size_t Index) -> std::pair<int, std::string> {
			if (CreationMenu->Search.front() == '\0')
			{
				const std::string Key = CreationMenuEntryKey(Catalog[Index]);
				if (FavoriteCreationMenuEntries.contains(Key)) return {0, "Favorites"};
				if (std::ranges::find(RecentCreationMenuEntries, Key) != RecentCreationMenuEntries.end())
					return {1, "Recently Used"};
			}
			return {2, Catalog[Index].Category};
		};
		const bool bCreationMenuResultsStale =
			CachedCreationMenuCatalogRevision != CatalogRevision
			|| CachedFavoriteCreationMenuRevision != FavoriteCreationMenuRevision
			|| CachedRecentCreationMenuRevision != RecentCreationMenuRevision
			|| CachedCreationMenuQuery != CreationMenu->Search.data()
			|| CachedCreationMenuSourceType != SourceType;
		if (bCreationMenuResultsStale)
		{
			CachedCreationMenuResults = FMaterialGraphOperations::SearchCatalogIndices(
				Catalog, CreationMenu->Search.data(), SourceType);
			// Preserve search relevance inside each category without interleaving groups.
			std::ranges::stable_sort(CachedCreationMenuResults,
				[&EntryGroup](size_t A, size_t B) { return EntryGroup(A) < EntryGroup(B); });
			CachedCreationMenuCatalogRevision = CatalogRevision;
			CachedFavoriteCreationMenuRevision = FavoriteCreationMenuRevision;
			CachedRecentCreationMenuRevision = RecentCreationMenuRevision;
			CachedCreationMenuQuery = CreationMenu->Search.data();
			CachedCreationMenuSourceType = SourceType;
		}
		const std::vector<size_t>& Results = CachedCreationMenuResults;

		const int32 PreviousSelection = CreationMenu->Selection;
		CreationMenu->Selection = std::clamp(CreationMenu->Selection, 0,
			std::max(static_cast<int32>(Results.size()) - 1, 0));
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !Results.empty())
			CreationMenu->Selection = std::min(CreationMenu->Selection + 1,
				static_cast<int32>(Results.size()) - 1);
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !Results.empty())
			CreationMenu->Selection = std::max(CreationMenu->Selection - 1, 0);
		if (SourceType)
			ImGui::TextDisabled("Compatible with %s output", GetProgramTypeName(*SourceType));
		else ImGui::TextDisabled("Add Node");
		ImGui::Separator();

		bool bActivateSelection = bSearchSubmitted;
		if (ImGui::BeginChild("NodeCreationMenuResults",
			{0.0f, -2.0f * ImGui::GetFrameHeightWithSpacing()}))
		{
			if (Results.empty()) ImGui::TextDisabled("No matching nodes.");
			std::string PreviousGroup;
			for (size_t EntryIndex = 0; EntryIndex < Results.size(); ++EntryIndex)
			{
				const FMaterialGraphCatalogEntry& Entry = Catalog[Results[EntryIndex]];
				const std::string Group = EntryGroup(Results[EntryIndex]).second;
				if (Group != PreviousGroup)
				{
					ImGui::SeparatorText(Group.c_str());
					PreviousGroup = Group;
				}
				const std::string Key = CreationMenuEntryKey(Entry);
				ImGui::PushID(static_cast<int>(EntryIndex));
				const bool bFavorite = FavoriteCreationMenuEntries.contains(Key);
				if (ImGui::SmallButton(bFavorite ? "*" : "+"))
				{
					if (bFavorite) FavoriteCreationMenuEntries.erase(Key);
					else FavoriteCreationMenuEntries.insert(Key);
					++FavoriteCreationMenuRevision;
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(bFavorite
						? "Remove from favorites" : "Add to favorites");
				ImGui::SameLine();
				const std::string Label = std::format("{}  ({})", Entry.OperationName,
					GetProgramTypeName(Entry.NodeTemplate.ResultType));
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
		}
		ImGui::EndChild();

		if (!SourceType)
		{
			if (HasClipboard() && ImGui::Button("Paste"))
			{
				const ImVec2 Position = CreationMenu->GraphPosition;
				PasteNodes(Material, Transactions, Position, ReportError);
				ResetInteraction();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
			if (HasClipboard()) ImGui::SameLine();
			if (ImGui::Button("Auto Layout"))
			{
				const auto Layout = FMaterialGraphOperations::Layout(Material, {}, &Transactions);
				ReportCommand(Layout, ReportError);
				if (Layout) SurfaceGraphPosition.reset();
				ResetInteraction();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
		}
		if (bActivateSelection && !Results.empty())
		{
			const FMaterialGraphCatalogEntry& Entry = Catalog[
				Results[static_cast<size_t>(CreationMenu->Selection)]];
			if (Entry.NodeTemplate.Opcode == EMaterialProgramOpcode::Parameter
				|| Entry.NodeTemplate.Opcode == EMaterialProgramOpcode::TextureParameter)
			{
				CreationMenu->PendingParameter = Entry.NodeTemplate;
				ImGui::EndPopup();
				return;
			}
			FMaterialProgramNode Candidate = Entry.NodeTemplate;
			if (SourceType) Candidate.Inputs.front() = {CreationMenu->SourceNode, 0};
			const FMaterialGraphCommandResult Created =
				FMaterialGraphOperations::CreateNodeWithDefaultInputs(Material, {
					.Node = std::move(Candidate),
					.X = static_cast<int32>(std::round(CreationMenu->GraphPosition.x)),
					.Y = static_cast<int32>(std::round(CreationMenu->GraphPosition.y)),
				}, Entry.AcceptedInputTypes, &Transactions);
			ReportCommand(Created, ReportError);
			if (Created)
			{
				if (!Created.GeneratedNodeIds.empty())
					SelectedNodes = {Created.GeneratedNodeIds.front()};
				RememberCreation(Entry.NodeTemplate);
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
