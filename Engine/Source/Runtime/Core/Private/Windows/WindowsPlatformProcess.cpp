#include "Windows/WindowsPlatformProcess.h"
#include "Misc/StringConvert.h"

namespace Durin
{
	auto FWindowsPlatformProcess::ExecutablePath() -> const char*
	{
		static std::string ExecutablePath = []() {
			wchar_t WBuffer[MAX_PATH];
			GetModuleFileNameW(nullptr, WBuffer, MAX_PATH);
			return String::WideToUtf8(WBuffer);
		}();
		return ExecutablePath.c_str();
	}

	auto FWindowsPlatformProcess::LaunchProcess(std::string_view Executable, std::string_view Arguments, std::string* OutError) -> bool
	{
		std::wstring CommandLine = L"\"" + String::Utf8ToWide(Executable) + L"\" " + String::Utf8ToWide(Arguments);
		STARTUPINFOW StartupInfo{};
		StartupInfo.cb = sizeof(StartupInfo);
		PROCESS_INFORMATION ProcessInfo{};
		if (!CreateProcessW(nullptr, CommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &StartupInfo, &ProcessInfo))
		{
			if (OutError) *OutError = std::format("CreateProcess failed with error {}.", GetLastError());
			return false;
		}
		CloseHandle(ProcessInfo.hThread);
		CloseHandle(ProcessInfo.hProcess);
		return true;
	}
}
