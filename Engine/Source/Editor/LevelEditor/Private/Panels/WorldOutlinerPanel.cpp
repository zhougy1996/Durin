#include "Panels/WorldOutlinerPanel.h"

#include "Engine/Actor.h"
#include "Engine/World.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		auto ContainsInsensitive(std::string_view Text, std::string_view Filter) -> bool
		{
			if (Filter.empty())
			{
				return true;
			}
			std::string LowerText(Text);
			std::string LowerFilter(Filter);
			std::ranges::transform(LowerText, LowerText.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			std::ranges::transform(LowerFilter, LowerFilter.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return LowerText.find(LowerFilter) != std::string::npos;
		}
	} // namespace

	auto FWorldOutlinerPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (!ImGui::Begin("World Outliner###WorldOutliner", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("###OutlinerSearch", "Search actors...", SearchText.data(), SearchText.size());
		ImGui::Separator();

		if (Context.World == nullptr)
		{
			ImGui::TextDisabled("No world is loaded.");
			ImGui::End();
			return;
		}

		for (const TObjectPtr<AActor>& ActorPtr : Context.World->GetActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (Actor == nullptr || !ContainsInsensitive(Actor->GetName(), SearchText.data()))
			{
				continue;
			}

			ImGui::PushID(Actor);
			const bool bSelected = Context.SelectedActor.Get() == Actor;
			if (ImGui::Selectable(Actor->GetName().c_str(), bSelected, ImGuiSelectableFlags_SpanAllColumns))
			{
				Context.SelectActor(Actor);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", Actor->GetClass()->GetName().c_str());
			}
			ImGui::PopID();
		}

		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
		{
			Context.ClearSelection();
		}
		ImGui::End();
	}
} // namespace Durin
