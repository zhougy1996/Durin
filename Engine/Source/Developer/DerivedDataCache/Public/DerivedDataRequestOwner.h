#pragma once
#include "CoreMinimal.h"
#include "DerivedDataRequest.h"
#include "DerivedDataCacheAPI.h"
namespace Durin::DerivedData
{
	namespace Private
	{
		class IRequestOwnerInternal;
		struct FOwnerAccess;
	} // namespace Private
	// Tracks requests and callback retirement without exposing scheduler handles or leases.
	class IRequestOwner
	{
	public:
		virtual ~IRequestOwner() = default;
		IRequestOwner(const IRequestOwner&) = delete;
		auto operator=(const IRequestOwner&) -> IRequestOwner& = delete;
		DERIVEDDATACACHE_API auto Begin(std::shared_ptr<IRequest> Request) -> void;
		DERIVEDDATACACHE_API auto End(IRequest* Request) -> void;
		DERIVEDDATACACHE_API auto RequestCancel() -> void;
		DERIVEDDATACACHE_API auto Cancel() -> void;
		DERIVEDDATACACHE_API auto IsCanceled() const -> bool;
		DERIVEDDATACACHE_API auto GetPriority() const -> ERequestPriority;
		DERIVEDDATACACHE_API auto SetPriority(ERequestPriority Priority) -> void;
		// The body runs once, including canceled/rejected dispatch; it may inspect owner cancellation.
		DERIVEDDATACACHE_API auto LaunchTask(std::string_view Name, std::function<void()> Work) -> void;
		DERIVEDDATACACHE_API auto Poll() const -> bool;
		// Self-waits return WouldBlock; completion includes callbacks and tracked native task retirement.
		DERIVEDDATACACHE_API auto Wait() const -> ERequestWaitResult;

	protected:
		explicit IRequestOwner(std::shared_ptr<Private::IRequestOwnerInternal> State)
			: State(std::move(State))
		{
		}

	private:
		friend struct Private::FOwnerAccess;
		std::shared_ptr<Private::IRequestOwnerInternal> State;
	};
	// A barrier keeps the owner pending while a submission or callback can add requests.
	class FRequestBarrier
	{
	public:
		DERIVEDDATACACHE_API explicit FRequestBarrier(IRequestOwner& Owner);
		DERIVEDDATACACHE_API ~FRequestBarrier();
		FRequestBarrier(const FRequestBarrier&) = delete;
		auto operator=(const FRequestBarrier&) -> FRequestBarrier& = delete;

	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
	class FRequestOwner final : public IRequestOwner
	{
	public:
		DERIVEDDATACACHE_API explicit FRequestOwner(ERequestPriority Priority = ERequestPriority::Normal);
		// Destruction cancels and waits; it must occur outside this owner's callbacks.
		DERIVEDDATACACHE_API ~FRequestOwner() override;
		FRequestOwner(const FRequestOwner&) = delete;
		auto operator=(const FRequestOwner&) -> FRequestOwner& = delete;
	};
}
