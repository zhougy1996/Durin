#include "Windows/WindowsPlatformProcess.h"
#include "Misc/StringConvert.h"

namespace Durin
{
	auto FWindowsPlatformProcess::ExecutablePath() -> const char*
	{
		static std::string ExecutablePath = []() {
			wchar_t WBuffer[MAX_PATH];
			GetModuleFileNameW(nullptr, WBuffer, MAX_PATH);
			return StringConvert::WideToUtf8(WBuffer);
		}();
		return ExecutablePath.c_str();
	}
}