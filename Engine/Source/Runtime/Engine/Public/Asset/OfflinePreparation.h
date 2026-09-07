#pragma once
#include "EngineAPI.h"
namespace Durin
{
	// Selects synchronous derived preparation on the object owner thread. Does not
	// restrict loading or mutation, and owns no inputs or objects.
	class FScopedOfflinePreparation
	{
	public:
		ENGINE_API FScopedOfflinePreparation();
		ENGINE_API ~FScopedOfflinePreparation();
		FScopedOfflinePreparation(const FScopedOfflinePreparation&) = delete;
		auto operator=(const FScopedOfflinePreparation&) -> FScopedOfflinePreparation& = delete;
		ENGINE_API static auto IsActive() -> bool;
	};
}
