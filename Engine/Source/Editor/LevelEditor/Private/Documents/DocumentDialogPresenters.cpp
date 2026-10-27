#include "Documents/DocumentDialogPresenters.h"

#include "MonaImGui.h"

namespace Durin::Editor::Level
{
	auto FLevelLoadingDialogPresenter::Draw(
		std::string_view LevelPath, const std::optional<FSceneLoadProgress>& Progress) -> bool
	{
		constexpr const char* Title = "Loading Level###Durin.LoadingLevel";
		if (Progress && !ImGui::IsPopupOpen(Title)) ImGui::OpenPopup(Title);
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(MonaImGui::ScaleUI(440.0f), 0.0f),
			ImVec2(MonaImGui::ScaleUI(640.0f), std::numeric_limits<float>::max()));
		if (!ImGui::BeginPopupModal(Title, nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return false;
		bool bCancel = false;
		if (!Progress)
			ImGui::CloseCurrentPopup();
		else
		{
			ImGui::TextWrapped("%.*s", static_cast<int>(LevelPath.size()), LevelPath.data());
			const auto Label = Progress->Phase == ESceneLoadPhase::Ready
				? std::string_view("Activating level") : GetSceneLoadPhaseLabel(Progress->Phase);
			ImGui::TextUnformatted(Label.data(), Label.data() + Label.size());
			// Work quantities are not a completion fraction; show an indeterminate animation.
			ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f), "");
			ImGui::Text("Elapsed: %.1f s", Progress->ElapsedSeconds);
			if (Progress->bProductsKnown)
				ImGui::Text("Meshes: %llu  Materials: %llu  Shaders preparing: %llu",
					static_cast<unsigned long long>(Progress->Meshes), static_cast<unsigned long long>(Progress->Materials),
					static_cast<unsigned long long>(Progress->ProgramsInFlight));
			ImGui::TextUnformatted("Editing is paused until the level is ready.");
			bCancel = ImGui::Button("Cancel");
		}
		ImGui::EndPopup();
		return bCancel;
	}

	auto FUnsavedLevelDialogPresenter::Draw(bool bRequestOpen, const FResolve& Resolve)
		-> std::optional<EUnsavedLevelDialogDecision>
	{
		if (bRequestOpen) ImGui::OpenPopup("Unsaved Level");

		if (!ImGui::BeginPopupModal(
			"Unsaved Level",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return std::nullopt;

		std::optional<EUnsavedLevelDialogDecision> Decision;
		ImGui::TextUnformatted("The current level has unsaved changes.");
		if (ImGui::Button("Save")) Decision = EUnsavedLevelDialogDecision::Save;
		ImGui::SameLine();
		if (ImGui::Button("Discard")) Decision = EUnsavedLevelDialogDecision::Discard;
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) Decision = EUnsavedLevelDialogDecision::Cancel;

		if (Decision && (!Resolve || Resolve(*Decision))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return Decision;
	}
} // namespace Durin::Editor::Level
