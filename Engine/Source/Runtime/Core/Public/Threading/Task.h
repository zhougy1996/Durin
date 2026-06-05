#pragma once

#include "CoreAPI.h"

#include "HAL/Platform.h"

namespace Durin
{
	using FTaskFunction = std::function<void()>;

	class FTaskCompletionState;

	class FTaskHandle
	{
	public:
		CORE_API FTaskHandle();

		CORE_API auto IsValid() const -> bool;
		CORE_API auto IsComplete() const -> bool;
		CORE_API auto GetDebugName() const -> const char*;

	private:
		explicit FTaskHandle(std::shared_ptr<FTaskCompletionState> InState);

		friend CORE_API auto LaunchTask(const char* Name, FTaskFunction&& Function) -> FTaskHandle;
		friend CORE_API auto WaitTask(const FTaskHandle& Task) -> void;

		std::shared_ptr<FTaskCompletionState> State;
	};

	CORE_API auto LaunchTask(const char* Name, FTaskFunction&& Function) -> FTaskHandle;
	CORE_API auto WaitTask(const FTaskHandle& Task) -> void;
	CORE_API auto WaitAll(std::span<const FTaskHandle> Tasks) -> void;
}
