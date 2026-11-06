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
		virtual auto RequestCancel() -> bool = 0;
	};
	struct FOwnerAccess
	{
		DERIVEDDATACACHE_API static auto LaunchTask(IRequestOwner& Owner, IBuildScheduler& Scheduler, std::string_view Name, std::function<void()> Work, std::function<void()> OnCanceled) -> TRefCountPtr<IRequest>;
		static auto RequestCancel(FRequestOwner& Owner) -> bool { return RequestCancel(*static_cast<IRequest*>(Owner)); }
		// Native tasks observe owner cancellation; signaling must never wait for them.
		static auto RequestCancel(IRequest& Request) -> bool { if (auto* Signal = dynamic_cast<ICancelableRequest*>(&Request)) return Signal->RequestCancel(); return false; }
	};
}
