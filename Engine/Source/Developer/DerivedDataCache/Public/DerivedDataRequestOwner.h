#pragma once

#include "CoreMinimal.h"
#include "DerivedDataRequest.h"
#include "DerivedDataBuildScheduler.h"
#include "DerivedDataCacheAPI.h"
#include "Threading/Task.h"

namespace Durin::DerivedData
{
	namespace Private { class FOwnerState; }
	// External work participates in the same request-end boundary as DDC work.
	struct FRequestDependency
	{
		std::function<bool()> IsFinished;
		std::function<ERequestWaitResult()> Wait;
		std::function<void()> RequestCancel;
	};
	class IRequestOwner
	{
	public:
		virtual ~IRequestOwner() = default;
		// Begin returns the shared owner lease retained by the request until End.
		virtual auto Begin(std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwner> = 0;
		virtual auto End(IRequest* Request) -> void = 0;
		virtual auto RequestCancel() -> void = 0;
		virtual auto Cancel() -> void = 0;
		virtual auto IsCanceled() const -> bool = 0;
		virtual auto GetPriority() const -> ERequestPriority = 0;
		virtual auto SetPriority(ERequestPriority Priority) -> void = 0;
		virtual auto LaunchTask(IRequest* Request, IBuildScheduler& Scheduler, std::string_view Name, std::function<void()> Work, std::function<void()> OnCanceled)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> = 0;
		virtual auto Poll() const -> bool = 0;
		virtual auto Wait() const -> ERequestWaitResult = 0;
	};
	// A scoped request keeps the owner pending until newly dispatched handles
	// are published. Construct and destroy on the submitting thread.
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
		// Destruction waits for request end; it must occur outside callbacks.
		DERIVEDDATACACHE_API ~FRequestOwner() override;
		FRequestOwner(const FRequestOwner&) = delete;
		auto operator=(const FRequestOwner&) -> FRequestOwner& = delete;
		DERIVEDDATACACHE_API auto Begin(std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwner> override;
		DERIVEDDATACACHE_API auto End(IRequest* Request) -> void override;
		DERIVEDDATACACHE_API auto RequestCancel() -> void override;
		DERIVEDDATACACHE_API auto Cancel() -> void override;
		DERIVEDDATACACHE_API auto IsCanceled() const -> bool override;
		DERIVEDDATACACHE_API auto GetPriority() const -> ERequestPriority override;
		DERIVEDDATACACHE_API auto SetPriority(ERequestPriority Priority) -> void override;
		DERIVEDDATACACHE_API auto LaunchTask(IRequest* Request, IBuildScheduler& Scheduler, std::string_view Name, std::function<void()> Work, std::function<void()> OnCanceled)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override;
		DERIVEDDATACACHE_API auto Poll() const -> bool override;
		DERIVEDDATACACHE_API auto Wait() const -> ERequestWaitResult override;
		// Register before leaving the submission barrier. Poll/Wait include these
		// handles. Dependency queries run outside owner locks and must not wait
		// on this owner. Native task cancellation remains with its source.
		DERIVEDDATACACHE_API auto TrackTask(FTaskHandle Task) -> void;
		DERIVEDDATACACHE_API auto TrackDependency(FRequestDependency Dependency) -> void;
	private:
		std::shared_ptr<Private::FOwnerState> State;
	};
}
