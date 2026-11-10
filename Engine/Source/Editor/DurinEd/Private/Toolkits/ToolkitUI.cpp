#include "Toolkits/AssetEditorToolkit.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"
#include "Toolkits/AssetEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/TabLayout.h"
#include "Misc/Paths.h"
#include "Yaml/Yaml.h"
#include "Misc/FileHelper.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorEngine.h"

namespace Durin
{
	struct FToolkitUIState
	{
		struct FEntry { uint64 Generation; FToolkitUIExtension Extension; };
		std::vector<FEntry> Entries;
		std::unordered_map<std::string, bool> Visibility;
		std::string FilePath;
		mutable std::unordered_set<std::string> DirtyPanels;
		std::unordered_map<std::string, std::string> DockSignatures;
		std::unordered_set<ImGuiID> RuntimeDockSpaces;
		uint64 NextGeneration = 1;
		bool bRetired = false;
		bool bRestored = false;
		auto Save() const -> bool
		{
			if (FilePath.empty()) return true;
			FYamlDocument Document;
			if (std::filesystem::exists(FilePath) && !Document.LoadFromFile(FilePath)) return false;
			auto Panels = Document.GetMutableRoot().GetRef("Panels");
			if (!Panels.IsMap()) Panels = Document.GetMutableRoot().AddMap("Panels");
			for (const auto& [Id, Open] : Visibility) if (DirtyPanels.contains(Id) || !Panels.GetView(Id).IsValid()) Panels.SetChildValue(Id, Open);
			std::error_code Error;
			std::filesystem::create_directories(std::filesystem::path(FilePath).parent_path(), Error);
			const auto Text = Document.ToString();
			if (Error || !FFileHelper::SaveArrayToFileAtomically(std::as_bytes(std::span(Text.data(), Text.size())), std::filesystem::path(FilePath))) return false;
			DirtyPanels.clear();
			return true;
		}
	};

	FToolkitUIRegistration::~FToolkitUIRegistration() { Reset(); }
	FToolkitUIRegistration::FToolkitUIRegistration(FToolkitUIRegistration&& Other) noexcept : Release(std::move(Other.Release)) { Other.Release = {}; }
	auto FToolkitUIRegistration::operator=(FToolkitUIRegistration&& Other) noexcept -> FToolkitUIRegistration&
	{ if (this != &Other) { Reset(); Release = std::move(Other.Release); Other.Release = {}; } return *this; }
	auto FToolkitUIRegistration::Reset() -> void { if (auto Callback = std::exchange(Release, {})) Callback(); }
	FBaseAssetToolkit::FBaseAssetToolkit(bool Viewport, bool Details)
		: bCreateViewport(Viewport), bCreateDetails(Details), UI(std::make_shared<FToolkitUIState>()) {}
	FBaseAssetToolkit::~FBaseAssetToolkit() = default;
	auto FBaseAssetToolkit::OnSessionBound() -> void
	{
		if (DefaultPanels) { RestorePanelLayout(); return; }
		FToolkitUIExtension Defaults;
		if (bCreateViewport)
		{
			auto Panel = CreateDefaultViewportPanel();
			requiref(Panel.has_value(), "An enabled default viewport requires a panel factory");
			Defaults.Panels.push_back(std::move(*Panel));
		}
		if (bCreateDetails)
		{
			auto Panel = CreateDefaultDetailsPanel();
			requiref(Panel.has_value(), "Enabled default details require a panel factory");
			Defaults.Panels.push_back(std::move(*Panel));
		}
		if (!Defaults.Panels.empty())
		{
			DefaultPanels = RegisterUIExtension(std::move(Defaults));
			requiref(static_cast<bool>(DefaultPanels), "Default panel identities must be unique within their toolkit");
		}
		RestorePanelLayout();
	}

	auto FBaseAssetToolkit::RegisterUIExtension(FToolkitUIExtension Extension) -> FToolkitUIRegistration
	{
		FToolkitUIRegistration Result;
		const auto* Owner = GetOwningAssetEditor();
		if (UI->bRetired || IsRetired() || (Owner ? Owner->IsClosed() : !IsSessionActive())) return Result;
		std::unordered_set<std::string> Panels, Commands;
		for (const auto& Entry : UI->Entries)
		{
			for (const auto& Panel : Entry.Extension.Panels) Panels.insert(Panel.Id);
			for (const auto& Command : Entry.Extension.Commands) Commands.insert(Command.Id);
		}
		for (const auto& Panel : Extension.Panels)
			if (Panel.Id.empty() || Panel.Label.empty() || !Panel.Draw || !Panels.insert(Panel.Id).second) return Result;
		for (const auto& Command : Extension.Commands)
			if (Command.Id.empty() || Command.Label.empty() || !Command.Execute || !Commands.insert(Command.Id).second) return Result;
		if (Extension.Panels.empty() && Extension.Commands.empty()) return Result;
		const auto Generation = UI->NextGeneration++;
		for (const auto& Panel : Extension.Panels) UI->Visibility.try_emplace(Panel.Id, Panel.bInitiallyOpen);
		UI->Entries.push_back({Generation, std::move(Extension)});
		if (UI->bRestored) RestorePanelLayout(UI->FilePath);
		Result.Release = [Weak = std::weak_ptr(UI), Generation] {
			if (const auto State = Weak.lock()) std::erase_if(State->Entries, [&](const auto& Entry) { return Entry.Generation == Generation; });
		};
		return Result;
	}
	auto FBaseAssetToolkit::CanInvokeCommand(std::string_view Id) const -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (UI->bRetired || !IsSessionActive()) return false;
		const auto* Owner = GetOwningAssetEditor();
		if (!Owner || !Owner->GetAssetEditorSubsystem() || Owner->GetAssetEditorSubsystem()->GetActiveAssetToolkit().get() != this) return false;
		for (const auto& Entry : UI->Entries)
			for (const auto& Command : Entry.Extension.Commands)
				if (Command.Id == Id)
				{
					const auto Predicate = Command.CanExecute;
					return (!Command.bMutating || !IsReadOnly()) && (!Predicate || Predicate());
				}
		return false;
	}
	auto FBaseAssetToolkit::InvokeCommand(std::string_view Id) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const auto Lease = weak_from_this().lock();
		auto* Owner = GetOwningAssetEditor();
		if (!Owner || !Lease) return false;
		const FStrongObjectPtr KeepAlive(Owner);
		bool Executed = false;
		if (auto* Service = Owner->GetAssetEditorSubsystem())
			Service->DispatchEditorCallback([&] {
				if (!CanInvokeCommand(Id) || UI->bRetired || !IsSessionActive()
					|| Service->GetActiveAssetToolkit().get() != this) return;
				for (const auto& Entry : UI->Entries)
					for (const auto& Command : Entry.Extension.Commands)
						if (Command.Id == Id) { const auto Execute = Command.Execute; Execute(); Executed = true; return; }
			});
		return Executed;
	}
	auto FBaseAssetToolkit::SetPanelOpen(std::string_view Id, bool Open) -> bool
	{
		if (UI->bRetired) return false;
		for (const auto& Entry : UI->Entries)
			for (const auto& Panel : Entry.Extension.Panels)
				if (Panel.Id == Id) { UI->Visibility[Panel.Id] = Open; UI->DirtyPanels.insert(Panel.Id); return UI->Save(); }
		return false;
	}
	auto FBaseAssetToolkit::IsPanelOpen(std::string_view Id) const -> bool
	{
		const auto It = UI->Visibility.find(std::string(Id));
		return !UI->bRetired && It != UI->Visibility.end() && It->second;
	}
	auto FBaseAssetToolkit::ResetPanels() -> void
	{
		for (const auto& Entry : UI->Entries)
			for (const auto& Panel : Entry.Extension.Panels) { UI->Visibility[Panel.Id] = Panel.bInitiallyOpen; UI->DirtyPanels.insert(Panel.Id); }
		UI->Save();
		if (ImGui::GetCurrentContext())
			if (const auto* Owner = GetOwningAssetEditor())
				ImGui::DockBuilderRemoveNode(TabLayout::MakeDockSpaceId(FAssetEditorToolkitId(GetPresentationRootKey(GetToolkitId().GetValue())), 1));
	}
	auto FBaseAssetToolkit::RestorePanelLayout(std::string FilePath) -> bool
	{
		if (FilePath.empty())
		{
			const auto* Owner = GetOwningAssetEditor();
			if (!Owner) return false;
			FilePath = FPaths::LaunchConfigsDir() + "AssetEditorPanels/" + FXxHash128::HashBuffer(GetLayoutProfileKey()).ToString() + ".yaml";
		}
		UI->FilePath = std::move(FilePath);
		UI->bRestored = true;
		if (!std::filesystem::exists(UI->FilePath)) return true;
		FYamlDocument Document;
		if (!Document.LoadFromFile(UI->FilePath)) return false;
		const auto Panels = Document.GetRootView().GetView("Panels");
		for (auto& [Id, Open] : UI->Visibility) Open = Panels.GetView(Id).GetBool(Open);
		return true;
	}
	auto FBaseAssetToolkit::DrawWindowMenu() -> void
	{
		const auto Snapshot = UI->Entries;
		for (const auto& Entry : Snapshot)
			for (const auto& Panel : Entry.Extension.Panels)
				if (ImGui::MenuItem(Panel.Label.c_str(), nullptr, IsPanelOpen(Panel.Id))) SetPanelOpen(Panel.Id, !IsPanelOpen(Panel.Id));
	}
	auto FBaseAssetToolkit::DrawRegisteredUI() -> void
	{
		if (UI->bRetired || !IsSessionActive() || UI->Entries.empty()) return;
		if (!UI->bRestored) RestorePanelLayout();
		const auto Snapshot = UI->Entries;
		auto StillAttached = [&](uint64 Generation) { return !UI->bRetired && std::ranges::any_of(UI->Entries, [&](const auto& Entry) { return Entry.Generation == Generation; }); };
		for (const auto& Entry : Snapshot)
			for (const auto& Command : Entry.Extension.Commands)
				if (StillAttached(Entry.Generation) && Command.bToolbar)
				{
					ImGui::BeginDisabled(!CanInvokeCommand(Command.Id));
					if (ImGui::Button(Command.Label.c_str())) InvokeCommand(Command.Id);
					ImGui::EndDisabled(); ImGui::SameLine();
				}
		ImGui::NewLine();
		const auto* Owner = GetOwningAssetEditor();
		if (!Owner || UI->bRetired) return;
		const FAssetEditorToolkitId LayoutId(GetPresentationRootKey(GetToolkitId().GetValue()));
		std::vector<std::string_view> PanelIds;
		for (const auto& Entry : Snapshot) for (const auto& Panel : Entry.Extension.Panels) PanelIds.push_back(Panel.Id);
		RestoreDockLayout("Extensions", LayoutId, 1, PanelIds);
		const auto DockId = TabLayout::SubmitDockSpace(LayoutId, 1, ImVec2(0, 0));
		for (const auto& Entry : Snapshot)
			for (const auto& Panel : Entry.Extension.Panels)
				if (StillAttached(Entry.Generation) && IsPanelOpen(Panel.Id))
				{
					ImGui::SetNextWindowDockID(DockId, ImGuiCond_FirstUseEver);
					bool Open = true;
					if (TabLayout::BeginDockablePanel(LayoutId, Panel.Label, Panel.Id, &Open, ImGuiWindowFlags_NoSavedSettings)) Panel.Draw();
					ImGui::End();
					if (!Open && StillAttached(Entry.Generation)) SetPanelOpen(Panel.Id, false);
				}
		SaveDockLayout("Extensions", LayoutId, 1, PanelIds);
	}
	namespace
	{
		auto CopyDockTemplate(const FAssetEditorToolkitId& Source, const FAssetEditorToolkitId& Destination,
			uint32 Version, std::span<const std::string_view> Panels, bool RuntimeDestination) -> void
		{
			std::vector<std::string> Names;
			Names.reserve(Panels.size() * 2);
			for (const auto Panel : Panels)
			{
				Names.push_back(TabLayout::MakePanelWindowName("", Source, Panel));
				Names.push_back(TabLayout::MakePanelWindowName("", Destination, Panel));
			}
			ImVector<const char*> Remap;
			for (const auto& Name : Names) Remap.push_back(Name.c_str());
			ImGui::DockBuilderCopyDockSpace(TabLayout::MakeDockSpaceId(Source, Version), TabLayout::MakeDockSpaceId(Destination, Version), &Remap);
			const auto SetClass = [&](auto&& Self, ImGuiDockNode* Node) -> void {
				if (!Node) return;
				Node->WindowClass = TabLayout::MakeWindowClass(Destination);
				Self(Self, Node->ChildNodes[0]); Self(Self, Node->ChildNodes[1]);
			};
			SetClass(SetClass, ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(Destination, Version)));
			if (RuntimeDestination)
				for (size_t I = 1; I < Names.size(); I += 2)
					if (auto* Settings = ImGui::FindWindowSettingsByID(ImHashStr(Names[I].c_str()))) Settings->WantDelete = true;
		}
	}
	auto FBaseAssetToolkit::RestoreDockLayout(std::string_view Slot, const FAssetEditorToolkitId& DockType,
		uint32 Version, std::span<const std::string_view> Panels) -> void
	{
		if (!ImGui::GetCurrentContext() || !IsSessionActive()) return;
		UI->RuntimeDockSpaces.insert(TabLayout::MakeDockSpaceId(DockType, Version));
		const FAssetEditorToolkitId Profile(GetLayoutProfileKey() + "." + std::string(Slot));
		if (!ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(Profile, Version))) return;
		if (!ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Version))) CopyDockTemplate(Profile, DockType, Version, Panels, true);
		TabLayout::SubmitDockSpace(Profile, Version, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
	}
	auto FBaseAssetToolkit::SaveDockLayout(std::string_view Slot, const FAssetEditorToolkitId& DockType,
		uint32 Version, std::span<const std::string_view> Panels) -> void
	{
		const auto* Owner = GetOwningAssetEditor();
		if (!ImGui::GetCurrentContext() || !IsSessionActive() || !Owner
			|| Owner->GetAssetEditorSubsystem()->GetActiveAssetToolkit().get() != this) return;
		const auto* Root = ImGui::DockBuilderGetNode(TabLayout::MakeDockSpaceId(DockType, Version));
		if (!Root) return;
		// Cache only live changes. The named template is serialized by ImGui's existing handler.
		std::string Signature;
		const auto Visit = [&](auto&& Self, const ImGuiDockNode* Node) -> void {
			if (!Node) return;
			Signature += std::format("{}:{}:{}:{};", Node->ID, Node->SizeRef.x, Node->SizeRef.y, Node->LocalFlags);
			Self(Self, Node->ChildNodes[0]); Self(Self, Node->ChildNodes[1]);
		};
		Visit(Visit, Root);
		for (auto Panel : Panels)
			if (const auto* Window = ImGui::FindWindowByName(TabLayout::MakePanelWindowName("", DockType, Panel).c_str()))
				Signature += std::format("{}:{}:{}:{}:{}:{};", Panel, Window->DockId, Window->Pos.x, Window->Pos.y, Window->Size.x, Window->Size.y);
		const FAssetEditorToolkitId Profile(GetLayoutProfileKey() + "." + std::string(Slot));
		auto& Last = UI->DockSignatures[std::string(Slot)];
		if (Last != Signature)
		{
			CopyDockTemplate(DockType, Profile, Version, Panels, false);
			Last = std::move(Signature);
			ImGui::MarkIniSettingsDirty();
		}
		TabLayout::SubmitDockSpace(Profile, Version, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
	}
	auto FBaseAssetToolkit::ReleaseSessionUI() -> void
	{
		UI->bRetired = true; UI->Entries.clear();
		if (ImGui::GetCurrentContext()) for (auto Id : UI->RuntimeDockSpaces) ImGui::DockBuilderRemoveNode(Id);
		UI->RuntimeDockSpaces.clear();
	}
	auto FBaseAssetToolkit::OnHostChanged() -> void
	{
		UI->Save();
		if (ImGui::GetCurrentContext()) for (auto Id : UI->RuntimeDockSpaces) ImGui::DockBuilderRemoveNode(Id);
		UI->RuntimeDockSpaces.clear();
		UI->FilePath.clear();
		UI->DockSignatures.clear();
		UI->bRestored = false;
		for (const auto& Entry : UI->Entries)
			for (const auto& Panel : Entry.Extension.Panels) UI->Visibility[Panel.Id] = Panel.bInitiallyOpen;
	}
}
