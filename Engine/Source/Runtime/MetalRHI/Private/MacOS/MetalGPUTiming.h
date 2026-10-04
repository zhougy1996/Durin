#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"
#include <Metal/Metal.hpp>

namespace Durin
{
	// A bounded shared buffer avoids Metal's per-device sample-buffer count limit.
	struct FMetalGPUTimingPool
	{
		static constexpr uint32 Capacity = 256;
		NS::SharedPtr<MTL::CounterSampleBuffer> Buffer;
		NS::SharedPtr<MTL::Buffer> Marker;
		uint64 Generation = 0;
		double NanosecondsPerTick = 0;
		std::mutex Mutex;
		std::array<bool, Capacity> Used{};
		~FMetalGPUTimingPool()
		{
			const FMetalAutoreleasePool Pool;
			Buffer.reset();
			Marker.reset();
		}
	};

	class FMetalGPUTimingQuery final : public FRHIGPUTimingQuery
	{
	public:
		FMetalGPUTimingQuery(std::shared_ptr<FMetalGPUTimingPool> InPool, uint32 InSlot)
			: Pool(std::move(InPool)), Slot(InSlot) {}
		~FMetalGPUTimingQuery() override
		{
			std::lock_guard Lock(Pool->Mutex);
			Pool->Used[Slot] = false;
		}
		auto Invalidate() -> void { PublishInvalid(); }
		auto Resolve() -> void
		{
			const FMetalAutoreleasePool AutoreleasePool;
			if (GetResult().State != ERHIGPUTimingResultState::Pending) return;
			auto* Data = Pool->Buffer->resolveCounterRange(NS::Range::Make(Slot * 2, 2));
			if (!Data || Data->length() != sizeof(MTL::CounterResultTimestamp) * 2)
				return PublishInvalid();
			MTL::CounterResultTimestamp Samples[2];
			std::memcpy(Samples, Data->bytes(), sizeof(Samples));
			if (!Samples[0].timestamp || !Samples[1].timestamp
				|| Samples[0].timestamp == MTL::CounterErrorValue
				|| Samples[1].timestamp == MTL::CounterErrorValue
				|| Samples[1].timestamp < Samples[0].timestamp)
				return PublishInvalid();
			const long double Duration = (Samples[1].timestamp - Samples[0].timestamp)
				* static_cast<long double>(Pool->NanosecondsPerTick);
			if (!std::isfinite(Duration) || Duration >= std::numeric_limits<uint64>::max())
				return PublishInvalid();
			PublishReady(static_cast<uint64>(std::round(Duration)));
		}
		std::shared_ptr<FMetalGPUTimingPool> Pool;
		uint32 Slot;
	};
}
