#include "Framework/TabManager.h"
#include "Framework/TabLayout.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitHost.h"
#include "ThirdParty/ImGui/imgui_internal.h"

namespace Durin
{
	auto FEditorTabWindow::Begin(const FEditorTabWindowConfig& Config) -> FEditorTabWindowState
	{
		checkf(!bWindowBegun, "An editor workspace root window must be ended before it begins again");
		auto WindowClass = TabLayout::MakeRootWindowClass();
		if (Config.OpenLocation == EAssetEditorOpenLocation::NewWindow) WindowClass.ViewportFlagsOverrideSet |= ImGuiViewportFlags_NoAutoMerge;
		ImGui::SetNextWindowClass(&WindowClass);
		// Opening location selects initial placement; subsequent docking belongs to the user.
		ImGui::SetNextWindowDockID(Config.OpenLocation == EAssetEditorOpenLocation::NewWindow ? 0
			: TabLayout::MakeMainFrameDockSpaceId(TabLayout::MainFrameLayoutVersion), ImGuiCond_FirstUseEver);
		if (bFocusRequested)
		{
			ImGui::SetNextWindowFocus();
			bFocusRequested = false;
		}

		const std::string DisplayName = Config.bDirty ? std::format("{} *", Config.DisplayName) : std::string(Config.DisplayName);
		const std::string RootWindowName = TabLayout::MakeRootWindowName(DisplayName, Config.RootKey);
		// Nested dock layouts need usable geometry on their first frame.
		if (!(ImGui::GetCurrentContext()->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize))
			ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(1200.0f), MonaImGui::ScaleUI(800.0f)), ImGuiCond_FirstUseEver);
		if (Config.bZeroPadding) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool bOpen = true;
		const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoCollapse | Config.AdditionalFlags |
			(Config.bDirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None);
		FEditorTabWindowState State;
		State.bVisible = ImGui::Begin(RootWindowName.c_str(), &bOpen, Flags);
		bWindowBegun = true;
		if (Config.bZeroPadding) ImGui::PopStyleVar();
		State.bFocused = State.bVisible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const ImGuiWindow* Window = ImGui::GetCurrentWindow();
		// Begin() may return true one frame before a queued dock-tab selection becomes visible.
		// Use the dock state directly so document activation follows the tab the user actually sees.
		const bool bDockTabSelected = Window->DockIsActive && Window->DockTabIsVisible;
		// A selected dock tab may not transfer focus to one of its child controls.
		State.bActivated = bDockTabSelected && !bWasDockTabSelected;
		bWasDockTabSelected = bDockTabSelected;
		State.bCloseRequested = !bOpen;

		if (!State.bVisible && Config.InternalDockSpace &&
			ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(
				Config.InternalDockSpace->ToolkitId,
				Config.InternalDockSpace->LayoutVersion
			)) != nullptr)
		{
			TabLayout::SubmitDockSpace(
				Config.InternalDockSpace->ToolkitId,
				Config.InternalDockSpace->LayoutVersion,
				ImVec2(0.0f, 0.0f),
				ImGuiDockNodeFlags_KeepAliveOnly
			);
		}
		return State;
	}

	auto FEditorTabWindow::End() -> void
	{
		checkf(bWindowBegun, "An editor workspace root window must begin before it ends");
		ImGui::End();
		bWindowBegun = false;
	}

	auto FTabManager::RequestFocus(DObject& Asset) -> void
	{
		SelectAsset(FObjectKey(&Asset));
		SessionWindow.RequestFocus();
	}

	auto FTabManager::DrawAssets(FAssetEditorToolkit& Toolkit, std::string_view RootKey,
		const std::function<void(DObject&)>& DrawAsset,
		const std::function<void(DObject&, bool)>& UpdateVisibility) -> bool
	{
		const auto Assets = Toolkit.GetEditingObjects();
		if (Assets.empty()) return false;
		const auto Host = Toolkit.GetToolkitHost();
		const auto Label = Assets.size() == 1 ? Toolkit.GetEditorDisplayName()
			: std::format("{} ({} assets)", Toolkit.GetEditorName().ToString(), Assets.size());
		const auto Window = SessionWindow.Begin({.DisplayName = Label, .RootKey = RootKey,
			.bDirty = std::ranges::any_of(Assets, [&](auto* Asset) { return Toolkit.IsAssetDirty(*Asset); }),
			.OpenLocation = Toolkit.GetOpenLocation(), .AdditionalFlags = ImGuiWindowFlags_NoSavedSettings});
		if (Host) Host->OnRootWindowShown(ImGui::GetWindowViewport()->ID);
		std::vector<FObjectKey> CloseRequests;
		if (Window.bVisible && Assets.size() > 1 && ImGui::BeginTabBar("Assets"))
		{
			const auto Selection = std::exchange(FocusedAsset, {});
			for (auto* Asset : Assets)
			{
				const FObjectKey Key(Asset);
				bool bOpen = true;
				const auto Flags = Key == Selection ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
				const auto Name = std::format("{}###Asset.{}", Asset->GetName(), FObjectKeyHash{}(Key));
				if (ImGui::BeginTabItem(Name.c_str(), &bOpen, Flags))
				{
					auto* Active = Toolkit.GetActiveAsset();
					if (Asset != Active && !Toolkit.ActivateAsset(*Asset) && Toolkit.IsEditorOpen())
						SelectAsset(FObjectKey(Active));
					ImGui::EndTabItem();
				}
				if (!bOpen) CloseRequests.push_back(Key);
			}
			ImGui::EndTabBar();
		}
		auto* Active = Toolkit.GetActiveAsset();
		if (UpdateVisibility)
			for (auto* Asset : Assets)
				if (Toolkit.IsEditorOpen()) UpdateVisibility(*Asset, Asset == Active && Window.bVisible);
		if (Window.bVisible && Active && Toolkit.IsEditorOpen())
		{
			DrawAsset(*Active);
			if (Toolkit.IsEditorOpen())
				if (auto* Base = dynamic_cast<FBaseAssetToolkit*>(&Toolkit)) Base->DrawRegisteredUI();
		}
		SessionWindow.End();
		for (const auto Key : CloseRequests)
			if (auto* Asset = Key.ResolveObjectPtr()) Toolkit.RequestCloseAsset(*Asset);
		if (Window.bCloseRequested) Toolkit.CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
		return Window.bFocused || Window.bActivated;
	}
}
