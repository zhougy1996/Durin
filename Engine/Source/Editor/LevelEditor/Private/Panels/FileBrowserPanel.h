#pragma once

#include "Panels/LevelEditorPanel.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace Durin
{
	struct FLevelEditorContext;

	struct FFileBrowserDirectoryNode
	{
		std::string DisplayName;
		std::string PhysicalPath;
		bool bIsMountRoot = false;
		bool bChildrenPopulated = false;
		std::vector<FFileBrowserDirectoryNode> Children;
	};

	struct FFileBrowserFileEntry
	{
		std::string FileName;
		std::string Extension;
		std::string PhysicalPath;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime;
	};

	class FFileBrowserPanel final : public ILevelEditorPanel
	{
	public:
		explicit FFileBrowserPanel(std::function<bool(const std::string&)> OpenAsset);
		~FFileBrowserPanel() override = default;

		auto GetWindowName() const -> const char* override { return "File Browser"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		// Data model
		auto BuildRootNodes() -> void;
		auto PopulateChildren(FFileBrowserDirectoryNode& Node) -> void;
		auto RefreshCurrentDirectory() -> void;
		auto SelectDirectory(FFileBrowserDirectoryNode& Node) -> void;

		// Navigation
		auto NavigateTo(const std::string& PhysicalPath) -> void;
		auto CanGoBack() const -> bool;
		auto CanGoForward() const -> bool;
		auto GoBack() -> void;
		auto GoForward() -> void;

		// Directory tree
		auto DrawDirectoryTree() -> void;
		auto DrawDirectoryNode(FFileBrowserDirectoryNode& Node, bool bIsRoot) -> void;
		auto DrawDirectoryContextMenu(FFileBrowserDirectoryNode& Node) -> void;

		// File list
		auto DrawFileList() -> void;
		auto DrawFileContextMenu(const FFileBrowserFileEntry& File) -> void;
		auto FormatFileSize(uintmax_t Bytes) const -> std::string;
		auto FormatFileTime(const std::filesystem::file_time_type& Time) const -> std::string;
		auto GetFileTypePrefix(std::string_view Extension) const -> const char*;

		// File operations
		auto OpenFile(const FFileBrowserFileEntry& File) -> void;
		auto DeleteFile(const FFileBrowserFileEntry& File) -> void;
		auto DeleteDirectory(FFileBrowserDirectoryNode& Node) -> void;
		auto ShowInExplorer(const std::string& PhysicalPath) -> void;
		auto CreateNewFolder(FFileBrowserDirectoryNode& ParentNode) -> void;

		// Directory tree data
		std::vector<FFileBrowserDirectoryNode> RootNodes;
		FFileBrowserDirectoryNode* SelectedDirectory = nullptr;

		// Current directory contents
		std::vector<FFileBrowserFileEntry> CurrentFiles;
		std::vector<FFileBrowserDirectoryNode> CurrentSubdirectories;

		// Navigation history
		std::vector<std::string> NavigationHistory;
		int32 HistoryIndex = -1;

		// UI state
		std::array<char, 256> SearchFilter{};
		float DirectoryTreeWidth = 0.30f;
		bool bShowHiddenFiles = false;

		// Rename state
		bool bRenameActive = false;
		std::string RenameTargetPath;
		std::array<char, 256> RenameBuffer{};

		// Pending delete confirmation
		enum class EPendingDelete { None, File, Directory };
		EPendingDelete PendingDelete = EPendingDelete::None;
		std::string PendingDeletePath;
		std::string PendingDeleteName;
		std::function<bool(const std::string&)> OpenAsset;
	};
} // namespace Durin
