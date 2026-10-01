#pragma once

#include "EngineAPI.h"
#include "Rendering/SharedMeshStream.h"

#include "RenderResource.h"

namespace Durin
{
	// Owns retained CPU position data and its render-thread vertex buffer.
	class FPositionVertexBuffer : public FVertexBuffer
	{
	public:
		/** Default constructor. */
		ENGINE_API FPositionVertexBuffer();

		/** Destructor. */
		ENGINE_API ~FPositionVertexBuffer() override;

		ENGINE_API auto Init(
			std::vector<FVector3f> InPositions,
			bool bInNeedsCPUAccess = true) -> void;

		// FRenderResource interface.
		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FPositionVertexBuffer";
		}

		auto GetNumVertices() const -> uint32
		{
			return static_cast<uint32>(GetPositions().size());
		}
		auto GetStride() const -> uint32 { return sizeof(FVector3f); }
		auto NeedsCPUAccess() const -> bool { return bNeedsCPUAccess; }
		auto IsReady() const -> bool
		{
			return GetNumVertices() > 0 && GetRHI() != nullptr;
		}
		auto GetVertexPosition(uint32 VertexIndex) const -> const FVector3f&
		{
			check(VertexIndex < GetPositions().size());
			return GetPositions()[VertexIndex];
		}
		auto GetPositions() const -> std::span<const FVector3f>
		{
			return MeshStreamPrivate::Read(Positions, SharedPositions);
		}
		auto GetMutablePositions() -> std::vector<FVector3f>&
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Detach(Positions, SharedPositions);
		}

		auto FreezePositions() -> FSharedByteBuffer
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Freeze(Positions, SharedPositions);
		}
		auto SetSharedPositions(FSharedByteBuffer Value) -> bool
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Retain(Positions, SharedPositions, std::move(Value));
		}
		auto GetPositionCapacity() const -> size_t { return MeshStreamPrivate::Capacity(Positions, SharedPositions); }

	private:
		std::vector<FVector3f> Positions;
		FSharedByteBuffer SharedPositions;
		bool bNeedsCPUAccess = true;
	};
}
