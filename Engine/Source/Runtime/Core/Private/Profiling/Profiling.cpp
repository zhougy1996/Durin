#include "Profiling/Profiling.h"

#include <deque>
#include <mutex>

namespace Durin::Profiling
{
	namespace
	{
		std::mutex GProgramIdentityMutex;
		std::deque<std::string> GProgramIdentityStorage;
	}

	auto FormatProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string
	{
		const std::string_view RuntimeLabel = RuntimeVariant.empty() ? std::string_view{"Durin"} : RuntimeVariant;
		const std::string_view ProjectLabel = ProjectName.empty() ? std::string_view{"No Project"} : ProjectName;
		return std::format("{} | {} | PID {}", RuntimeLabel, ProjectLabel, ProcessId);
	}

	auto SetProgramIdentity(
		std::string_view RuntimeVariant,
		std::string_view ProjectName,
		uint32 ProcessId
	) -> std::string_view
	{
		std::scoped_lock Lock(GProgramIdentityMutex);
		const std::string ProgramIdentity = FormatProgramIdentity(RuntimeVariant, ProjectName, ProcessId);
		if (GProgramIdentityStorage.empty() || GProgramIdentityStorage.back() != ProgramIdentity)
			GProgramIdentityStorage.emplace_back(ProgramIdentity);
		const std::string_view StoredIdentity = GProgramIdentityStorage.back();
#if DURIN_WITH_TRACY
		TracySetProgramName(StoredIdentity.data());
#endif
		return StoredIdentity;
	}

	auto GetProgramIdentity() -> std::string
	{
		std::scoped_lock Lock(GProgramIdentityMutex);
		return GProgramIdentityStorage.empty() ? std::string{} : GProgramIdentityStorage.back();
	}
}
