#pragma once
#include "CoreMinimal.h"
#include "DerivedDataRequest.h"
#include "DerivedDataCacheAPI.h"
namespace Durin::DerivedData
{
	class FRequestBarrier;
	enum class ERequestBarrierFlags : uint32 { None = 0, Priority = 1 << 0 };
	// Tracks requests and completion callbacks without exposing scheduler handles or leases.
	class IRequestOwner
	{
	public:
		DERIVEDDATACACHE_API virtual ~IRequestOwner();
		IRequestOwner(const IRequestOwner&) = delete;
		auto operator=(const IRequestOwner&) -> IRequestOwner& = delete;
		virtual auto Begin(IRequest* Request) -> void = 0;
		// Ends tracking and returns the owner's request reference. The callback overload
		// retains it through the callback and protects continuation submission with a barrier.
		virtual auto End(IRequest* Request) -> TRefCountPtr<IRequest> = 0;
		template <typename F, typename... Args>
		auto End(IRequest* Request, F&& Callback, Args&&... Arguments) -> TRefCountPtr<IRequest>;
		virtual auto IsCanceled() const -> bool = 0;
		virtual auto GetPriority() const -> ERequestPriority = 0;
		// The body runs once, including canceled dispatch; it may inspect owner cancellation.
		DERIVEDDATACACHE_API auto LaunchTask(std::string_view Name, TMoveOnlyFunction<void()>&& Work) -> void;

	protected:
		IRequestOwner() = default;

	public:
		virtual auto BeginBarrier(const FRequestBarrier& Barrier) -> void = 0;
		virtual auto EndBarrier(const FRequestBarrier& Barrier) -> void = 0;
	};
	// A barrier keeps the owner pending while a submission or callback can add requests.
	class FRequestBarrier
	{
	public:
		DERIVEDDATACACHE_API explicit FRequestBarrier(IRequestOwner& Owner, ERequestBarrierFlags Flags = ERequestBarrierFlags::None);
		DERIVEDDATACACHE_API ~FRequestBarrier();
		FRequestBarrier(const FRequestBarrier&) = delete;
		auto operator=(const FRequestBarrier&) -> FRequestBarrier& = delete;

		auto GetFlags() const -> ERequestBarrierFlags { return Flags; }
		DERIVEDDATACACHE_API static auto HasBarrierForOwnerOnCallingThread(IRequestOwner& Owner) -> bool;
	private:
		IRequestOwner& Owner;
		FRequestBarrier* NextOnThread;
		ERequestBarrierFlags Flags;
	};
	template <typename F, typename... Args>
	auto IRequestOwner::End(IRequest* Request, F&& Callback, Args&&... Arguments) -> TRefCountPtr<IRequest>
	{
		FRequestBarrier Barrier(*this, ERequestBarrierFlags::Priority);
		auto Retained = End(Request);
		std::invoke(std::forward<F>(Callback), std::forward<Args>(Arguments)...);
		return Retained;
	}
	class FRequestOwner final
	{
	public:
		DERIVEDDATACACHE_API explicit FRequestOwner(ERequestPriority Priority = ERequestPriority::Normal);
		DERIVEDDATACACHE_API ~FRequestOwner();
		FRequestOwner(const FRequestOwner&) = delete;
		auto operator=(const FRequestOwner&) -> FRequestOwner& = delete;
		DERIVEDDATACACHE_API FRequestOwner(FRequestOwner&& Other) noexcept;
		DERIVEDDATACACHE_API auto operator=(FRequestOwner&& Other) noexcept -> FRequestOwner&;
		DERIVEDDATACACHE_API operator IRequestOwner&() const;
		DERIVEDDATACACHE_API operator IRequest*() const;
		DERIVEDDATACACHE_API auto Cancel() -> void;
		DERIVEDDATACACHE_API auto Wait() const -> void;
		DERIVEDDATACACHE_API auto Poll() const -> bool;
		DERIVEDDATACACHE_API auto SetPriority(ERequestPriority Priority) -> void;
		DERIVEDDATACACHE_API auto GetPriority() const -> ERequestPriority;
		DERIVEDDATACACHE_API auto IsCanceled() const -> bool;
		DERIVEDDATACACHE_API auto LaunchTask(std::string_view Name, TMoveOnlyFunction<void()>&& Work) -> void;
		DERIVEDDATACACHE_API auto KeepAlive() -> void;
	private:
		struct FDeleteOwner { DERIVEDDATACACHE_API auto operator()(IRequestOwner* Owner) const -> void; };
		std::unique_ptr<IRequestOwner, FDeleteOwner> Owner;
	};
}
