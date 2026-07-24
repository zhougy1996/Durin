#include "Panels/ContentBrowserPanel.h"

#include "AssetSystem.h"
#include "Assets/ContentBrowserDragDrop.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Engine/Level.h"
#include "Icons/FontAwesomeIcons.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorHelpers.h"
#include "Workspace/LevelEditorUILayout.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Misc/Paths.h"
#include "Misc/LexicalPath.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Math/Vector.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Assets/SourceImageThumbnailCache.h"
#include "Assets/SourceImageThumbnailDecoder.h"
#include "Texture/Texture2D.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::DrawToolbarIconButton;
		using StringUtils::ContainsInsensitive;

		auto NormalizePath(std::string_view Path) -> std::string
		{
			if (Path.empty()) return {};
			return std::filesystem::absolute(std::filesystem::path(Path)).lexically_normal().generic_string();
		}

		auto ClassLeaf(std::string_view QualifiedName) -> std::string
		{
			const size_t Separator = QualifiedName.rfind("::");
			std::string Name = Separator == std::string_view::npos ? std::string(QualifiedName) : std::string(QualifiedName.substr(Separator + 2));
			if (Name.starts_with('D') && Name.size() > 1) Name.erase(Name.begin());
			return Name;
		}

		auto FindTextureSourceFile(const std::filesystem::path& PackagePath) -> std::filesystem::path
		{
			std::error_code Ec;
			for (std::filesystem::directory_iterator It(PackagePath.parent_path(), std::filesystem::directory_options::skip_permission_denied, Ec), End; !Ec && It != End; It.increment(Ec))
			{
				if (!It->is_regular_file(Ec) || It->path().stem() != PackagePath.stem()) continue;
				if (IsSupportedSourceImageExtension(It->path().extension().generic_string())) return It->path();
			}
			return {};
		}

		constexpr ImGuiTableFlags DetailsTableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings;

		constexpr float FullToolbarWidth = 900.0f;
		constexpr float CompactToolbarWidth = 620.0f;
		constexpr float MinimumTreeWidth = 145.0f;
		constexpr float MinimumContentWidth = 240.0f;
		constexpr float MinimumContentHeight = 80.0f;
		constexpr float GridCellExtraWidth = 26.0f;
		constexpr float GridCardRounding = 7.0f;
		constexpr float GridIconScale = 0.76f;
		constexpr float GridIconVerticalPadding = 2.0f;
		constexpr float GridIconNameSpacing = 1.0f;
	} // namespace

	FContentBrowserPanel::FContentBrowserPanel(FLevelEditorSessionSettings& InSessionSettings, FOpenAsset InOpenAsset, FRequestImport InRequestImport, FMoveAssets InMoveAssets)
		: SessionSettings(InSessionSettings)
		, OpenAsset(std::move(InOpenAsset))
		, RequestImport(std::move(InRequestImport))
		, MoveAssets(std::move(InMoveAssets))
		, IconSize(InSessionSettings.GetContentBrowserIconSize())
		, DirectoryTreeWidth(InSessionSettings.GetContentBrowserTreeWidth())
	{
		RefreshMountSnapshot();
		ThumbnailCache = std::make_unique<FSourceImageThumbnailCache>();
		ViewMode = static_cast<EContentBrowserViewMode>(SessionSettings.GetContentBrowserViewMode());
		bIconSizeLocked = SessionSettings.IsContentBrowserIconSizeLocked();
		bShowSourceFiles = SessionSettings.GetContentBrowserShowSourceFiles();
		if (!SessionSettings.GetContentBrowserLastDirectory().empty())
			NavigateToPhysical(SessionSettings.GetContentBrowserLastDirectory());
		if (CurrentPhysicalPath.empty())
		{
			for (const FMountSnapshot& Mount : MountSnapshot)
			{
				if (std::filesystem::is_directory(Mount.PhysicalRoot) && NavigateToPhysical(Mount.PhysicalRoot)) break;
			}
		}
	}

	FContentBrowserPanel::~FContentBrowserPanel() = default;

	auto FContentBrowserPanel::RefreshMountSnapshot() -> void
	{
		const auto& RegisteredMounts = PathUtilities::GetRegisteredMountPoints();
		const bool bUnchanged = RegisteredMounts.size() == MountSnapshot.size() && std::ranges::equal(RegisteredMounts, MountSnapshot, [](const auto& Registered, const FMountSnapshot& Cached) {
			return Registered.VirtualRoot == Cached.VirtualRoot && Registered.PhysicalPath == Cached.SourcePhysicalRoot;
		});
		if (bUnchanged) return;

		MountSnapshot.clear();
		MountSnapshot.reserve(RegisteredMounts.size());
		for (const PathUtilities::FMountPoint& Mount : RegisteredMounts)
			MountSnapshot.push_back({Mount.VirtualRoot, Mount.PhysicalPath, NormalizePath(Mount.PhysicalPath)});
	}

	auto FContentBrowserPanel::PhysicalToVirtualDirectory(std::string_view PhysicalPath) const -> std::string
	{
		const std::string Normalized = NormalizePath(PhysicalPath);
		for (const FMountSnapshot& Mount : MountSnapshot)
		{
			std::filesystem::path Relative;
			if (!PathUtilities::TryMakeLexicalRelativePath(Normalized, Mount.PhysicalRoot, Relative)) continue;
			std::string Virtual = Mount.VirtualRoot;
			if (!Relative.empty()) Virtual += Relative.generic_string();
			if (!Virtual.ends_with('/')) Virtual += '/';
			return Virtual;
		}
		return {};
	}

	auto FContentBrowserPanel::VirtualToPhysical(std::string_view VirtualPath) const -> std::string
	{
		for (const FMountSnapshot& Mount : MountSnapshot)
		{
			if (!VirtualPath.starts_with(Mount.VirtualRoot)) continue;
			std::string_view Relative = VirtualPath.substr(Mount.VirtualRoot.size());
			return NormalizePath((std::filesystem::path(Mount.PhysicalRoot) / std::filesystem::path(Relative)).generic_string());
		}
		return {};
	}

	auto FContentBrowserPanel::NavigateToPhysical(std::string_view PhysicalPath, bool bAddHistory) -> bool
	{
		const std::string Normalized = NormalizePath(PhysicalPath);
		const std::string Virtual = PhysicalToVirtualDirectory(Normalized);
		if (Virtual.empty() || !std::filesystem::is_directory(Normalized)) return false;
		CurrentPhysicalPath = Normalized;
		CurrentVirtualPath = Virtual;
		Selection.clear();
		SelectionAnchor.clear();
		if (bAddHistory)
		{
			if (HistoryIndex >= 0 && static_cast<size_t>(HistoryIndex + 1) < NavigationHistory.size())
				NavigationHistory.resize(static_cast<size_t>(HistoryIndex + 1));
			if (NavigationHistory.empty() || NavigationHistory.back() != Normalized)
			{
				NavigationHistory.push_back(Normalized);
				HistoryIndex = static_cast<int32>(NavigationHistory.size() - 1);
			}
		}
		RefreshItemsSnapshot();
		return true;
	}

	auto FContentBrowserPanel::NavigateHistory(int32 Delta) -> void
	{
		const int32 Target = HistoryIndex + Delta;
		if (Target < 0 || static_cast<size_t>(Target) >= NavigationHistory.size()) return;
		HistoryIndex = Target;
		if (!NavigateToPhysical(NavigationHistory[static_cast<size_t>(HistoryIndex)], false))
		{
			NavigationHistory.erase(NavigationHistory.begin() + HistoryIndex);
			HistoryIndex = std::min(HistoryIndex, static_cast<int32>(NavigationHistory.size()) - 1);
		}
	}

	auto FContentBrowserPanel::IsInsideCurrentDirectory(std::string_view PhysicalPath, bool bRecursive) const -> bool
	{
		return PathUtilities::IsLexicalDescendantPath(NormalizePath(PhysicalPath), CurrentPhysicalPath, bRecursive);
	}

	auto FContentBrowserPanel::Refresh(bool bRescanRegistry) -> void
	{
		RefreshMountSnapshot();
		if (bRescanRegistry)
		{
			const Asset::FAssetResult Result = Asset::GetAssetRegistry().ScanMountedContent(Asset::EAssetRegistryScanMode::Incremental);
			if (!Result) SetError(Result.Message);
		}
		if (!CurrentPhysicalPath.empty() && !std::filesystem::is_directory(CurrentPhysicalPath))
		{
			for (const FMountSnapshot& Mount : MountSnapshot)
				if (NavigateToPhysical(Mount.PhysicalRoot)) return;
		}
		RefreshItemsSnapshot();
	}

	auto FContentBrowserPanel::MatchesTypeFilter(const FContentBrowserItem& Item) const -> bool
	{
		if (TypeFilter == 0 || Item.Kind == EContentBrowserItemKind::Folder) return true;
		const std::string Type = ItemTypeLabel(Item);
		if (TypeFilter == 1) return Type == "Level";
		if (TypeFilter == 2) return Type == "StaticMesh";
		if (TypeFilter == 3) return Type.find("Material") != std::string::npos;
		if (TypeFilter == 4) return Type == "Texture2D";
		return Item.Kind != EContentBrowserItemKind::Asset || (Type != "Level" && Type != "StaticMesh" && Type.find("Material") == std::string::npos && Type != "Texture2D");
	}

	auto FContentBrowserPanel::RefreshItemsSnapshot() -> void
	{
		ThumbnailCache->CancelPendingRequests();
		DirectoryChildrenCache.clear();
		ItemsSnapshot.clear();
		if (CurrentPhysicalPath.empty())
		{
			Items.clear();
			return;
		}

		std::error_code Ec;
		for (std::filesystem::recursive_directory_iterator It(CurrentPhysicalPath, std::filesystem::directory_options::skip_permission_denied, Ec), End; !Ec && It != End; It.increment(Ec))
		{
			const std::filesystem::directory_entry& Entry = *It;
			const std::string Name = Entry.path().filename().generic_string();
			if (Entry.is_directory(Ec))
				ItemsSnapshot.push_back({EContentBrowserItemKind::Folder, Name, PhysicalToVirtualDirectory(Entry.path().generic_string()), NormalizePath(Entry.path().generic_string())});
			else if (Entry.is_regular_file(Ec) && Entry.path().extension() != ".dasset")
			{
				FContentBrowserItem Item{EContentBrowserItemKind::SourceFile, Name, {}, NormalizePath(Entry.path().generic_string())};
				Item.Extension = Entry.path().extension().generic_string();
				std::error_code FileEc;
				Item.FileSize = Entry.file_size(FileEc);
				FileEc.clear();
				Item.LastWriteTime = Entry.last_write_time(FileEc);
				ItemsSnapshot.push_back(std::move(Item));
			}
		}

		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			if (!IsInsideCurrentDirectory(Data.PhysicalPath, true)) continue;
			const std::string Name(Path.GetAssetName());
			FContentBrowserItem Item{EContentBrowserItemKind::Asset, Name, Path.ToString(), NormalizePath(Data.PhysicalPath), Data.AssetClassName, ".dasset"};
			std::error_code FileEc;
			Item.FileSize = std::filesystem::file_size(Data.PhysicalPath, FileEc);
			Item.LastWriteTime = Data.LastWriteTime;
			if (Data.AssetClassName == DTexture2D::StaticClass()->GetQualifiedName().ToString())
			{
				const std::filesystem::path ThumbnailPath = FindTextureSourceFile(Data.PhysicalPath);
				if (!ThumbnailPath.empty())
				{
					Item.ThumbnailPhysicalPath = NormalizePath(ThumbnailPath.generic_string());
					FileEc.clear();
					Item.ThumbnailFileSize = std::filesystem::file_size(ThumbnailPath, FileEc);
					FileEc.clear();
					Item.ThumbnailLastWriteTime = std::filesystem::last_write_time(ThumbnailPath, FileEc);
				}
			}
			ItemsSnapshot.push_back(std::move(Item));
		}
		RebuildItems();
	}

	auto FContentBrowserPanel::RebuildItems() -> void
	{
		ThumbnailCache->CancelPendingRequests();
		Items.clear();
		const bool bSearching = SearchBuffer[0] != '\0';
		for (const FContentBrowserItem& Item : ItemsSnapshot)
		{
			// Snapshot paths are normalized when captured, so lexical checks keep interactive filtering free of filesystem I/O.
			const std::filesystem::path Relative = std::filesystem::path(Item.PhysicalPath).lexically_relative(CurrentPhysicalPath);
			if (Relative.empty() || Relative == "." || Relative.native().starts_with(L"..")) continue;
			if (!bSearching && !Relative.parent_path().empty()) continue;
			if (!bShowSourceFiles && Item.Kind == EContentBrowserItemKind::SourceFile) continue;
			if (!bShowSourceFiles && Item.Kind == EContentBrowserItemKind::Folder)
			{
				if (std::ranges::any_of(Relative, [](const std::filesystem::path& Component) { return Component.generic_string().starts_with('.'); })) continue;
			}
			const std::string Type = Item.Kind == EContentBrowserItemKind::Folder ? "Folder" : Item.Kind == EContentBrowserItemKind::SourceFile ? "Source File" : ClassLeaf(Item.AssetClassName);
			const std::string_view SearchPath = Item.VirtualPath.empty() ? std::string_view(Item.PhysicalPath) : std::string_view(Item.VirtualPath);
			if (bSearching && !ContainsInsensitive(Item.Name, SearchBuffer.data()) && !ContainsInsensitive(SearchPath, SearchBuffer.data()) && !ContainsInsensitive(Type, SearchBuffer.data())) continue;
			if (MatchesTypeFilter(Item)) Items.push_back(Item);
		}

		auto Compare = [&](const FContentBrowserItem& A, const FContentBrowserItem& B) {
			if (A.Kind == EContentBrowserItemKind::Folder && B.Kind != EContentBrowserItemKind::Folder) return true;
			if (A.Kind != EContentBrowserItemKind::Folder && B.Kind == EContentBrowserItemKind::Folder) return false;
			int32 Result = 0;
			switch (SortColumn)
			{
			case ESortColumn::Type: Result = ItemTypeLabel(A).compare(ItemTypeLabel(B)); break;
			case ESortColumn::Size:
				Result = A.FileSize < B.FileSize ? -1 : A.FileSize > B.FileSize ? 1 :
																				  0;
				break;
			case ESortColumn::Modified:
				Result = A.LastWriteTime < B.LastWriteTime ? -1 : A.LastWriteTime > B.LastWriteTime ? 1 :
																									  0;
				break;
			default: Result = A.Name.compare(B.Name); break;
			}
			if (Result == 0) Result = A.Name.compare(B.Name);
			return bSortAscending ? Result < 0 : Result > 0;
		};
		std::ranges::sort(Items, Compare);
		std::erase_if(Selection, [&](const std::string& Id) { return std::ranges::none_of(Items, [&](const FContentBrowserItem& Item) { return Item.StableId() == Id; }); });
	}

	auto FContentBrowserPanel::Draw(FLevelEditorContext& Context) -> void
	{
		(void)Context;
		RefreshMountSnapshot();
		if (!EditorWorkspaceUI::BeginDockablePanel(LevelEditorWorkspace::Type, "Content Browser", "ContentBrowser", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}
		DrawToolbar();

		const float StatusHeight = ImGui::GetTextLineHeightWithSpacing();
		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		const float SplitterWidth = Metrics.SplitterThickness;
		const float ScaledMinimumTreeWidth = MonaImGui::ScaleUI(MinimumTreeWidth);
		const float ScaledMinimumContentWidth = MonaImGui::ScaleUI(MinimumContentWidth);
		const float TreeWidth = std::clamp(AvailableWidth * DirectoryTreeWidth, ScaledMinimumTreeWidth, std::max(ScaledMinimumTreeWidth, AvailableWidth - ScaledMinimumContentWidth));
		const float ContentHeight = std::max(MonaImGui::ScaleUI(MinimumContentHeight), ImGui::GetContentRegionAvail().y - StatusHeight);
		if (ImGui::BeginChild("ContentBrowserTree", ImVec2(TreeWidth, ContentHeight), ImGuiChildFlags_Borders)) DrawDirectoryTree();
		ImGui::EndChild();
		ImGui::SameLine();
		MonaImGui::DrawSplitter("ContentBrowserSplitter", MonaImGui::EUISplitterAxis::X, ContentHeight, AvailableWidth, ScaledMinimumTreeWidth, ScaledMinimumContentWidth, DirectoryTreeWidth);
		ImGui::SameLine();
		if (ImGui::BeginChild("ContentBrowserItems", ImVec2(0.0f, ContentHeight), ImGuiChildFlags_Borders)) DrawContentArea();
		ImGui::EndChild();
		// Context-menu actions can rebuild Items, so execute them only after both browser panes
		// have finished traversing their current frame snapshots.
		if (DeferredContentAction)
		{
			auto Action = std::move(DeferredContentAction);
			DeferredContentAction = {};
			Action();
		}
		DrawStatusBar();
		DrawDialogs();

		SessionSettings.SetContentBrowserState(static_cast<uint8>(ViewMode), IconSize, bIconSizeLocked, DirectoryTreeWidth, bShowSourceFiles, CurrentPhysicalPath);
		ImGui::End();
	}

	auto FContentBrowserPanel::DrawToolbar() -> void
	{
		const float ToolbarWidth = ImGui::GetContentRegionAvail().x;
		const EEditorUILayoutMode LayoutMode = ResolveEditorUILayout(ToolbarWidth, MonaImGui::ScaleUI(CompactToolbarWidth), MonaImGui::ScaleUI(FullToolbarWidth));
		const bool bFullLayout = LayoutMode == EEditorUILayoutMode::Full;
		const bool bCompactLayout = LayoutMode != EEditorUILayoutMode::Narrow;

		ImGui::BeginDisabled(HistoryIndex <= 0);
		if (DrawToolbarIconButton(Icons::ArrowLeft, "ContentBrowserBack")) NavigateHistory(-1);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
		ImGui::SameLine();
		ImGui::BeginDisabled(HistoryIndex < 0 || static_cast<size_t>(HistoryIndex + 1) >= NavigationHistory.size());
		if (DrawToolbarIconButton(Icons::ArrowRight, "ContentBrowserForward")) NavigateHistory(1);
		ImGui::EndDisabled();
		ImGui::SameLine();
		const std::filesystem::path Parent = std::filesystem::path(CurrentPhysicalPath).parent_path();
		ImGui::BeginDisabled(PhysicalToVirtualDirectory(Parent.generic_string()).empty());
		if (DrawToolbarIconButton(Icons::ArrowUp, "ContentBrowserUp")) NavigateToPhysical(Parent.generic_string());
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (DrawToolbarIconButton(Icons::Refresh, "ContentBrowserRefresh")) Refresh(true);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan mounted content");

		auto DrawBreadcrumb = [&](float Width) {
			if (!ImGui::BeginChild("ContentBrowserBreadcrumb", ImVec2(Width, ImGui::GetFrameHeight()), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar))
			{
				ImGui::EndChild();
				return;
			}
			ImGui::AlignTextToFramePadding();
			auto DrawPathSegment = [&](std::string_view Label, std::string_view PhysicalPath, bool bCurrent) {
				ImGui::PushID(std::string(PhysicalPath).c_str());
				ImGui::TextColored(bCurrent ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", std::string(Label).c_str());
				if (ImGui::IsItemHovered())
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					const ImVec2 Min = ImGui::GetItemRectMin();
					const ImVec2 Max = ImGui::GetItemRectMax();
					ImGui::GetWindowDrawList()->AddLine(ImVec2(Min.x, Max.y), Max, ImGui::GetColorU32(ImGuiCol_TextDisabled));
				}
				if (ImGui::IsItemClicked()) NavigateToPhysical(PhysicalPath);
				ImGui::PopID();
			};
			bool bFirst = true;
			for (const FMountSnapshot& Mount : MountSnapshot)
			{
				std::filesystem::path Relative;
				if (!PathUtilities::TryMakeLexicalRelativePath(CurrentPhysicalPath, Mount.PhysicalRoot, Relative)) continue;
				std::filesystem::path Progressive = Mount.PhysicalRoot;
				std::string RootLabel = Mount.VirtualRoot;
				if (RootLabel.starts_with('/')) RootLabel.erase(RootLabel.begin());
				if (RootLabel.ends_with('/')) RootLabel.pop_back();
				DrawPathSegment(RootLabel, Mount.PhysicalRoot, Relative.empty());
				if (!Relative.empty())
					for (const auto& Segment : Relative)
					{
						ImGui::SameLine();
						ImGui::TextDisabled("/");
						ImGui::SameLine();
						Progressive /= Segment;
						const std::string SegmentPath = Progressive.generic_string();
						DrawPathSegment(Segment.generic_string(), SegmentPath, CurrentPhysicalPath == NormalizePath(SegmentPath));
					}
				bFirst = false;
				break;
			}
			if (bFirst) ImGui::TextDisabled("No mounted directory");
			ImGui::EndChild();
		};

		const char* Filters[] = {"All types", "Levels", "Static meshes", "Materials", "Textures", "Other"};
		const float Spacing = ImGui::GetStyle().ItemSpacing.x;

		auto DrawViewControls = [&]() {
			const bool bGridView = ViewMode == EContentBrowserViewMode::Grid;
			const bool bToggleView = DrawToolbarIconButton(bGridView ? Icons::List : Icons::TableCells, "ContentBrowserView");
			if (bToggleView) ViewMode = bGridView ? EContentBrowserViewMode::Details : EContentBrowserViewMode::Grid;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(bGridView ? "Switch to list view" : "Switch to icon view");
			ImGui::SameLine();
			if (DrawToolbarIconButton(Icons::Info, "ContentBrowserDetails")) bShowSelectionDetails = !bShowSelectionDetails;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle selection details");
			ImGui::SameLine();
			if (DrawToolbarIconButton(Icons::Gear, "ContentBrowserSettings")) ImGui::OpenPopup("ContentBrowserSettingsPopup");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Content Browser settings");
			if (ImGui::BeginPopup("ContentBrowserSettingsPopup"))
			{
				if (!bFullLayout)
				{
					ImGui::TextDisabled("Type filter");
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::Combo("##CompactContentTypeFilter", &TypeFilter, Filters, std::size(Filters))) RebuildItems();
					ImGui::Separator();
				}
				ImGui::TextDisabled("Thumbnail size");
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::SliderFloat("##ContentIconSize", &IconSize, FLevelEditorSessionSettings::MinimumContentBrowserIconSize, FLevelEditorSessionSettings::MaximumContentBrowserIconSize, "%.0f px");
				ImGui::Checkbox("Lock Ctrl + wheel resizing", &bIconSizeLocked);
				ImGui::Separator();
				if (ImGui::MenuItem("Show Source Files", nullptr, bShowSourceFiles))
				{
					bShowSourceFiles = !bShowSourceFiles;
					RebuildItems();
				}
				ImGui::EndPopup();
			}
		};

		if (bFullLayout)
		{
			const float FilterWidth = MonaImGui::ScaleUI(120.0f);
			const float SearchWidth = MonaImGui::ScaleUI(210.0f);
			const float ViewControlsWidth = ImGui::GetFrameHeight() * 3.0f + Spacing * 2.0f;
			ImGui::SameLine();
			const float BreadcrumbWidth = std::max(MonaImGui::ScaleUI(160.0f), ImGui::GetContentRegionAvail().x - ViewControlsWidth - FilterWidth - SearchWidth - Spacing * 3.0f);
			DrawBreadcrumb(BreadcrumbWidth);
			ImGui::SameLine();
			DrawViewControls();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(FilterWidth);
			if (ImGui::Combo("##ContentTypeFilter", &TypeFilter, Filters, std::size(Filters))) RebuildItems();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(SearchWidth);
			if (ImGui::InputTextWithHint("##ContentSearch", "Search current folder...", SearchBuffer.data(), SearchBuffer.size())) RebuildItems();
		}
		else if (bCompactLayout)
		{
			ImGui::SameLine();
			DrawViewControls();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputTextWithHint("##ContentSearch", "Search current folder...", SearchBuffer.data(), SearchBuffer.size())) RebuildItems();
		}

		if (!bCompactLayout)
		{
			ImGui::NewLine();
			DrawViewControls();
			ImGui::NewLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputTextWithHint("##ContentSearchNarrow", "Search current folder...", SearchBuffer.data(), SearchBuffer.size())) RebuildItems();
		}
	}

	auto FContentBrowserPanel::DrawDirectoryTree() -> void
	{
		for (const FMountSnapshot& Mount : MountSnapshot)
		{
			if (!std::filesystem::is_directory(Mount.PhysicalRoot)) continue;
			std::string Label = Mount.VirtualRoot;
			if (Label.starts_with('/')) Label.erase(Label.begin());
			if (Label.ends_with('/')) Label.pop_back();
			DrawDirectoryNode(std::filesystem::path(Mount.PhysicalRoot), Label, true);
		}
		if (ImGui::BeginPopupContextWindow("ContentBrowserTreeBackground", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			const bool bCurrentIsMountRoot = std::ranges::any_of(MountSnapshot, [&](const FMountSnapshot& Mount) {
				return Mount.PhysicalRoot == CurrentPhysicalPath;
			});
			DrawDirectoryContextMenu(CurrentPhysicalPath, bCurrentIsMountRoot);
			ImGui::EndPopup();
		}
	}

	auto FContentBrowserPanel::DrawDirectoryNode(const std::filesystem::path& Path, std::string_view Label, bool bMountRoot) -> void
	{
		const std::string Physical = NormalizePath(Path.generic_string());
		auto [CachedIt, bInserted] = DirectoryChildrenCache.try_emplace(Physical);
		if (bInserted)
		{
			std::error_code Ec;
			for (std::filesystem::directory_iterator It(Path, std::filesystem::directory_options::skip_permission_denied, Ec), End; !Ec && It != End; It.increment(Ec))
			{
				if (It->is_directory(Ec)) CachedIt->second.push_back(It->path());
			}
			std::ranges::sort(CachedIt->second);
		}
		// Recursing can grow and rehash the cache, so keep this node's traversal independent of cached iterators.
		const std::vector<std::filesystem::path> Children = CachedIt->second;
		const bool bHasChildren = std::ranges::any_of(Children, [&](const std::filesystem::path& Child) { return bShowSourceFiles || !Child.filename().generic_string().starts_with('.'); });
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (!bHasChildren) Flags |= ImGuiTreeNodeFlags_Leaf;
		if (CurrentPhysicalPath == Physical) Flags |= ImGuiTreeNodeFlags_Selected;
		if (bMountRoot) Flags |= ImGuiTreeNodeFlags_DefaultOpen;
		const std::string NodeLabel = std::format("{} {}##{}", CurrentPhysicalPath == Physical ? Icons::FolderOpen : Icons::Folder, Label, Physical);
		const bool bOpen = ImGui::TreeNodeEx(NodeLabel.c_str(), Flags);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) NavigateToPhysical(Physical);
		AcceptAssetDrop(Physical, true);
		if (ImGui::BeginPopupContextItem())
		{
			DrawDirectoryContextMenu(Physical, bMountRoot);
			ImGui::EndPopup();
		}
		if (bOpen)
		{
			for (const std::filesystem::path& Child : Children)
			{
				const std::string Name = Child.filename().generic_string();
				if (!bShowSourceFiles && Name.starts_with('.')) continue;
				DrawDirectoryNode(Child, Name, false);
			}
			ImGui::TreePop();
		}
	}

	auto FContentBrowserPanel::DrawContentArea() -> void
	{
		ThumbnailCache->BeginFrame();
		const ImGuiIO& IO = ImGui::GetIO();
		if (ViewMode == EContentBrowserViewMode::Grid && !bIconSizeLocked && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && IO.KeyCtrl && IO.MouseWheel != 0.0f)
			IconSize = std::clamp(IconSize + IO.MouseWheel * MonaImGui::ScaleUI(8.0f), FLevelEditorSessionSettings::MinimumContentBrowserIconSize, FLevelEditorSessionSettings::MaximumContentBrowserIconSize);
		const bool bReserveDetails = bShowSelectionDetails && Selection.size() == 1;
		if (bReserveDetails) ImGui::BeginChild("ContentBrowserMainView", ImVec2(0.0f, -132.0f));
		bContentItemHovered = false;
		bRenameEditorHovered = false;
		if (ViewMode == EContentBrowserViewMode::Grid)
			DrawGrid();
		else
			DrawDetails();
		ThumbnailCache->EndFrame();
		// Resolve outside clicks after every item has been drawn so the result does not depend on whether
		// the clicked item appears before or after the active rename editor in ImGui's submission order.
		if (!RenameTarget.empty() && !bFocusRename && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !bRenameEditorHovered)
			RenameTarget.clear();
		if (Items.empty())
		{
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 24.0f);
			ImGui::TextDisabled(SearchBuffer[0] ? "No matching assets or folders." : "This folder is empty.");
		}
		if (bReserveDetails)
		{
			ImGui::EndChild();
			ImGui::SeparatorText("Selection Details");
			DrawSelectionDetails();
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
		{
			if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_N)) CreateFolder(CurrentPhysicalPath);
			if (ImGui::IsKeyPressed(ImGuiKey_F5)) Refresh(true);
			if (ImGui::IsKeyPressed(ImGuiKey_Enter) && Selection.size() == 1)
				if (auto It = std::ranges::find_if(Items, [&](const FContentBrowserItem& Item) { return Selection.contains(Item.StableId()); }); It != Items.end()) OpenItem(*It);
			if (ImGui::IsKeyPressed(ImGuiKey_F2) && Selection.size() == 1)
				if (auto It = std::ranges::find_if(Items, [&](const FContentBrowserItem& Item) { return Selection.contains(Item.StableId()); }); It != Items.end()) BeginRename(*It);
			if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !Selection.empty()) RequestDeleteSelection();
		}
	}

	auto FContentBrowserPanel::DrawSelectionDetails() -> void
	{
		auto It = std::ranges::find_if(Items, [&](const FContentBrowserItem& Item) { return Selection.contains(Item.StableId()); });
		if (It == Items.end()) return;
		const FContentBrowserItem& Item = *It;
		if (ImGui::BeginTable("ContentBrowserSelectionDetails", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 92.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			auto Row = [](const char* Label, std::string_view Value) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", Label);
				ImGui::TableNextColumn();
				const std::string DisplayValue(Value);
				ImGui::TextUnformatted(DisplayValue.c_str());
				if (ImGui::IsItemHovered() && ImGui::CalcTextSize(DisplayValue.c_str()).x > ImGui::GetContentRegionAvail().x)
					ImGui::SetTooltip("%s", DisplayValue.c_str());
			};
			Row("Type", ItemTypeLabel(Item));
			Row("Virtual", Item.VirtualPath.empty() ? "-" : Item.VirtualPath);
			Row("Physical", Item.PhysicalPath);
			if (Item.Kind == EContentBrowserItemKind::Asset)
			{
				FAssetPath Path;
				if (FAssetPath::TryCreate(Item.VirtualPath, Path))
				{
					if (const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(Path))
					{
						Row("Dependencies", std::format("{}", Data->Dependencies.size()));
						size_t ReferencerCount = 0;
						for (const auto& [OtherPath, OtherData] : Asset::GetAssetRegistry().GetAssets())
							if (OtherPath != Path && std::ranges::find(OtherData.Dependencies, Path) != OtherData.Dependencies.end()) ++ReferencerCount;
						Row("Referencers", std::format("{}", ReferencerCount));
					}
				}
			}
			ImGui::EndTable();
		}
	}

	auto FContentBrowserPanel::SelectItem(size_t Index) -> void
	{
		if (Index >= Items.size()) return;
		const std::string& Id = Items[Index].StableId();
		const ImGuiIO& IO = ImGui::GetIO();
		if (IO.KeyShift && !SelectionAnchor.empty())
		{
			auto AnchorIt = std::ranges::find_if(Items, [&](const FContentBrowserItem& Item) { return Item.StableId() == SelectionAnchor; });
			if (AnchorIt != Items.end())
			{
				const size_t AnchorIndex = static_cast<size_t>(std::distance(Items.begin(), AnchorIt));
				if (!IO.KeyCtrl) Selection.clear();
				for (size_t I = std::min(Index, AnchorIndex); I <= std::max(Index, AnchorIndex); ++I)
					Selection.insert(Items[I].StableId());
				return;
			}
		}
		if (IO.KeyCtrl)
		{
			if (!Selection.erase(Id)) Selection.insert(Id);
		}
		else
		{
			Selection.clear();
			Selection.insert(Id);
		}
		SelectionAnchor = Id;
	}

	auto FContentBrowserPanel::DrawGrid() -> void
	{
		const float CellWidth = IconSize + MonaImGui::ScaleUI(GridCellExtraWidth);
		const float NameFontSize = MonaImGui::QuantizeDynamicFontSize(
			std::clamp(IconSize * 0.22f, ImGui::GetFontSize() * 0.70f, ImGui::GetFontSize() * 1.40f));
		const float IconFontSize = MonaImGui::QuantizeDynamicFontSize(IconSize * GridIconScale);
		// Keep the label gap tied to UI scale instead of thumbnail size so zooming does not
		// amplify the unused space around the icon glyph.
		const float IconAreaHeight = IconFontSize + MonaImGui::ScaleUI(GridIconVerticalPadding * 2.0f);
		const float NamePositionY = IconAreaHeight + MonaImGui::ScaleUI(GridIconNameSpacing);
		const float NameAreaHeight = NameFontSize * 2.0f + MonaImGui::ScaleUI(8.0f);
		const float TileHeight = IconAreaHeight + NameAreaHeight;
		const float LayoutSentinelHeight = 1.0f;
		const float RowHeight = TileHeight + ImGui::GetStyle().ItemSpacing.y + LayoutSentinelHeight;
		const int32 Columns = std::max(1, static_cast<int32>(ImGui::GetContentRegionAvail().x / CellWidth));
		const int32 RowCount = static_cast<int32>((Items.size() + static_cast<size_t>(Columns) - 1) / static_cast<size_t>(Columns));
		if (!ImGui::BeginTable("ContentBrowserGrid", Columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY)) return;

		ImGuiListClipper Clipper;
		Clipper.Begin(RowCount, RowHeight);
		while (Clipper.Step())
		{
			const int32 PrefetchStart = std::max(0, Clipper.DisplayStart - 2);
			const int32 PrefetchEnd = std::min(RowCount, Clipper.DisplayEnd + 2);
			for (int32 Row = PrefetchStart; Row < PrefetchEnd; ++Row)
				for (int32 Column = 0; Column < Columns; ++Column)
				{
					const size_t Index = static_cast<size_t>(Row) * Columns + Column;
					if (Index >= Items.size()) break;
					const FContentBrowserItem& Item = Items[Index];
					const std::string& ThumbnailPath = Item.ThumbnailPhysicalPath.empty() ? Item.PhysicalPath : Item.ThumbnailPhysicalPath;
					if ((bShowSourceFiles && Item.Kind == EContentBrowserItemKind::SourceFile && IsSupportedSourceImageExtension(Item.Extension)) || !Item.ThumbnailPhysicalPath.empty())
						ThumbnailCache->Request(ThumbnailPath, Item.ThumbnailPhysicalPath.empty() ? Item.FileSize : Item.ThumbnailFileSize, Item.ThumbnailPhysicalPath.empty() ? Item.LastWriteTime : Item.ThumbnailLastWriteTime, Row >= Clipper.DisplayStart && Row < Clipper.DisplayEnd);
				}

			for (int32 Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				ImGui::TableNextRow(ImGuiTableRowFlags_None, RowHeight);
				for (int32 Column = 0; Column < Columns; ++Column)
				{
					const size_t Index = static_cast<size_t>(Row) * Columns + Column;
					if (Index >= Items.size()) break;
					const FContentBrowserItem& Item = Items[Index];
					ImGui::TableSetColumnIndex(Column);
					ImGui::PushID(Item.StableId().c_str());
					const ImVec2 TileSize(ImGui::GetContentRegionAvail().x, TileHeight);
					const bool bSelected = Selection.contains(Item.StableId());
					const ImVec2 TileStart = ImGui::GetCursorScreenPos();
					ImGui::InvisibleButton("##Tile", TileSize);
					const ImVec2 CursorAfterTile = ImGui::GetCursorScreenPos();
					const bool bHovered = ImGui::IsItemHovered();
					bContentItemHovered |= bHovered;
					if (ImGui::IsItemClicked()) SelectItem(Index);
					if (bHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenItem(Item);
					BeginAssetDragDrop(Item);
					if (Item.Kind == EContentBrowserItemKind::Folder) AcceptAssetDrop(Item.VirtualPath);
					if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
					{
						if (!bSelected)
						{
							Selection.clear();
							Selection.insert(Item.StableId());
						}
						ImGui::OpenPopup("ItemContext");
					}

					ImDrawList* DrawList = ImGui::GetWindowDrawList();
					const float CardInset = MonaImGui::ScaleUI(2.0f);
					const float CardRounding = MonaImGui::ScaleUI(GridCardRounding);
					const ImVec2 CardMin(TileStart.x + CardInset, TileStart.y + CardInset);
					const ImVec2 CardMax(TileStart.x + TileSize.x - CardInset, TileStart.y + TileSize.y - CardInset);
					if (bSelected || bHovered)
					{
						const ImGuiCol BackgroundColor = bSelected ? (bHovered ? ImGuiCol_HeaderActive : ImGuiCol_Header) : ImGuiCol_HeaderHovered;
						DrawList->AddRectFilled(CardMin, CardMax, ImGui::GetColorU32(BackgroundColor), CardRounding);
						if (bSelected) DrawList->AddRect(CardMin, CardMax, ImGui::GetColorU32(ImGuiCol_CheckMark), CardRounding, 0, MonaImGui::ScaleUI(1.0f));
					}

					const FSourceImageThumbnailView Thumbnail = ThumbnailCache->Find(Item.ThumbnailPhysicalPath.empty() ? Item.PhysicalPath : Item.ThumbnailPhysicalPath);
					bool bDrewThumbnail = false;
					if (Thumbnail.State == ESourceImageThumbnailState::Ready && Thumbnail.Texture && Mona::GActiveUIBackend)
					{
						const float ThumbnailInset = MonaImGui::ScaleUI(6.0f);
						const ImVec2 ImageAreaMin(TileStart.x + ThumbnailInset, TileStart.y + ThumbnailInset);
						const ImVec2 ImageAreaMax(TileStart.x + TileSize.x - ThumbnailInset, TileStart.y + IconAreaHeight - ThumbnailInset);
						const ImVec2 AvailableImageSize(std::max(1.0f, ImageAreaMax.x - ImageAreaMin.x), std::max(1.0f, ImageAreaMax.y - ImageAreaMin.y));
						const float Scale = std::min(AvailableImageSize.x / static_cast<float>(Thumbnail.Width), AvailableImageSize.y / static_cast<float>(Thumbnail.Height));
						const ImVec2 ImageSize(std::max(1.0f, Thumbnail.Width * Scale), std::max(1.0f, Thumbnail.Height * Scale));
						const ImVec2 ImagePosition(ImageAreaMin.x + (AvailableImageSize.x - ImageSize.x) * 0.5f, ImageAreaMin.y + (AvailableImageSize.y - ImageSize.y) * 0.5f);
						// Keep rectangular source images away from the rounded card edge and clip any
						// subpixel spill introduced by scaling or UI DPI conversion.
						DrawList->PushClipRect(ImageAreaMin, ImageAreaMax, true);
						if (Thumbnail.bHasTransparency)
						{
							const float CheckerSize = MonaImGui::ScaleUI(7.0f);
							DrawList->PushClipRect(ImagePosition, ImVec2(ImagePosition.x + ImageSize.x, ImagePosition.y + ImageSize.y), true);
							for (float Y = 0.0f; Y < ImageSize.y; Y += CheckerSize)
								for (float X = 0.0f; X < ImageSize.x; X += CheckerSize)
								{
									const bool bLight = (static_cast<int32>(X / CheckerSize) + static_cast<int32>(Y / CheckerSize)) % 2 == 0;
									const ImU32 Color = ImGui::GetColorU32(bLight ? ImVec4(0.62f, 0.62f, 0.62f, 1.0f) : ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
									DrawList->AddRectFilled(ImVec2(ImagePosition.x + X, ImagePosition.y + Y), ImVec2(ImagePosition.x + std::min(X + CheckerSize, ImageSize.x), ImagePosition.y + std::min(Y + CheckerSize, ImageSize.y)), Color);
								}
							DrawList->PopClipRect();
						}
						ImGui::SetCursorScreenPos(ImagePosition);
						bDrewThumbnail = Mona::GActiveUIBackend->DrawImage(Thumbnail.Texture, FVector2f(ImageSize.x, ImageSize.y));
						ImGui::SetCursorScreenPos(CursorAfterTile);
						DrawList->PopClipRect();
					}
					if (!bDrewThumbnail)
					{
						const MonaImGui::EUIThemeColor IconColorRole = Item.Kind == EContentBrowserItemKind::Folder ? MonaImGui::EUIThemeColor::Folder :
															   Item.Kind == EContentBrowserItemKind::Asset ? MonaImGui::EUIThemeColor::Asset : MonaImGui::EUIThemeColor::SourceFile;
						const ImVec2 IconExtent = ImGui::GetFont()->CalcTextSizeA(IconFontSize, FLT_MAX, 0.0f, ItemIcon(Item));
						const ImVec2 IconPosition(TileStart.x + std::max(0.0f, (TileSize.x - IconExtent.x) * 0.5f), TileStart.y + std::max(0.0f, (IconAreaHeight - IconExtent.y) * 0.5f));
						DrawList->AddText(ImGui::GetFont(), IconFontSize, IconPosition, MonaImGui::GetThemeColorU32(IconColorRole), ItemIcon(Item));
						if (Thumbnail.State == ESourceImageThumbnailState::Queued || Thumbnail.State == ESourceImageThumbnailState::Decoding || Thumbnail.State == ESourceImageThumbnailState::Uploading)
							DrawList->AddText(ImVec2(TileStart.x + TileSize.x - MonaImGui::ScaleUI(18.0f), TileStart.y + MonaImGui::ScaleUI(4.0f)), ImGui::GetColorU32(ImGuiCol_TextDisabled), "...");
						else if (Thumbnail.State == ESourceImageThumbnailState::Failed)
							DrawList->AddText(ImVec2(TileStart.x + TileSize.x - MonaImGui::ScaleUI(20.0f), TileStart.y + MonaImGui::ScaleUI(4.0f)), MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Warning), Icons::Warning);
					}

					if (RenameTarget == Item.StableId())
					{
						const float RenameInset = MonaImGui::ScaleUI(7.0f);
						ImGui::SetCursorScreenPos(ImVec2(TileStart.x + RenameInset, TileStart.y + NamePositionY));
						ImGui::SetNextItemWidth(std::max(MonaImGui::ScaleUI(40.0f), TileSize.x - RenameInset * 2.0f));
						DrawRenameEditor(Item);
					}
					else
					{
						const float TextInset = MonaImGui::ScaleUI(8.0f);
						const float TextWidth = std::max(MonaImGui::ScaleUI(40.0f), TileSize.x - TextInset * 2.0f);
						const ImVec2 TextExtent = ImGui::GetFont()->CalcTextSizeA(NameFontSize, FLT_MAX, TextWidth, Item.Name.c_str());
						const ImVec2 TextPosition(TileStart.x + std::max(TextInset, (TileSize.x - TextExtent.x) * 0.5f), TileStart.y + NamePositionY);
						DrawList->PushClipRect(ImVec2(TileStart.x + TextInset, TileStart.y + NamePositionY), ImVec2(TileStart.x + TileSize.x - TextInset, TileStart.y + TileSize.y - MonaImGui::ScaleUI(4.0f)), true);
						DrawList->AddText(ImGui::GetFont(), NameFontSize, TextPosition, ImGui::GetColorU32(ImGuiCol_Text), Item.Name.c_str(), nullptr, TextWidth);
						DrawList->PopClipRect();
					}
					if (bHovered && RenameTarget != Item.StableId() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
					{
						ImGui::BeginTooltip();
						ImGui::TextUnformatted(Item.Name.c_str());
						ImGui::Separator();
						ImGui::TextDisabled("Type");
						ImGui::SameLine();
						ImGui::TextUnformatted(ItemTypeLabel(Item).c_str());
						if (Item.Kind != EContentBrowserItemKind::Folder)
						{
							ImGui::TextDisabled("Size");
							ImGui::SameLine();
							ImGui::TextUnformatted(FormatFileSize(Item.FileSize).c_str());
							ImGui::TextDisabled("Modified");
							ImGui::SameLine();
							ImGui::TextUnformatted(FormatFileTime(Item.LastWriteTime).c_str());
						}
						if (Thumbnail.State == ESourceImageThumbnailState::Failed && !Thumbnail.Error.empty())
						{
							ImGui::TextDisabled("Preview");
							ImGui::SameLine();
							ImGui::TextWrapped("%s", Thumbnail.Error.c_str());
						}
						ImGui::TextDisabled(Item.VirtualPath.empty() ? "Path" : "Virtual Path");
						ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
						ImGui::TextUnformatted((Item.VirtualPath.empty() ? Item.PhysicalPath : Item.VirtualPath).c_str());
						ImGui::PopTextWrapPos();
						ImGui::EndTooltip();
					}
					if (ImGui::BeginPopup("ItemContext"))
					{
						DrawItemContextMenu(Item);
						ImGui::EndPopup();
					}
					ImGui::SetCursorScreenPos(CursorAfterTile);
					// Absolute positioning keeps overlays inside the tile, but ImGui still needs a final
					// submitted item at that position to commit the table cell's layout boundary.
					ImGui::Dummy(ImVec2(LayoutSentinelHeight, LayoutSentinelHeight));
					ImGui::PopID();
				}
			}
		}
		DrawBackgroundContextMenu();
		ImGui::EndTable();
	}

	auto FContentBrowserPanel::DrawDetails() -> void
	{
		const ImVec2 CellPadding(MonaImGui::ScaleUI(10.0f), ImGui::GetStyle().CellPadding.y);
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, CellPadding);
		if (!ImGui::BeginTable("ContentBrowserDetails", 4, DetailsTableFlags))
		{
			ImGui::PopStyleVar();
			return;
		}
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, static_cast<ImGuiID>(ESortColumn::Name));
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(130.0f), static_cast<ImGuiID>(ESortColumn::Type));
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(85.0f), static_cast<ImGuiID>(ESortColumn::Size));
		ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(145.0f), static_cast<ImGuiID>(ESortColumn::Modified));
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs(); Specs && Specs->SpecsDirty && Specs->SpecsCount > 0)
		{
			SortColumn = static_cast<ESortColumn>(Specs->Specs[0].ColumnUserID);
			bSortAscending = Specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
			Specs->SpecsDirty = false;
			RebuildItems();
		}
		for (size_t Index = 0; Index < Items.size(); ++Index)
		{
			const FContentBrowserItem& Item = Items[Index];
			ImGui::PushID(Item.StableId().c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (RenameTarget == Item.StableId())
				DrawRenameEditor(Item);
			else
			{
				const std::string Label = std::format("{} {}", ItemIcon(Item), Item.Name);
				ImGui::Selectable(Label.c_str(), Selection.contains(Item.StableId()), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
				bContentItemHovered |= ImGui::IsItemHovered();
				if (ImGui::IsItemClicked()) SelectItem(Index);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenItem(Item);
				BeginAssetDragDrop(Item);
				if (Item.Kind == EContentBrowserItemKind::Folder) AcceptAssetDrop(Item.VirtualPath);
				if (ImGui::BeginPopupContextItem("ItemContext"))
				{
					if (!Selection.contains(Item.StableId()))
					{
						Selection.clear();
						Selection.insert(Item.StableId());
					}
					DrawItemContextMenu(Item);
					ImGui::EndPopup();
				}
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(ItemTypeLabel(Item).c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Item.Kind == EContentBrowserItemKind::Folder ? "-" : FormatFileSize(Item.FileSize).c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Item.Kind == EContentBrowserItemKind::Folder ? "-" : FormatFileTime(Item.LastWriteTime).c_str());
			ImGui::PopID();
		}
		DrawBackgroundContextMenu();
		ImGui::EndTable();
		ImGui::PopStyleVar();
	}

	auto FContentBrowserPanel::DrawItemContextMenu(const FContentBrowserItem& Item) -> void
	{
		if (ImGui::MenuItem(Item.Kind == EContentBrowserItemKind::Folder ? "Open Folder" : "Open")) OpenItem(Item);
		if (Item.Kind == EContentBrowserItemKind::Folder)
		{
			if (ImGui::BeginMenu("Create"))
			{
				DrawCreateMenu(Item.PhysicalPath, Item.VirtualPath);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Import"))
			{
				DrawImportMenu(Item.VirtualPath);
				ImGui::EndMenu();
			}
			ImGui::Separator();
		}
		if (ImGui::MenuItem("Rename", "F2", false, Selection.size() == 1)) BeginRename(Item);
		if (ImGui::MenuItem("Delete", "Delete")) RequestDeleteSelection();
		ImGui::Separator();
		if (ImGui::MenuItem("Copy Name")) CopyToClipboard(Item.Name);
		if (!Item.VirtualPath.empty() && ImGui::MenuItem("Copy Virtual Path")) CopyToClipboard(Item.VirtualPath);
		if (ImGui::MenuItem("Copy Physical Path")) CopyToClipboard(Item.PhysicalPath);
		if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(Item.PhysicalPath);
	}

	auto FContentBrowserPanel::DrawCreateMenu(std::string_view PhysicalDirectory, std::string_view VirtualDirectory) -> void
	{
		if (ImGui::MenuItem("New Folder", "Ctrl+Shift+N"))
			DeferredContentAction = [this, Directory = std::string(PhysicalDirectory)] { CreateFolder(Directory); };
		ImGui::SeparatorText("Assets");
		if (ImGui::MenuItem("Level"))
			DeferredContentAction = [this, Directory = std::string(VirtualDirectory)] { CreateLevelAsset(Directory); };
		if (ImGui::MenuItem("Material"))
			DeferredContentAction = [this, Directory = std::string(VirtualDirectory)] { CreateMaterialAsset(Directory, false); };
		if (ImGui::MenuItem("Material Instance"))
			DeferredContentAction = [this, Directory = std::string(VirtualDirectory)] { CreateMaterialAsset(Directory, true); };
	}

	auto FContentBrowserPanel::DrawImportMenu(std::string_view VirtualDirectory) -> void
	{
		ImGui::BeginDisabled(!RequestImport);
		if (ImGui::MenuItem("Texture..."))
			DeferredContentAction = [this, Directory = std::string(VirtualDirectory)] { RequestImport(Directory, EContentBrowserImportType::Texture); };
		if (ImGui::MenuItem("Static Mesh..."))
			DeferredContentAction = [this, Directory = std::string(VirtualDirectory)] { RequestImport(Directory, EContentBrowserImportType::StaticMesh); };
		ImGui::EndDisabled();
	}

	auto FContentBrowserPanel::DrawDirectoryContextMenu(std::string_view PhysicalDirectory, bool bMountRoot) -> void
	{
		const std::string VirtualDirectory = PhysicalToVirtualDirectory(PhysicalDirectory);
		const bool bIsCurrent = NormalizePath(PhysicalDirectory) == CurrentPhysicalPath;
		if (ImGui::MenuItem("Open Folder", nullptr, false, !bIsCurrent)) NavigateToPhysical(PhysicalDirectory);
		if (ImGui::BeginMenu("Create", !VirtualDirectory.empty()))
		{
			DrawCreateMenu(PhysicalDirectory, VirtualDirectory);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Import", !VirtualDirectory.empty()))
		{
			DrawImportMenu(VirtualDirectory);
			ImGui::EndMenu();
		}
		ImGui::Separator();
		ImGui::BeginDisabled(bMountRoot);
		if (ImGui::MenuItem("Rename", "F2"))
			if (const FContentBrowserItem* Item = FocusFolderInParent(PhysicalDirectory)) BeginRename(*Item);
		if (ImGui::MenuItem("Delete", "Delete"))
			if (FocusFolderInParent(PhysicalDirectory)) RequestDeleteSelection();
		ImGui::EndDisabled();
		ImGui::Separator();
		if (!VirtualDirectory.empty() && ImGui::MenuItem("Copy Virtual Path")) CopyToClipboard(VirtualDirectory);
		if (ImGui::MenuItem("Copy Physical Path")) CopyToClipboard(PhysicalDirectory);
		if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(PhysicalDirectory);
		if (ImGui::MenuItem("Refresh", "F5")) Refresh(true);
		if (ImGui::MenuItem("Show Source Files", nullptr, bShowSourceFiles))
		{
			bShowSourceFiles = !bShowSourceFiles;
			RebuildItems();
		}
	}

	auto FContentBrowserPanel::BeginAssetDragDrop(const FContentBrowserItem& Item) -> void
	{
		if (Item.Kind != EContentBrowserItemKind::Asset || !ImGui::BeginDragDropSource()) return;
		FContentBrowserAssetPayload Payload;
		std::memcpy(Payload.AssetPath.data(), Item.VirtualPath.data(), std::min(Item.VirtualPath.size(), Payload.AssetPath.size() - 1));
		std::memcpy(Payload.AssetClassName.data(), Item.AssetClassName.data(), std::min(Item.AssetClassName.size(), Payload.AssetClassName.size() - 1));
		ImGui::SetDragDropPayload(ContentBrowserAssetPayloadType, &Payload, sizeof(Payload));
		ImGui::Text("%s %s", ItemIcon(Item), Item.Name.c_str());
		ImGui::EndDragDropSource();
	}

	auto FContentBrowserPanel::AcceptAssetDrop(std::string_view DestinationDirectory, bool bPhysicalDirectory) -> void
	{
		if (!ImGui::BeginDragDropTarget()) return;
		if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(ContentBrowserAssetPayloadType); Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(FContentBrowserAssetPayload))
		{
			const auto* AssetPayload = static_cast<const FContentBrowserAssetPayload*>(Payload->Data);
			FAssetPath OldPath;
			if (FAssetPath::TryCreate(AssetPayload->AssetPath.data(), OldPath))
			{
				std::string Destination = bPhysicalDirectory ? PhysicalToVirtualDirectory(DestinationDirectory) : std::string(DestinationDirectory);
				if (Destination.empty())
				{
					ImGui::EndDragDropTarget();
					return;
				}
				if (!Destination.ends_with('/')) Destination += '/';
				FAssetPath NewPath;
				if (FAssetPath::TryCreate(Destination + std::string(OldPath.GetAssetName()), NewPath) && NewPath != OldPath)
				{
					const FEditorAssetMove Move{OldPath, NewPath};
					const Asset::FAssetResult Result = MoveAssets(std::span{&Move, 1});
					if (!Result)
						SetError(Result.Message);
					else
						Refresh(true);
				}
			}
		}
		ImGui::EndDragDropTarget();
	}

	auto FContentBrowserPanel::DrawBackgroundContextMenu() -> void
	{
		const bool bBackgroundHovered = !bContentItemHovered && !bRenameEditorHovered && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
		if (bBackgroundHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			Selection.clear();
			SelectionAnchor.clear();
		}
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			// Lock the click origin before an item popup can obscure its item and make the
			// release frame look like a background interaction.
			bBackgroundContextPending = bBackgroundHovered;
			if (bBackgroundContextPending)
			{
				Selection.clear();
				SelectionAnchor.clear();
			}
		}
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		{
			if (bBackgroundContextPending) ImGui::OpenPopup("ContentBrowserBackground");
			bBackgroundContextPending = false;
		}
		if (ImGui::BeginPopup("ContentBrowserBackground"))
		{
			if (ImGui::BeginMenu("Create"))
			{
				DrawCreateMenu(CurrentPhysicalPath, CurrentVirtualPath);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Import"))
			{
				DrawImportMenu(CurrentVirtualPath);
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Refresh", "F5")) Refresh(true);
			if (ImGui::MenuItem("Show Source Files", nullptr, bShowSourceFiles))
			{
				bShowSourceFiles = !bShowSourceFiles;
				RebuildItems();
			}
			if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(CurrentPhysicalPath);
			ImGui::EndPopup();
		}
	}

	auto FContentBrowserPanel::DrawStatusBar() -> void
	{
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MonaImGui::GetUIStyleMetrics().SpacingM);
		ImGui::Text("%zu item%s", Items.size(), Items.size() == 1 ? "" : "s");
		if (!Selection.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("| %zu selected", Selection.size());
		}
		if (!Asset::GetAssetRegistry().GetScanErrors().empty())
		{
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
			ImGui::Text("| %zu asset scan error(s)", Asset::GetAssetRegistry().GetScanErrors().size());
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Asset::GetAssetRegistry().GetScanErrors().front().Message.c_str());
		}
	}

	auto FContentBrowserPanel::OpenItem(const FContentBrowserItem& Item) -> void
	{
		if (Item.Kind == EContentBrowserItemKind::Folder)
		{
			NavigateToPhysical(Item.PhysicalPath);
			return;
		}
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			if (!OpenAsset || !OpenAsset(Item.VirtualPath, Item.AssetClassName)) SetError(std::format("No editor is registered for {} assets.", ItemTypeLabel(Item)));
			return;
		}
#ifdef _WIN32
		const std::wstring WidePath = std::filesystem::path(Item.PhysicalPath).make_preferred().wstring();
		ShellExecuteW(nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOW);
#endif
	}

	auto FContentBrowserPanel::BeginRename(const FContentBrowserItem& Item) -> void
	{
		RenameTarget = Item.StableId();
		RenameBuffer.fill(0);
		const std::string& Initial = Item.Name;
		std::memcpy(RenameBuffer.data(), Initial.data(), std::min(Initial.size(), RenameBuffer.size() - 1));
		bFocusRename = true;
	}

	auto FContentBrowserPanel::DrawRenameEditor(const FContentBrowserItem& Item) -> void
	{
		if (bFocusRename)
		{
			ImGui::SetKeyboardFocusHere();
			bFocusRename = false;
		}
		const bool bSubmit = ImGui::InputText("##Rename", RenameBuffer.data(), RenameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		bRenameEditorHovered = ImGui::IsItemHovered();
		if (bSubmit)
			CommitRename(Item);
		else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			RenameTarget.clear();
	}

	auto FContentBrowserPanel::CommitRename(const FContentBrowserItem& Item) -> bool
	{
		std::string NewName = RenameBuffer.data();
		if (NewName == Item.Name)
		{
			RenameTarget.clear();
			return true;
		}
		if (NewName.empty() || NewName == "." || NewName == ".." || NewName.find_first_of("/\\:*") != std::string::npos)
		{
			SetError("The new name is empty or contains invalid path characters.");
			return false;
		}
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const size_t Slash = Item.VirtualPath.find_last_of('/');
			FAssetPath OldPath, NewPath;
			if (!FAssetPath::TryCreate(Item.VirtualPath, OldPath) || !FAssetPath::TryCreate(Item.VirtualPath.substr(0, Slash + 1) + NewName, NewPath))
			{
				SetError("The resulting asset path is invalid.");
				return false;
			}
			const FEditorAssetMove Move{OldPath, NewPath};
			const Asset::FAssetResult Result = MoveAssets(std::span{&Move, 1});
			if (!Result)
			{
				SetError(Result.Message);
				return false;
			}
			Selection.clear();
			Selection.insert(VirtualToPhysical(NewPath.ToString() + ".dasset"));
		}
		else if (Item.Kind == EContentBrowserItemKind::Folder)
		{
			if (!RenameFolder(Item, NewName)) return false;
		}
		else
		{
			if (IsManagedCompanion(Item))
			{
				SetError("This source file is managed by an asset. Rename or move the asset instead.");
				return false;
			}
			std::filesystem::path Destination = std::filesystem::path(Item.PhysicalPath).parent_path() / NewName;
			if (Item.Kind == EContentBrowserItemKind::SourceFile && Destination.extension().empty()) Destination += Item.Extension;
			if (std::filesystem::exists(Destination))
			{
				SetError("An item with that name already exists.");
				return false;
			}
			std::error_code Ec;
			std::filesystem::rename(Item.PhysicalPath, Destination, Ec);
			if (Ec)
			{
				SetError(std::format("Rename failed: {}", Ec.message()));
				return false;
			}
			Selection.clear();
			Selection.insert(NormalizePath(Destination.generic_string()));
		}
		RenameTarget.clear();
		Refresh(true);
		return true;
	}

	auto FContentBrowserPanel::RenameFolder(const FContentBrowserItem& Item, std::string_view NewName) -> bool
	{
		const std::filesystem::path OldFolder(Item.PhysicalPath);
		const std::filesystem::path NewFolder = OldFolder.parent_path() / std::filesystem::path(NewName);
		if (std::filesystem::exists(NewFolder))
		{
			SetError("A folder with that name already exists.");
			return false;
		}
		const std::string OldVirtual = PhysicalToVirtualDirectory(OldFolder.generic_string());
		const std::string NewVirtual = PhysicalToVirtualDirectory(NewFolder.generic_string());
		if (OldVirtual.empty() || NewVirtual.empty())
		{
			SetError("Folder moves must stay inside the same mounted content root.");
			return false;
		}

		std::vector<FEditorAssetMove> Moves;
		std::unordered_set<std::string> ManagedFiles;
		std::vector<std::filesystem::path> RelativeDirectories;
		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			if (!PathUtilities::IsLexicalDescendantPath(NormalizePath(Data.PhysicalPath), Item.PhysicalPath, true)) continue;
			if (!Path.GetView().starts_with(OldVirtual))
			{
				SetError("An asset inside the folder has an inconsistent virtual path.");
				return false;
			}
			FAssetPath NewPath;
			if (!FAssetPath::TryCreate(NewVirtual + std::string(Path.GetView().substr(OldVirtual.size())), NewPath))
			{
				SetError("The destination contains an invalid asset path.");
				return false;
			}
			if (Asset::GetAssetRegistry().FindAsset(NewPath) || Asset::FindLoadedPackage(NewPath))
			{
				SetError(std::format("Asset {} already exists.", NewPath.ToString()));
				return false;
			}
			Moves.push_back({Path, NewPath});
			const std::filesystem::path AssetFile(Data.PhysicalPath);
			ManagedFiles.insert(NormalizePath(AssetFile.generic_string()));
			std::error_code Ec;
			for (std::filesystem::directory_iterator It(AssetFile.parent_path(), std::filesystem::directory_options::skip_permission_denied, Ec), End; !Ec && It != End; It.increment(Ec))
				if (It->is_regular_file(Ec) && It->path().stem() == AssetFile.stem()) ManagedFiles.insert(NormalizePath(It->path().generic_string()));
		}

		std::error_code Ec;
		for (std::filesystem::recursive_directory_iterator It(OldFolder, std::filesystem::directory_options::skip_permission_denied, Ec), End; !Ec && It != End; It.increment(Ec))
		{
			if (It->is_directory(Ec)) RelativeDirectories.push_back(std::filesystem::relative(It->path(), OldFolder, Ec));
			if (It->is_regular_file(Ec) && !ManagedFiles.contains(NormalizePath(It->path().generic_string())))
			{
				SetError(std::format("Folder contains an unmanaged file: {}. Move it separately before renaming the folder.", It->path().filename().generic_string()));
				return false;
			}
		}
		if (Ec)
		{
			SetError(std::format("Could not inspect folder contents: {}", Ec.message()));
			return false;
		}

		if (Moves.empty())
		{
			std::filesystem::rename(OldFolder, NewFolder, Ec);
			if (Ec)
			{
				SetError(std::format("Folder rename failed: {}", Ec.message()));
				return false;
			}
			return true;
		}

		const Asset::FAssetResult MoveResult = MoveAssets(Moves);
		if (!MoveResult)
		{
			SetError(MoveResult.Message);
			return false;
		}
		for (const std::filesystem::path& RelativeDirectory : RelativeDirectories)
		{
			Ec.clear();
			std::filesystem::create_directories(NewFolder / RelativeDirectory, Ec);
			if (Ec)
			{
				std::vector<FEditorAssetMove> RollbackMoves;
				RollbackMoves.reserve(Moves.size());
				for (auto RollbackIt = Moves.rbegin(); RollbackIt != Moves.rend(); ++RollbackIt)
					RollbackMoves.push_back({RollbackIt->NewPath, RollbackIt->OldPath});
				const Asset::FAssetResult RollbackResult = MoveAssets(RollbackMoves);
				SetError(std::format("Could not recreate an empty directory: {}{}", Ec.message(), RollbackResult ? "" : std::format(" Asset rollback also failed: {}", RollbackResult.Message)));
				return false;
			}
		}

		std::vector<std::filesystem::path> OldDirectories;
		Ec.clear();
		if (std::filesystem::exists(OldFolder))
		{
			for (std::filesystem::recursive_directory_iterator It(OldFolder, std::filesystem::directory_options::skip_permission_denied, Ec), End; !Ec && It != End; It.increment(Ec))
				if (It->is_directory(Ec)) OldDirectories.push_back(It->path());
			std::ranges::sort(OldDirectories, [](const auto& A, const auto& B) { return A.native().size() > B.native().size(); });
			for (const auto& Directory : OldDirectories)
			{
				Ec.clear();
				std::filesystem::remove(Directory, Ec);
			}
			Ec.clear();
			std::filesystem::remove(OldFolder, Ec);
		}
		return true;
	}

	auto FContentBrowserPanel::IsManagedCompanion(const FContentBrowserItem& Item) const -> bool
	{
		if (Item.Kind != EContentBrowserItemKind::SourceFile) return false;
		const std::filesystem::path Source(Item.PhysicalPath);
		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			const std::filesystem::path AssetFile(Data.PhysicalPath);
			if (NormalizePath(AssetFile.parent_path().generic_string()) == NormalizePath(Source.parent_path().generic_string()) && AssetFile.stem() == Source.stem()) return true;
		}
		return false;
	}

	auto FContentBrowserPanel::CreateFolder(std::string_view PhysicalDirectory) -> void
	{
		const std::string NormalizedDirectory = NormalizePath(PhysicalDirectory);
		if (PhysicalToVirtualDirectory(NormalizedDirectory).empty())
		{
			SetError("Folders can only be created inside a mounted content directory.");
			return;
		}
		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const std::string Name = Suffix == 0 ? "New Folder" : std::format("New Folder ({})", Suffix + 1);
			const std::filesystem::path Path = std::filesystem::path(NormalizedDirectory) / Name;
			if (std::filesystem::exists(Path)) continue;
			std::error_code Ec;
			if (!std::filesystem::create_directory(Path, Ec) || Ec)
			{
				SetError(std::format("Could not create folder: {}", Ec.message()));
				return;
			}
			if (CurrentPhysicalPath != NormalizedDirectory)
				NavigateToPhysical(NormalizedDirectory);
			else
				Refresh(false);
			if (auto It = std::ranges::find_if(Items, [&](const FContentBrowserItem& Item) { return Item.PhysicalPath == NormalizePath(Path.generic_string()); }); It != Items.end())
			{
				Selection.clear();
				Selection.insert(It->StableId());
				BeginRename(*It);
			}
			return;
		}
		SetError("Could not find a unique folder name in this directory.");
	}

	auto FContentBrowserPanel::CreateLevelAsset(std::string_view VirtualDirectory) -> void
	{
		std::string Directory(VirtualDirectory);
		if (!Directory.ends_with('/')) Directory += '/';
		FAssetPath AssetPath;
		bool bFoundPath = false;
		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const std::string Name = Suffix == 0 ? "NewLevel" : std::format("NewLevel{}", Suffix + 1);
			if (!FAssetPath::TryCreate(Directory + Name, AssetPath)) continue;
			if (!Asset::GetAssetRegistry().FindAsset(AssetPath) && !Asset::FindLoadedPackage(AssetPath))
			{
				bFoundPath = true;
				break;
			}
		}
		if (!bFoundPath)
		{
			SetError("Could not find a unique level asset name in this folder.");
			return;
		}

		DLevel* Level = nullptr;
		Asset::FAssetResult Result = Asset::CreateAsset(AssetPath, Level);
		if (!Result || !Level)
		{
			SetError(Result ? "Could not create the level asset." : Result.Message);
			return;
		}
		Result = Asset::SavePackage(Level->GetPackage());
		if (!Result)
		{
			Asset::UnloadPackage(AssetPath);
			SetError(Result.Message);
			return;
		}
		Refresh(true);
		RevealAsset(AssetPath.ToString());
	}

	auto FContentBrowserPanel::CreateMaterialAsset(std::string_view VirtualDirectory, bool bInstance) -> void
	{
		std::string Directory(VirtualDirectory);
		if (!Directory.ends_with('/')) Directory += '/';
		const std::string BaseName = bInstance ? "NewMaterialInstance" : "NewMaterial";
		FAssetPath AssetPath;
		bool bFoundPath = false;
		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const std::string Name = Suffix == 0 ? BaseName : std::format("{}{}", BaseName, Suffix + 1);
			if (!FAssetPath::TryCreate(Directory + Name, AssetPath)) continue;
			if (!Asset::GetAssetRegistry().FindAsset(AssetPath) && !Asset::FindLoadedPackage(AssetPath))
			{
				bFoundPath = true;
				break;
			}
		}
		if (!bFoundPath)
		{
			SetError("Could not find a unique material asset name in this folder.");
			return;
		}

		DMaterialInterface* CreatedMaterial = nullptr;
		Asset::FAssetResult Result;
		if (bInstance)
		{
			DMaterialInstance* Instance = nullptr;
			Result = Asset::CreateAsset(AssetPath, Instance);
			CreatedMaterial = Instance;
		}
		else
		{
			DMaterial* Material = nullptr;
			Result = Asset::CreateAsset(AssetPath, Material);
			CreatedMaterial = Material;
		}
		if (!Result || !CreatedMaterial)
		{
			SetError(Result ? "Could not create the material asset." : Result.Message);
			return;
		}
		Result = Asset::SavePackage(CreatedMaterial->GetPackage());
		if (!Result)
		{
			Asset::UnloadPackage(AssetPath);
			SetError(Result.Message);
			return;
		}
		Refresh(true);
		RevealAsset(AssetPath.ToString());
		if (OpenAsset && !OpenAsset(AssetPath.ToString(), CreatedMaterial->GetClass()->GetQualifiedName().ToString()))
			SetError("The material was created, but its editor could not be opened.");
	}

	auto FContentBrowserPanel::FocusFolderInParent(std::string_view PhysicalDirectory) -> const FContentBrowserItem*
	{
		const std::string NormalizedDirectory = NormalizePath(PhysicalDirectory);
		const std::filesystem::path Parent = std::filesystem::path(NormalizedDirectory).parent_path();
		if (!NavigateToPhysical(Parent.generic_string()))
		{
			SetError("The folder's parent is outside the mounted content roots.");
			return nullptr;
		}
		auto It = std::ranges::find_if(Items, [&](const FContentBrowserItem& Item) {
			return Item.Kind == EContentBrowserItemKind::Folder && Item.PhysicalPath == NormalizedDirectory;
		});
		if (It == Items.end())
		{
			SetError("The folder could not be found after refreshing its parent directory.");
			return nullptr;
		}
		Selection.clear();
		Selection.insert(It->StableId());
		SelectionAnchor = It->StableId();
		return &*It;
	}

	auto FContentBrowserPanel::RequestDeleteSelection() -> void
	{
		if (!Selection.empty()) bDeletePopupRequested = true;
	}

	auto FContentBrowserPanel::DeleteEmptyFolder(const FContentBrowserItem& Item) -> bool
	{
		std::error_code Ec;
		if (!std::filesystem::is_empty(Item.PhysicalPath, Ec))
		{
			SetError("Folders must be empty before they can be deleted. Delete or move their assets first.");
			return false;
		}
		if (!std::filesystem::remove(Item.PhysicalPath, Ec) || Ec)
		{
			SetError(std::format("Could not delete folder: {}", Ec.message()));
			return false;
		}
		return true;
	}

	auto FContentBrowserPanel::DeleteSelection() -> void
	{
		std::vector<FContentBrowserItem> Targets;
		for (const FContentBrowserItem& Item : Items)
			if (Selection.contains(Item.StableId())) Targets.push_back(Item);
		std::ranges::sort(Targets, [](const FContentBrowserItem& A, const FContentBrowserItem& B) { return A.Kind != EContentBrowserItemKind::Folder && B.Kind == EContentBrowserItemKind::Folder; });
		for (const FContentBrowserItem& Item : Targets)
		{
			if (Item.Kind == EContentBrowserItemKind::Asset)
			{
				FAssetPath Path;
				if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
				{
					SetError("Selected asset has an invalid path.");
					break;
				}
				const Asset::FAssetResult Result = Asset::DeleteAsset(Path);
				if (!Result)
				{
					SetError(Result.Message);
					break;
				}
			}
			else if (Item.Kind == EContentBrowserItemKind::Folder)
			{
				if (!DeleteEmptyFolder(Item)) break;
			}
			else
			{
				if (IsManagedCompanion(Item))
				{
					SetError("This source file is managed by an asset. Delete the asset instead.");
					break;
				}
				std::error_code Ec;
				if (!std::filesystem::remove(Item.PhysicalPath, Ec) || Ec)
				{
					SetError(std::format("Could not delete file: {}", Ec.message()));
					break;
				}
			}
		}
		Selection.clear();
		Refresh(true);
	}

	auto FContentBrowserPanel::DrawDialogs() -> void
	{
		if (bDeletePopupRequested)
		{
			ImGui::OpenPopup("Delete Content");
			bDeletePopupRequested = false;
		}
		if (ImGui::BeginPopupModal("Delete Content", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::Text("Permanently delete %zu selected item%s?", Selection.size(), Selection.size() == 1 ? "" : "s");
			ImGui::TextDisabled("Loaded assets will be unloaded automatically. This cannot be undone.");
			bool bBlocked = false;
			bool bWillUnload = false;
			for (const FContentBrowserItem& Item : Items)
			{
				if (!Selection.contains(Item.StableId()) || Item.Kind != EContentBrowserItemKind::Asset) continue;
				FAssetPath Path;
				Asset::FAssetDeleteAnalysis Analysis;
				if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
				{
					bBlocked = true;
					ImGui::TextWrapped("%s has an invalid asset path.", Item.Name.c_str());
					continue;
				}

				const Asset::FAssetResult Result = Asset::AnalyzeAssetDeletion(Path, Analysis);
				if (!Result)
				{
					bBlocked = true;
					ImGui::TextWrapped("%s: %s", Item.Name.c_str(), Result.Message.c_str());
				}
				else if (Analysis.bLoading)
				{
					bBlocked = true;
					ImGui::TextWrapped("%s is currently loading. Try again when loading finishes.", Item.Name.c_str());
				}
				else if (!Analysis.DirectReferencers.empty())
				{
					bBlocked = true;
					ImGui::TextWrapped("%s is referenced by:", Item.Name.c_str());
					const size_t VisibleReferencerCount = std::min<size_t>(Analysis.DirectReferencers.size(), 4);
					for (size_t Index = 0; Index < VisibleReferencerCount; ++Index)
					{
						ImGui::BulletText("%s", Analysis.DirectReferencers[Index].ToString().c_str());
					}
					if (VisibleReferencerCount < Analysis.DirectReferencers.size())
						ImGui::TextDisabled("... and %zu more", Analysis.DirectReferencers.size() - VisibleReferencerCount);
					ImGui::TextDisabled("Remove the references and save those assets before deleting.");
				}
				else if (Analysis.bLoaded)
				{
					bWillUnload = true;
					ImGui::TextWrapped("%s is loaded and will be unloaded before deletion.", Item.Name.c_str());
				}
			}
			ImGui::BeginDisabled(bBlocked);
			if (MonaImGui::DialogButton(bWillUnload ? "Unload & Delete" : "Delete"))
			{
				DeleteSelection();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (MonaImGui::DialogButton("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (!ErrorMessage.empty()) ImGui::OpenPopup("Content Browser Error");
		if (ImGui::BeginPopupModal("Content Browser Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextWrapped("%s", ErrorMessage.c_str());
			if (MonaImGui::DialogButton("OK"))
			{
				ErrorMessage.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	auto FContentBrowserPanel::RevealAsset(std::string_view AssetPath) -> void
	{
		FAssetPath Path;
		if (!FAssetPath::TryCreate(AssetPath, Path)) return;
		const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(Path);
		if (!Data) return;
		NavigateToPhysical(std::filesystem::path(Data->PhysicalPath).parent_path().generic_string());
		Selection.clear();
		Selection.insert(NormalizePath(Data->PhysicalPath));
	}

	auto FContentBrowserPanel::ItemTypeLabel(const FContentBrowserItem& Item) const -> std::string
	{
		if (Item.Kind == EContentBrowserItemKind::Folder) return "Folder";
		if (Item.Kind == EContentBrowserItemKind::Asset) return ClassLeaf(Item.AssetClassName);
		return Item.Extension.empty() ? "Source File" : Item.Extension.substr(1) + " file";
	}

	auto FContentBrowserPanel::ItemIcon(const FContentBrowserItem& Item) const -> const char*
	{
		if (Item.Kind == EContentBrowserItemKind::Folder) return Icons::Folder;
		if (Item.Kind == EContentBrowserItemKind::SourceFile) return Icons::File;
		const std::string Type = ItemTypeLabel(Item);
		if (Type == "StaticMesh") return Icons::Cube;
		if (Type == "Level") return Icons::Home;
		return Icons::FileLines;
	}

	auto FContentBrowserPanel::FormatFileSize(uintmax_t Bytes) const -> std::string
	{
		if (Bytes < 1024) return std::format("{} B", Bytes);
		if (Bytes < 1024 * 1024) return std::format("{:.1f} KB", Bytes / 1024.0);
		if (Bytes < 1024ull * 1024ull * 1024ull) return std::format("{:.1f} MB", Bytes / (1024.0 * 1024.0));
		return std::format("{:.2f} GB", Bytes / (1024.0 * 1024.0 * 1024.0));
	}

	auto FContentBrowserPanel::FormatFileTime(const std::filesystem::file_time_type& Time) const -> std::string
	{
		if (Time == std::filesystem::file_time_type{}) return "-";
		const auto SysTime = std::chrono::clock_cast<std::chrono::system_clock>(Time);
		const std::time_t TimeT = std::chrono::system_clock::to_time_t(SysTime);
		std::tm LocalTime{};
		localtime_s(&LocalTime, &TimeT);
		std::array<char, 32> Buffer{};
		std::strftime(Buffer.data(), Buffer.size(), "%Y-%m-%d %H:%M", &LocalTime);
		return Buffer.data();
	}

	auto FContentBrowserPanel::ShowInExplorer(std::string_view PhysicalPath) const -> void
	{
#ifdef _WIN32
		const std::filesystem::path Path(PhysicalPath);
		std::filesystem::path PreferredPath = Path;
		const std::wstring WidePath = PreferredPath.make_preferred().wstring();
		if (std::filesystem::is_directory(Path))
			ShellExecuteW(nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOW);
		else
		{
			const std::wstring Args = L"/select,\"" + WidePath + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", Args.c_str(), nullptr, SW_SHOW);
		}
#endif
	}

	auto FContentBrowserPanel::CopyToClipboard(std::string_view Text) const -> void { ImGui::SetClipboardText(std::string(Text).c_str()); }
	auto FContentBrowserPanel::SetError(std::string Message) -> void { ErrorMessage = std::move(Message); }
} // namespace Durin
