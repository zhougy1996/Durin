#include "NativeTestApplicationProtocol.h"

#include <cstddef>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

namespace Durin::Testing::ApplicationHost
{
	namespace
	{
		constexpr uint32_t MaximumFieldCount = 65536;
		constexpr uint32_t MaximumFieldSize = 16 * 1024 * 1024;

		auto WriteAll(int File, const void* Data, size_t Size) -> bool
		{
			const auto* Bytes = static_cast<const std::byte*>(Data);
			while (Size > 0)
			{
				const ssize_t Written = write(File, Bytes, Size);
				if (Written < 0 && errno == EINTR) continue;
				if (Written <= 0) return false;
				Bytes += Written;
				Size -= static_cast<size_t>(Written);
			}
			return true;
		}

		auto ReadAll(int File, void* Data, size_t Size) -> bool
		{
			auto* Bytes = static_cast<std::byte*>(Data);
			while (Size > 0)
			{
				const ssize_t Read = read(File, Bytes, Size);
				if (Read < 0 && errno == EINTR) continue;
				if (Read <= 0) return false;
				Bytes += Read;
				Size -= static_cast<size_t>(Read);
			}
			return true;
		}

		auto TemporaryPath(const std::filesystem::path& Path) -> std::filesystem::path
		{
			return Path.string() + ".tmp-p" + std::to_string(getpid());
		}
	}

	auto IsContainedPath(const std::filesystem::path& Root,
		const std::filesystem::path& Candidate) -> bool
	{
		auto RootIterator = Root.begin();
		auto CandidateIterator = Candidate.begin();
		for (; RootIterator != Root.end(); ++RootIterator, ++CandidateIterator)
		{
			if (CandidateIterator == Candidate.end() || *RootIterator != *CandidateIterator)
			{
				return false;
			}
		}
		return CandidateIterator != Candidate.end();
	}

	auto WriteStringVectorAtomic(const std::filesystem::path& Path,
		const std::vector<std::string>& Values, std::string& Error) -> bool
	{
		if (Values.size() > MaximumFieldCount)
		{
			Error = "field count exceeds protocol limit";
			return false;
		}
		const std::filesystem::path Temporary = TemporaryPath(Path);
		const int File = open(Temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (File < 0)
		{
			Error = "cannot create protocol file: " + std::string(std::strerror(errno));
			return false;
		}
		bool Success = true;
		const uint32_t MagicSize = static_cast<uint32_t>(ProtocolMagic.size());
		const uint32_t Count = static_cast<uint32_t>(Values.size());
		Success = WriteAll(File, &MagicSize, sizeof(MagicSize))
			&& WriteAll(File, ProtocolMagic.data(), ProtocolMagic.size())
			&& WriteAll(File, &Count, sizeof(Count));
		for (const std::string& Value : Values)
		{
			if (Value.size() > MaximumFieldSize)
			{
				Success = false;
				Error = "field size exceeds protocol limit";
				break;
			}
			const uint32_t Size = static_cast<uint32_t>(Value.size());
			Success = Success && WriteAll(File, &Size, sizeof(Size))
				&& WriteAll(File, Value.data(), Value.size());
			if (!Success) break;
		}
		if (Success) Success = fsync(File) == 0;
		int SavedError = errno;
		close(File);
		if (Success)
		{
			if (rename(Temporary.c_str(), Path.c_str()) == 0) return true;
			SavedError = errno;
		}
		if (Error.empty())
		{
			Error = "cannot publish protocol file: "
				+ std::string(std::strerror(SavedError));
		}
		unlink(Temporary.c_str());
		return false;
	}

	auto ReadStringVector(const std::filesystem::path& Path,
		std::vector<std::string>& Values, std::string& Error) -> bool
	{
		const int File = open(Path.c_str(), O_RDONLY | O_NOFOLLOW);
		if (File < 0)
		{
			Error = "cannot open protocol file: " + std::string(std::strerror(errno));
			return false;
		}
		uint32_t MagicSize = 0;
		uint32_t Count = 0;
		bool Success = ReadAll(File, &MagicSize, sizeof(MagicSize));
		if (!Success || MagicSize != ProtocolMagic.size())
		{
			Error = "protocol magic length is invalid";
			close(File);
			return false;
		}
		std::string Magic(MagicSize, '\0');
		Success = ReadAll(File, Magic.data(), Magic.size())
			&& ReadAll(File, &Count, sizeof(Count));
		if (!Success || Magic != ProtocolMagic || Count > MaximumFieldCount)
		{
			Error = "protocol header is invalid";
			close(File);
			return false;
		}
		Values.clear();
		Values.reserve(Count);
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			uint32_t Size = 0;
			if (!ReadAll(File, &Size, sizeof(Size)) || Size > MaximumFieldSize)
			{
				Error = "protocol field length is invalid";
				close(File);
				return false;
			}
			std::string Value(Size, '\0');
			if (!ReadAll(File, Value.data(), Value.size()))
			{
				Error = "protocol field is truncated";
				close(File);
				return false;
			}
			Values.push_back(std::move(Value));
		}
		char Trailing = 0;
		const ssize_t TrailingRead = read(File, &Trailing, 1);
		close(File);
		if (TrailingRead != 0)
		{
			Error = "protocol file has trailing data";
			return false;
		}
		return true;
	}

	auto WritePidAtomic(const std::filesystem::path& Path, int Pid,
		std::string& Error) -> bool
	{
		return WriteStringVectorAtomic(Path, {std::to_string(Pid)}, Error);
	}

	auto ReadPid(const std::filesystem::path& Path, int& Pid,
		std::string& Error) -> bool
	{
		std::vector<std::string> Values;
		if (!ReadStringVector(Path, Values, Error) || Values.size() != 1)
		{
			if (Error.empty()) Error = "PID record is invalid";
			return false;
		}
		const char* Begin = Values[0].data();
		const char* End = Begin + Values[0].size();
		const auto [Position, ConversionError] = std::from_chars(Begin, End, Pid);
		if (ConversionError != std::errc{} || Position != End || Pid <= 1)
		{
			Error = "PID value is invalid";
			return false;
		}
		return true;
	}

	auto WriteResultAtomic(const std::filesystem::path& Path,
		const FResult& Result, std::string& Error) -> bool
	{
		return WriteStringVectorAtomic(Path,
			{Result.Nonce, Result.Stage, Result.Status, std::to_string(Result.ExitCode),
				std::to_string(Result.Signal), Result.Message}, Error);
	}

	auto ReadResult(const std::filesystem::path& Path, FResult& Result,
		std::string& Error) -> bool
	{
		std::vector<std::string> Values;
		if (!ReadStringVector(Path, Values, Error) || Values.size() != 6)
		{
			if (Error.empty()) Error = "completion record is invalid";
			return false;
		}
		Result.Nonce = Values[0];
		Result.Stage = Values[1];
		Result.Status = Values[2];
		Result.Message = Values[5];
		const auto ParseInteger = [](const std::string& Value, int& Output)
		{
			const auto [Position, ErrorCode] = std::from_chars(
				Value.data(), Value.data() + Value.size(), Output);
			return ErrorCode == std::errc{} && Position == Value.data() + Value.size();
		};
		if (!ParseInteger(Values[3], Result.ExitCode)
			|| !ParseInteger(Values[4], Result.Signal))
		{
			Error = "completion record numeric fields are invalid";
			return false;
		}
		return true;
	}

	auto ValidateResult(const FResult& Result, std::string& Error) -> bool
	{
		if (Result.Nonce.empty())
		{
			Error = "completion nonce is empty";
			return false;
		}
		if (Result.Status == "passed")
		{
			if (Result.Stage == "test" && Result.ExitCode == 0
				&& Result.Signal == 0) return true;
		}
		else if (Result.Status == "failed")
		{
			if (Result.Stage == "test" && Result.ExitCode > 0
				&& Result.ExitCode <= 255 && Result.Signal == 0) return true;
		}
		else if (Result.Status == "crashed")
		{
			if (Result.Stage == "test" && Result.Signal > 0
				&& Result.ExitCode == 128 + Result.Signal
				&& Result.ExitCode <= 255) return true;
		}
		else if (Result.Status == "cancelled")
		{
			if (Result.Stage == "cancellation" && Result.ExitCode == 124
				&& Result.Signal >= 0) return true;
		}
		else if (Result.Status == "launcher-failure")
		{
			static constexpr std::string_view FailureStages[]{
				"host-admission", "request-read", "request-validation",
				"environment-read", "environment-validation", "output-open",
				"child-start", "child-publication", "test"};
			for (const std::string_view Stage : FailureStages)
			{
				if (Result.Stage == Stage && Result.ExitCode == 125
					&& Result.Signal == 0) return true;
			}
		}
		Error = "completion status fields are inconsistent";
		return false;
	}
}
