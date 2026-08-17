#include "NativeTestApplicationProtocol.h"
#include "NativeTestApplicationProcess.h"

#include <cstddef>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <utility>
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

		auto ParseInteger(std::string_view Value, int& Output) -> bool
		{
			const auto [Position, ErrorCode] = std::from_chars(
				Value.data(), Value.data() + Value.size(), Output);
			return ErrorCode == std::errc{}
				&& Position == Value.data() + Value.size();
		}

		auto ContainsNul(std::string_view Value) -> bool
		{
			return Value.find('\0') != std::string_view::npos;
		}

		auto ParseResultStage(std::string_view Value, EResultStage& Stage) -> bool
		{
			static constexpr std::pair<std::string_view, EResultStage> Values[]{
				{"host-admission", EResultStage::HostAdmission},
				{"request-read", EResultStage::RequestRead},
				{"request-validation", EResultStage::RequestValidation},
				{"environment-read", EResultStage::EnvironmentRead},
				{"environment-validation", EResultStage::EnvironmentValidation},
				{"output-open", EResultStage::OutputOpen},
				{"child-start", EResultStage::ChildStart},
				{"child-publication", EResultStage::ChildPublication},
				{"test", EResultStage::Test},
				{"cancellation", EResultStage::Cancellation},
			};
			for (const auto& [Name, Candidate] : Values)
			{
				if (Value == Name)
				{
					Stage = Candidate;
					return true;
				}
			}
			return false;
		}

		auto ParseResultStatus(std::string_view Value, EResultStatus& Status) -> bool
		{
			static constexpr std::pair<std::string_view, EResultStatus> Values[]{
				{"passed", EResultStatus::Passed},
				{"failed", EResultStatus::Failed},
				{"crashed", EResultStatus::Crashed},
				{"cancelled", EResultStatus::Cancelled},
				{"launcher-failure", EResultStatus::LauncherFailure},
			};
			for (const auto& [Name, Candidate] : Values)
			{
				if (Value == Name)
				{
					Status = Candidate;
					return true;
				}
			}
			return false;
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
		FScopedFileDescriptor File(open(
			Temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
		if (!File.IsValid())
		{
			Error = "cannot create protocol file: " + std::string(std::strerror(errno));
			return false;
		}
		bool Success = true;
		const uint32_t MagicSize = static_cast<uint32_t>(ProtocolMagic.size());
		const uint32_t Count = static_cast<uint32_t>(Values.size());
		Success = WriteAll(File.Get(), &MagicSize, sizeof(MagicSize))
			&& WriteAll(File.Get(), ProtocolMagic.data(), ProtocolMagic.size())
			&& WriteAll(File.Get(), &Count, sizeof(Count));
		for (const std::string& Value : Values)
		{
			if (Value.size() > MaximumFieldSize)
			{
				Success = false;
				Error = "field size exceeds protocol limit";
				break;
			}
			const uint32_t Size = static_cast<uint32_t>(Value.size());
			Success = Success && WriteAll(File.Get(), &Size, sizeof(Size))
				&& WriteAll(File.Get(), Value.data(), Value.size());
			if (!Success) break;
		}
		if (Success) Success = fsync(File.Get()) == 0;
		int SavedError = errno;
		File.Close();
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
		FScopedFileDescriptor File(open(Path.c_str(), O_RDONLY | O_NOFOLLOW));
		if (!File.IsValid())
		{
			Error = "cannot open protocol file: " + std::string(std::strerror(errno));
			return false;
		}
		uint32_t MagicSize = 0;
		uint32_t Count = 0;
		bool Success = ReadAll(File.Get(), &MagicSize, sizeof(MagicSize));
		if (!Success || MagicSize != ProtocolMagic.size())
		{
			Error = "protocol magic length is invalid";
			return false;
		}
		std::string Magic(MagicSize, '\0');
		Success = ReadAll(File.Get(), Magic.data(), Magic.size())
			&& ReadAll(File.Get(), &Count, sizeof(Count));
		if (!Success || Magic != ProtocolMagic || Count > MaximumFieldCount)
		{
			Error = "protocol header is invalid";
			return false;
		}
		Values.clear();
		Values.reserve(Count);
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			uint32_t Size = 0;
			if (!ReadAll(File.Get(), &Size, sizeof(Size)) || Size > MaximumFieldSize)
			{
				Error = "protocol field length is invalid";
				return false;
			}
			std::string Value(Size, '\0');
			if (!ReadAll(File.Get(), Value.data(), Value.size()))
			{
				Error = "protocol field is truncated";
				return false;
			}
			Values.push_back(std::move(Value));
		}
		char Trailing = 0;
		const ssize_t TrailingRead = read(File.Get(), &Trailing, 1);
		if (TrailingRead != 0)
		{
			Error = "protocol file has trailing data";
			return false;
		}
		return true;
	}

	auto WriteRequestAtomic(const std::filesystem::path& Path,
		const FRequest& Request, std::string& Error) -> bool
	{
		std::vector<std::string> Values{
			Request.Nonce,
			Request.Executable.string(),
			Request.WorkingDirectory.string(),
			std::to_string(Request.ControllerPid),
			Request.ArtifactRoot.string(),
		};
		Values.insert(Values.end(), Request.Arguments.begin(), Request.Arguments.end());
		return WriteStringVectorAtomic(Path, Values, Error);
	}

	auto ReadRequest(const std::filesystem::path& Path, FRequest& Request,
		std::string& Error) -> bool
	{
		std::vector<std::string> Values;
		if (!ReadStringVector(Path, Values, Error)) return false;
		if (Values.size() < 5)
		{
			Error = "request record has fewer than five required fields";
			return false;
		}
		int ControllerPid = 0;
		if (!ParseInteger(Values[3], ControllerPid))
		{
			Error = "request controllerPid is not an integer";
			return false;
		}
		Request.Nonce = std::move(Values[0]);
		Request.Executable = std::move(Values[1]);
		Request.WorkingDirectory = std::move(Values[2]);
		Request.ControllerPid = ControllerPid;
		Request.ArtifactRoot = std::move(Values[4]);
		Request.Arguments.assign(
			std::make_move_iterator(Values.begin() + 5),
			std::make_move_iterator(Values.end()));
		return true;
	}

	auto ValidateRequest(const FRequest& Request, std::string_view ExpectedNonce,
		std::string& Error) -> bool
	{
		if (Request.Nonce.empty())
		{
			Error = "request nonce is empty";
			return false;
		}
		if (ContainsNul(Request.Nonce))
		{
			Error = "request nonce contains an embedded NUL";
			return false;
		}
		if (!ExpectedNonce.empty() && Request.Nonce != ExpectedNonce)
		{
			Error = "request nonce mismatch";
			return false;
		}
		const std::pair<std::string_view, std::string> Paths[]{
			{"executable", Request.Executable.string()},
			{"workingDirectory", Request.WorkingDirectory.string()},
			{"artifactRoot", Request.ArtifactRoot.string()},
		};
		for (const auto& [Name, Value] : Paths)
		{
			if (Value.empty())
			{
				Error = "request " + std::string(Name) + " is empty";
				return false;
			}
			if (ContainsNul(Value))
			{
				Error = "request " + std::string(Name) + " contains an embedded NUL";
				return false;
			}
		}
		if (Request.ControllerPid <= 1)
		{
			Error = "request controllerPid must be greater than one";
			return false;
		}
		for (size_t Index = 0; Index < Request.Arguments.size(); ++Index)
		{
			if (ContainsNul(Request.Arguments[Index]))
			{
				Error = "request argument[" + std::to_string(Index)
					+ "] contains an embedded NUL";
				return false;
			}
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
			{Result.Nonce, std::string(ResultStageName(Result.Stage)),
				std::string(ResultStatusName(Result.Status)), std::to_string(Result.ExitCode),
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
		if (!ParseResultStage(Values[1], Result.Stage))
		{
			Error = "completion stage '" + Values[1] + "' is unknown";
			return false;
		}
		if (!ParseResultStatus(Values[2], Result.Status))
		{
			Error = "completion status '" + Values[2] + "' is unknown";
			return false;
		}
		Result.Message = Values[5];
		if (!ParseInteger(Values[3], Result.ExitCode)
			|| !ParseInteger(Values[4], Result.Signal))
		{
			Error = "completion record numeric fields are invalid";
			return false;
		}
		return true;
	}

	auto ValidateResult(const FResult& Result, std::string_view ExpectedNonce,
		std::string& Error) -> bool
	{
		if (Result.Nonce.empty())
		{
			Error = "completion nonce is empty";
			return false;
		}
		if (ContainsNul(Result.Nonce))
		{
			Error = "completion nonce contains an embedded NUL";
			return false;
		}
		if (!ExpectedNonce.empty() && Result.Nonce != ExpectedNonce)
		{
			Error = "completion nonce mismatch";
			return false;
		}
		if (ContainsNul(Result.Message))
		{
			Error = "completion message contains an embedded NUL";
			return false;
		}
		switch (Result.Status)
		{
		case EResultStatus::Passed:
			if (Result.Stage == EResultStage::Test && Result.ExitCode == 0
				&& Result.Signal == 0) return true;
			Error = "passed completion requires stage=test, exitCode=0, signal=0";
			return false;
		case EResultStatus::Failed:
			if (Result.Stage == EResultStage::Test && Result.ExitCode > 0
				&& Result.ExitCode <= 255 && Result.Signal == 0) return true;
			Error = "failed completion requires stage=test, exitCode=1..255, signal=0";
			return false;
		case EResultStatus::Crashed:
			if (Result.Stage == EResultStage::Test && Result.Signal > 0
				&& Result.ExitCode == 128 + Result.Signal
				&& Result.ExitCode <= 255) return true;
			Error = "crashed completion requires stage=test and exitCode=128+signal";
			return false;
		case EResultStatus::Cancelled:
			if (Result.Stage == EResultStage::Cancellation && Result.ExitCode == 124
				&& Result.Signal >= 0) return true;
			Error = "cancelled completion requires stage=cancellation and exitCode=124";
			return false;
		case EResultStatus::LauncherFailure:
			switch (Result.Stage)
			{
			case EResultStage::HostAdmission:
			case EResultStage::RequestRead:
			case EResultStage::RequestValidation:
			case EResultStage::EnvironmentRead:
			case EResultStage::EnvironmentValidation:
			case EResultStage::OutputOpen:
			case EResultStage::ChildStart:
			case EResultStage::ChildPublication:
			case EResultStage::Test:
				if (Result.ExitCode == 125 && Result.Signal == 0) return true;
				break;
			case EResultStage::Cancellation:
				break;
			}
			Error = "launcher-failure completion requires a launcher stage, exitCode=125, signal=0";
			return false;
		}
		Error = "completion status is invalid";
		return false;
	}

	auto ResultStageName(EResultStage Stage) -> std::string_view
	{
		switch (Stage)
		{
		case EResultStage::HostAdmission: return "host-admission";
		case EResultStage::RequestRead: return "request-read";
		case EResultStage::RequestValidation: return "request-validation";
		case EResultStage::EnvironmentRead: return "environment-read";
		case EResultStage::EnvironmentValidation: return "environment-validation";
		case EResultStage::OutputOpen: return "output-open";
		case EResultStage::ChildStart: return "child-start";
		case EResultStage::ChildPublication: return "child-publication";
		case EResultStage::Test: return "test";
		case EResultStage::Cancellation: return "cancellation";
		}
		return "unknown";
	}

	auto ResultStatusName(EResultStatus Status) -> std::string_view
	{
		switch (Status)
		{
		case EResultStatus::Passed: return "passed";
		case EResultStatus::Failed: return "failed";
		case EResultStatus::Crashed: return "crashed";
		case EResultStatus::Cancelled: return "cancelled";
		case EResultStatus::LauncherFailure: return "launcher-failure";
		}
		return "unknown";
	}
}
