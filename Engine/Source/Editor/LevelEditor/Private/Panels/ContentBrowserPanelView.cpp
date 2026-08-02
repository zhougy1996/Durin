#include "Panels/ContentBrowserPanel.h"

#include "StaticMesh/StaticMesh.h"
#include "AssetImportCore.h"
#include "MultiOutputImport.h"
#include "Panels/ContentBrowserItemView.h"

#include "AssetSystem.h"
#include "Assets/ContentBrowserDragDrop.h"
#include "Assets/ContentBrowserThumbnailCache.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Icons/FontAwesomeIcons.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorHelpers.h"
#include "Workspace/LevelEditorUILayout.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Misc/Paths.h"
#include "Misc/LexicalPath.h"
#include "MonaImGui.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Math/Vector.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	using ContentBrowserItemView::FGridMetrics;

	namespace
	{
		using LevelEditorHelpers::DrawToolbarIconButton;

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

		auto MakeRenderedThumbnailFingerprint(const FContentBrowserItem& Item)
			-> std::optional<FAssetThumbnailPackageFingerprint>
		{
			if (!Item.ThumbnailSourcePath.empty()) return std::nullopt;
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Item.VirtualPath, Path)) return std::nullopt;
			return FAssetThumbnailPackageFingerprint{
				.VirtualPath = std::move(Path),
				.AssetClassName = Item.AssetClassName,
				.PackageFormatVersion = Item.ThumbnailPackageFormatVersion,
				.FileSize = static_cast<uint64>(Item.ThumbnailFileSize),
				.LastWriteTimeTicks = Item.ThumbnailLastWriteTimeTicks};
		}

		constexpr ImGuiTableFlags DetailsTableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings;
		constexpr float FullToolbarWidth = 900.0f;
		constexpr float CompactToolbarWidth = 620.0f;
		constexpr float MinimumTreeWidth = 145.0f;
		constexpr float MinimumContentWidth = 240.0f;
		constexpr float MinimumContentHeight = 80.0f;
	} // namespace

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
		// Context-menu actions can rebuild items, so execute them only after both browser panes
		// have finished traversing their current frame snapshots.
		if (DeferredContentAction)
		{
			auto Action = std::move(DeferredContentAction);
			DeferredContentAction = {};
			Action();
		}
		DrawStatusBar();
		DrawDialogs();

		SessionSettings.SetContentBrowserState(static_cast<uint8>(ViewMode), IconSize, bIconSizeLocked, DirectoryTreeWidth, Model.IsShowingSourceFiles(), Model.GetCurrentPhysicalPath());
		ImGui::End();
	}

	auto FContentBrowserPanel::DrawToolbar() -> void
	{
		int32 TypeFilter = Model.GetTypeFilter();
		const float ToolbarWidth = ImGui::GetContentRegionAvail().x;
		const EEditorUILayoutMode LayoutMode = ResolveEditorUILayout(ToolbarWidth, MonaImGui::ScaleUI(CompactToolbarWidth), MonaImGui::ScaleUI(FullToolbarWidth));
		const bool bFullLayout = LayoutMode == EEditorUILayoutMode::Full;
		const bool bCompactLayout = LayoutMode != EEditorUILayoutMode::Narrow;

		ImGui::BeginDisabled(Model.GetHistoryIndex() <= 0);
		if (DrawToolbarIconButton(Icons::ArrowLeft, "ContentBrowserBack")) NavigateHistory(-1);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
		ImGui::SameLine();
		ImGui::BeginDisabled(Model.GetHistoryIndex() < 0 || static_cast<size_t>(Model.GetHistoryIndex() + 1) >= Model.GetHistory().size());
		if (DrawToolbarIconButton(Icons::ArrowRight, "ContentBrowserForward")) NavigateHistory(1);
		ImGui::EndDisabled();
		ImGui::SameLine();
		const std::filesystem::path Parent = std::filesystem::path(Model.GetCurrentPhysicalPath()).parent_path();
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
			for (const FContentBrowserModel::FMountSnapshot& Mount : Model.GetMounts())
			{
				std::filesystem::path Relative;
				if (!PathUtilities::TryMakeLexicalRelativePath(Model.GetCurrentPhysicalPath(), Mount.PhysicalRoot, Relative)) continue;
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
						DrawPathSegment(Segment.generic_string(), SegmentPath, Model.GetCurrentPhysicalPath() == NormalizePath(SegmentPath));
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
					if (ImGui::Combo("##CompactContentTypeFilter", &TypeFilter, Filters, std::size(Filters))) Model.SetTypeFilter(TypeFilter);
					ImGui::Separator();
				}
				ImGui::TextDisabled("Thumbnail size");
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::SliderFloat("##ContentIconSize", &IconSize, FLevelEditorSessionSettings::MinimumContentBrowserIconSize, FLevelEditorSessionSettings::MaximumContentBrowserIconSize, "%.0f px");
				ImGui::Checkbox("Lock Ctrl + wheel resizing", &bIconSizeLocked);
				ImGui::Separator();
				if (ImGui::MenuItem("Show Source Files", nullptr, Model.IsShowingSourceFiles()))
				{
					Model.SetShowSourceFiles(!Model.IsShowingSourceFiles());
					RepairSelection();
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
			if (ImGui::Combo("##ContentTypeFilter", &TypeFilter, Filters, std::size(Filters))) Model.SetTypeFilter(TypeFilter);
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
		for (const FContentBrowserModel::FMountSnapshot& Mount : Model.GetMounts())
		{
			if (!std::filesystem::is_directory(Mount.PhysicalRoot)) continue;
			std::string Label = Mount.VirtualRoot;
			if (Label.starts_with('/')) Label.erase(Label.begin());
			if (Label.ends_with('/')) Label.pop_back();
			DrawDirectoryNode(std::filesystem::path(Mount.PhysicalRoot), Label, true);
		}
		if (ImGui::BeginPopupContextWindow("ContentBrowserTreeBackground", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			const bool bCurrentIsMountRoot = std::ranges::any_of(Model.GetMounts(), [&](const FContentBrowserModel::FMountSnapshot& Mount) {
				return Mount.PhysicalRoot == Model.GetCurrentPhysicalPath();
			});
			DrawDirectoryContextMenu(Model.GetCurrentPhysicalPath(), bCurrentIsMountRoot);
			ImGui::EndPopup();
		}
	}

	auto FContentBrowserPanel::DrawDirectoryNode(const std::filesystem::path& Path, std::string_view Label, bool bMountRoot) -> void
	{
		const std::string Physical = NormalizePath(Path.generic_string());
		// Recursing can grow and rehash the model cache, so keep this node's traversal independent.
		const std::span<const std::filesystem::path> CachedChildren =
			Model.GetDirectoryChildren(Physical);
		const std::vector<std::filesystem::path> Children(
			CachedChildren.begin(), CachedChildren.end());
		const bool bHasChildren = std::ranges::any_of(Children, [&](const std::filesystem::path& Child) { return Model.IsShowingSourceFiles() || !Child.filename().generic_string().starts_with('.'); });
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (!bHasChildren) Flags |= ImGuiTreeNodeFlags_Leaf;
		if (Model.GetCurrentPhysicalPath() == Physical) Flags |= ImGuiTreeNodeFlags_Selected;
		if (bMountRoot) Flags |= ImGuiTreeNodeFlags_DefaultOpen;
		const std::string NodeLabel = std::format("{} {}##{}", Model.GetCurrentPhysicalPath() == Physical ? Icons::FolderOpen : Icons::Folder, Label, Physical);
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
				if (!Model.IsShowingSourceFiles() && Name.starts_with('.')) continue;
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
		if (Model.GetItems().empty())
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
			if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_N)) CreateFolder(Model.GetCurrentPhysicalPath());
			if (ImGui::IsKeyPressed(ImGuiKey_F5)) Refresh(true);
			if (ImGui::IsKeyPressed(ImGuiKey_Enter) && Selection.size() == 1)
				if (auto It = std::ranges::find_if(Model.GetItems(), [&](const FContentBrowserItem& Item) { return Selection.contains(Item.StableId()); }); It != Model.GetItems().end()) OpenItem(*It);
			if (ImGui::IsKeyPressed(ImGuiKey_F2) && Selection.size() == 1)
				if (auto It = std::ranges::find_if(Model.GetItems(), [&](const FContentBrowserItem& Item) { return Selection.contains(Item.StableId()); }); It != Model.GetItems().end()) BeginRename(*It);
			if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !Selection.empty()) RequestDeleteSelection();
		}
	}

	auto FContentBrowserPanel::DrawSelectionDetails() -> void
	{
		auto It = std::ranges::find_if(Model.GetItems(), [&](const FContentBrowserItem& Item) { return Selection.contains(Item.StableId()); });
		if (It == Model.GetItems().end()) return;
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
				if (ClassLeaf(Item.AssetClassName) == "TextureCube")
				{
					FAssetPath CubePath;
					DTextureCube* Cube = nullptr;
					if (FAssetPath::TryCreate(Item.VirtualPath, CubePath) && Asset::LoadAsset(CubePath, Cube) && Cube)
					{
						const bool bPanorama =
							Cube->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama;
						Row("Source Layout", bPanorama ? "Equirectangular Panorama" : "Six Faces");
						if (bPanorama)
						{
							Row("Source", Cube->GetPanoramaSourceFile());
							Row("Source Size", std::format("{}x{}", Cube->GetOriginalSourceWidth(),
								Cube->GetOriginalSourceHeight()));
							Row("Face Override", Cube->GetPanoramaFaceDimension() == 0
								? "Auto" : std::format("{} px", Cube->GetPanoramaFaceDimension()));
							const bool bHDR = std::filesystem::path(Cube->GetPanoramaSourceFile())
								.extension().generic_string() == ".hdr";
							Row("Input Range", bHDR ? "Radiance HDR" : "LDR");
							if (bHDR) Row("Exposure", std::format("{:+.1f} EV", Cube->GetPanoramaExposureEV()));
						}
						if (const FTextureCubePlatformData* Platform = Cube->GetPlatformData();
							Platform && Platform->IsValid())
						{
							Row("Dimensions", std::format("{}x{}", Platform->Faces[0].Mips[0].Width,
								Platform->Faces[0].Mips[0].Height));
							Row("Faces", std::format("{}", TextureCubeFaceCount));
							Row("Mips", std::format("{}", Platform->Faces[0].Mips.size()));
							Row("Output", std::format("{} (LDR)", GetPixelFormatInfo(Platform->PixelFormat).Name));
						}
						else
						{
							Row("Build", Cube->GetLastBuildError().empty()
								? "Source data is unavailable." : Cube->GetLastBuildError());
						}
					}
				}
			}
			ImGui::EndTable();
		}
	}

	auto FContentBrowserPanel::DrawGrid() -> void
	{
		const FGridMetrics Metrics =
			FGridMetrics::FromPreviewExtent(IconSize);
		const float LayoutSentinelHeight = 1.0f;
		const int32 Columns = std::max(
			1,
			static_cast<int32>(ImGui::GetContentRegionAvail().x / Metrics.CellWidth));
		const int32 RowCount = static_cast<int32>((Model.GetItems().size() + static_cast<size_t>(Columns) - 1) / static_cast<size_t>(Columns));
		if (!ImGui::BeginTable("ContentBrowserGrid", Columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY)) return;

		ImGuiListClipper Clipper;
		Clipper.Begin(RowCount, Metrics.RowHeight);
		while (Clipper.Step())
		{
			const int32 PrefetchStart = std::max(0, Clipper.DisplayStart - 2);
			const int32 PrefetchEnd = std::min(RowCount, Clipper.DisplayEnd + 2);
			for (int32 Row = PrefetchStart; Row < PrefetchEnd; ++Row)
				for (int32 Column = 0; Column < Columns; ++Column)
				{
					const size_t Index = static_cast<size_t>(Row) * Columns + Column;
					if (Index >= Model.GetItems().size()) break;
					const FContentBrowserItem& Item = Model.GetItems()[Index];
					if (!Item.ThumbnailIdentity.empty())
						ThumbnailCache->Request({
							.Identity = Item.ThumbnailIdentity,
							.SourcePhysicalPath = Item.ThumbnailSourcePath,
							.SourceFileSize = Item.ThumbnailFileSize,
							.SourceLastWriteTime = Item.ThumbnailLastWriteTime,
							.Asset = MakeRenderedThumbnailFingerprint(Item),
							.Priority = Row >= Clipper.DisplayStart && Row < Clipper.DisplayEnd
								? EAssetThumbnailPriority::Visible
								: EAssetThumbnailPriority::Prefetch});
				}

			for (int32 Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				ImGui::TableNextRow(ImGuiTableRowFlags_None, Metrics.RowHeight);
				for (int32 Column = 0; Column < Columns; ++Column)
				{
					const size_t Index = static_cast<size_t>(Row) * Columns + Column;
					if (Index >= Model.GetItems().size()) break;
					const FContentBrowserItem& Item = Model.GetItems()[Index];
					ImGui::TableSetColumnIndex(Column);
					ImGui::PushID(Item.StableId().c_str());
					const ImVec2 TileSize(
						ImGui::GetContentRegionAvail().x,
						Metrics.TileHeight);
					const bool bSelected = Selection.contains(Item.StableId());
					const ImVec2 TileStart = ImGui::GetCursorScreenPos();
					const ImVec2 PreviewMin = Metrics.PreviewMin(TileStart, TileSize);
					const ImVec2 PreviewMax = Metrics.PreviewMax(TileStart, TileSize);
					const ImVec2 NameMin = Metrics.NameMin(TileStart, TileSize);
					const ImVec2 NameMax = Metrics.NameMax(TileStart, TileSize);
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
					const ImVec2 CardMin = Metrics.CardMin(TileStart);
					const ImVec2 CardMax = Metrics.CardMax(TileStart, TileSize);
					if (bSelected || bHovered)
					{
						const ImGuiCol BackgroundColor = bSelected ? (bHovered ? ImGuiCol_HeaderActive : ImGuiCol_Header) : ImGuiCol_HeaderHovered;
						DrawList->AddRectFilled(
							CardMin,
							CardMax,
							ImGui::GetColorU32(BackgroundColor),
							Metrics.CardRounding);
						if (bSelected)
							DrawList->AddRect(
								CardMin,
								CardMax,
								ImGui::GetColorU32(ImGuiCol_CheckMark),
								Metrics.CardRounding,
								0,
								MonaImGui::ScaleUI(1.0f));
					}

					const FAssetThumbnailView Thumbnail = ThumbnailCache->Find(Item.ThumbnailIdentity);
					const ContentBrowserItemView::EThumbnailPresentation ThumbnailPresentation =
						ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail);
					const bool bDrewThumbnail = ContentBrowserItemView::DrawThumbnail(
						Item,
						Thumbnail,
						Metrics,
						PreviewMin,
						PreviewMax,
						CursorAfterTile);
					if (!bDrewThumbnail)
						ContentBrowserItemView::DrawFallbackIcon(
							Item,
							ThumbnailPresentation,
							Metrics,
							PreviewMin,
							PreviewMax);

					if (RenameTarget == Item.StableId())
					{
						ImGui::SetCursorScreenPos(NameMin);
						ImGui::SetNextItemWidth(
							std::max(MonaImGui::ScaleUI(40.0f), NameMax.x - NameMin.x));
						DrawRenameEditor(Item);
					}
					else
						ContentBrowserItemView::DrawLabel(
							Item, Metrics, NameMin, NameMax);
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
						if (Thumbnail.State == EAssetThumbnailState::Failed && !Thumbnail.Diagnostic.empty())
						{
							ImGui::TextDisabled("Preview");
							ImGui::SameLine();
							ImGui::TextWrapped("%s", Thumbnail.Diagnostic.c_str());
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
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, static_cast<ImGuiID>(EContentBrowserSortColumn::Name));
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(130.0f), static_cast<ImGuiID>(EContentBrowserSortColumn::Type));
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(85.0f), static_cast<ImGuiID>(EContentBrowserSortColumn::Size));
		ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(145.0f), static_cast<ImGuiID>(EContentBrowserSortColumn::Modified));
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs(); Specs && Specs->SpecsDirty && Specs->SpecsCount > 0)
		{
			Model.SetSort(
				static_cast<EContentBrowserSortColumn>(Specs->Specs[0].ColumnUserID),
				Specs->Specs[0].SortDirection != ImGuiSortDirection_Descending);
			Specs->SpecsDirty = false;
		}
		for (size_t Index = 0; Index < Model.GetItems().size(); ++Index)
		{
			const FContentBrowserItem& Item = Model.GetItems()[Index];
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
		bool bManagedByRecord = false;
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			FAssetPath ItemPath;
			AssetImport::FImportRecordInspection Inspection;
			if (FAssetPath::TryCreate(Item.VirtualPath, ItemPath))
			{
				Inspection = Item.AssetClassName ==
					AssetImport::DImportRecord::StaticClass()->GetQualifiedName().ToString()
					? AssetImport::InspectImportRecord(
						ItemPath, AssetImport::GetImportRecordIndex())
					: AssetImport::InspectImportRecordForOutput(
						ItemPath, AssetImport::GetImportRecordIndex());
			}
			if (Inspection && Inspection.Record)
			{
				bManagedByRecord = Inspection.SelectedOutputPath.IsValid();
				if (bManagedByRecord && ImGui::MenuItem("Reveal Import Record"))
					DeferredContentAction = [this, Path = Inspection.RecordPath.ToString()] {
						RevealAsset(Path);
					};
				if (ImGui::BeginMenu("Reveal Managed Output"))
				{
					for (const AssetImport::FImportRecordManagement& Output : Inspection.Outputs)
					{
						const auto Recorded = std::ranges::find(
							Inspection.Record->GetOutputs(), Output.OutputIdentity,
							&AssetImport::FImportRecordOutput::StableIdentity);
						if (Recorded != Inspection.Record->GetOutputs().end()
							&& ImGui::MenuItem(Recorded->AssetPath.GetAssetName().data()))
							DeferredContentAction = [this, Path = Recorded->AssetPath.ToString()] {
								RevealAsset(Path);
							};
					}
					ImGui::EndMenu();
				}
				const AssetImport::FImportRecordCapabilitySet Capabilities =
					AssetImport::QueryImportRecordCapabilities(
						Inspection, AssetImport::GetImportRecordHandlerRegistry());
				for (const AssetImport::EImportRecordAction Action : {
					AssetImport::EImportRecordAction::Reimport,
					AssetImport::EImportRecordAction::RecreateMissingOutputs,
					AssetImport::EImportRecordAction::RepairManagedOutputs})
				{
					const AssetImport::FImportRecordCapability* Capability = Capabilities.Find(Action);
					if (!Capability) continue;
					if (ImGui::MenuItem(Capability->Label.c_str(), nullptr, false, Capability->bAvailable))
						DeferredContentAction = [this, Item, Action] { ReimportAsset(Item, Action); };
					if (!Capability->bAvailable && ImGui::IsItemHovered()
						&& !Capability->Diagnostics.empty())
						ImGui::SetTooltip("%s", Capability->Diagnostics.back().Message.c_str());
				}
				if (bManagedByRecord && ImGui::MenuItem("Detach from Import Record"))
				{
					const auto Managed = std::ranges::find_if(
						Inspection.Record->GetOutputs(), [&](const AssetImport::FImportRecordOutput& Output) {
							return Output.AssetPath == Inspection.SelectedOutputPath;
						});
					if (Managed != Inspection.Record->GetOutputs().end())
						DeferredContentAction = [this,
							RecordPath = Inspection.RecordPath,
							Identity = Managed->StableIdentity] {
							AssetImport::DImportRecord* Record = nullptr;
							const Asset::FAssetResult Load = Asset::LoadAsset(RecordPath, Record);
							if (!Load || !Record) { SetError(Load ? "Import record is unavailable." : Load.Message); return; }
							const AssetImport::FImportRecordEditResult Result =
								AssetImport::DetachImportRecordOutput(
									*Record, Identity, AssetImport::GetImportRecordIndex());
							if (!Result) { SetError(Result.Message); return; }
							Refresh(true);
							RevealAsset(Result.RevealPath.ToString());
						};
				}
				if (Inspection.bConflicted && ImGui::MenuItem("Repair Record Identity"))
					DeferredContentAction = [this, RecordPath = Inspection.RecordPath] {
						AssetImport::DImportRecord* Record = nullptr;
						const Asset::FAssetResult Load = Asset::LoadAsset(RecordPath, Record);
						if (!Load || !Record) { SetError(Load ? "Import record is unavailable." : Load.Message); return; }
						const AssetImport::FImportRecordEditResult Result =
							AssetImport::RepairDuplicatedImportRecord(
								*Record, AssetImport::GetImportRecordIndex());
						if (!Result) { SetError(Result.Message); return; }
						Refresh(true);
						RevealAsset(Result.RevealPath.ToString());
					};
				ImGui::Separator();
			}
		}
		if (Item.Kind == EContentBrowserItemKind::Asset
			&& !bManagedByRecord
			&& AssetImport::GetSingleAssetHandlerRegistry().Find(Item.AssetClassName))
		{
			FAssetPath CapabilityPath;
			DObject* CapabilityAsset = nullptr;
			const bool bLoadedForCapabilities = FAssetPath::TryCreate(Item.VirtualPath, CapabilityPath)
				&& Asset::FAssetManager::Get().LoadAsset(CapabilityPath, CapabilityAsset);
			const AssetImport::FSingleAssetCapability* ReimportCapability = nullptr;
			AssetImport::FSingleAssetCapabilitySet CapabilitySet;
			if (bLoadedForCapabilities && CapabilityAsset)
			{
				CapabilitySet = AssetImport::QuerySingleAssetCapabilities(
					*CapabilityAsset, AssetImport::GetProviderRegistry(),
					AssetImport::GetSingleAssetHandlerRegistry());
				ReimportCapability = CapabilitySet.Find(
					AssetImport::ESingleAssetImportCapability::ReimportCurrentSource);
			}
			const bool bCanSingleReimport = ReimportCapability && ReimportCapability->bAvailable;
			if (ImGui::MenuItem(
				bCanSingleReimport ? ReimportCapability->Label.c_str() : "Reimport from Current Source",
				nullptr, false, bCanSingleReimport))
				DeferredContentAction = [this, Item] {
					ReimportAsset(Item, AssetImport::EImportRecordAction::Reimport);
				};
			if (ReimportCapability && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", ReimportCapability->ReplacedStateDescription.c_str());
			if (!bCanSingleReimport && ReimportCapability && ImGui::IsItemHovered()
				&& !ReimportCapability->Diagnostics.empty())
				ImGui::SetTooltip("%s", ReimportCapability->Diagnostics.back().Message.c_str());
			if (Item.AssetClassName == DStaticMesh::StaticClass()->GetQualifiedName().ToString()
				&& !bCanSingleReimport
				&& ImGui::MenuItem("Reimport Legacy Scene and Recreate Missing Outputs"))
				DeferredContentAction = [this, Item] {
					ReimportAsset(Item, AssetImport::EImportRecordAction::RecreateMissingOutputs);
				};
			if (!LastReimportOrphans.empty()
				&& ImGui::BeginMenu("Reveal Last Reimport Orphan"))
			{
				for (const FAssetPath& Orphan : LastReimportOrphans)
				{
					if (ImGui::MenuItem(Orphan.GetAssetName().data()))
						DeferredContentAction = [this, Path = Orphan.ToString()] {
							RevealAsset(Path);
						};
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
		}
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
		if (ImGui::MenuItem("Texture Cube..."))
			DeferredContentAction = [this, Directory = std::string(VirtualDirectory)] { RequestImport(Directory, EContentBrowserImportType::TextureCube); };
		if (ImGui::MenuItem("Static Mesh..."))
			DeferredContentAction = [this, Directory = std::string(VirtualDirectory)] { RequestImport(Directory, EContentBrowserImportType::StaticMesh); };
		ImGui::EndDisabled();
	}

	auto FContentBrowserPanel::DrawDirectoryContextMenu(std::string_view PhysicalDirectory, bool bMountRoot) -> void
	{
		const std::string VirtualDirectory = PhysicalToVirtualDirectory(PhysicalDirectory);
		const bool bIsCurrent = NormalizePath(PhysicalDirectory) == Model.GetCurrentPhysicalPath();
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
		if (ImGui::MenuItem("Show Source Files", nullptr, Model.IsShowingSourceFiles()))
		{
			Model.SetShowSourceFiles(!Model.IsShowingSourceFiles());
			RepairSelection();
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
					const Asset::FAssetResult Result = Operations.Move(std::span{&Move, 1});
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
				DrawCreateMenu(Model.GetCurrentPhysicalPath(), Model.GetCurrentVirtualPath());
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Import"))
			{
				DrawImportMenu(Model.GetCurrentVirtualPath());
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Refresh", "F5")) Refresh(true);
			if (ImGui::MenuItem("Show Source Files", nullptr, Model.IsShowingSourceFiles()))
			{
				Model.SetShowSourceFiles(!Model.IsShowingSourceFiles());
				RepairSelection();
			}
			if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(Model.GetCurrentPhysicalPath());
			ImGui::EndPopup();
		}
	}

	auto FContentBrowserPanel::DrawStatusBar() -> void
	{
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MonaImGui::GetUIStyleMetrics().SpacingM);
		ImGui::Text("%zu item%s", Model.GetItems().size(), Model.GetItems().size() == 1 ? "" : "s");
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
			for (const FContentBrowserItem& Item : Model.GetItems())
			{
				if (!Selection.contains(Item.StableId()) || Item.Kind != EContentBrowserItemKind::Asset) continue;
				const auto ErrorIt = std::ranges::find(DeleteAnalysisErrors, Item.StableId(),
					&std::pair<std::string, Asset::FAssetResult>::first);
				if (ErrorIt != DeleteAnalysisErrors.end())
				{
					bBlocked = true;
					ImGui::TextWrapped("%s: %s", Item.Name.c_str(), ErrorIt->second.Message.c_str());
					continue;
				}
				const auto AnalysisIt = std::ranges::find(DeleteAnalysis, Item.StableId(),
					&std::pair<std::string, Asset::FAssetDeleteAnalysis>::first);
				if (AnalysisIt == DeleteAnalysis.end())
				{
					bBlocked = true;
					ImGui::TextWrapped("%s could not be analyzed.", Item.Name.c_str());
					continue;
				}
				const Asset::FAssetDeleteAnalysis& Analysis = AnalysisIt->second;
				if (!Analysis.Warning.empty())
					ImGui::TextWrapped("%s: Warning: %s", Item.Name.c_str(), Analysis.Warning.c_str());
				if (Analysis.bLoading)
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

} // namespace Durin
