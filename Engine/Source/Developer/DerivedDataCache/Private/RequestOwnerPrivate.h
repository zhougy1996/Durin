#pragma once

#include "RequestOwnerInternal.h"

namespace Durin::DerivedData::Private
{
	auto CreateRequestOwnerState() -> std::shared_ptr<IRequestOwnerInternal>;
	struct FRequestOwnerScope
	{
		IRequestOwnerInternal* Owner;
		IRequest* Request;
		const void* Domain;
		FRequestOwnerScope* Previous;
		static thread_local FRequestOwnerScope* Current;
		explicit FRequestOwnerScope(IRequestOwnerInternal* Owner, IRequest* Request = nullptr, const void* Domain = nullptr)
			: Owner(Owner)
			, Request(Request)
			, Domain(Domain)
			, Previous(Current)
		{
			Current = this;
		}
		~FRequestOwnerScope() { Current = Previous; }
		static auto Blocks(const IRequest* Request, const void* Domain) -> bool
		{
			for (auto* Scope = Current; Scope; Scope = Scope->Previous)
				if ((Request && Scope->Request == Request) || (Domain && Scope->Domain == Domain)) return true;
			return false;
		}
	};
	// Shared callback completion, dispatch publication, and request-end protocol.
	struct FRequestCompletion : IRequest
	{
		std::mutex Mutex;
		std::condition_variable Changed;
		bool Dispatching = true, CompletionReleased = false, OwnerReleased = false, Finished = false, StageDispatching = false;
		uint64 ScheduleRevision = 0;
		std::shared_ptr<IRequestOwnerInternal> Group;
		std::shared_ptr<IBuildScheduledWork> Scheduled;
		const void* WaitDomain = nullptr;
		virtual auto OnFinished() -> void {}
		auto Cancel() -> bool final
		{
			requiref(!FRequestOwnerScope::Blocks(this, WaitDomain), "Synchronous cancellation cannot wait for its own callback; use RequestCancel.");
			const bool Changed = RequestCancel();
			requiref(Wait() == ERequestWaitResult::Completed, "Synchronous cancellation must wait for request end.");
			return Changed;
		}
		auto ReleaseOwner() -> void
		{
			std::shared_ptr<IRequestOwnerInternal> Owner;
			{
				std::lock_guard Lock(Mutex);
				if (Dispatching || !CompletionReleased || OwnerReleased) return;
				OwnerReleased = true;
				Owner = Group;
			}
			OnFinished();
			if (Owner) Owner->End(this);
			{ std::lock_guard Lock(Mutex); Finished = true; }
			Changed.notify_all();
		}
		auto DispatchReturned() -> void { { std::lock_guard Lock(Mutex); Dispatching = false; } ReleaseOwner(); Changed.notify_all(); }
		auto Complete() -> void { { std::lock_guard Lock(Mutex); CompletionReleased = true; } ReleaseOwner(); Changed.notify_all(); }
		template <typename F> auto Complete(F&& Callback, const void* Domain) -> void
		{
			FRequestOwnerScope Scope(Group.get(), this, Domain);
			try { Callback(); } catch (...) {}
			Complete();
		}
		auto Wait() -> ERequestWaitResult override
		{
			{ std::lock_guard Lock(Mutex); if (Finished) return ERequestWaitResult::Completed; }
			if (FRequestOwnerScope::Blocks(this, WaitDomain)) return ERequestWaitResult::WouldBlock;
			for (;;)
			{
				std::shared_ptr<IBuildScheduledWork> Work;
				uint64 Revision;
				{
					std::unique_lock Lock(Mutex);
					Changed.wait(Lock, [&] { return !Dispatching && (!StageDispatching || CompletionReleased); });
					if (CompletionReleased)
					{
						Changed.wait(Lock, [&] { return Finished; });
						return ERequestWaitResult::Completed;
					}
					Work = Scheduled;
					Revision = ScheduleRevision;
				}
				if (Work && Work->Wait() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock;
				std::unique_lock Lock(Mutex); Changed.wait(Lock, [&] { return CompletionReleased || ScheduleRevision != Revision; });
			}
		}
	};
}
