#pragma once

#include "Threading/TaskComposition.h"

namespace Durin::Private
{
	// Audited package/asset save bridge. This is registered result delivery, not a
	// standalone promise. The domain must separately join its finish/capture retirement.
	template<typename T> class TExternalTaskResult
	{
	public:
		explicit TExternalTaskResult(const Tasks::FTaskExecutionOptions& Options)
			: Source(Tasks::Detail::FExternalResultAccess::Create<T>(Options)) {}
		auto TakeTask() -> Tasks::TTask<T> { return Source.TakeTask(); }
		auto IsCancellationRequested() const -> bool
		{ return FTaskRuntimeAccess::IsCancellationRequested(Source.GetTaskHandle()); }
		auto TrySetValue(T Value) const -> bool { return Source.TrySetValue(std::move(Value)); }
	private:
		Tasks::Detail::TExternalResultSource<T> Source;
	};
}
