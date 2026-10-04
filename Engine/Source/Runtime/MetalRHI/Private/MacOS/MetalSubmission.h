#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "MetalGPUTiming.h"
#include "Backend/RHICompletionBackend.h"
#include "RHIResources.h"
#include "RHITextureReadback.h"
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

namespace Durin
{
	class FDynamicRHI;
	struct FMetalSubmissionState
	{
		NS::SharedPtr<MTL::CommandQueue> Queue;
		~FMetalSubmissionState()
		{
			const FMetalAutoreleasePool Pool;
			Queue.reset();
		}
		uint64 Generation = 0;
		std::shared_ptr<FMetalGPUTimingPool> TimingPool;
		std::mutex Mutex;
		std::condition_variable Completion;
		std::unique_ptr<FRHIGPUQueueTimeline> Timeline;
		size_t PendingCallbacks = 0;
	};

	// Completion payload owns GPU dependencies independently of backend/command lifetime.
	// It deliberately contains no command buffer, avoiding a handler ownership cycle.
	struct FMetalSubmissionOwners
	{
		struct FTimingSample
		{
			TRefCountPtr<FMetalGPUTimingQuery> Query;
			bool bEnd = false;
		};
		struct FReadback
		{
			NS::SharedPtr<MTL::Buffer> Buffer;
			NS::UInteger ByteCount = 0;
			std::shared_ptr<FRHITextureReadback> Request;
		};
		FMetalSubmissionOwners() = default;
		FMetalSubmissionOwners(FMetalSubmissionOwners&&) = default;
		auto operator=(FMetalSubmissionOwners&&) -> FMetalSubmissionOwners& = default;
		~FMetalSubmissionOwners() { Release(); }
		auto Release() -> void
		{
			const FMetalAutoreleasePool Pool;
			Drawable.reset();
			Readbacks.clear();
			TimingSamples.clear();
			NativeResources.clear();
			ResourceOwners.clear();
			StorageOwners.clear();
		}
		std::vector<std::shared_ptr<void>> StorageOwners;
		std::vector<TRefCountPtr<FRHIResource>> ResourceOwners;
		std::vector<NS::SharedPtr<MTL::Resource>> NativeResources;
		std::vector<FReadback> Readbacks;
		std::vector<FTimingSample> TimingSamples;
		NS::SharedPtr<CA::MetalDrawable> Drawable;
	};

	struct FMetalPendingSubmission : FMetalSubmissionOwners
	{
		FMetalPendingSubmission() = default;
		FMetalPendingSubmission(FMetalPendingSubmission&&) = default;
		auto operator=(FMetalPendingSubmission&&) -> FMetalPendingSubmission& = default;
		~FMetalPendingSubmission()
		{
			const FMetalAutoreleasePool Pool;
			Command.reset();
		}
		NS::SharedPtr<MTL::CommandBuffer> Command;
		FRHIGPUSyncPointRef Producer;
	};

	// Borrow for backend qualification/diagnostics; valid only while RHI is initialized.
	auto GetMetalSubmissionQueue(FDynamicRHI& RHI) -> MTL::CommandQueue*;
	// Native completion status is an input so failure handling can be tested safely.
	auto CompleteMetalSubmission(FMetalSubmissionState& State,
		const FRHIGPUSyncPointRef& Producer, FMetalSubmissionOwners& Owners,
		MTL::CommandBufferStatus Status) -> void;
}
