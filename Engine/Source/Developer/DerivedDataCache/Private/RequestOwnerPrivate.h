#pragma once

#include "RequestOwnerAccess.h"

namespace Durin::DerivedData::Private
{
	struct FRequestOwnerScope
	{
		IRequest* Request;
		FRequestOwnerScope* Previous;
		static thread_local FRequestOwnerScope* Current;
		explicit FRequestOwnerScope(IRequest* Request)
			: Request(Request)
			, Previous(Current)
		{
			Current = this;
		}
		~FRequestOwnerScope() { Current = Previous; }
		static auto Blocks(const IRequest* Request) -> bool
		{
			for (auto* Scope = Current; Scope; Scope = Scope->Previous)
				if (Request && Scope->Request == Request) return true;
			return false;
		}
	};
	// Requests own callback completion; dispatch publication belongs to the submitter.
	struct FRequestCompletion : IRequest, ICancelableRequest
	{
		std::mutex Mutex;
		std::condition_variable Changed;
		bool Finished = false;
		IRequestOwner* Group = nullptr;
		std::shared_ptr<IBuildScheduledWork> Scheduled;
		virtual auto OnFinished() -> void {}
		auto Cancel() -> void final
		{
			requiref(!FRequestOwnerScope::Blocks(this), "Synchronous cancellation cannot wait for its own callback; use RequestCancel.");
			RequestCancel();
			requiref(WaitInternal() == ERequestWaitResult::Completed, "Synchronous cancellation must wait for request end.");
		}
		template <typename F> auto Complete(F&& Callback) -> void
		{
			FOwnerAccess::End(*Group, this, [&] {
				try { Callback(); } catch (...) {}
				OnFinished();
			});
			{ std::lock_guard Lock(Mutex); Finished = true; }
			Changed.notify_all();
		}
		auto Wait() -> void override { requiref(WaitInternal() == ERequestWaitResult::Completed, "Requests must be waited outside their callbacks."); }
		auto SetPriority(ERequestPriority Priority) -> void override
		{
			std::shared_ptr<IBuildScheduledWork> Work;
			{ std::lock_guard Lock(Mutex); Work = Scheduled; }
			if (Work) Work->SetPriority(Priority);
		}
		virtual auto WaitInternal() -> ERequestWaitResult
		{
			for (;;) {
				std::shared_ptr<IBuildScheduledWork> Work;
				{ std::lock_guard Lock(Mutex); if (Finished) return ERequestWaitResult::Completed; Work = Scheduled; }
				if (FRequestOwnerScope::Blocks(this)) return ERequestWaitResult::WouldBlock;
				if (Work && Work->Wait() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock;
				std::unique_lock Lock(Mutex);
				Changed.wait(Lock, [&] { return Finished || Scheduled != Work; });
			}
		}
	};
}
