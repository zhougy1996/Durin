#pragma once

#include "Modules/ModuleManager.h"

namespace Durin::Tests
{
	enum class EDynamicUnloadFixtureEvent : uint8
	{
		Startup,
		SynchronousEntered,
		SynchronousExited,
		AsyncWorkerCompleted,
		AsyncPublished,
		AsyncCaptureDestroyed,
		BlockingWorkerEntered,
		RetainedResultReady,
		Shutdown,
		ConsoleCaptureDestroyed,
		ModuleDestroyed,
	};

	struct FDynamicUnloadFixtureEvent
	{
		EDynamicUnloadFixtureEvent Phase = EDynamicUnloadFixtureEvent::Startup;
		uint64 InstanceSerial = 0;
	};

	// Process-resident callbacks obtained on GameThread; the fixture owns worker drain.
	struct FDynamicUnloadWorkerCallbacks
	{
		std::function<void(FDynamicUnloadFixtureEvent)> Record;
		std::function<void(uint64)> WaitForAsyncRelease;
	};

	class IDynamicUnloadHostFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.DynamicUnloadHost";

		virtual auto AllocateInstanceSerial() -> uint64 = 0;
		virtual auto Record(FDynamicUnloadFixtureEvent Event) -> void = 0;
		virtual auto GetWorkerCallbacks() -> FDynamicUnloadWorkerCallbacks = 0;
		virtual auto WaitForAsyncRelease(uint64 InstanceSerial) -> void = 0;
	};

	class IDynamicUnloadFixtureFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.DynamicUnloadFixture";

		virtual auto GetInstanceSerial() const -> uint64 = 0;
		virtual auto RunSynchronousCall() -> void = 0;
		virtual auto StartDrainedAsyncChain() -> bool = 0;
		virtual auto StartRetainedResultForFailure() -> bool = 0;
		virtual auto StartBlockingWorkerForFailure() -> bool = 0;
		virtual auto SetThrowOnShutdownForFailure() -> void = 0;
		virtual auto RequestRecursiveUnloadForFailure() -> bool = 0;
	};

	template<typename T, typename F>
	auto VisitOnlyUnloadFeature(F&& Visitor)
	{
		FScopedModularFeatureAccess Scope;
		const auto Features = IModularFeatures::Get().GetModularFeatureImplementations<T>(FName(T::FeatureName));
		using TResult = std::invoke_result_t<F, T&>;
		if constexpr (std::is_void_v<TResult>)
		{
			if (Features.size() != 1) return false;
			std::invoke(Visitor, *Features.front());
			return true;
		}
		else
		{
			if (Features.size() != 1) return std::optional<TResult>{};
			return std::optional<TResult>(std::invoke(Visitor, *Features.front()));
		}
	}
}
