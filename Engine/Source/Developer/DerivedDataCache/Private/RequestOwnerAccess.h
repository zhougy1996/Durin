#pragma once
#include "CoreMinimal.h"
#include "DerivedDataRequestOwner.h"
#include "BuildSchedulerPrivate.h"
namespace Durin::DerivedData::Private
{
	class ICancelableRequest
	{
	public:
		virtual ~ICancelableRequest() = default;
		virtual auto RequestCancel() -> void = 0;
	};
	struct FOwnerAccess
	{
		DERIVEDDATACACHE_API static auto End(IRequestOwner& Owner, IRequest* Request, TMoveOnlyFunction<void()>&& Callback) -> TRefCountPtr<IRequest>;
		DERIVEDDATACACHE_API static auto Invoke(IRequestOwner& Owner, TMoveOnlyFunction<void()>&& Callback) -> void;
		DERIVEDDATACACHE_API static auto LaunchTask(IRequestOwner& Owner, IBuildScheduler& Scheduler, std::string_view Name, std::function<void()> Work, std::function<void()> OnCanceled) -> TRefCountPtr<IRequest>;
		static auto RequestCancel(FRequestOwner& Owner) -> void { RequestCancel(*static_cast<IRequest*>(Owner)); }
		// Native tasks observe owner cancellation; signaling must never wait for them.
		static auto RequestCancel(IRequest& Request) -> void { if (auto* Signal = dynamic_cast<ICancelableRequest*>(&Request)) Signal->RequestCancel(); }
	};
}
