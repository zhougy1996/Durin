#include "Panels/FileBrowserPanel.h"

#include "AssetSystem.h"
#include "Icons/FontAwesomeIcons.h"
#include "LevelEditorContext.h"
#include "LevelEditorHelpers.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

#ifdef _WIN32
#include <shellapi.h>
#endif

namespace Durin
{
	namespace
	{
		using LevelEditorHelpers::DrawToolbarIconButton;
		using StringUtils::ContainsInsensitive;

		auto PathLeafName(std::string_view Path) -> std::string
		{
			if (Path.empty()) return {};
			if (Path.back() == '/' || Path.back() == '\\') Path.remove_suffix(1);
			const size_t Pos = Path.find_last_of("/\\");
			return Pos == std::string_view::npos ? std::string(Path) : std::string(Path.substr(Pos + 1));
		}

		constexpr ImGuiTableFlags FileTableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings;
	} // namespace

	FFileBrowserPanel::FFileBrowserPanel(std::function<bool(const std::string&)> InOpenAsset)
		: OpenAsset(std::move(InOpenAsset))
	{
		BuildRootNodes();
	}

	auto FFileBrowserPanel::BuildRootNodes() -> void
	{
		RootNodes.clear();
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			if (!std::filesystem::is_directory(Mount.PhysicalPath)) continue;
			FFileBrowserDirectoryNode Node;
			Node.DisplayName = PathLeafName(Mount.VirtualRoot);
			if (Node.DisplayName.empty()) Node.DisplayName = Mount.VirtualRoot;
			Node.PhysicalPath = Mount.PhysicalPath;
			Node.bIsMountRoot = true;
			RootNodes.push_back(std::move(Node));
		}
	}

	auto FFileBrowserPanel::PopulateChildren(FFileBrowserDirectoryNode& Node) -> void
	{
		if (Node.bChildrenPopulated) return;
		Node.bChildrenPopulated = true;
		Node.Children.clear();

		std::error_code Ec;
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Node.PhysicalPath, Ec))
		{
			if (Ec) break;
			if (!Entry.is_directory(Ec)) continue;
			if (Ec) continue;
			const std::string DirName = Entry.path().filename().string();
			if (!bShowHiddenFiles && !DirName.empty() && DirName.front() == '.') continue;
			FFileBrowserDirectoryNode Child;
			Child.DisplayName = DirName;
			Child.PhysicalPath = Entry.path().string();
			Node.Children.push_back(std::move(Child));
		}
		std::ranges::sort(Node.Children, [](const FFileBrowserDirectoryNode& A, const FFileBrowserDirectoryNode& B) {
			return A.DisplayName < B.DisplayName;
		});
	}

	auto FFileBrowserPanel::RefreshCurrentDirectory() -> void
	{
		CurrentSubdirectories.clear();
		CurrentFiles.clear();
		if (!SelectedDirectory) return;

		const std::string& DirPath = SelectedDirectory->PhysicalPath;
		std::error_code Ec;
		std::vector<FFileBrowserFileEntry> Files;
		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(DirPath, Ec))
		{
			if (Ec) break;
			const std::string Name = Entry.path().filename().string();
			if (!bShowHiddenFiles && !Name.empty() && Name.front() == '.') continue;

			if (Entry.is_directory(Ec))
			{
				if (Ec) { Ec.clear(); continue; }
				FFileBrowserDirectoryNode SubDir;
				SubDir.DisplayName = Name;
				SubDir.PhysicalPath = Entry.path().string();
				CurrentSubdirectories.push_back(std::move(SubDir));
			}
			else if (Entry.is_regular_file(Ec))
			{
				if (Ec) { Ec.clear(); continue; }
				FFileBrowserFileEntry File;
				File.FileName = Name;
				File.Extension = Entry.path().extension().string();
				std::ranges::transform(File.Extension, File.Extension.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
				File.PhysicalPath = Entry.path().string();
				File.FileSize = Entry.is_regular_file(Ec) ? Entry.file_size(Ec) : 0;
				if (Ec) { Ec.clear(); File.FileSize = 0; }
				File.LastWriteTime = Entry.last_write_time(Ec);
				if (Ec) Ec.clear();
				if (!SearchFilter[0] || ContainsInsensitive(File.FileName, SearchFilter.data()))
					Files.push_back(std::move(File));
			}
		}
		std::ranges::sort(CurrentSubdirectories, [](const FFileBrowserDirectoryNode& A, const FFileBrowserDirectoryNode& B) {
			return A.DisplayName < B.DisplayName;
		});
		std::ranges::sort(Files, [](const FFileBrowserFileEntry& A, const FFileBrowserFileEntry& B) {
			return A.FileName < B.FileName;
		});
		CurrentFiles = std::move(Files);
	}

	auto FFileBrowserPanel::SelectDirectory(FFileBrowserDirectoryNode& Node) -> void
	{
		SelectedDirectory = &Node;
		RefreshCurrentDirectory();
	}

	auto FFileBrowserPanel::NavigateTo(const std::string& PhysicalPath) -> void
	{
		// Trim forward history
		if (HistoryIndex >= 0 && static_cast<size_t>(HistoryIndex) + 1 < NavigationHistory.size())
			NavigationHistory.resize(static_cast<size_t>(HistoryIndex) + 1);
		if (NavigationHistory.empty() || NavigationHistory.back() != PhysicalPath)
		{
			NavigationHistory.push_back(PhysicalPath);
			HistoryIndex = static_cast<int32>(NavigationHistory.size()) - 1;
		}
	}

	auto FFileBrowserPanel::CanGoBack() const -> bool { return HistoryIndex > 0; }
	auto FFileBrowserPanel::CanGoForward() const -> bool
	{
		return HistoryIndex >= 0 && static_cast<size_t>(HistoryIndex) + 1 < NavigationHistory.size();
	}

	auto FFileBrowserPanel::GoBack() -> void
	{
		if (!CanGoBack()) return;
		--HistoryIndex;
		// Find or create a temp node for the target
		const std::string& TargetPath = NavigationHistory[static_cast<size_t>(HistoryIndex)];
		// Use NavigateToPath helper: find among all nodes
		std::function<FFileBrowserDirectoryNode*(std::vector<FFileBrowserDirectoryNode>&)> FindNode = [&](std::vector<FFileBrowserDirectoryNode>& Nodes) -> FFileBrowserDirectoryNode* {
			for (FFileBrowserDirectoryNode& Node : Nodes)
			{
				if (Node.PhysicalPath == TargetPath) return &Node;
				if (Node.bChildrenPopulated)
				{
					if (FFileBrowserDirectoryNode* Found = FindNode(Node.Children)) return Found;
				}
			}
			return nullptr;
		};
		if (FFileBrowserDirectoryNode* Found = FindNode(RootNodes))
		{
			SelectDirectory(*Found);
		}
	}

	auto FFileBrowserPanel::GoForward() -> void
	{
		if (!CanGoForward()) return;
		++HistoryIndex;
		const std::string& TargetPath = NavigationHistory[static_cast<size_t>(HistoryIndex)];
		std::function<FFileBrowserDirectoryNode*(std::vector<FFileBrowserDirectoryNode>&)> FindNode = [&](std::vector<FFileBrowserDirectoryNode>& Nodes) -> FFileBrowserDirectoryNode* {
			for (FFileBrowserDirectoryNode& Node : Nodes)
			{
				if (Node.PhysicalPath == TargetPath) return &Node;
				if (Node.bChildrenPopulated)
				{
					if (FFileBrowserDirectoryNode* Found = FindNode(Node.Children)) return Found;
				}
			}
			return nullptr;
		};
		if (FFileBrowserDirectoryNode* Found = FindNode(RootNodes))
		{
			SelectDirectory(*Found);
		}
	}

	auto FFileBrowserPanel::Draw(FLevelEditorContext& Context) -> void
	{
		(void)Context;

		if (!ImGui::Begin("File Browser###FileBrowser", GetOpenPtr()))
		{
			ImGui::End();
			return;
		}

		// --- Top toolbar ---
		{
			ImGui::BeginDisabled(!CanGoBack());
			if (DrawToolbarIconButton(Icons::ArrowLeft, "FileBrowserBack")) GoBack();
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Back");
			ImGui::SameLine();
			ImGui::BeginDisabled(!CanGoForward());
			if (DrawToolbarIconButton(Icons::ArrowRight, "FileBrowserForward")) GoForward();
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Forward");
			ImGui::SameLine();
			const bool bCanGoUp = SelectedDirectory && !SelectedDirectory->bIsMountRoot;
			ImGui::BeginDisabled(!bCanGoUp);
			if (DrawToolbarIconButton(Icons::ArrowUp, "FileBrowserUp"))
			{
				if (SelectedDirectory && !SelectedDirectory->bIsMountRoot)
				{
					const std::filesystem::path ParentPath = std::filesystem::path(SelectedDirectory->PhysicalPath).parent_path();
					if (!ParentPath.empty())
					{
						std::string ParentPathStr = ParentPath.string();
						NavigateTo(ParentPathStr);
						// Search for the parent in RootNodes (need to find among populated children)
						std::function<FFileBrowserDirectoryNode*(std::vector<FFileBrowserDirectoryNode>&, const std::string&)> FindByPath = [&](std::vector<FFileBrowserDirectoryNode>& Nodes, const std::string& Path) -> FFileBrowserDirectoryNode* {
							for (FFileBrowserDirectoryNode& Node : Nodes)
							{
								if (Node.PhysicalPath == Path) return &Node;
								if (Node.bChildrenPopulated)
									if (FFileBrowserDirectoryNode* Found = FindByPath(Node.Children, Path)) return Found;
							}
							return nullptr;
						};
						if (FFileBrowserDirectoryNode* Parent = FindByPath(RootNodes, ParentPathStr))
							SelectDirectory(*Parent);
						else
						{
							// Parent not in tree yet — navigate without tree selection
							SelectedDirectory = nullptr;
							RefreshCurrentDirectory();
						}
					}
				}
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Up to parent");

			ImGui::SameLine();
			ImGui::TextUnformatted(SelectedDirectory ? SelectedDirectory->PhysicalPath.c_str() : "(no directory selected)");

			ImGui::SameLine();
			const float FilterWidth = 200.0f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - FilterWidth);
			ImGui::SetNextItemWidth(FilterWidth);
			if (ImGui::InputTextWithHint("###FileBrowserFilter", "Filter...", SearchFilter.data(), SearchFilter.size()))
				RefreshCurrentDirectory();
		}

		ImGui::Separator();

		// --- Splitter: directory tree (left) | file list (right) ---
		const float StatusBarHeight = ImGui::GetFrameHeight();
		const float SplitterThickness = 6.0f;
		const float AvailWidth = ImGui::GetContentRegionAvail().x;
		const float TreeWidth = std::clamp(AvailWidth * DirectoryTreeWidth, 150.0f, std::max(150.0f, AvailWidth - 200.0f));
		const float FileListWidth = AvailWidth - TreeWidth - SplitterThickness;
		const float ContentHeight = std::max(ImGui::GetContentRegionAvail().y - StatusBarHeight - ImGui::GetFrameHeightWithSpacing(), 50.0f);

		// --- Directory tree ---
		if (ImGui::BeginChild("FileBrowserTree", ImVec2(TreeWidth, ContentHeight), ImGuiChildFlags_Borders))
		{
			for (FFileBrowserDirectoryNode& RootNode : RootNodes)
				DrawDirectoryNode(RootNode, true);
		}
		ImGui::EndChild();

		ImGui::SameLine();

		// --- Splitter ---
		{
			const ImVec2 SplitterPos = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("FileBrowserSplitter", ImVec2(SplitterThickness, ContentHeight));
			if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
			if (ImGui::IsItemActive())
				DirectoryTreeWidth = (ImGui::GetIO().MousePos.x - ImGui::GetWindowPos().x) / ImGui::GetWindowWidth();
			ImGui::SameLine();
		}

		// --- File list ---
		if (ImGui::BeginChild("FileBrowserFiles", ImVec2(FileListWidth, ContentHeight), ImGuiChildFlags_Borders))
		{
			DrawFileList();
		}
		ImGui::EndChild();

		// --- Status bar ---
		{
			ImGui::Separator();
			ImGui::Text("%zu items", CurrentSubdirectories.size() + CurrentFiles.size());
			if (SelectedDirectory)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("| %s", SelectedDirectory->PhysicalPath.c_str());
			}
		}

		// --- Pending delete confirmation ---
		if (PendingDelete != EPendingDelete::None)
		{
			ImGui::OpenPopup("Delete Confirm?");
			PendingDelete = EPendingDelete::None;
		}
		if (ImGui::BeginPopupModal("Delete Confirm?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Delete '%s'?", PendingDeleteName.c_str());
			ImGui::TextDisabled("This action cannot be undone.");
			if (ImGui::Button("Delete"))
			{
				std::error_code Ec;
				std::filesystem::remove_all(PendingDeletePath, Ec);
				// Invalidate the tree cache for the current directory so it repopulates on next expand
				if (SelectedDirectory)
				{
					// If we just deleted the selected directory itself, navigate to parent
					if (SelectedDirectory->PhysicalPath == PendingDeletePath)
					{
						const std::filesystem::path ParentPath = std::filesystem::path(PendingDeletePath).parent_path();
						const std::string ParentPathStr = ParentPath.string();
						SelectedDirectory->bChildrenPopulated = false;
						SelectedDirectory->Children.clear();
						SelectedDirectory = nullptr;
						for (FFileBrowserDirectoryNode& Root : RootNodes)
						{
							if (Root.PhysicalPath == ParentPathStr)
							{
								SelectDirectory(Root);
								break;
							}
							if (Root.bChildrenPopulated)
							{
								for (FFileBrowserDirectoryNode& Child : Root.Children)
								{
									if (Child.PhysicalPath == ParentPathStr)
									{
										SelectDirectory(Child);
										break;
									}
								}
							}
						}
					}
					else
					{
						SelectedDirectory->bChildrenPopulated = false;
						SelectedDirectory->Children.clear();
					}
				}
				RefreshCurrentDirectory();
				PendingDeletePath.clear();
				PendingDeleteName.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				PendingDeletePath.clear();
				PendingDeleteName.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGui::End();
	}

	auto FFileBrowserPanel::DrawDirectoryNode(FFileBrowserDirectoryNode& Node, bool bIsRoot) -> void
	{
		const char* Label = Node.DisplayName.c_str();
		ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (Node.Children.empty() && !Node.bIsMountRoot)
		{
			// Check if it has any subdirectories
			std::error_code Ec;
			bool bHasSubdirs = false;
			for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Node.PhysicalPath, Ec))
			{
				if (Ec) break;
				if (Entry.is_directory(Ec))
				{
					const std::string Name = Entry.path().filename().string();
					if (!bShowHiddenFiles && !Name.empty() && Name.front() == '.') continue;
					bHasSubdirs = true;
					break;
				}
			}
			if (!bHasSubdirs) Flags |= ImGuiTreeNodeFlags_Leaf;
		}
		if (bIsRoot && RootNodes.size() == 1) Flags |= ImGuiTreeNodeFlags_DefaultOpen;

		// Populate children before opening
		if (!Node.bChildrenPopulated && !(Flags & ImGuiTreeNodeFlags_Leaf))
			PopulateChildren(Node);

		bool bOpen = ImGui::TreeNodeEx(Label, Flags);

		// Click to select
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		{
			NavigateTo(Node.PhysicalPath);
			SelectDirectory(Node);
		}

		// Right-click context menu for directories
		if (ImGui::BeginPopupContextItem())
		{
			DrawDirectoryContextMenu(Node);
			ImGui::EndPopup();
		}

		if (bOpen)
		{
			for (FFileBrowserDirectoryNode& Child : Node.Children)
				DrawDirectoryNode(Child, false);
			ImGui::TreePop();
		}
	}

	auto FFileBrowserPanel::DrawDirectoryContextMenu(FFileBrowserDirectoryNode& Node) -> void
	{
		if (ImGui::MenuItem("New Folder"))
			CreateNewFolder(Node);
		if (!Node.bIsMountRoot)
		{
			if (ImGui::MenuItem("Delete"))
				DeleteDirectory(Node);
			ImGui::Separator();
			if (ImGui::MenuItem("Show in Explorer"))
				ShowInExplorer(Node.PhysicalPath);
		}
	}

	auto FFileBrowserPanel::DrawFileList() -> void
	{
		if (!SelectedDirectory)
		{
			ImGui::TextDisabled("Select a directory to browse files.");
			return;
		}

		if (ImGui::BeginTable("FileBrowserTable", 3, FileTableFlags))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableHeadersRow();

			// Subdirectories first
			for (size_t DirIdx = 0; DirIdx < CurrentSubdirectories.size(); ++DirIdx)
			{
				const FFileBrowserDirectoryNode& SubDir = CurrentSubdirectories[DirIdx];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				ImGui::PushID(static_cast<int>(DirIdx));
				std::string DisplayName = std::format("[D] {}", SubDir.DisplayName);
				if (ImGui::Selectable(DisplayName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
				{
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						NavigateTo(SubDir.PhysicalPath);
						if (SelectedDirectory)
						{
							if (!SelectedDirectory->bChildrenPopulated)
								PopulateChildren(*SelectedDirectory);
							for (FFileBrowserDirectoryNode& Child : SelectedDirectory->Children)
							{
								if (Child.PhysicalPath == SubDir.PhysicalPath)
								{
									SelectDirectory(Child);
									break;
								}
							}
						}
					}
				}
				ImGui::PopID();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted("-");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted("-");
			}

			// Files
			for (size_t FileIdx = 0; FileIdx < CurrentFiles.size(); ++FileIdx)
			{
				const FFileBrowserFileEntry& File = CurrentFiles[FileIdx];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				ImGui::PushID(static_cast<int>(FileIdx + 10000));
				std::string DisplayName = std::format("{} {}", GetFileTypePrefix(File.Extension), File.FileName);
				if (ImGui::Selectable(DisplayName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
				{
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						OpenFile(File);
				}

				if (ImGui::BeginPopupContextItem())
				{
					DrawFileContextMenu(File);
					ImGui::EndPopup();
				}

				ImGui::PopID();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(FormatFileSize(File.FileSize).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(FormatFileTime(File.LastWriteTime).c_str());
			}

			ImGui::EndTable();
		}
	}

	auto FFileBrowserPanel::DrawFileContextMenu(const FFileBrowserFileEntry& File) -> void
	{
		if (ImGui::MenuItem("Open"))
			OpenFile(File);
		ImGui::Separator();
		if (ImGui::MenuItem("Delete"))
			DeleteFile(File);
		if (ImGui::MenuItem("Rename"))
		{
			bRenameActive = true;
			RenameTargetPath = File.PhysicalPath;
			const std::string Stem = std::filesystem::path(File.FileName).stem().string();
			std::ranges::copy(Stem, RenameBuffer.begin());
			RenameBuffer[std::min(Stem.size(), RenameBuffer.size() - 1)] = '\0';
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Show in Explorer"))
			ShowInExplorer(File.PhysicalPath);
	}

	auto FFileBrowserPanel::FormatFileSize(uintmax_t Bytes) const -> std::string
	{
		if (Bytes < 1024) return std::format("{} B", Bytes);
		if (Bytes < 1024 * 1024) return std::format("{:.1f} KB", Bytes / 1024.0);
		if (Bytes < 1024 * 1024 * 1024) return std::format("{:.1f} MB", Bytes / (1024.0 * 1024.0));
		return std::format("{:.2f} GB", Bytes / (1024.0 * 1024.0 * 1024.0));
	}

	auto FFileBrowserPanel::FormatFileTime(const std::filesystem::file_time_type& Time) const -> std::string
	{
		const auto SysTime = std::chrono::clock_cast<std::chrono::system_clock>(Time);
		const std::time_t TimeT = std::chrono::system_clock::to_time_t(SysTime);
		std::tm LocalTime{};
		localtime_s(&LocalTime, &TimeT);
		std::array<char, 64> Buffer{};
		std::strftime(Buffer.data(), Buffer.size(), "%Y-%m-%d %H:%M", &LocalTime);
		return std::string(Buffer.data());
	}

	auto FFileBrowserPanel::GetFileTypePrefix(std::string_view Extension) const -> const char*
	{
		if (Extension == ".dasset") return "[A]";
		if (Extension == ".yaml" || Extension == ".yml" || Extension == ".json") return "[*]";
		if (Extension == ".obj" || Extension == ".fbx" || Extension == ".gltf" || Extension == ".glb") return "[M]";
		if (Extension == ".png" || Extension == ".jpg" || Extension == ".jpeg" || Extension == ".tga" || Extension == ".dds")
			return "[T]";
		if (Extension == ".slang" || Extension == ".hlsl" || Extension == ".glsl") return "[S]";
		return "[F]";
	}

	// --- File operations ---

	auto FFileBrowserPanel::OpenFile(const FFileBrowserFileEntry& File) -> void
	{
		if (File.Extension == ".dasset" && OpenAsset)
		{
			const std::filesystem::path SelectedPath = std::filesystem::absolute(File.PhysicalPath).lexically_normal();
			for (const auto& [AssetPath, AssetData] : Asset::GetAssetRegistry().GetAssets())
			{
				if (std::filesystem::absolute(AssetData.PhysicalPath).lexically_normal() == SelectedPath && OpenAsset(AssetPath.ToString()))
					return;
			}
		}
#ifdef _WIN32
		std::filesystem::path FsPath(File.PhysicalPath);
		const std::wstring WidePath = FsPath.make_preferred().wstring();
		ShellExecuteW(nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOW);
#else
		(void)File;
#endif
	}

	auto FFileBrowserPanel::DeleteFile(const FFileBrowserFileEntry& File) -> void
	{
		PendingDelete = EPendingDelete::File;
		PendingDeletePath = File.PhysicalPath;
		PendingDeleteName = File.FileName;
	}

	auto FFileBrowserPanel::DeleteDirectory(FFileBrowserDirectoryNode& Node) -> void
	{
		PendingDelete = EPendingDelete::Directory;
		PendingDeletePath = Node.PhysicalPath;
		PendingDeleteName = Node.DisplayName;
	}

	auto FFileBrowserPanel::ShowInExplorer(const std::string& PhysicalPath) -> void
	{
#ifdef _WIN32
		std::filesystem::path FsPath(PhysicalPath);
		std::wstring WidePath = FsPath.make_preferred().wstring();
		if (std::filesystem::is_directory(FsPath))
			ShellExecuteW(nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOW);
		else
		{
			const std::wstring Params = L"/select,\"" + WidePath + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", Params.c_str(), nullptr, SW_SHOW);
		}
#else
		(void)PhysicalPath;
#endif
	}

	auto FFileBrowserPanel::CreateNewFolder(FFileBrowserDirectoryNode& ParentNode) -> void
	{
		std::string FolderName;
		for (int32 Counter = 1; Counter < 1000; ++Counter)
		{
			const std::string Candidate = Counter == 1 ? "New Folder" : std::format("New Folder ({})", Counter);
			const std::filesystem::path CandidatePath = std::filesystem::path(ParentNode.PhysicalPath) / Candidate;
			if (!std::filesystem::exists(CandidatePath))
			{
				FolderName = Candidate;
				break;
			}
		}
		if (FolderName.empty()) return;

		const std::filesystem::path NewFolderPath = std::filesystem::path(ParentNode.PhysicalPath) / FolderName;
		std::error_code Ec;
		if (std::filesystem::create_directory(NewFolderPath, Ec) && !Ec)
		{
			// Add directly to children instead of repopulating, to avoid invalidating SelectedDirectory pointer
			FFileBrowserDirectoryNode NewChild;
			NewChild.DisplayName = FolderName;
			NewChild.PhysicalPath = NewFolderPath.string();
			ParentNode.Children.push_back(std::move(NewChild));
			std::ranges::sort(ParentNode.Children, [](const FFileBrowserDirectoryNode& A, const FFileBrowserDirectoryNode& B) {
				return A.DisplayName < B.DisplayName;
			});
			ParentNode.bChildrenPopulated = true;
			if (&ParentNode == SelectedDirectory)
				RefreshCurrentDirectory();
		}
	}
} // namespace Durin
