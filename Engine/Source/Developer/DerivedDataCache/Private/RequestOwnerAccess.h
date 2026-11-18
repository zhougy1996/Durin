#pragma once
#include "CoreMinimal.h"
#include "DerivedDataRequestOwner.h"
#include "BuildSchedulerPrivate.h"
namespace Durin::DerivedData::Private
{
	struct FOwnerAccess
	{
		DERIVEDDATACACHE_API static auto LaunchTask(IRequestOwner& Owner, IBuildScheduler& Scheduler, std::string_view Name, std::function<void()> Work, std::function<void()> OnCanceled) -> TRefCountPtr<IRequest>;
	};
}
