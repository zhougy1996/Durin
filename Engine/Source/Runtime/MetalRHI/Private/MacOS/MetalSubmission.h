#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "MetalGPUTiming.h"
#include "Backend/RHICompletionBackend.h"
#include "RHIResources.h"
#include "RHIGPUReadback.h"
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
			FreeReadbackBuffers.clear();
			Queue.reset();
		}
		uint64 Generation = 0;
		std::shared_ptr<FMetalGPUTimingPool> TimingPool;
		std::mutex Mutex;
		std::condition_variable Completion;
		std::unique_ptr<FRHIGPUQueueTimeline> Timeline;
		size_t PendingCallbacks = 0;
		std::vector<NS::SharedPtr<MTL::Buffer>> FreeReadbackBuffers;
		uint64 CachedReadbackBytes = 0, PendingReadbackBytes = 0;
		uint32 PendingBufferReadbacks = 0;
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
			FReadback(NS::SharedPtr<MTL::Buffer> InBuffer, NS::UInteger InBytes, std::shared_ptr<RHIPrivate::FReadbackState> InRequest,
				std::shared_ptr<FMetalSubmissionState> InPool = {})
				: Buffer(std::move(InBuffer)), ByteCount(InBytes), Request(std::move(InRequest)), Pool(std::move(InPool)) {}
			FReadback(FReadback&&) noexcept = default;
			auto operator=(FReadback&& Other) noexcept -> FReadback&
			{
				if (this != &Other) { ReleasePool(); Buffer = std::move(Other.Buffer); ByteCount = Other.ByteCount; Request = std::move(Other.Request); Pool = std::move(Other.Pool); bReusable = Other.bReusable; }
				return *this;
			}
			~FReadback() { ReleasePool(); }
			auto ReleasePool() -> void
			{
				if (!Pool) return;
				std::lock_guard Lock(Pool->Mutex);
				const uint64 Capacity = Buffer->length();
				--Pool->PendingBufferReadbacks; Pool->PendingReadbackBytes -= Capacity;
				if (bReusable && Pool->FreeReadbackBuffers.size() < 8 && Capacity <= 16ull * 1024 * 1024 - Pool->CachedReadbackBytes)
				{ Pool->CachedReadbackBytes += Capacity; Pool->FreeReadbackBuffers.push_back(Buffer); }
			}
			NS::SharedPtr<MTL::Buffer> Buffer;
			NS::UInteger ByteCount = 0;
			std::shared_ptr<RHIPrivate::FReadbackState> Request;
			std::shared_ptr<FMetalSubmissionState> Pool;
			bool bReusable = true;
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
