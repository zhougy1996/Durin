#include "ProcessFixtureProtocol.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace
{
	auto Utf8(std::wstring_view Text) -> std::string
	{
		if (Text.empty()) return {};
		const int Size = WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
		std::string Result(Size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Result.data(), Size, nullptr, nullptr);
		return Result;
	}

	auto Environment(const char* Name) -> std::optional<std::string>
	{
		const int NameSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Name, -1, nullptr, 0);
		std::wstring WideName(NameSize, L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Name, -1, WideName.data(), NameSize);
		SetLastError(ERROR_SUCCESS);
		const DWORD Size = GetEnvironmentVariableW(WideName.c_str(), nullptr, 0);
		if (Size == 0) return GetLastError() == ERROR_ENVVAR_NOT_FOUND
			? std::nullopt : std::optional<std::string>("");
		std::wstring Value(Size, L'\0');
		const DWORD Length = GetEnvironmentVariableW(WideName.c_str(), Value.data(), Size);
		return Utf8(std::wstring_view(Value.data(), Length));
	}
}

int wmain(int Count, wchar_t** Arguments)
#else
#include <unistd.h>

namespace
{
	auto Environment(const char* Name) -> std::optional<std::string>
	{
		const char* Value = std::getenv(Name);
		return Value ? std::optional<std::string>(Value) : std::nullopt;
	}
}

int main(int Count, char** Arguments)
#endif
{
	using namespace Durin::Tests::ProcessFixture;
	if (Count < 2) return 90;
	FReport Report;
#if defined(_WIN32)
	Report.ProcessId = GetCurrentProcessId();
	for (int Index = 1; Index < Count; ++Index) Report.Arguments.push_back(Utf8(Arguments[Index]));
#else
	Report.ProcessId = static_cast<uint32_t>(getpid());
	for (int Index = 1; Index < Count; ++Index) Report.Arguments.emplace_back(Arguments[Index]);
#endif
	const auto Directory = std::filesystem::current_path().generic_u8string();
	Report.WorkingDirectory.assign(reinterpret_cast<const char*>(Directory.data()), Directory.size());
	for (size_t Index = 0; Index < EnvironmentNames.size(); ++Index)
		Report.Environment[Index] = Environment(EnvironmentNames[Index]);
	if (!WriteReport(std::filesystem::u8path(Report.Arguments.front()), Report)) return 91;
	if (const auto Release = Environment("DURIN_PROCESS_PROBE_RELEASE"))
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!std::filesystem::exists(std::filesystem::u8path(*Release)))
		{
			if (std::chrono::steady_clock::now() >= Deadline) return 92;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	std::ofstream(std::filesystem::u8path(Report.Arguments.front() + ".finished")).close();
	const auto ExitCode = Environment("DURIN_PROCESS_PROBE_EXIT");
	return ExitCode ? std::stoi(*ExitCode) : 0;
}
