#include "MonaImGuiBottomDrawer.h"

#include "MonaImGui.h"

namespace Durin::MonaImGui
{
	namespace
	{
		auto SmoothStep(float Value) -> float
		{
			const float Clamped = std::clamp(Value, 0.0f, 1.0f);
			return Clamped * Clamped * (3.0f - 2.0f * Clamped);
		}
	} // namespace

	auto FBottomDrawerState::Reset() -> void
	{
		checkf(!bBegun, "A bottom drawer cannot reset between BeginBottomDrawer and EndBottomDrawer");
		HeightFraction = 0.0f;
		Visibility = 0.0f;
		bOpen = false;
		bReceivedFocus = false;
		LastDrawFrame = -1;
	}

	auto AdvanceBottomDrawerAnimation(
		FBottomDrawerState& State,
		float DeltaSeconds,
		float DurationSeconds) -> void
	{
		const float Target = State.IsOpen() ? 1.0f : 0.0f;
		if (!std::isfinite(DeltaSeconds) || !std::isfinite(DurationSeconds)
			|| DeltaSeconds <= 0.0f || DurationSeconds <= 0.0f)
		{
			State.Visibility = Target;
			return;
		}
		const float Step = DeltaSeconds / DurationSeconds;
		State.Visibility = Target > State.Visibility
			? std::min(Target, State.Visibility + Step)
			: std::max(Target, State.Visibility - Step);
	}

	auto ResolveBottomDrawerGeometry(
		const FBottomDrawerConfig& Config,
		const FBottomDrawerState& State) -> FBottomDrawerGeometry
	{
		FBottomDrawerGeometry Geometry;
		const float AnchorWidth = std::max(0.0f, Config.AnchorMax.x - Config.AnchorMin.x);
		const float AnchorHeight = std::max(0.0f, Config.AnchorMax.y - Config.AnchorMin.y);
		if (AnchorWidth <= 0.0f || AnchorHeight <= 0.0f) return Geometry;

		const float MinimumHeight = std::min(AnchorHeight, std::max(1.0f, ScaleUI(Config.MinimumHeight)));
		const float MaximumHeight = std::clamp(ScaleUI(Config.MaximumHeight), MinimumHeight, AnchorHeight);
		const float RequestedFraction = std::isfinite(State.HeightFraction)
			&& State.HeightFraction > 0.0f
			? State.HeightFraction : Config.InitialHeightFraction;
		Geometry.OpenHeight = std::clamp(AnchorHeight * RequestedFraction, MinimumHeight, MaximumHeight);
		Geometry.VisibleHeight = Geometry.OpenHeight * SmoothStep(State.Visibility);
		Geometry.Min = ImVec2(Config.AnchorMin.x, Config.AnchorMax.y - Geometry.VisibleHeight);
		Geometry.Size = ImVec2(AnchorWidth, Geometry.VisibleHeight);
		return Geometry;
	}

	auto ShouldDismissBottomDrawerForDrag(
		const FBottomDrawerConfig& Config,
		const FBottomDrawerGeometry& Geometry,
		bool bDragActive,
		ImVec2 MousePosition) -> bool
	{
		if (!Config.bDismissWhenDragLeavesBounds || !bDragActive) return false;
		return MousePosition.x < Geometry.Min.x
			|| MousePosition.y < Geometry.Min.y
			|| MousePosition.x >= Geometry.Min.x + Geometry.Size.x
			|| MousePosition.y >= Geometry.Min.y + Geometry.Size.y;
	}

	auto BeginBottomDrawer(
		const FBottomDrawerConfig& Config,
		FBottomDrawerState& State) -> bool
	{
		checkf(!State.bBegun, "A bottom drawer must end before it begins again");
		checkf(Config.Id != nullptr && Config.Id[0] != '\0', "A bottom drawer requires a stable ImGui ID");
		checkf(std::isfinite(Config.AnchorMin.x) && std::isfinite(Config.AnchorMin.y)
			&& std::isfinite(Config.AnchorMax.x) && std::isfinite(Config.AnchorMax.y),
			"A bottom drawer requires finite anchor bounds");
		AdvanceBottomDrawerAnimation(State, ImGui::GetIO().DeltaTime, Config.AnimationDuration);
		if (!State.IsVisible()) return false;

		const int Frame = ImGui::GetFrameCount();
		checkf(State.LastDrawFrame != Frame, "A bottom drawer state can be drawn only once per frame");
		State.LastDrawFrame = Frame;
		const FBottomDrawerGeometry Geometry = ResolveBottomDrawerGeometry(Config, State);
		if (Geometry.Size.x <= 0.0f || Geometry.Size.y <= 0.0f) return false;
		if (ShouldDismissBottomDrawerForDrag(
			Config, Geometry, ImGui::GetDragDropPayload() != nullptr, ImGui::GetMousePos()))
			State.Close();

		ImGui::SetNextWindowPos(Geometry.Min, ImGuiCond_Always);
		ImGui::SetNextWindowSize(Geometry.Size, ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoSavedSettings;
		const bool bVisible = ImGui::Begin(Config.Id, nullptr, Flags);
		ImGui::PopStyleVar(2);
		if (!bVisible)
		{
			ImGui::End();
			return false;
		}
		State.bBegun = true;
		const bool bFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (bFocused) State.bReceivedFocus = true;
		else if (Config.bDismissOnFocusLoss && State.bReceivedFocus
			&& !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()
			&& ImGui::GetDragDropPayload() == nullptr
			&& !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
		{
			State.Close();
		}

		if (Config.bAllowResize)
		{
			const float ResizeHeight = std::max(ScaleUI(5.0f), ImGui::GetStyle().SeparatorSize);
			ImGui::SetCursorScreenPos(ImGui::GetWindowPos());
			ImGui::InvisibleButton("##BottomDrawerResize", ImVec2(ImGui::GetWindowWidth(), ResizeHeight));
			if (ImGui::IsItemHovered() || ImGui::IsItemActive())
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
			if (ImGui::IsItemActive())
			{
				const float AnchorHeight = Config.AnchorMax.y - Config.AnchorMin.y;
				if (AnchorHeight > 0.0f)
				{
					if (State.HeightFraction <= 0.0f)
						State.HeightFraction = Geometry.OpenHeight / AnchorHeight;
					State.HeightFraction = std::clamp(
						State.HeightFraction - ImGui::GetIO().MouseDelta.y / AnchorHeight,
						0.0f, 1.0f);
				}
			}
			const ImU32 ResizeColor = ImGui::GetColorU32(
				ImGui::IsItemHovered() || ImGui::IsItemActive()
					? ImGuiCol_SeparatorHovered : ImGuiCol_Separator);
			ImGui::GetWindowDrawList()->AddLine(
				ImGui::GetItemRectMin(),
				ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y),
				ResizeColor);
			ImGui::SetCursorScreenPos(ImGui::GetWindowPos()
				+ ImVec2(ImGui::GetStyle().WindowPadding.x,
					ResizeHeight + ImGui::GetStyle().WindowPadding.y));
		}

		if (Config.bAllowEscapeDismissal
			&& ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& ImGui::IsKeyPressed(ImGuiKey_Escape, false)
			&& !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()
			&& ImGui::GetDragDropPayload() == nullptr
			&& !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
		{
			State.Close();
		}
		return true;
	}

	auto EndBottomDrawer(FBottomDrawerState& State) -> void
	{
		checkf(State.bBegun, "EndBottomDrawer requires a matching successful BeginBottomDrawer");
		ImGui::End();
		State.bBegun = false;
	}
} // namespace Durin::MonaImGui
