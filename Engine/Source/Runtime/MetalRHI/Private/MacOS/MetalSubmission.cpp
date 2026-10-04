#include "MetalSubmission.h"

namespace Durin
{
	auto CompleteMetalSubmission(FMetalSubmissionState& State,
		const FRHIGPUSyncPointRef& Producer, FMetalSubmissionOwners& Owners,
		MTL::CommandBufferStatus Status) -> void
	{
		const FMetalAutoreleasePool Pool;
		for (const auto& Readback : Owners.Readbacks)
		{
			if (Status != MTL::CommandBufferStatusCompleted)
			{
				Readback.Request->Fail();
				continue;
			}
			const auto* Bytes = static_cast<const std::byte*>(Readback.Buffer->contents());
			if (!Bytes) Readback.Request->Fail();
			else Readback.Request->Complete(FByteBuffer(Bytes, Bytes + Readback.ByteCount));
		}
		// Release GPU dependencies before publishing completion/allowing shutdown to return.
		Owners.Release();
		std::lock_guard Lock(State.Mutex);
		if (Status == MTL::CommandBufferStatusCompleted)
			State.Timeline->ObserveCompleted(Producer);
		else
			State.Timeline->Fail();
		--State.PendingCallbacks;
		State.Completion.notify_all();
	}
}
