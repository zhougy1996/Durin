#include "Panels/ContentBrowserPanel.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "DObject/Package.h"
#include "Panels/ContentBrowserItemView.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Asset.h"
#include "AssetRegistry/References.h"
#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Editor/AssetDragDrop.h"
#include "Editor/WorkspaceUI.h"
#include "Icons/FontAwesomeIcons.h"
#include "Misc/Paths.h"
#include "MonaImGui.h"
#include "MonaImGuiWidgets.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "Math/Vector.h"

namespace Durin::Editor::ContentBrowser::Private
{
	using ContentBrowserItemView::FGridMetrics;

	namespace
	{
		using ContentBrowserFilesystem::NormalizePath;
		using ContentBrowserItemView::ClassLeaf;

		enum class EToolbarLayout : uint8
		{
			Narrow,
			Compact,
			Full,
		};

		auto DrawToolbarIconButton(const char* Icon, const char* Id) -> bool
		{
			return MonaImGui::ToolbarIconButton(Icon, Id);
		}

		constexpr auto ResolveToolbarLayout(
			float AvailableWidth, float CompactMinimumWidth,
			float FullMinimumWidth) -> EToolbarLayout
		{
			if (AvailableWidth >= FullMinimumWidth) return EToolbarLayout::Full;
			if (AvailableWidth >= CompactMinimumWidth) return EToolbarLayout::Compact;
			return EToolbarLayout::Narrow;
		}

		auto MakeThumbnailFingerprint(const FContentBrowserItem& Item)
			-> std::optional<::Durin::Editor::FAssetThumbnailPackageFingerprint>
		{
			if (!Item.ThumbnailSourcePath.empty()) return std::nullopt;
			FTopLevelAssetPath Path;
			if (!FTopLevelAssetPath::TryCreate(Item.VirtualPath, Path)) return std::nullopt;
			return ::Durin::Editor::FAssetThumbnailPackageFingerprint{
				.AssetPath = std::move(Path),
				.PackagePath = Item.PackagePath,
				.AssetClassName = Item.AssetClassName,
				.PackageFormatVersion = Item.ThumbnailPackageFormatVersion,
				.FileSize = static_cast<uint64>(Item.ThumbnailFileSize),
				.LastWriteTimeTicks = Item.ThumbnailLastWriteTimeTicks};
		}

		auto ResolveStateLabel(EAssetPathResolveState State)
			-> std::string_view
		{
			switch (State)
			{
			case EAssetPathResolveState::Resolved: return "Resolved";
			case EAssetPathResolveState::NotFound: return "Not found";
			case EAssetPathResolveState::MissingRedirectTarget:
				return "Missing target";
			case EAssetPathResolveState::RedirectCycle: return "Cycle";
			case EAssetPathResolveState::RedirectDepthExceeded:
				return "Depth exceeded";
			case EAssetPathResolveState::UnknownTargetClass:
				return "Unknown target class";
			case EAssetPathResolveState::RedirectTypeMismatch:
				return "Type mismatch";
			case EAssetPathResolveState::CorruptRedirector:
				return "Corrupt redirector";
			}
			return "Unknown";
		}

		constexpr ImGuiTableFlags DetailsTableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings;
		constexpr float FullToolbarWidth = 900.0f;
		constexpr float CompactToolbarWidth = 620.0f;
		constexpr float MinimumTreeWidth = 145.0f;
		constexpr float MinimumContentWidth = 240.0f;
		constexpr float MinimumContentHeight = 80.0f;
	} // namespace

	auto FContentBrowserPanel::TickWhenHidden() -> void
	{
		if (AdmissionState != ::Durin::Editor::ContentBrowser::EAdmissionState::Accepting)
			return;
		PrepareForDraw();
	}

	auto FContentBrowserPanel::DrawContents(bool bInAllowAssetMutation) -> void
	{
		if (AdmissionState != ::Durin::Editor::ContentBrowser::EAdmissionState::Accepting)
			return;
		bAllowAssetMutation = bInAllowAssetMutation;
		PrepareForDraw();
		DrawBrowserContents();
	}

	auto FContentBrowserPanel::DrawHostPresenters(
		bool bInAllowAssetMutation) -> void
	{
		if (AdmissionState != ::Durin::Editor::ContentBrowser::EAdmissionState::Accepting)
			return;
		for (const auto& Extension :
			::Durin::Editor::ContentBrowser::CaptureHostPresenters())
			(void)::Durin::Editor::ContentBrowser::DrawHostPresentation(
				Extension, bInAllowAssetMutation);
	}

	auto FContentBrowserPanel::PrepareForDraw() -> void
	{
		// The host browser is constructed after workspace modules register their
		// thumbnail renderers. Rebuild the restored directory snapshot once
		// those registrations have completed and the panel is first submitted.
		if (bRefreshItemsOnFirstDraw)
		{
			RefreshItemsSnapshot();
			bRefreshItemsOnFirstDraw = false;
		}
		SynchronizeMountedContentMutation();
		RefreshMountSnapshot();
		Model.RefreshRequestedDirectoryChildrenSnapshots();
	}

	auto FContentBrowserPanel::QueueTreeAction(std::function<void()> Action) -> void
	{
		check(!DeferredTreeAction);
		if (DeferredTreeAction) return;
		DeferredTreeAction = std::move(Action);
	}

	auto FContentBrowserPanel::QueueContentAction(std::function<void()> Action) -> void
	{
		check(!DeferredContentAction);
		if (DeferredContentAction) return;
		DeferredContentAction = std::move(Action);
	}

	auto FContentBrowserPanel::ExecuteTreeAction() -> void
	{
		if (!DeferredTreeAction) return;
		auto Action = std::move(DeferredTreeAction);
		DeferredTreeAction = {};
		Action();
	}

	auto FContentBrowserPanel::ExecuteContentAction() -> void
	{
		if (!DeferredContentAction) return;
		auto Action = std::move(DeferredContentAction);
		DeferredContentAction = {};
		Action();
	}

	auto FContentBrowserPanel::DrawBrowserContents() -> void
	{
		DrawToolbar();

		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		const float SplitterWidth = Metrics.SplitterThickness;
		const float ScaledMinimumTreeWidth = MonaImGui::ScaleUI(MinimumTreeWidth);
		const float ScaledMinimumContentWidth = MonaImGui::ScaleUI(MinimumContentWidth);
		const float TreeWidth = std::clamp(AvailableWidth * DirectoryTreeWidth, ScaledMinimumTreeWidth, std::max(ScaledMinimumTreeWidth, AvailableWidth - ScaledMinimumContentWidth));
		const float ContentHeight = std::max(MonaImGui::ScaleUI(MinimumContentHeight), ImGui::GetContentRegionAvail().y);
		if (ImGui::BeginChild("ContentBrowserTree", ImVec2(TreeWidth, ContentHeight), ImGuiChildFlags_Borders)) DrawDirectoryTree();
		ImGui::EndChild();
		ExecuteTreeAction();
		ImGui::SameLine();
		MonaImGui::DrawSplitter("ContentBrowserSplitter", MonaImGui::EUISplitterAxis::X, ContentHeight, AvailableWidth, ScaledMinimumTreeWidth, ScaledMinimumContentWidth, DirectoryTreeWidth);
		ImGui::SameLine();
		if (ImGui::BeginChild("ContentBrowserItems", ImVec2(0.0f, ContentHeight), ImGuiChildFlags_Borders)) DrawContentArea();
		ImGui::EndChild();
		// Context-menu actions can rebuild items, so execute them only after both browser panes
		// have finished traversing their current frame snapshots.
		ExecuteContentAction();
		DrawDialogs();

		::Durin::Editor::ContentBrowser::FPresentationSettings NextSettings =
			PresentationSettings;
		NextSettings.ViewMode = static_cast<uint8>(ViewMode);
		NextSettings.IconSize = IconSize;
		NextSettings.bIconSizeLocked = bIconSizeLocked;
		NextSettings.TreeWidth = DirectoryTreeWidth;
		NextSettings.bShowHiddenFiles = Model.IsShowingHiddenFiles();
		NextSettings.LastDirectory = Model.GetCurrentPhysicalPath();
		if (NextSettings != PresentationSettings)
		{
			PresentationSettings = std::move(NextSettings);
			if (SavePresentationSettings)
				SavePresentationSettings(PresentationSettings);
		}
	}

	auto FContentBrowserPanel::DrawToolbar() -> void
	{
		auto FocusSearch = [this] {
			if (!bFocusSearch) return;
			ImGui::SetKeyboardFocusHere();
			bFocusSearch = false;
		};
		int32 TypeFilter = static_cast<int32>(Model.GetTypeFilter());
		const float ToolbarWidth = ImGui::GetContentRegionAvail().x;
		const EToolbarLayout LayoutMode = ResolveToolbarLayout(ToolbarWidth, MonaImGui::ScaleUI(CompactToolbarWidth), MonaImGui::ScaleUI(FullToolbarWidth));
		const bool bFullLayout = LayoutMode == EToolbarLayout::Full;
		const bool bCompactLayout = LayoutMode != EToolbarLayout::Narrow;

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
				if (!FPaths::TryMakeLexicalRelativePath(Model.GetCurrentPhysicalPath(), Mount.PhysicalRoot, Relative)) continue;
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

		const char* Filters[] = {"All content", "Assets", "Files", "Levels", "Static meshes", "Materials", "Textures", "Other assets", "Redirectors"};
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
					if (ImGui::Combo("##CompactContentTypeFilter", &TypeFilter, Filters, std::size(Filters)))
						Model.SetTypeFilter(static_cast<EContentBrowserTypeFilter>(TypeFilter));
					ImGui::Separator();
				}
				bool bShowHiddenFiles = Model.IsShowingHiddenFiles();
				if (ImGui::Checkbox("Show hidden files and folders", &bShowHiddenFiles))
					Model.SetShowHiddenFiles(bShowHiddenFiles);
				bool bShowRedirectors = Model.IsShowingRedirectors();
				if (ImGui::Checkbox("Show redirectors", &bShowRedirectors))
					Model.SetShowRedirectors(bShowRedirectors);
				ImGui::Separator();
				ImGui::TextDisabled("Thumbnail size");
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::SliderFloat("##ContentIconSize", &IconSize,
					::Durin::Editor::ContentBrowser::FPresentationSettings::MinimumIconSize,
					::Durin::Editor::ContentBrowser::FPresentationSettings::MaximumIconSize,
					"%.0f px");
				ImGui::Checkbox("Lock Ctrl + wheel resizing", &bIconSizeLocked);
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
			if (ImGui::Combo("##ContentTypeFilter", &TypeFilter, Filters, std::size(Filters)))
				Model.SetTypeFilter(static_cast<EContentBrowserTypeFilter>(TypeFilter));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(SearchWidth);
			FocusSearch();
			if (ImGui::InputTextWithHint("##ContentSearch", "Search current folder...", SearchBuffer.data(), SearchBuffer.size())) RebuildItems();
		}
		else if (bCompactLayout)
		{
			ImGui::SameLine();
			DrawViewControls();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			FocusSearch();
			if (ImGui::InputTextWithHint("##ContentSearch", "Search current folder...", SearchBuffer.data(), SearchBuffer.size())) RebuildItems();
		}

		if (!bCompactLayout)
		{
			ImGui::NewLine();
			DrawViewControls();
			ImGui::NewLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			FocusSearch();
			if (ImGui::InputTextWithHint("##ContentSearchNarrow", "Search current folder...", SearchBuffer.data(), SearchBuffer.size())) RebuildItems();
		}
	}

	auto FContentBrowserPanel::DrawDirectoryTree() -> void
	{
		for (const FContentBrowserModel::FMountSnapshot& Mount : Model.GetMounts())
		{
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
		const bool bHasChildrenSnapshot =
			Model.HasDirectoryChildrenSnapshot(Physical);
		if (!bHasChildrenSnapshot)
			Model.RequestDirectoryChildrenSnapshot(Physical);
		// Tree actions cannot clear or mutate observed entries until traversal ends;
		// inserting distinct cache entries does not invalidate this span.
		const std::span<const std::filesystem::path> Children =
			Model.GetDirectoryChildren(Physical);
		const bool bHasChildren = !bHasChildrenSnapshot
			|| std::ranges::any_of(Children, [&](const std::filesystem::path& Child) { return Model.IsShowingHiddenFiles() || !Child.filename().generic_string().starts_with('.'); });
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (!bHasChildren) Flags |= ImGuiTreeNodeFlags_Leaf;
		if (Model.GetCurrentPhysicalPath() == Physical) Flags |= ImGuiTreeNodeFlags_Selected;
		if (bMountRoot) Flags |= ImGuiTreeNodeFlags_DefaultOpen;
		const std::string NodeLabel = std::format("{} {}###{}", Model.GetCurrentPhysicalPath() == Physical ? Icons::FolderOpen : Icons::Folder, Label, Physical);
		const bool bOpen = ImGui::TreeNodeEx(NodeLabel.c_str(), Flags);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			QueueTreeAction([this, Physical] { NavigateToPhysical(Physical); });
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
				if (!Model.IsShowingHiddenFiles() && Name.starts_with('.')) continue;
				DrawDirectoryNode(Child, Name, false);
			}
			ImGui::TreePop();
		}
	}

	auto FContentBrowserPanel::DrawContentArea() -> void
	{
		ThumbnailReferences->BeginFrame();
		const ImGuiIO& IO = ImGui::GetIO();
		if (ViewMode == EContentBrowserViewMode::Grid && !bIconSizeLocked && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && IO.KeyCtrl && IO.MouseWheel != 0.0f)
			IconSize = std::clamp(IconSize + IO.MouseWheel * MonaImGui::ScaleUI(8.0f),
				::Durin::Editor::ContentBrowser::FPresentationSettings::MinimumIconSize,
				::Durin::Editor::ContentBrowser::FPresentationSettings::MaximumIconSize);
		const bool bReserveDetails = bShowSelectionDetails && Selection.size() == 1;
		if (bReserveDetails) ImGui::BeginChild("ContentBrowserMainView", ImVec2(0.0f, -132.0f));
		if (bResetContentScroll) ImGui::SetScrollY(0.0f);
		bContentItemHovered = false;
		bRenameEditorHovered = false;
		if (ViewMode == EContentBrowserViewMode::Grid)
			DrawGrid();
		else
			DrawDetails();
		bResetContentScroll = false;
		ThumbnailReferences->EndFrame();
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
			PrepareSelectionDetails();
			ImGui::SeparatorText("Selection Details");
			DrawSelectionDetails();
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
		{
			if (bAllowAssetMutation && ImGui::GetIO().KeyCtrl
				&& ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_N))
				CreateFolder(Model.GetCurrentPhysicalPath());
			if (ImGui::IsKeyPressed(ImGuiKey_F5)) Refresh(true);
			if (ImGui::IsKeyPressed(ImGuiKey_Enter) && Selection.size() == 1)
				if (auto It = std::ranges::find_if(Model.GetItems(), [&](const FContentBrowserItem& Item) { return Selection.contains(Item.StableId()); }); It != Model.GetItems().end()) OpenItem(*It);
			if (bAllowAssetMutation && ImGui::IsKeyPressed(ImGuiKey_F2)
				&& Selection.size() == 1)
				if (auto It = std::ranges::find_if(
					Model.GetItems(),
					[&](const FContentBrowserItem& Item) {
						return Selection.contains(Item.StableId());
					}); It != Model.GetItems().end()
					&& It->Kind != EContentBrowserItemKind::Redirector)
					BeginRename(*It);
			if (bAllowAssetMutation && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_D, false)
				&& Selection.size() == 1)
				if (auto It = std::ranges::find_if(
						Model.GetItems(),
						[&](const FContentBrowserItem& Item) {
							return Selection.contains(Item.StableId());
						});
					It != Model.GetItems().end()
						&& It->Kind == EContentBrowserItemKind::Asset)
					QueueContentAction([this, Item = *It] { DuplicateAsset(Item); });
			if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
				CopyAssetSelection();
			if (bAllowAssetMutation && ImGui::GetIO().KeyCtrl
				&& ImGui::IsKeyPressed(ImGuiKey_V, false)
				&& HasAssetClipboard())
				QueueContentAction([this] { PasteAsset(); });
			if (bAllowAssetMutation && ImGui::IsKeyPressed(ImGuiKey_Delete)
				&& !Selection.empty()) RequestDeleteSelection();
		}
	}

	auto FContentBrowserPanel::PrepareSelectionDetails() -> void
	{
		TextureCubeDetailsSnapshot = nullptr;
		const auto It = std::ranges::find_if(
			Model.GetItems(),
			[&](const FContentBrowserItem& Item) {
				return Selection.contains(Item.StableId());
			});
		if (It == Model.GetItems().end()
			|| It->Kind != EContentBrowserItemKind::Asset
			|| ClassLeaf(It->AssetClassName) != "TextureCube")
			return;
		TextureCubeDetailsSnapshot = &TextureCubeDetailsCache.Get(
			It->PhysicalPath,
			GetAssetCatalogRevision());
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
			Row("Type", ContentBrowserItemView::TypeLabel(Item));
			Row("Virtual", Item.VirtualPath.empty() ? "-" : Item.VirtualPath);
			Row("Physical", Item.PhysicalPath);
			if (Item.Kind == EContentBrowserItemKind::Asset
				|| Item.Kind == EContentBrowserItemKind::Redirector)
			{
				const FPackagePath& Path = Item.PackagePath;
				if (Path.IsValid())
				{
					if (const FAssetCatalogEntry Data =
						FindAssetExact(Path))
					{
						Row("Hard dependencies", std::format("{}", Data->Dependencies.size()));
						Row("Soft dependencies", std::format("{}", Data->SoftDependencies.size()));
						const FAssetReferenceIndex ReferenceIndex =
							CaptureAssetReferenceIndex();
						size_t HardReferencers = 0;
						size_t SoftReferencers = 0;
						size_t RedirectReferencers = 0;
						for (const FAssetPackageReferenceEdge& Edge :
							 ReferenceIndex.FindReferencers(Path))
							switch (Edge.Kind)
							{
							case EAssetReferenceKind::HardObject:
								++HardReferencers; break;
							case EAssetReferenceKind::SoftObject:
								++SoftReferencers; break;
							case EAssetReferenceKind::Redirect:
								++RedirectReferencers; break;
							}
						Row("Hard refs", std::format("{}", HardReferencers));
						Row("Soft refs", std::format("{}", SoftReferencers));
						Row("Redirect refs", std::format("{}", RedirectReferencers));
						Row("Reference index", ReferenceIndex.IsComplete()
							? "Complete"
							: std::format(
								"Incomplete ({} error{})",
								ReferenceIndex.GetErrors().size(),
								ReferenceIndex.GetErrors().size() == 1 ? "" : "s"));
						if (Item.Kind == EContentBrowserItemKind::Redirector)
						{
							Row("Destination", Item.RedirectDestination.ToString());
							FObjectPath ObjectPath;
							FObjectPath::TryCreate(Item.VirtualPath, ObjectPath);
							const FObjectPathResolveResult Resolution =
								ResolveAssetObjectPath(ObjectPath);
							Row("State", ResolveStateLabel(Resolution.State));
							Row("Final", Resolution.FinalPath.IsValid()
								? Resolution.FinalPath.ToString()
								: "-");
							Row("Chain", std::format("{}", Resolution.RedirectChain.size()));
						}
					}
				}
				if (Item.Kind == EContentBrowserItemKind::Asset
					&& ClassLeaf(Item.AssetClassName) == "TextureCube")
				{
					const ContentBrowserItemView::FTextureCubeDetailsSnapshot Unavailable;
					const ContentBrowserItemView::FTextureCubeDetailsSnapshot& Details =
						TextureCubeDetailsSnapshot
						? *TextureCubeDetailsSnapshot
						: Unavailable;
					if (Details.bAvailable)
					{
						Row("Source Layout", Details.SourceLayout);
						Row("Source", Details.Source);
						Row("Source Size", Details.SourceSize);
						if (Details.bPanorama)
						{
							Row("Face Override", Details.FaceOverride);
							Row("Input Range", Details.InputRange);
							Row("Exposure", Details.Exposure);
						}
						Row("Dimensions", Details.Dimensions);
						Row("Faces", Details.Faces);
						Row("Mips", Details.Mips);
						Row("Output", Details.Output);
					}
					Row("Build", Details.BuildDiagnostic);
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
		if (bResetContentScroll)
			ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
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
						ThumbnailReferences->Request({
							.Identity = Item.ThumbnailIdentity,
							.SourcePhysicalPath = Item.ThumbnailSourcePath,
							.SourceFileSize = Item.ThumbnailFileSize,
							.SourceLastWriteTime = Item.ThumbnailLastWriteTime,
							.Asset = MakeThumbnailFingerprint(Item),
							.Priority = Row >= Clipper.DisplayStart && Row < Clipper.DisplayEnd
								? ::Durin::Editor::EAssetThumbnailPriority::Visible
								: ::Durin::Editor::EAssetThumbnailPriority::Prefetch});
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
					if (bHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						QueueContentAction([this, Item] { OpenItem(Item); });
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

					const ::Durin::Editor::FAssetThumbnailView Thumbnail = ThumbnailReferences->Find(Item.ThumbnailIdentity);
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
						ImGui::TextUnformatted(ContentBrowserItemView::TypeLabel(Item).c_str());
						if (Item.Kind != EContentBrowserItemKind::Folder)
						{
							ImGui::TextDisabled("Size");
							ImGui::SameLine();
							ImGui::TextUnformatted(ContentBrowserItemView::FormatFileSize(Item.FileSize).c_str());
							ImGui::TextDisabled("Modified");
							ImGui::SameLine();
							ImGui::TextUnformatted(ContentBrowserItemView::FormatFileTime(Item.LastWriteTime).c_str());
						}
						if (Thumbnail.State == ::Durin::Editor::EAssetThumbnailState::Failed && !Thumbnail.Diagnostic.empty())
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
		if (bResetContentScroll)
			ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
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
		ImGuiListClipper Clipper;
		Clipper.Begin(static_cast<int32>(Model.GetItems().size()));
		while (Clipper.Step())
		{
			for (int32 Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
			{
				const FContentBrowserItem& Item = Model.GetItems()[static_cast<size_t>(Index)];
				ImGui::PushID(Item.StableId().c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (RenameTarget == Item.StableId())
					DrawRenameEditor(Item);
				else
				{
					const std::string Label = std::format(
						"{} {}", ContentBrowserItemView::Icon(Item), Item.Name);
					ImGui::Selectable(Label.c_str(), Selection.contains(Item.StableId()), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
					bContentItemHovered |= ImGui::IsItemHovered();
					if (ImGui::IsItemClicked()) SelectItem(static_cast<size_t>(Index));
					if (ImGui::IsItemHovered()
						&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						QueueContentAction([this, Item] { OpenItem(Item); });
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
				ImGui::TextUnformatted(ContentBrowserItemView::TypeLabel(Item).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Item.Kind == EContentBrowserItemKind::Folder ? "-"
					: ContentBrowserItemView::FormatFileSize(Item.FileSize).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Item.Kind == EContentBrowserItemKind::Folder ? "-"
					: ContentBrowserItemView::FormatFileTime(Item.LastWriteTime).c_str());
				ImGui::PopID();
			}
		}
		DrawBackgroundContextMenu();
		ImGui::EndTable();
		ImGui::PopStyleVar();
	}

	auto FContentBrowserPanel::DrawItemContextMenu(const FContentBrowserItem& Item) -> void
	{
		if (ImGui::MenuItem(Item.Kind == EContentBrowserItemKind::Folder
			? "Open Folder"
			: Item.Kind == EContentBrowserItemKind::Redirector
				? "Open Destination"
				: "Open"))
			QueueContentAction([this, Item] { OpenItem(Item); });
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const FPackagePath& PackagePath = Item.PackagePath;
			if (PackagePath.IsValid())
			{
				DPackage* LoadedPackage = FindResidentPackage(PackagePath);
				const bool bCanSave = LoadedPackage && LoadedPackage->IsDirty();
				ImGui::BeginDisabled(!bAllowAssetMutation);
				if (ImGui::MenuItem("Save Package", nullptr, false, bCanSave))
					QueueContentAction([this, PackagePath] { SaveAssetPackage(PackagePath); });
				if (!bCanSave && ImGui::IsItemHovered())
					ImGui::SetTooltip("Available only for a loaded package with authored changes.");
				if (ImGui::MenuItem(LoadedPackage && LoadedPackage->IsCanonicalResaveRecommended()
					? "Resave Package (recommended)" : "Resave Package"))
					QueueContentAction([this, PackagePath] { ResaveAssetPackages({PackagePath}); });
				if (Selection.size() > 1 && ImGui::MenuItem("Resave Selected Packages"))
				{
					std::vector<FPackagePath> SelectedPaths;
					for (const FContentBrowserItem& SelectedItem : Model.GetItems())
						if (SelectedItem.Kind == EContentBrowserItemKind::Asset
							&& Selection.contains(SelectedItem.StableId()))
						{
							if (SelectedItem.PackagePath.IsValid())
								SelectedPaths.push_back(SelectedItem.PackagePath);
						}
					QueueContentAction([this, Paths = std::move(SelectedPaths)]() mutable {
						ResaveAssetPackages(std::move(Paths));
					});
				}
				ImGui::EndDisabled();
				ImGui::Separator();
			}
		}
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			const auto Availability = QueryReimport
				? QueryReimport(Item.AssetClassName)
				: ::Durin::Editor::ContentBrowser::FReimportAvailability{};
			if (Availability.bCanReimport || Availability.bCanReimportFromFile)
			{
				if (Availability.bCanReimport)
				{
					ImGui::BeginDisabled(!bAllowAssetMutation);
					if (ImGui::MenuItem("Reimport (source unverified)"))
						QueueContentAction([this, Path = Item.VirtualPath] {
							if (Reimport)
								Reimport(false, Path, [this](std::string Message) {
										SetError(std::move(Message));
									});
						});
					ImGui::EndDisabled();
				}
				if (Availability.bCanReimportFromFile)
				{
					ImGui::BeginDisabled(!bAllowAssetMutation);
					if (ImGui::MenuItem("Reimport From File..."))
						QueueContentAction([this, Path = Item.VirtualPath] {
							if (Reimport)
								Reimport(true, Path, [this](std::string Message) {
									SetError(std::move(Message));
								});
						});
					ImGui::EndDisabled();
				}
			}
			ImGui::Separator();
		}
		if (Item.Kind == EContentBrowserItemKind::Folder)
		{
			ImGui::BeginDisabled(!bAllowAssetMutation);
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
			ImGui::EndDisabled();
			ImGui::Separator();
		}
		if (Item.Kind == EContentBrowserItemKind::Asset
			|| Item.Kind == EContentBrowserItemKind::Redirector)
		{
			const FPackagePath& Path = Item.PackagePath;
			if (Path.IsValid())
			{
				std::vector<FPackagePath> Referencers;
				for (const FAssetPackageReferenceEdge& Edge :
					 CaptureAssetReferenceIndex().FindReferencers(Path))
					if (std::ranges::find(Referencers, Edge.SourcePackage)
						== Referencers.end())
						Referencers.push_back(Edge.SourcePackage);
				std::ranges::sort(
					Referencers,
					[](const FPackagePath& A, const FPackagePath& B) {
						return A.GetView() < B.GetView();
					});
				if (ImGui::BeginMenu("Reveal Referencer", !Referencers.empty()))
				{
					for (const FPackagePath& Referencer : Referencers)
						if (ImGui::MenuItem(Referencer.ToString().c_str()))
							QueueContentAction(
								[this, Referencer] {
									RevealAsset(Referencer.ToString());
								});
					ImGui::EndMenu();
				}
			}
		}
		if (Item.Kind == EContentBrowserItemKind::Redirector)
		{
			if (ImGui::MenuItem("Reveal Destination"))
				QueueContentAction([this, Destination = Item.RedirectDestination] {
					RevealAsset(Destination.ToString());
				});
			ImGui::BeginDisabled(!bAllowAssetMutation);
			if (ImGui::MenuItem(
				Selection.size() > 1
					? "Fix Up Selected Redirectors"
					: "Fix Up Redirector"))
				QueueContentAction([this, Item] { FixUpRedirector(Item); });
			ImGui::EndDisabled();
			ImGui::Separator();
		}
		ImGui::BeginDisabled(!bAllowAssetMutation);
		if (ImGui::MenuItem(
			"Duplicate", "Ctrl+D", false,
			Selection.size() == 1
				&& Item.Kind == EContentBrowserItemKind::Asset))
			QueueContentAction([this, Item] { DuplicateAsset(Item); });
		ImGui::EndDisabled();
		if (ImGui::MenuItem(
			"Copy Asset", "Ctrl+C", false,
			Selection.size() == 1
				&& Item.Kind == EContentBrowserItemKind::Asset))
			CopyAssetSelection();
		ImGui::BeginDisabled(!bAllowAssetMutation);
		if (ImGui::MenuItem(
			Item.Kind == EContentBrowserItemKind::Folder
				? "Paste Asset Into Folder"
				: "Paste Asset",
			"Ctrl+V", false, HasAssetClipboard()))
			QueueContentAction([this, Item] {
				PasteAsset(Item.Kind == EContentBrowserItemKind::Folder
					? std::string_view(Item.VirtualPath)
					: std::string_view{});
			});
		if (ImGui::MenuItem(
			"Rename", "F2", false,
			Selection.size() == 1
				&& Item.Kind != EContentBrowserItemKind::Redirector))
			BeginRename(Item);
		if (ImGui::MenuItem("Delete", "Delete")) RequestDeleteSelection();
		ImGui::EndDisabled();
		ImGui::Separator();
		if (ImGui::BeginMenu("Copy Details"))
		{
			if (ImGui::MenuItem("Name")) CopyToClipboard(Item.Name);
			if (!Item.VirtualPath.empty() && ImGui::MenuItem("Virtual Path")) CopyToClipboard(Item.VirtualPath);
			if (ImGui::MenuItem("Physical Path")) CopyToClipboard(Item.PhysicalPath);
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(Item.PhysicalPath);
	}

	auto FContentBrowserPanel::DrawCreateMenu(std::string_view PhysicalDirectory, std::string_view VirtualDirectory) -> void
	{
		ImGui::BeginDisabled(!bAllowAssetMutation);
		if (ImGui::MenuItem("New Folder", "Ctrl+Shift+N"))
			QueueContentAction([this, Directory = std::string(PhysicalDirectory)] { CreateFolder(Directory); });
		ImGui::SeparatorText("Assets");
		const ::Durin::Editor::ContentBrowser::FExtensionContext Context{
			.VirtualDirectory = std::string(VirtualDirectory)};
		for (const auto& Extension :
			::Durin::Editor::ContentBrowser::CaptureExtensions(
				::Durin::Editor::ContentBrowser::EExtensionCategory::Create))
		{
			if (!Extension.IsApplicable || !Extension.IsApplicable(Context)) continue;
			if (ImGui::MenuItem(Extension.Label.c_str()))
				QueueContentAction([this, Extension, Context] {
					::Durin::Editor::ContentBrowser::InvokeExtension(
						Extension, {
							.Context = Context,
							.bAllowAssetMutation = bAllowAssetMutation,
							.RevealAsset = [this](std::string_view Path) {
								return RevealAsset(Path);
							},
							.RevealDirectory = [this](std::string_view Path) {
								return RevealDirectory(Path);
							},
							.OpenAsset = [this](std::string_view Path, std::string_view Class) {
								return OpenAsset && OpenAsset(std::string(Path), std::string(Class));
							},
							.NotifyMountedContentChanged = [this] {
								PublishMountedContentMutation();
							},
							.ReportError = [this](std::string Message) {
								SetError(std::move(Message));
							},
						});
				});
		}
		ImGui::EndDisabled();
	}

	auto FContentBrowserPanel::DrawImportMenu(std::string_view VirtualDirectory) -> void
	{
		ImGui::BeginDisabled(!bAllowAssetMutation);
		const ::Durin::Editor::ContentBrowser::FExtensionContext Context{
			.VirtualDirectory = std::string(VirtualDirectory)};
		for (const auto& Extension :
			::Durin::Editor::ContentBrowser::CaptureExtensions(
				::Durin::Editor::ContentBrowser::EExtensionCategory::Import))
		{
			if (!Extension.IsApplicable || !Extension.IsApplicable(Context)) continue;
			if (ImGui::MenuItem(Extension.Label.c_str()))
				QueueContentAction([this, Extension, Context] {
					::Durin::Editor::ContentBrowser::InvokeExtension(
						Extension, {
							.Context = Context,
							.bAllowAssetMutation = bAllowAssetMutation,
							.RevealAsset = [this](std::string_view Path) {
								return RevealAsset(Path);
							},
							.RevealDirectory = [this](std::string_view Path) {
								return RevealDirectory(Path);
							},
							.OpenAsset = [this](std::string_view Path, std::string_view Class) {
								return OpenAsset && OpenAsset(
									std::string(Path), std::string(Class));
							},
							.NotifyMountedContentChanged = [this] {
								PublishMountedContentMutation();
							},
							.ReportError = [this](std::string Message) {
								SetError(std::move(Message));
							},
						});
				});
		}
		ImGui::EndDisabled();
	}

	auto FContentBrowserPanel::DrawDirectoryContextMenu(std::string_view PhysicalDirectory, bool bMountRoot) -> void
	{
		const std::string VirtualDirectory = PhysicalToVirtualDirectory(PhysicalDirectory);
		const bool bIsCurrent = NormalizePath(PhysicalDirectory) == Model.GetCurrentPhysicalPath();
		if (ImGui::MenuItem("Open Folder", nullptr, false, !bIsCurrent))
			QueueTreeAction([this, Directory = std::string(PhysicalDirectory)] {
				NavigateToPhysical(Directory);
			});
		ImGui::BeginDisabled(!bAllowAssetMutation);
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
			QueueTreeAction([this, Directory = std::string(PhysicalDirectory)] {
				if (const FContentBrowserItem* Item = FocusFolderInParent(Directory))
					BeginRename(*Item);
			});
		if (ImGui::MenuItem("Delete", "Delete"))
			QueueTreeAction([this, Directory = std::string(PhysicalDirectory)] {
				if (FocusFolderInParent(Directory)) RequestDeleteSelection();
			});
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::Separator();
		if (!VirtualDirectory.empty() && ImGui::MenuItem("Copy Virtual Path")) CopyToClipboard(VirtualDirectory);
		ImGui::BeginDisabled(!bAllowAssetMutation);
		if (!VirtualDirectory.empty() && ImGui::MenuItem("Fix Up Redirectors in Folder"))
			QueueContentAction([this, VirtualDirectory] {
				FixUpFolder(VirtualDirectory);
			});
		ImGui::EndDisabled();
		if (ImGui::MenuItem("Copy Physical Path")) CopyToClipboard(PhysicalDirectory);
		if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(PhysicalDirectory);
		if (ImGui::MenuItem("Refresh", "F5"))
			QueueTreeAction([this] { Refresh(true); });
	}

	auto FContentBrowserPanel::BeginAssetDragDrop(const FContentBrowserItem& Item) -> void
	{
		if (Item.Kind != EContentBrowserItemKind::Asset || !ImGui::BeginDragDropSource()) return;
		::Durin::Editor::FAssetDragDropPayload Payload;
		std::memcpy(Payload.AssetPath.data(), Item.VirtualPath.data(), std::min(Item.VirtualPath.size(), Payload.AssetPath.size() - 1));
		std::memcpy(Payload.AssetClassName.data(), Item.AssetClassName.data(), std::min(Item.AssetClassName.size(), Payload.AssetClassName.size() - 1));
		ImGui::SetDragDropPayload(::Durin::Editor::AssetDragDropPayloadType, &Payload, sizeof(Payload));
		ImGui::Text("%s %s", ContentBrowserItemView::Icon(Item), Item.Name.c_str());
		ImGui::EndDragDropSource();
	}

	auto FContentBrowserPanel::AcceptAssetDrop(std::string_view DestinationDirectory, bool bPhysicalDirectory) -> void
	{
		if (!bAllowAssetMutation) return;
		if (!ImGui::BeginDragDropTarget()) return;
		if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(::Durin::Editor::AssetDragDropPayloadType); Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(::Durin::Editor::FAssetDragDropPayload))
		{
			const auto* AssetPayload = static_cast<const ::Durin::Editor::FAssetDragDropPayload*>(Payload->Data);
			FObjectPath OldObjectPath;
			if (FObjectPath::TryCreate(AssetPayload->AssetPath.data(), OldObjectPath))
			{
				const FPackagePath& OldPath = OldObjectPath.GetPackagePath();
				std::string Destination = bPhysicalDirectory ? PhysicalToVirtualDirectory(DestinationDirectory) : std::string(DestinationDirectory);
				if (Destination.empty())
				{
					ImGui::EndDragDropTarget();
					return;
				}
				if (!Destination.ends_with('/')) Destination += '/';
				FPackagePath NewPath;
				if (FPackagePath::TryCreate(Destination + std::string(OldPath.GetAssetName()), NewPath) && NewPath != OldPath)
				{
					const FEditorAssetMove Move{OldPath, NewPath};
					QueueContentAction([this, Move] {
						const FAssetResult Result = Operations.Move(std::span{&Move, 1});
						if (!Result)
							SetError(Result.Message);
						else
							PublishMountedContentMutation();
					});
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
			ImGui::BeginDisabled(!bAllowAssetMutation);
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
			if (ImGui::MenuItem(
				"Paste Asset", "Ctrl+V", false, HasAssetClipboard()))
				QueueContentAction([this] { PasteAsset(); });
			ImGui::Separator();
			if (!Model.GetCurrentVirtualPath().empty()
				&& ImGui::MenuItem("Fix Up Redirectors in Folder"))
				QueueContentAction([this,
					Directory = Model.GetCurrentVirtualPath()] {
					FixUpFolder(Directory);
				});
			if (ImGui::MenuItem("Fix Up All Redirectors"))
				QueueContentAction([this] { FixUpProject(); });
			ImGui::EndDisabled();
			ImGui::Separator();
			if (ImGui::MenuItem("Refresh", "F5")) Refresh(true);
			if (ImGui::MenuItem("Show in Explorer")) ShowInExplorer(Model.GetCurrentPhysicalPath());
			ImGui::EndPopup();
		}
	}

	auto FContentBrowserPanel::DrawRenameEditor(const FContentBrowserItem& Item) -> void
	{
		if (!bAllowAssetMutation)
		{
			RenameTarget.clear();
			return;
		}
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
			const FContentDeletionPlan* Plan = PendingDeletionPlan.get();
			if (Plan)
			{
				ImGui::Text("Delete %s?", Plan->DisplayName.c_str());
				ImGui::TextDisabled(
					"%llu asset%s, %llu file%s, %llu companion file%s, %llu folder%s",
					static_cast<unsigned long long>(Plan->Summary.AssetCount),
					Plan->Summary.AssetCount == 1 ? "" : "s",
					static_cast<unsigned long long>(Plan->Summary.FileCount),
					Plan->Summary.FileCount == 1 ? "" : "s",
					static_cast<unsigned long long>(Plan->Summary.CompanionCount),
					Plan->Summary.CompanionCount == 1 ? "" : "s",
					static_cast<unsigned long long>(Plan->Summary.FolderCount),
					Plan->Summary.FolderCount == 1 ? "" : "s");
				ImGui::TextDisabled(
					"This permanently deletes local content and cannot be undone. Restore through version control if needed.");
				if (bDeletionPlanRefreshed)
				{
					ImGui::Spacing();
					ImGui::TextWrapped(
						"Content changed after this dialog opened. Review the updated summary and confirm again.");
				}
				if (!Plan->Blockers.empty())
				{
					ImGui::Spacing();
					ImGui::Text("Deletion is blocked:");
					const size_t VisibleBlockerCount =
						std::min<size_t>(Plan->Blockers.size(), 6);
					for (size_t Index = 0; Index < VisibleBlockerCount; ++Index)
					{
						const FContentDeletionBlocker& Blocker = Plan->Blockers[Index];
						const std::string& Label = Blocker.DisplayName.empty()
							? Blocker.PhysicalPath
							: Blocker.DisplayName;
						ImGui::BulletText("%s", Label.c_str());
						if (!Blocker.RelatedAssetPath.empty())
							ImGui::TextDisabled("Referenced by %s", Blocker.RelatedAssetPath.c_str());
						if (!Blocker.Details.empty())
							ImGui::TextWrapped("%s", Blocker.Details.c_str());
					}
					if (VisibleBlockerCount < Plan->Blockers.size())
						ImGui::TextDisabled(
							"... and %zu more blocker%s",
							Plan->Blockers.size() - VisibleBlockerCount,
							Plan->Blockers.size() - VisibleBlockerCount == 1 ? "" : "s");
				}
				if (!Plan->Warnings.empty())
				{
					ImGui::Spacing();
					ImGui::Text("Deletion warnings:");
					for (const FContentDeletionWarning& Warning : Plan->Warnings)
					{
						ImGui::BulletText("%s", Warning.DisplayName.c_str());
						ImGui::TextWrapped("%s", Warning.Details.c_str());
					}
				}
			}
			else
				ImGui::TextWrapped("The selected content could not be analyzed.");

			const bool bBlocked = !bAllowAssetMutation || !Plan || !Plan->CanExecute();
			const bool bDeletesFolder = Plan && Plan->Summary.FolderCount != 0;
			ImGui::BeginDisabled(bBlocked);
			if (MonaImGui::DialogButton(bDeletesFolder ? "Delete Folder" : "Delete"))
			{
				DeleteSelection();
				if (!PendingDeletionPlan) ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (MonaImGui::DialogButton("Cancel"))
			{
				PendingDeletionPlan.reset();
				bDeletionPlanRefreshed = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		MonaImGui::ErrorDialog("Content Browser Error", ErrorMessage);
		MonaImGui::ErrorDialog("Content Browser Warning", WarningMessage);
	}

} // namespace Durin::Editor::ContentBrowser::Private
