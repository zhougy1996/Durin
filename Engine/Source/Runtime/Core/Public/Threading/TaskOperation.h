#pragma once

#include "Threading/TaskComposition.h"

namespace Durin::Tasks
{
	// Owner records and retained payload have independent admission budgets.
	struct FTaskOperationLimits
	{
		uint32 MaxOperations = 128;
		uint64 MaxPayloadBytes = 64ull * 1024 * 1024;
	};

	// Reserves terminal publication and an owning-thread commit position before expensive work.
	// Domain request identity, supersession, and mutation policy remain in the calling subsystem.
	template<typename T>
	class TTaskOperationQueue
	{
		static_assert(!std::is_void_v<T>);
		struct FState;
		struct FRecord
		{
			FRecord(TCompletionSource<void> InSource, FTaskCancellationSource InCancellation)
				: Source(std::move(InSource)), Cancellation(std::move(InCancellation)) {}
			std::weak_ptr<FState> Owner;
			TCompletionSource<void> Source;
			FTaskCancellationSource Cancellation;
			std::shared_ptr<Durin::Private::FTaskTerminalHook> Hook;
			std::shared_ptr<TUniqueTaskResultState<T>> Payload;
			std::shared_ptr<FTaskFailure> Failure;
			uint64 RequestId = 0;
			uint64 Generation = 0;
			uint64 Bytes = 0;
			std::atomic<bool> bProducerReady = false;
			std::atomic<bool> bCanceled = false;
			bool bBound = false;
			bool bCommitting = false;
		};
		struct FState
		{
			FState(FTaskScopeToken InScope, FTaskOperationLimits InLimits)
				: Scope(std::move(InScope)), Limits(InLimits), Slots(InLimits.MaxOperations), OwnerThread(std::this_thread::get_id())
			{
				Ready.reserve(Limits.MaxOperations);
			}
			auto AssertOwner() const -> void { require(std::this_thread::get_id() == OwnerThread); }
			auto Release(const std::shared_ptr<FRecord>& Record) -> void
			{
				std::shared_ptr<FRecord> Detached;
				{
					std::lock_guard Lock(Mutex);
					auto Slot = std::ranges::find(Slots, Record);
					if (Slot == Slots.end()) return;
					Detached = std::move(*Slot);
					std::erase(Ready, Record);
					require(ReservedBytes >= Record->Bytes && ActiveCount > 0);
					ReservedBytes -= Record->Bytes;
					--ActiveCount;
				}
			}
			auto OnReady(const std::shared_ptr<FRecord>& Record, ETaskState Terminal) -> void
			{
				bool bCancel;
				{
					std::lock_guard Lock(Mutex);
					Record->bProducerReady = true;
					bCancel = bClosed || Record->bCanceled.load();
					if (!bCancel && Terminal == ETaskState::Succeeded)
					{
						Ready.emplace_back(Record);
						return;
					}
				}
				if (bCancel || Terminal == ETaskState::Canceled) Record->Source.TrySetCanceled();
				else Record->Source.TrySetFailure(Record->Failure ? *Record->Failure : FTaskFailure{});
				Record->Payload->Discard();
				Release(Record);
			}
			FTaskScopeToken Scope;
			FTaskOperationLimits Limits;
			mutable std::mutex Mutex;
			std::vector<std::shared_ptr<FRecord>> Slots;
			std::vector<std::shared_ptr<FRecord>> Ready;
			std::thread::id OwnerThread;
			uint64 ReservedBytes = 0;
			uint32 ActiveCount = 0;
			bool bClosed = false;
		};
	public:
		// A single unbound ticket owns failure publication if construction is abandoned.
		class FTicket
		{
		public:
			FTicket(FTicket&&) noexcept = default;
			auto operator=(FTicket&& Other) noexcept -> FTicket&
			{
				if (this != &Other) { Abandon(); Record = std::move(Other.Record); }
				return *this;
			}
			FTicket(const FTicket&) = delete;
			~FTicket() { Abandon(); }
			auto GetCompletion() const -> FTaskCompletion { return Record->Source.GetCompletion(); }
			auto GetCancellationToken() const -> FTaskCancellationToken { return Record->Cancellation.GetToken(); }
			auto FailAdmission(FTaskAdmissionError Error) -> void
			{
				require(Record && !Record->bBound);
				Record->Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, Error, ETaskFailureCode::AdmissionRejected});
				if (auto Owner = Record->Owner.lock()) Owner->Release(Record);
				Record.reset();
			}
			// Requires a valid unique producer independent of this ticket's own completion.
			// All hook and dependency storage was reserved before the ticket was returned.
			auto Bind(TTask<T>&& Producer) -> void
			{
				require(Record && !Record->bBound && Producer.IsValid());
				if (auto Owner = Record->Owner.lock()) Owner->AssertOwner();
				auto& Native = Detail::FTaskAccess::Native(Producer);
				Record->Payload = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
				Record->Failure = Detail::FTaskAccess::Failure(Producer);
				require(Record->Payload->GetEstimatedResultBytes() <= Record->Bytes);
				const uint64 Claim = Record->Payload->ReserveClaim();
				require(Claim != 0);
				const auto Handle = Native.GetTaskHandle();
				Record->bBound = true;
				Durin::Private::FTaskRuntimeAccess::BindReservedDependency(Record->Source.GetCompletion().GetTaskHandle(), Handle);
				Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Native);
				Durin::Private::FTaskRuntimeAccess::BindTerminal(Handle, std::move(Record->Hook));
				if (Record->bCanceled) CancelTask(Handle);
			}
		private:
			friend class TTaskOperationQueue;
			explicit FTicket(std::shared_ptr<FRecord> InRecord) : Record(std::move(InRecord)) {}
			auto Abandon() -> void
			{
				if (Record && !Record->bBound)
				{
					Record->Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, {}, ETaskFailureCode::AbandonedSource});
					if (auto Owner = Record->Owner.lock()) Owner->Release(Record);
				}
			}
			std::shared_ptr<FRecord> Record;
		};

		explicit TTaskOperationQueue(FTaskGroup& Group, FTaskOperationLimits Limits = {})
			: State(std::make_shared<FState>(Group.GetToken(), Limits))
		{
			require(Group.IsValid() && Limits.MaxOperations > 0 && Limits.MaxPayloadBytes > 0);
		}
		TTaskOperationQueue(const TTaskOperationQueue&) = delete;
		~TTaskOperationQueue() { Close(); }
		auto TryReserve(uint64 RequestId, uint64 Generation, uint64 PayloadBytes) -> TTaskAdmission<FTicket>
		{
			using FAdmission = TTaskAdmission<FTicket>;
			State->AssertOwner();
			if (PayloadBytes < sizeof(T)) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
			{
				std::lock_guard Lock(State->Mutex);
				if (State->bClosed) return FAdmission::Failure({ETaskAdmissionErrorCode::GroupClosed});
				if (State->ActiveCount >= State->Limits.MaxOperations || PayloadBytes > State->Limits.MaxPayloadBytes - State->ReservedBytes)
					return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted});
			}
			try
			{
				FTaskCancellationSource Cancellation;
				FTaskExecutionOptions Options;
				Options.DebugName = "OwnerOperation";
				Options.Cancellation = Cancellation.GetToken();
				auto SourceAdmission = TCompletionSource<void>::TryCreate(State->Scope, Options);
				if (!SourceAdmission.HasValue()) return FAdmission::Failure(SourceAdmission.GetError());
				auto Record = std::make_shared<FRecord>(std::move(SourceAdmission).TakeValue(), std::move(Cancellation));
				Record->Owner = State;
				Record->RequestId = RequestId;
				Record->Generation = Generation;
				Record->Bytes = PayloadBytes;
				Record->Hook = std::make_shared<Durin::Private::FTaskTerminalHook>();
				Record->Hook->Function = [WeakOwner = std::weak_ptr<FState>(State), WeakRecord = std::weak_ptr<FRecord>(Record)](ETaskState Terminal) {
					if (auto Owner = WeakOwner.lock()) if (auto Record = WeakRecord.lock()) Owner->OnReady(Record, Terminal);
				};
				{
					std::lock_guard Lock(State->Mutex);
					if (State->bClosed) return FAdmission::Failure({ETaskAdmissionErrorCode::GroupClosed});
					if (State->ActiveCount >= State->Limits.MaxOperations || PayloadBytes > State->Limits.MaxPayloadBytes - State->ReservedBytes)
						return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted});
					auto Slot = std::ranges::find(State->Slots, std::shared_ptr<FRecord>{});
					require(Slot != State->Slots.end());
					*Slot = Record;
					++State->ActiveCount;
					State->ReservedBytes += PayloadBytes;
				}
				return FAdmission::Success(FTicket(std::move(Record)));
			}
			catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
		}
		// Close and commit run on the same owner thread; callbacks never run under queue locks.
		template<typename F>
		auto Pump(uint64 CurrentGeneration, F&& Apply, uint32 MaxCallbacks = 64) -> uint32
		{
			auto SharedState = State;
			SharedState->AssertOwner();
			uint32 Count = 0;
			while (Count < MaxCallbacks)
			{
				std::shared_ptr<FRecord> Record;
				{
					std::lock_guard Lock(SharedState->Mutex);
					if (SharedState->bClosed || SharedState->Ready.empty()) break;
					Record = std::move(SharedState->Ready.front());
					SharedState->Ready.erase(SharedState->Ready.begin());
				}
				++Count;
				if (Record->Generation != CurrentGeneration || Record->bCanceled
					|| Durin::Private::FTaskRuntimeAccess::IsCancellationRequested(Record->Source.GetCompletion().GetTaskHandle()))
				{
					Record->Source.TrySetCanceled();
					Record->Payload->Discard();
				}
				else
				{
					Record->bCommitting = true;
					bool bSucceeded = true;
					try
					{
						auto Value = Record->Payload->TakePublished();
						require(Value);
						std::invoke(Apply, Record->RequestId, std::move(*Value));
					}
					catch (...) { bSucceeded = false; }
					Record->bCommitting = false;
					if (Record->bCanceled || SharedState->bClosed) Record->Source.TrySetCanceled();
					else if (bSucceeded) Record->Source.TrySetValue();
					else Record->Source.TrySetFailure({});
				}
				SharedState->Release(Record);
			}
			return Count;
		}
		auto Close() -> void
		{
			State->AssertOwner();
			{
				std::lock_guard Lock(State->Mutex);
				if (State->bClosed) return;
				State->bClosed = true;
			}
			for (size_t Index = 0; Index < State->Slots.size(); ++Index)
			{
				std::shared_ptr<FRecord> Record;
				{ std::lock_guard Lock(State->Mutex); Record = State->Slots[Index]; }
				if (!Record) continue;
				Record->bCanceled = true;
				Record->Cancellation.RequestCancellation();
				if (Record->bCommitting) continue;
				Record->Source.TrySetCanceled();
				if (!Record->bBound || Record->bProducerReady)
				{
					if (Record->Payload) Record->Payload->Discard();
					State->Release(Record);
				}
			}
		}
		auto GetReservedBytes() const -> uint64 { std::lock_guard Lock(State->Mutex); return State->ReservedBytes; }
		auto GetActiveCount() const -> uint32 { std::lock_guard Lock(State->Mutex); return State->ActiveCount; }
	private:
		std::shared_ptr<FState> State;
	};
}
