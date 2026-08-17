#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Durin::Testing::ApplicationHost
{
	inline constexpr std::string_view RequestFile = "request.bin";
	inline constexpr std::string_view EnvironmentFile = "environment.bin";
	inline constexpr std::string_view HostPidFile = "host.pid";
	inline constexpr std::string_view ChildPidFile = "child.pid";
	inline constexpr std::string_view StandardOutputFile = "stdout.log";
	inline constexpr std::string_view StandardErrorFile = "stderr.log";
	inline constexpr std::string_view ResultFile = "result.bin";
	inline constexpr std::string_view ProtocolMagic = "DURIN_NATIVE_TEST_HOST_V1";

	// Carries the terminal state published atomically by the application host.
	struct FResult
	{
		std::string Nonce;
		std::string Stage;
		std::string Status;
		int ExitCode = 0;
		int Signal = 0;
		std::string Message;
	};

	auto IsContainedPath(const std::filesystem::path& Root,
		const std::filesystem::path& Candidate) -> bool;
	auto WriteStringVectorAtomic(const std::filesystem::path& Path,
		const std::vector<std::string>& Values, std::string& Error) -> bool;
	auto ReadStringVector(const std::filesystem::path& Path,
		std::vector<std::string>& Values, std::string& Error) -> bool;
	auto WritePidAtomic(const std::filesystem::path& Path, int Pid,
		std::string& Error) -> bool;
	auto ReadPid(const std::filesystem::path& Path, int& Pid,
		std::string& Error) -> bool;
	auto WriteResultAtomic(const std::filesystem::path& Path,
		const FResult& Result, std::string& Error) -> bool;
	auto ReadResult(const std::filesystem::path& Path, FResult& Result,
		std::string& Error) -> bool;
	auto ValidateResult(const FResult& Result, std::string& Error) -> bool;
}
