#pragma once

#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Owns a pending name and invocation; opening or cancelling never creates a package.
	class FAssetCreationDialog
	{
	public:
		explicit FAssetCreationDialog(FAssetCreationDescriptor InDescriptor);
		auto Open(const FExtensionInvocation& InInvocation) -> void;
		auto Cancel() -> void;
		auto SetName(std::string_view InName) -> void;
		// Revalidates occupancy and mutation admission immediately before calling the provider.
		auto Confirm(bool bAllowAssetMutation) -> bool;
		auto Draw(bool bAllowAssetMutation) -> void;
		auto IsPending() const -> bool { return bPending; }
		auto GetError() const -> const std::string& { return Error; }

	private:
		auto Validate(FTopLevelAssetPath& OutPath, std::string& OutError) const -> bool;

		FAssetCreationDescriptor Descriptor;
		FExtensionInvocation Invocation;
		std::string PopupTitle;
		std::string Directory;
		std::array<char, 256> Name{};
		std::string Error;
		bool bPending = false;
		bool bOpenRequested = false;
	};
}
