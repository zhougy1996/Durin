#pragma once
#include "CoreMinimal.h"
#include "DerivedDataRequestOwner.h"
#include "BuildSchedulerPrivate.h"
#include "Threading/Task.h"
namespace Durin::DerivedData::Private
{
	// External work participates in the same request-end boundary as DDC work.
	struct FRequestDependency
	{
		std::function<bool()> IsFinished;
		std::function<ERequestWaitResult()> Wait;
		std::function<void()> RequestCancel;
	};
	class IRequestOwnerInternal
	{
	public:
		virtual ~IRequestOwnerInternal() = default;
		// Begin returns the shared owner lease retained by the request until End.
		virtual auto Begin(std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwnerInternal> = 0;
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
	struct FOwnerAccess
	{
		static auto Get(IRequestOwner& Owner) -> std::shared_ptr<IRequestOwnerInternal> { return Owner.State; }
		DERIVEDDATACACHE_API static auto TrackTask(FRequestOwner& Owner, Tasks::FTask Task) -> void;
		DERIVEDDATACACHE_API static auto TrackDependency(FRequestOwner& Owner, FRequestDependency Dependency) -> void;
	};
	inline auto BeginRequest(IRequestOwner& Owner, std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwnerInternal>
	{
		return FOwnerAccess::Get(Owner)->Begin(std::move(Request));
	}
} // namespace Durin::DerivedData::Private

namespace Durin::DerivedData::Private
{
	inline auto BeginRequest(IRequestOwnerInternal& Owner, std::shared_ptr<IRequest> Request) -> std::shared_ptr<IRequestOwnerInternal>
	{
		return Owner.Begin(std::move(Request));
	}
} // namespace Durin::DerivedData::Private
