#include "Windows/WindowsPlatformProcess.h"
#include "Misc/StringConvert.h"

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
	}

	auto FWindowsPlatformProcess::ExecutablePath() -> const char*
	{
		static std::string ExecutablePath = []() {
			wchar_t WBuffer[MAX_PATH];
			GetModuleFileNameW(nullptr, WBuffer, MAX_PATH);
			return StringUtils::WideToUtf8(WBuffer);
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
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format("OpenProcess failed with error {}.", Error)});
		}
		const DWORD WaitResult = WaitForSingleObject(Process, INFINITE);
		CloseHandle(Process);
		if (WaitResult == WAIT_OBJECT_0) return {};
		return std::unexpected(FPlatformProcessError{EPlatformProcessError::Wait, std::format("WaitForSingleObject failed with result {}.", WaitResult)});
	}

	auto FWindowsPlatformProcess::LaunchProcess(std::string_view Executable, std::string_view Arguments) -> std::expected<void, FPlatformProcessError>
	{
		std::wstring CommandLine = L"\"" + StringUtils::Utf8ToWide(Executable) + L"\" " + StringUtils::Utf8ToWide(Arguments);
		STARTUPINFOW StartupInfo{};
		StartupInfo.cb = sizeof(StartupInfo);
		PROCESS_INFORMATION ProcessInfo{};
		if (!CreateProcessW(nullptr, CommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &StartupInfo, &ProcessInfo))
		{
			const DWORD Error = GetLastError();
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Launch, std::format("Could not launch \"{}\": {}.", Executable, FormatWindowsError(Error))});
		}
		CloseHandle(ProcessInfo.hThread);
		CloseHandle(ProcessInfo.hProcess);
		return {};
	}

	auto FWindowsPlatformProcess::OpenPath(std::string_view Path) -> std::expected<void, FPlatformProcessError>
	{
		const std::wstring WidePath = StringUtils::Utf8ToWide(Path);
		const INT_PTR Result = reinterpret_cast<INT_PTR>(
			ShellExecuteW(nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
		);
		if (Result > 32) return {};
		return std::unexpected(FPlatformProcessError{EPlatformProcessError::OpenPath, std::format("Could not open \"{}\": ShellExecuteW returned error {}.", Path, Result)});
	}

	auto FWindowsPlatformProcess::ExecuteProcess(
		std::string_view Executable,
		std::string_view Arguments) -> std::expected<int32, FPlatformProcessError>
	{
		std::wstring CommandLine = L"\"" + StringUtils::Utf8ToWide(Executable)
			+ L"\" " + StringUtils::Utf8ToWide(Arguments);
		STARTUPINFOW StartupInfo{};
		StartupInfo.cb = sizeof(StartupInfo);
		PROCESS_INFORMATION ProcessInfo{};
		if (!CreateProcessW(nullptr, CommandLine.data(), nullptr, nullptr, FALSE, 0,
			nullptr, nullptr, &StartupInfo, &ProcessInfo))
		{
			const DWORD Error = GetLastError();
			return std::unexpected(FPlatformProcessError{EPlatformProcessError::Launch, std::format(
				"Could not launch \"{}\": {}.", Executable, FormatWindowsError(Error))});
		}
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
				"Could not wait for \"{}\": {}.", Executable, FormatWindowsError(Error))});
		}
		return static_cast<int32>(ExitCode);
	}
}
