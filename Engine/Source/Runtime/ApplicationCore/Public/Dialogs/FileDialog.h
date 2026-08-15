#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	// Maps a user-facing file type name to a native-dialog pattern.
	struct FFileDialogFilter
	{
		std::string DisplayName;
		std::string Pattern;
	};

	// Describes a native open/save dialog request and its optional owner.
	struct FFileDialogRequest
	{
		// Non-owning native handle used to make the dialog modal when provided.
		void* ParentWindowHandle = nullptr;
		std::string Title;
		std::vector<FFileDialogFilter> Filters;
		std::string InitialDirectory;
		std::string DefaultFileName;
		// Tests and headless tools disable interaction and receive an explicit error.
		bool bAllowUserInteraction = true;
	};

	// Distinguishes a selection from cancellation and native-dialog failure.
	enum class EFileDialogStatus : uint8
	{
		Selected,
		Cancelled,
		Error
	};

	// Carries the selected path or an error message from a native file dialog.
	struct FFileDialogResult
	{
		EFileDialogStatus Status = EFileDialogStatus::Error;
		std::string FilePath;
		std::string ErrorMessage;
	};

	APPLICATIONCORE_API auto OpenFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult;
	APPLICATIONCORE_API auto OpenFolderDialog(const FFileDialogRequest& Request) -> FFileDialogResult;
	APPLICATIONCORE_API auto SaveFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult;
} // namespace Durin
