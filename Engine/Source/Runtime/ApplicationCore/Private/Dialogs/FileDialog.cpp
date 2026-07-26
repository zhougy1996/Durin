#include "Dialogs/FileDialog.h"

#include "Misc/StringConvert.h"

#if defined(_WIN32)
#include <shobjidl.h>
#endif

namespace Durin
{
#if defined(_WIN32)
	namespace
	{
		// Balances COM apartment initialization for one synchronous dialog call.
		class FComScope
		{
		public:
			FComScope()
				: Result(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
			{
			}

			~FComScope()
			{
				if (SUCCEEDED(Result)) ::CoUninitialize();
			}

			HRESULT Result;
		};

		auto HResultMessage(std::string_view Operation, HRESULT Result) -> std::string
		{
			return std::format("{} failed (HRESULT 0x{:08X}).", Operation, static_cast<uint32>(Result));
		}

		auto SetInitialFolder(IFileDialog* Dialog, std::string_view InitialDirectory) -> HRESULT
		{
			if (InitialDirectory.empty()) return S_OK;

			std::wstring NativeDirectory = StringUtils::Utf8ToWide(InitialDirectory);
			std::ranges::replace(NativeDirectory, L'/', L'\\');

			IShellItem* InitialFolder = nullptr;
			HRESULT Result = ::SHCreateItemFromParsingName(NativeDirectory.c_str(), nullptr, IID_PPV_ARGS(&InitialFolder));
			if (FAILED(Result)) return Result;

			Result = Dialog->SetFolder(InitialFolder);
			InitialFolder->Release();
			return Result;
		}
	}
#endif

	auto OpenFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult
	{
#if defined(_WIN32)
		FComScope ComScope;
		if (FAILED(ComScope.Result)) return {EFileDialogStatus::Error, {}, HResultMessage("COM initialization", ComScope.Result)};

		IFileOpenDialog* Dialog = nullptr;
		HRESULT Result = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog));
		if (FAILED(Result)) return {EFileDialogStatus::Error, {}, HResultMessage("Creating the file dialog", Result)};

		if (!Request.Title.empty())
		{
			const std::wstring Title = StringUtils::Utf8ToWide(Request.Title);
			Dialog->SetTitle(Title.c_str());
		}

		FILEOPENDIALOGOPTIONS Options = 0;
		if (SUCCEEDED(Dialog->GetOptions(&Options)))
		{
			Dialog->SetOptions(Options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
		}

		std::vector<std::wstring> FilterNames;
		std::vector<std::wstring> FilterPatterns;
		std::vector<COMDLG_FILTERSPEC> FilterSpecs;
		FilterNames.reserve(Request.Filters.size());
		FilterPatterns.reserve(Request.Filters.size());
		for (const FFileDialogFilter& Filter : Request.Filters)
		{
			FilterNames.push_back(StringUtils::Utf8ToWide(Filter.DisplayName));
			FilterPatterns.push_back(StringUtils::Utf8ToWide(Filter.Pattern));
		}
		FilterSpecs.reserve(Request.Filters.size());
		for (size_t Index = 0; Index < Request.Filters.size(); ++Index)
		{
			FilterSpecs.push_back({FilterNames[Index].c_str(), FilterPatterns[Index].c_str()});
		}
		if (!FilterSpecs.empty()) Dialog->SetFileTypes(static_cast<UINT>(FilterSpecs.size()), FilterSpecs.data());

		Result = SetInitialFolder(Dialog, Request.InitialDirectory);
		if (FAILED(Result))
		{
			Dialog->Release();
			return {EFileDialogStatus::Error, {}, HResultMessage("Setting the initial file dialog folder", Result)};
		}

		Result = Dialog->Show(static_cast<HWND>(Request.ParentWindowHandle));
		if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
		{
			Dialog->Release();
			return {EFileDialogStatus::Cancelled, {}, {}};
		}
		if (FAILED(Result))
		{
			Dialog->Release();
			return {EFileDialogStatus::Error, {}, HResultMessage("Showing the file dialog", Result)};
		}

		IShellItem* SelectedItem = nullptr;
		Result = Dialog->GetResult(&SelectedItem);
		Dialog->Release();
		if (FAILED(Result)) return {EFileDialogStatus::Error, {}, HResultMessage("Reading the selected file", Result)};

		PWSTR SelectedPath = nullptr;
		Result = SelectedItem->GetDisplayName(SIGDN_FILESYSPATH, &SelectedPath);
		SelectedItem->Release();
		if (FAILED(Result)) return {EFileDialogStatus::Error, {}, HResultMessage("Reading the selected file path", Result)};

		std::string FilePath = StringUtils::WideToUtf8(SelectedPath);
		::CoTaskMemFree(SelectedPath);
		return {EFileDialogStatus::Selected, std::move(FilePath), {}};
#else
		return {EFileDialogStatus::Error, {}, "Native file dialogs are not supported on this platform."};
#endif
	}

	auto SaveFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult
	{
#if defined(_WIN32)
		FComScope ComScope;
		if (FAILED(ComScope.Result)) return {EFileDialogStatus::Error, {}, HResultMessage("COM initialization", ComScope.Result)};

		IFileSaveDialog* Dialog = nullptr;
		HRESULT Result = ::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog));
		if (FAILED(Result)) return {EFileDialogStatus::Error, {}, HResultMessage("Creating the save dialog", Result)};

		if (!Request.Title.empty())
		{
			const std::wstring Title = StringUtils::Utf8ToWide(Request.Title);
			Dialog->SetTitle(Title.c_str());
		}
		if (!Request.DefaultFileName.empty())
		{
			const std::wstring DefaultFileName = StringUtils::Utf8ToWide(Request.DefaultFileName);
			Dialog->SetFileName(DefaultFileName.c_str());
		}

		FILEOPENDIALOGOPTIONS Options = 0;
		if (SUCCEEDED(Dialog->GetOptions(&Options)))
		{
			Dialog->SetOptions(Options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
		}

		std::vector<std::wstring> FilterNames;
		std::vector<std::wstring> FilterPatterns;
		std::vector<COMDLG_FILTERSPEC> FilterSpecs;
		FilterNames.reserve(Request.Filters.size());
		FilterPatterns.reserve(Request.Filters.size());
		for (const FFileDialogFilter& Filter : Request.Filters)
		{
			FilterNames.push_back(StringUtils::Utf8ToWide(Filter.DisplayName));
			FilterPatterns.push_back(StringUtils::Utf8ToWide(Filter.Pattern));
		}
		FilterSpecs.reserve(Request.Filters.size());
		for (size_t Index = 0; Index < Request.Filters.size(); ++Index)
		{
			FilterSpecs.push_back({FilterNames[Index].c_str(), FilterPatterns[Index].c_str()});
		}
		if (!FilterSpecs.empty()) Dialog->SetFileTypes(static_cast<UINT>(FilterSpecs.size()), FilterSpecs.data());
		Dialog->SetDefaultExtension(L"dasset");

		Result = SetInitialFolder(Dialog, Request.InitialDirectory);
		if (FAILED(Result))
		{
			Dialog->Release();
			return {EFileDialogStatus::Error, {}, HResultMessage("Setting the initial file dialog folder", Result)};
		}

		Result = Dialog->Show(static_cast<HWND>(Request.ParentWindowHandle));
		if (Result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
		{
			Dialog->Release();
			return {EFileDialogStatus::Cancelled, {}, {}};
		}
		if (FAILED(Result))
		{
			Dialog->Release();
			return {EFileDialogStatus::Error, {}, HResultMessage("Showing the save dialog", Result)};
		}

		IShellItem* SelectedItem = nullptr;
		Result = Dialog->GetResult(&SelectedItem);
		Dialog->Release();
		if (FAILED(Result)) return {EFileDialogStatus::Error, {}, HResultMessage("Reading the asset destination", Result)};

		PWSTR SelectedPath = nullptr;
		Result = SelectedItem->GetDisplayName(SIGDN_FILESYSPATH, &SelectedPath);
		SelectedItem->Release();
		if (FAILED(Result)) return {EFileDialogStatus::Error, {}, HResultMessage("Reading the asset destination path", Result)};

		std::string FilePath = StringUtils::WideToUtf8(SelectedPath);
		::CoTaskMemFree(SelectedPath);
		return {EFileDialogStatus::Selected, std::move(FilePath), {}};
#else
		return {EFileDialogStatus::Error, {}, "Native file dialogs are not supported on this platform."};
#endif
	}
} // namespace Durin
