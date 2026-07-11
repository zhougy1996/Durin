#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	struct FFileDialogFilter
	{
		std::string DisplayName;
		std::string Pattern;
	};

	struct FFileDialogRequest
	{
		void* ParentWindowHandle = nullptr;
		std::string Title;
		std::vector<FFileDialogFilter> Filters;
		std::string InitialDirectory;
		std::string DefaultFileName;
	};

	enum class EFileDialogStatus : uint8
	{
		Selected,
		Cancelled,
		Error
	};

	struct FFileDialogResult
	{
		EFileDialogStatus Status = EFileDialogStatus::Error;
		std::string FilePath;
		std::string ErrorMessage;
	};

	APPLICATIONCORE_API auto OpenFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult;
	APPLICATIONCORE_API auto SaveFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult;
} // namespace Durin
