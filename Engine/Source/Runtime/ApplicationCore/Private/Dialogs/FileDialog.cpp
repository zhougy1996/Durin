#include "Dialogs/FileDialog.h"

#include "Misc/StringConvert.h"

#if defined(_WIN32)
#include <shobjidl.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace Durin
{
	namespace
	{
		auto NonInteractiveDialogError() -> FFileDialogResult
		{
			return {EFileDialogStatus::Error, {},
				"Native dialog interaction is disabled for this request."};
		}
	}

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
#elif defined(__APPLE__)
	namespace
	{
		auto MacOSDialogError(std::string Message) -> FFileDialogResult
		{
			return {EFileDialogStatus::Error, {}, std::move(Message)};
		}

		auto FileExtensions(const FFileDialogRequest& Request) -> std::string
		{
			std::vector<std::string> Extensions;
			for (const FFileDialogFilter& Filter : Request.Filters)
			{
				std::string Pattern = Filter.Pattern;
				std::ranges::replace(Pattern, ';', ',');
				for (size_t Start = 0; Start <= Pattern.size();)
				{
					const size_t End = Pattern.find(',', Start);
					std::string Extension = Pattern.substr(
						Start, End == std::string::npos ? std::string::npos : End - Start);
					while (Extension.starts_with('*') || Extension.starts_with('.'))
						Extension.erase(Extension.begin());
					if (!Extension.empty()) Extensions.push_back(std::move(Extension));
					if (End == std::string::npos) break;
					Start = End + 1;
				}
			}
			std::ranges::sort(Extensions);
			Extensions.erase(std::unique(Extensions.begin(), Extensions.end()), Extensions.end());
			std::string Result;
			for (const std::string& Extension : Extensions)
			{
				if (!Result.empty()) Result.push_back(',');
				Result += Extension;
			}
			return Result;
		}

		auto RunMacOSDialog(
			const FFileDialogRequest& Request,
			std::string_view Kind) -> FFileDialogResult
		{
			if (pthread_main_np() == 0)
				return MacOSDialogError("macOS native dialogs must be opened on the main thread.");

			static constexpr std::string_view Script = R"APPLESCRIPT(
on splitExtensions(extensionText)
    if extensionText is "" then return {}
    set oldDelimiters to AppleScript's text item delimiters
    set AppleScript's text item delimiters to ","
    set extensionItems to text items of extensionText
    set AppleScript's text item delimiters to oldDelimiters
    return extensionItems
end splitExtensions

on run argv
    set dialogKind to item 1 of argv
    set promptText to item 2 of argv
    set initialText to item 3 of argv
    set extensionItems to splitExtensions(item 4 of argv)
    set defaultName to item 5 of argv
    if promptText is "" then set promptText to "Select a path"
    if dialogKind is "folder" then
        if initialText is "" then
            set picked to choose folder with prompt promptText
        else
            set picked to choose folder with prompt promptText default location POSIX file initialText
        end if
    else if dialogKind is "save" then
        if initialText is "" then
            set picked to choose file name with prompt promptText default name defaultName
        else
            set picked to choose file name with prompt promptText default name defaultName default location POSIX file initialText
        end if
    else
        if initialText is "" then
            if (count extensionItems) is 0 then
                set picked to choose file with prompt promptText
            else
                set picked to choose file with prompt promptText of type extensionItems
            end if
        else
            if (count extensionItems) is 0 then
                set picked to choose file with prompt promptText default location POSIX file initialText
            else
                set picked to choose file with prompt promptText of type extensionItems default location POSIX file initialText
            end if
        end if
    end if
    return POSIX path of picked
end run
)APPLESCRIPT";

			std::array<std::string, 10> Storage = {
				"/usr/bin/osascript", "-e", std::string(Script), "--",
				std::string(Kind), Request.Title, Request.InitialDirectory,
				FileExtensions(Request), Request.DefaultFileName, {}};
			std::array<char*, 10> Arguments{};
			for (size_t Index = 0; Index + 1 < Storage.size(); ++Index)
				Arguments[Index] = Storage[Index].data();

			int OutputPipe[2] = {-1, -1};
			if (pipe(OutputPipe) != 0)
				return MacOSDialogError(std::format(
					"Could not create native dialog output pipe: macOS error {}.", errno));
			posix_spawn_file_actions_t Actions;
			posix_spawn_file_actions_init(&Actions);
			posix_spawn_file_actions_adddup2(&Actions, OutputPipe[1], STDOUT_FILENO);
			posix_spawn_file_actions_adddup2(&Actions, OutputPipe[1], STDERR_FILENO);
			posix_spawn_file_actions_addclose(&Actions, OutputPipe[0]);
			pid_t Child = 0;
			const int SpawnResult = posix_spawn(&Child, Storage[0].c_str(), &Actions,
				nullptr, Arguments.data(), environ);
			posix_spawn_file_actions_destroy(&Actions);
			close(OutputPipe[1]);
			if (SpawnResult != 0)
			{
				close(OutputPipe[0]);
				return MacOSDialogError(std::format(
					"Could not start macOS native dialog: macOS error {}.", SpawnResult));
			}

			std::string Output;
			std::array<char, 1024> Buffer{};
			for (;;)
			{
				const ssize_t Count = read(OutputPipe[0], Buffer.data(), Buffer.size());
				if (Count > 0) Output.append(Buffer.data(), static_cast<size_t>(Count));
				else if (Count < 0 && errno == EINTR) continue;
				else break;
			}
			close(OutputPipe[0]);
			int Status = 0;
			while (waitpid(Child, &Status, 0) < 0 && errno == EINTR) {}
			while (!Output.empty() && std::isspace(static_cast<unsigned char>(Output.back())))
				Output.pop_back();
			if (WIFEXITED(Status) && WEXITSTATUS(Status) == 0 && !Output.empty())
				return {EFileDialogStatus::Selected,
					std::filesystem::path(Output).lexically_normal().generic_string(), {}};
			if (Output.find("(-128)") != std::string::npos
				|| Output.find("User canceled") != std::string::npos)
				return {EFileDialogStatus::Cancelled, {}, {}};
			return MacOSDialogError(Output.empty()
				? "macOS native dialog exited without a selection or diagnostic."
				: std::format("macOS native dialog failed: {}", Output));
		}
	}
#endif

	auto OpenFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult
	{
		if (!Request.bAllowUserInteraction) return NonInteractiveDialogError();

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
#elif defined(__APPLE__)
		return RunMacOSDialog(Request, "open");
#else
		return {EFileDialogStatus::Error, {}, "Native file dialogs are not supported on this platform."};
#endif
	}

	auto OpenFolderDialog(const FFileDialogRequest& Request) -> FFileDialogResult
	{
		if (!Request.bAllowUserInteraction) return NonInteractiveDialogError();

#if defined(_WIN32)
		FComScope ComScope;
		if (FAILED(ComScope.Result))
			return {EFileDialogStatus::Error, {},
				HResultMessage("COM initialization", ComScope.Result)};

		IFileOpenDialog* Dialog = nullptr;
		HRESULT Result = ::CoCreateInstance(
			CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog));
		if (FAILED(Result))
			return {EFileDialogStatus::Error, {},
				HResultMessage("Creating the folder dialog", Result)};

		if (!Request.Title.empty())
		{
			const std::wstring Title = StringUtils::Utf8ToWide(Request.Title);
			Dialog->SetTitle(Title.c_str());
		}

		FILEOPENDIALOGOPTIONS Options = 0;
		if (SUCCEEDED(Dialog->GetOptions(&Options)))
		{
			Dialog->SetOptions(Options | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS
				| FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
		}

		Result = SetInitialFolder(Dialog, Request.InitialDirectory);
		if (FAILED(Result))
		{
			Dialog->Release();
			return {EFileDialogStatus::Error, {},
				HResultMessage("Setting the initial folder dialog directory", Result)};
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
			return {EFileDialogStatus::Error, {},
				HResultMessage("Showing the folder dialog", Result)};
		}

		IShellItem* SelectedItem = nullptr;
		Result = Dialog->GetResult(&SelectedItem);
		Dialog->Release();
		if (FAILED(Result))
			return {EFileDialogStatus::Error, {},
				HResultMessage("Reading the selected folder", Result)};

		PWSTR SelectedPath = nullptr;
		Result = SelectedItem->GetDisplayName(SIGDN_FILESYSPATH, &SelectedPath);
		SelectedItem->Release();
		if (FAILED(Result))
			return {EFileDialogStatus::Error, {},
				HResultMessage("Reading the selected folder path", Result)};

		std::string FolderPath = StringUtils::WideToUtf8(SelectedPath);
		::CoTaskMemFree(SelectedPath);
		return {EFileDialogStatus::Selected, std::move(FolderPath), {}};
#elif defined(__APPLE__)
		return RunMacOSDialog(Request, "folder");
#else
		return {EFileDialogStatus::Error, {},
			"Native folder dialogs are not supported on this platform."};
#endif
	}

	auto SaveFileDialog(const FFileDialogRequest& Request) -> FFileDialogResult
	{
		if (!Request.bAllowUserInteraction) return NonInteractiveDialogError();

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
#elif defined(__APPLE__)
		return RunMacOSDialog(Request, "save");
#else
		return {EFileDialogStatus::Error, {}, "Native file dialogs are not supported on this platform."};
#endif
	}
} // namespace Durin
