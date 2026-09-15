#pragma once

#include "slang.h"
#include "slang-com-ptr.h"

#include <array>
#include <cstddef>
#include <utility>
#include <mutex>
#include <stdexcept>

namespace Durin
{
	// A lease owns the entire Slang object tree exclusively. Declare it before all
	// derived objects so they die before the global session returns to the pool.
	// Admission belongs to callers; acquiring a context never waits for a compiler.
	class FSlangGlobalSessionPool
	{
	public:
		class FLease
		{
		public:
			FLease(FSlangGlobalSessionPool& InOwner,
				Slang::ComPtr<slang::IGlobalSession> InSession)
				: Owner(InOwner), Session(std::move(InSession)) {}
			FLease(const FLease&) = delete;
			auto operator=(const FLease&) -> FLease& = delete;
			~FLease() { Owner.Release(std::move(Session)); }
			auto operator*() const -> slang::IGlobalSession& { return *Session; }
			auto operator->() const -> slang::IGlobalSession* { return Session.get(); }

		private:
			FSlangGlobalSessionPool& Owner;
			Slang::ComPtr<slang::IGlobalSession> Session;
		};

		auto Acquire() -> FLease
		{
			Slang::ComPtr<slang::IGlobalSession> Session;
			{
				std::lock_guard Lock(Mutex);
				if (IdleCount) Session = std::move(Idle[--IdleCount]);
			}
			// Both construction and destruction may be expensive; never hold the
			// inventory lock while calling Slang.
			if (!Session && SLANG_FAILED(slang_createGlobalSession(
				SLANG_API_VERSION, Session.writeRef())))
				throw std::runtime_error("slang_createGlobalSession failed");
			return FLease(*this, std::move(Session));
		}

	private:
		auto Release(Slang::ComPtr<slang::IGlobalSession> Session) -> void
		{
			std::lock_guard Lock(Mutex);
			if (IdleCount < Idle.size()) Idle[IdleCount++] = std::move(Session);
		}

		// Bound retained memory after bursts. Active contexts are bounded by the
		// caller's admission/worker budget, not another blocking queue here.
		std::mutex Mutex;
		std::array<Slang::ComPtr<slang::IGlobalSession>, 4> Idle;
		size_t IdleCount = 0;
	};
}
