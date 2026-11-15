#pragma once

#include "CoreMinimal.h"
#include "RHIAPI.h"

namespace Durin
{
	class FRHICommandListBase;
	class FRHIBuffer;
	class FRHIGPUMemoryReadback;
	enum class ERHIGPUReadbackState : uint8 { Pending, Ready, Failed, Canceled };
	using ERHITextureReadbackState = ERHIGPUReadbackState;

	namespace RHIPrivate
	{
		// Shared storage survives the facade. One copy may be in flight; reuse waits for retirement.
		class FReadbackState final
		{
		public:
			auto GetState() const -> ERHIGPUReadbackState { std::lock_guard Lock(Mutex); return State; }
			auto IsInFlight() const -> bool { std::lock_guard Lock(Mutex); return bInFlight; }
			auto GetDataSize() const -> size_t { std::lock_guard Lock(Mutex); return Data.size(); }
			auto BeginCopy() -> void
			{
				std::lock_guard Lock(Mutex);
				requiref(!bInFlight, "GPU readback reuse requires the previous copy to retire.");
				ClearReadyData(); State = ERHIGPUReadbackState::Pending; bInFlight = true;
			}
			auto ResetReady() -> void
			{ std::lock_guard Lock(Mutex); require(!bInFlight); ClearReadyData(); State = ERHIGPUReadbackState::Pending; }
			auto Cancel() -> void { std::lock_guard Lock(Mutex); CancelLocked(); }
			// Only a command/backend that will never complete this copy may abandon it.
			auto Abandon() -> void { std::lock_guard Lock(Mutex); CancelLocked(); bInFlight = false; }
			auto Complete(FByteBuffer Bytes) -> void
			{ std::lock_guard Lock(Mutex); bInFlight = false; if (State != ERHIGPUReadbackState::Pending) return; Data = std::move(Bytes); State = ERHIGPUReadbackState::Ready; }
			auto Fail() -> void
			{ std::lock_guard Lock(Mutex); bInFlight = false; if (State == ERHIGPUReadbackState::Pending) State = ERHIGPUReadbackState::Failed; }
		private:
			friend class Durin::FRHIGPUMemoryReadback;
			auto TakeData(FByteBuffer& Out) -> bool
			{ std::lock_guard Lock(Mutex); if (State != ERHIGPUReadbackState::Ready || bLocked || Data.empty()) return false; Out = std::move(Data); return true; }
			auto ClearReadyData() -> void
			{ if (bLocked && LockedData.empty()) LockedData = std::move(Data); else Data.clear(); }
			auto CancelLocked() -> void
			{ State = ERHIGPUReadbackState::Canceled; if (!bLocked || !LockedData.empty()) Data.clear(); }
			auto LockData(uint32 NumBytes) -> const void*
			{ std::lock_guard Lock(Mutex); if (State != ERHIGPUReadbackState::Ready || bLocked || !NumBytes || NumBytes > Data.size()) return nullptr; bLocked = true; return Data.data(); }
			auto UnlockData() -> void
			{ std::lock_guard Lock(Mutex); require(bLocked); bLocked = false; LockedData.clear(); if (State == ERHIGPUReadbackState::Canceled) Data.clear(); }
			mutable std::mutex Mutex;
			ERHIGPUReadbackState State = ERHIGPUReadbackState::Pending;
			FByteBuffer Data, LockedData;
			bool bInFlight = false, bLocked = false;
		};
	}

	class FRHIGPUMemoryReadback
	{
	public:
		FRHIGPUMemoryReadback() = default;
		FRHIGPUMemoryReadback(const FRHIGPUMemoryReadback&) = delete;
		auto operator=(const FRHIGPUMemoryReadback&) -> FRHIGPUMemoryReadback& = delete;
		virtual ~FRHIGPUMemoryReadback() { Cancel(); }
		auto GetState() const -> ERHIGPUReadbackState { return State->GetState(); }
		auto GetDataSize() const -> size_t { return State->GetDataSize(); }
		auto IsReady() const -> bool { return GetState() == ERHIGPUReadbackState::Ready; }
		auto IsInFlight() const -> bool { return State->IsInFlight(); }
		auto ResetReady() -> void { State->ResetReady(); }
		auto Cancel() -> void { State->Cancel(); }
	protected:
		// Consuming access is reserved for the texture facade; buffers only borrow bytes.
		auto TakeData(FByteBuffer& Out) -> bool { return State->TakeData(Out); }
		auto BeginCopy() -> std::shared_ptr<RHIPrivate::FReadbackState> { State->BeginCopy(); return State; }
		auto LockData(uint32 NumBytes) -> const void* { return State->LockData(NumBytes); }
		auto UnlockData() -> void { State->UnlockData(); }
	private:
		friend class FRHICommandListBase;
		const std::shared_ptr<RHIPrivate::FReadbackState> State = std::make_shared<RHIPrivate::FReadbackState>();
	};

	// Reusable facade. Concurrent copies/results belong to distinct readback objects.
	class FRHIGPUBufferReadback final : public FRHIGPUMemoryReadback
	{
	public:
		RHI_API auto EnqueueCopy(FRHICommandListBase& Commands, FRHIBuffer* Source, uint32 NumBytes = 0, uint32 Offset = 0) -> void;
		auto Lock(uint32 NumBytes) -> const void* { return LockData(NumBytes); }
		auto Unlock() -> void { UnlockData(); }
	};
}
