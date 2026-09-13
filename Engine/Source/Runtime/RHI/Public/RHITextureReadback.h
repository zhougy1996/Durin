#pragma once

#include "RHIAPI.h"

namespace Durin
{
	// CPU-visible result only; backend allocations never escape their RHI owner.
	enum class ERHITextureReadbackState : uint8 { Pending, Ready, Failed, Canceled };

	// Thread-safe single-use readback. Cancellation discards publication, not GPU work.
	class FRHITextureReadback final
	{
	public:
		auto GetState() const -> ERHITextureReadbackState
		{
			std::lock_guard Lock(Mutex);
			return State;
		}
		// Moves tightly packed pixels once. Returns false until successful completion.
		auto TakePixels(FByteBuffer& Out) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (State != ERHITextureReadbackState::Ready || Pixels.empty()) return false;
			Out = std::move(Pixels);
			return true;
		}
		auto Cancel() -> void
		{
			std::lock_guard Lock(Mutex);
			State = ERHITextureReadbackState::Canceled;
			Pixels.clear();
		}
		// Backend publication after GPU completion and host-memory invalidation.
		auto Complete(FByteBuffer InPixels) -> void
		{
			std::lock_guard Lock(Mutex);
			if (State != ERHITextureReadbackState::Pending) return;
			Pixels = std::move(InPixels);
			State = ERHITextureReadbackState::Ready;
		}
		auto Fail() -> void
		{
			std::lock_guard Lock(Mutex);
			if (State == ERHITextureReadbackState::Pending) State = ERHITextureReadbackState::Failed;
		}
	private:
		mutable std::mutex Mutex;
		ERHITextureReadbackState State = ERHITextureReadbackState::Pending;
		FByteBuffer Pixels;
	};
}
