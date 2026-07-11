#include "Panels/WorldOutlinerPanel.h"

#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		auto ClassDisplayName(const DClass* Class) -> std::string
		{
			const std::string Name = Class ? Class->GetName() : std::string();
			const size_t Separator = Name.rfind("::");
			return Separator == std::string::npos ? Name : Name.substr(Separator + 2);
		}

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

		if (Context.Level == nullptr) ImGui::BeginDisabled();
		if (ImGui::Button("Add Actor"))
		{
			ActorTypeSearchText.fill(0);
			ImGui::OpenPopup("Add Actor");
		}
		if (ImGui::BeginPopup("Add Actor"))
		{
			ImGui::InputTextWithHint("###ActorTypeSearch", "Search actor types...", ActorTypeSearchText.data(), ActorTypeSearchText.size());
			for (DClass* Class : GetDerivedClasses(AActor::StaticClass(), true))
			{
				if (!CanConstructObjectOfClass(Class, AActor::StaticClass())) continue;
				const std::string DisplayName = ClassDisplayName(Class);
				if (!ContainsInsensitive(DisplayName, ActorTypeSearchText.data())) continue;
				if (ImGui::Selectable(DisplayName.c_str()))
				{
					AActor* Actor = Context.World->SpawnActor(Class, FName(DisplayName));
					if (Actor) Context.SelectActor(Actor);
					else Context.SetError(std::format("Failed to create actor of class {}.", Class->GetQualifiedName().ToString()));
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}
		if (Context.Level == nullptr) ImGui::EndDisabled();

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("###OutlinerSearch", "Search actors...", SearchText.data(), SearchText.size());
		ImGui::Separator();

		if (Context.Level == nullptr)
		{
			ImGui::TextDisabled("No level is open.");
			ImGui::End();
			return;
		}

		bool bRequestDeleteActor = false;
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
			if (ImGui::BeginPopupContextItem("ActorContext"))
			{
				if (ImGui::MenuItem("Delete Actor")) { PendingDeleteActor = Actor; bRequestDeleteActor = true; }
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}

		const ImGuiIO& IO = ImGui::GetIO();
		if (Context.SelectedActor && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			PendingDeleteActor = Context.SelectedActor;
			bRequestDeleteActor = true;
		}
		if (bRequestDeleteActor) ImGui::OpenPopup("Delete Actor?");
		if (ImGui::BeginPopupModal("Delete Actor?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Delete actor '%s'?", PendingDeleteActor ? PendingDeleteActor->GetName().c_str() : "");
			ImGui::TextDisabled("This action cannot be undone.");
			if (ImGui::Button("Delete"))
			{
				AActor* Actor = PendingDeleteActor.Get();
				if (Actor && Context.World->DestroyActor(Actor)) Context.ClearSelection();
				else if (Actor) Context.SetError("Failed to delete actor.");
				PendingDeleteActor = nullptr;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) { PendingDeleteActor = nullptr; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
		{
			Context.ClearSelection();
		}
		ImGui::End();
	}
} // namespace Durin
