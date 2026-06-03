#pragma once

#include "CoreAPI.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace Durin
{
	class FThreadEvent
	{
	public:
		FThreadEvent() = default;

		auto Trigger() -> void
		{
			std::lock_guard Lock(Mutex);
			bIsTriggered = true;
			CV.notify_all();
		}

		auto Reset() -> void
		{
			std::lock_guard Lock(Mutex);
			bIsTriggered = false;
		}

		auto Wait() -> void
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [this]() {
				return bIsTriggered;
			});
		}

		auto WaitFor(double TimeoutSeconds) -> bool
		{
			std::unique_lock Lock(Mutex);
			return CV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), [this]() {
				return bIsTriggered;
			});
		}

		auto IsTriggered() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bIsTriggered;
		}

	private:
		mutable std::mutex Mutex;
		std::condition_variable CV;
		bool bIsTriggered = false;
	};
}
