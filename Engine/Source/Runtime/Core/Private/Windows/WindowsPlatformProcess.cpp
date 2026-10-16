#include "Windows/WindowsPlatformProcess.h"
#include "Misc/StringConvert.h"
#include "HAL/ProcessLaunch.h"

#include <shellapi.h>

namespace Durin
{
	namespace
	{
		auto FormatWindowsError(DWORD Error) -> std::string
		{
			wchar_t* MessageBuffer = nullptr;
			const DWORD MessageLength = FormatMessageW(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr,
				Error,
				0,
				reinterpret_cast<wchar_t*>(&MessageBuffer),
				0,
				nullptr
			);
			if (MessageLength == 0) return std::format("Windows error {}", Error);

			std::wstring_view Message(MessageBuffer, MessageLength);
			while (!Message.empty() && (Message.back() == L'\r' || Message.back() == L'\n' || Message.back() == L' '))
				Message.remove_suffix(1);
			const std::string Result = std::format("Windows error {}: {}", Error, StringUtils::WideToUtf8(Message));
			LocalFree(MessageBuffer);
			return Result;
		}

		// Microsoft CRT argv parsing doubles backslashes before quotes and before
		// the closing quote. Simple arguments need no quotes; empty ones always do.
		auto QuoteArgument(std::wstring_view Argument) -> std::wstring
		{
			if (!Argument.empty() && Argument.find_first_of(L" \t\r\n\"") == Argument.npos)
				return std::wstring(Argument);
			std::wstring Result = L"\"";
			size_t Backslashes = 0;
			for (const wchar_t Character : Argument)
			{
				if (Character == L'\\') { ++Backslashes; continue; }
				Result.append(Backslashes * (Character == L'"' ? 2 : 1), L'\\');
				Backslashes = 0;
				if (Character == L'"') Result.push_back(L'\\');
				Result.push_back(Character);
			}
			Result.append(Backslashes * 2, L'\\');
			Result.push_back(L'"');
			return Result;
		}

		struct FEnvironmentNameLess
		{
			auto operator()(const std::wstring& Left, const std::wstring& Right) const -> bool
			{
				return CompareStringOrdinal(Left.c_str(), -1, Right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
			}
		};

		auto BuildEnvironment(std::string_view Executable, const FProcessLaunchOptions& Options)
			-> std::expected<std::vector<wchar_t>, FPlatformProcessError>
		{
			std::map<std::wstring, std::wstring, FEnvironmentNameLess> Environment;
			const auto Native = GetEnvironmentStringsW();
			if (!Native)
			{
				const DWORD Error = GetLastError();
				return std::unexpected(FPlatformProcessError{EPlatformProcessError::Launch,
					FormatWindowsError(Error), std::string(Executable), Error});
			}
			std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> Snapshot(Native, &FreeEnvironmentStringsW);
			for (const wchar_t* Entry = Snapshot.get(); *Entry;)
			{
				const std::wstring_view Text(Entry);
				// Preserve Windows' inherited per-drive entries, such as =C:=C:\\.
				const auto Separator = Text.find(L'=', Text.front() == L'=' ? 1 : 0);
				if (Separator != Text.npos)
					Environment[std::wstring(Text.substr(0, Separator))] = Text.substr(Separator + 1);
				Entry += Text.size() + 1;
			}
			std::set<std::wstring, FEnvironmentNameLess> Names;
			for (const auto& [Name, Value] : Options.EnvironmentOverrides)
			{
				auto WideName = StringUtils::Utf8ToWide(Name);
				if (!Names.insert(WideName).second)
					return std::unexpected(Detail::InvalidProcessLaunch(Executable,
						"Environment overrides contain duplicate Windows names differing only in case."));
				if (Value) Environment[WideName] = StringUtils::Utf8ToWide(*Value);
				else Environment.erase(WideName);
			}
			std::vector<wchar_t> Block;
			for (const auto& [Name, Value] : Environment)
			{
				Block.insert(Block.end(), Name.begin(), Name.end());
				Block.push_back(L'=');
				Block.insert(Block.end(), Value.begin(), Value.end());
				Block.push_back(L'\0');
			}
			if (Block.empty()) Block.push_back(L'\0');
			Block.push_back(L'\0');
			return Block;
		}

		auto Spawn(std::string_view Executable, const FProcessLaunchOptions& Options)
			-> std::expected<PROCESS_INFORMATION, FPlatformProcessError>
		{
			if (const auto Valid = Detail::ValidateProcessLaunch(Executable, Options); !Valid)
				return std::unexpected(Valid.error());
			const auto WideExecutable = StringUtils::Utf8ToWide(Executable);
			std::wstring CommandLine = QuoteArgument(WideExecutable);
			for (const auto& Argument : Options.Arguments)
			{
				CommandLine.push_back(L' ');
				CommandLine += QuoteArgument(StringUtils::Utf8ToWide(Argument));
			}
			if (CommandLine.size() >= 32767)
				return std::unexpected(Detail::InvalidProcessLaunch(Executable, "Windows command line exceeds 32767 UTF-16 units including its terminator."));
			std::vector<wchar_t> Environment;
			if (!Options.EnvironmentOverrides.empty())
			{
				auto Built = BuildEnvironment(Executable, Options);
				if (!Built) return std::unexpected(Built.error());
				Environment = std::move(*Built);
			}
			STARTUPINFOW StartupInfo{};
			StartupInfo.cb = sizeof(StartupInfo);
			PROCESS_INFORMATION ProcessInfo{};
			if (!CreateProcessW(WideExecutable.c_str(), CommandLine.data(), nullptr, nullptr, FALSE,
				CREATE_UNICODE_ENVIRONMENT, Environment.empty() ? nullptr : Environment.data(),
				Options.WorkingDirectory.empty() ? nullptr : Options.WorkingDirectory.c_str(), &StartupInfo, &ProcessInfo))
			{
				const DWORD Error = GetLastError();
				return std::unexpected(FPlatformProcessError{EPlatformProcessError::Launch,
					std::format("Could not launch executable: {}.", FormatWindowsError(Error)), std::string(Executable), Error});
			}
			return ProcessInfo;
		}

	}

	auto FWindowsPlatformProcess::ExecutablePath() -> const char*
	{
		static const std::string ExecutablePath = []() {
			// GetModuleFileNameW includes the extended-length prefix when appropriate.
			std::vector<wchar_t> Buffer(256);
			for (;;)
			{
				const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
				if (Length == 0) return std::string{};
				if (Length < Buffer.size())
					return StringUtils::WideToUtf8(std::wstring_view(Buffer.data(), Length));
				if (Buffer.size() >= 32768) return std::string{};
				Buffer.resize(std::min<size_t>(Buffer.size() * 2, 32768));
			}
		}();
		return ExecutablePath.c_str();
	}

	auto FWindowsPlatformProcess::CurrentProcessId() -> uint32
	{
		return static_cast<uint32>(::GetCurrentProcessId());
	}

	auto FWindowsPlatformProcess::WaitForProcessExit(uint32 ProcessId) -> std::expected<void, FPlatformProcessError>
	{
		const HANDLE Process = OpenProcess(SYNCHRONIZE, FALSE, ProcessId);
		if (Process == nullptr)
		{
			const DWORD Error = GetLastError();
			if (Error == ERROR_INVALID_PARAMETER) return {};
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format("OpenProcess failed with error {}.", Error), {}, Error});
		}
		const DWORD WaitResult = WaitForSingleObject(Process, INFINITE);
		const DWORD Error = WaitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
		CloseHandle(Process);
		if (WaitResult == WAIT_OBJECT_0) return {};
		return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format("WaitForSingleObject failed with result {}.", WaitResult), {},
			WaitResult == WAIT_FAILED ? std::optional<int64>(Error) : std::nullopt});
	}

	auto FWindowsPlatformProcess::LaunchProcess(std::string_view Executable, const FProcessLaunchOptions& Options)
		-> std::expected<void, FPlatformProcessError>
	{
		const auto Spawned = Spawn(Executable, Options);
		if (!Spawned) return std::unexpected(Spawned.error());
		CloseHandle(Spawned->hThread);
		CloseHandle(Spawned->hProcess);
		return {};
	}

	auto FWindowsPlatformProcess::OpenPath(std::string_view Path) -> std::expected<void, FPlatformProcessError>
	{
		const std::wstring WidePath = StringUtils::Utf8ToWide(Path);
		const INT_PTR Result = reinterpret_cast<INT_PTR>(
			ShellExecuteW(nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
		);
		if (Result > 32) return {};
		return std::unexpected(FPlatformProcessError{EPlatformProcessError::OpenPath, std::format("Could not open \"{}\": ShellExecuteW returned error {}.", Path, Result), std::string(Path), Result});
	}

	auto FWindowsPlatformProcess::ExecuteProcess(std::string_view Executable, const FProcessLaunchOptions& Options)
		-> std::expected<int32, FPlatformProcessError>
	{
		const auto Spawned = Spawn(Executable, Options);
		if (!Spawned) return std::unexpected(Spawned.error());
		const auto& ProcessInfo = *Spawned;
		CloseHandle(ProcessInfo.hThread);
		const DWORD WaitResult = WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
		DWORD ExitCode = 0;
		const bool bReadExitCode = WaitResult == WAIT_OBJECT_0
			&& GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode) != FALSE;
		const DWORD Error = bReadExitCode ? ERROR_SUCCESS : GetLastError();
		CloseHandle(ProcessInfo.hProcess);
		if (!bReadExitCode)
		{
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format(
				"Could not wait for \"{}\": {}.", Executable, FormatWindowsError(Error)), std::string(Executable), Error});
		}
		return static_cast<int32>(ExitCode);
	}
}
