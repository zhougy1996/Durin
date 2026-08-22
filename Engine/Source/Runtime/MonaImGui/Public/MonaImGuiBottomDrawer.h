#pragma once

#include "MonaImGuiAPI.h"
#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Durin::MonaImGui
{
	struct FBottomDrawerConfig;
	struct FBottomDrawerState;
	// Begins a transient overlay window. Call EndBottomDrawer only when this returns true.
	MONAIMGUI_API auto BeginBottomDrawer(
		const FBottomDrawerConfig& Config,
		FBottomDrawerState& State) -> bool;
	MONAIMGUI_API auto EndBottomDrawer(FBottomDrawerState& State) -> void;

	// Retains caller-owned presentation state for one bottom drawer instance.
	struct FBottomDrawerState
	{
		auto Open() -> void
		{
			if (!bOpen) bReceivedFocus = false;
			bOpen = true;
		}
		auto Close() -> void { bOpen = false; }
		auto Toggle() -> void { bOpen ? Close() : Open(); }
		auto IsOpen() const -> bool { return bOpen; }
		auto IsVisible() const -> bool { return Visibility > 0.0f; }
		MONAIMGUI_API auto Reset() -> void;

		float HeightFraction = 0.0f;
		float Visibility = 0.0f;

	private:
		friend MONAIMGUI_API auto BeginBottomDrawer(
			const FBottomDrawerConfig& Config,
			FBottomDrawerState& State) -> bool;
		friend MONAIMGUI_API auto EndBottomDrawer(FBottomDrawerState& State) -> void;

		bool bOpen = false;
		bool bBegun = false;
		bool bReceivedFocus = false;
		int LastDrawFrame = -1;
	};

	// Defines one drawer in screen space; size limits use unscaled design units.
	struct FBottomDrawerConfig
	{
		const char* Id = nullptr;
		ImVec2 AnchorMin{};
		ImVec2 AnchorMax{};
		float InitialHeightFraction = 0.36f;
		float MinimumHeight = 180.0f;
		float MaximumHeight = 720.0f;
		float AnimationDuration = 0.14f;
		bool bAllowResize = true;
		bool bAllowEscapeDismissal = true;
		bool bDismissOnFocusLoss = false;
		bool bDismissWhenDragLeavesBounds = false;
	};

	// Reports the resolved animated drawer rectangle in screen space.
	struct FBottomDrawerGeometry
	{
		ImVec2 Min{};
		ImVec2 Size{};
		float OpenHeight = 0.0f;
		float VisibleHeight = 0.0f;
	};

	MONAIMGUI_API auto AdvanceBottomDrawerAnimation(
		FBottomDrawerState& State,
		float DeltaSeconds,
		float DurationSeconds) -> void;
	MONAIMGUI_API auto ResolveBottomDrawerGeometry(
		const FBottomDrawerConfig& Config,
		const FBottomDrawerState& State) -> FBottomDrawerGeometry;
	MONAIMGUI_API auto ShouldDismissBottomDrawerForDrag(
		const FBottomDrawerConfig& Config,
		const FBottomDrawerGeometry& Geometry,
		bool bDragActive,
		ImVec2 MousePosition) -> bool;
} // namespace Durin::MonaImGui
