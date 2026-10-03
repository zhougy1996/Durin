#pragma once

#include "CoreMinimal.h"

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
			const std::vector<FVector3f>& InPositions,
			bool bInNeedsCPUAccess = true) -> void;

		ENGINE_API auto Init(uint32 NumVertices, bool bInNeedsCPUAccess = true) -> void;

		// Durin ownership-transfer overload; borrowed input uses the const-reference overload.
		ENGINE_API auto Init(std::vector<FVector3f>&& InPositions, bool bInNeedsCPUAccess = true) -> void;

		// FRenderResource interface.
		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FPositionVertexBuffer";
		}

		auto GetNumVertices() const -> uint32
		{
			return NumVertices;
		}
		auto GetStride() const -> uint32 { return sizeof(FVector3f); }
		auto GetAllowCPUAccess() const -> bool { return bNeedsCPUAccess; }
		auto IsReady() const -> bool
		{
			return GetNumVertices() > 0 && GetRHI() != nullptr;
		}
		auto VertexPosition(uint32 VertexIndex) const -> const FVector3f&
		{
			check(VertexIndex < GetPositions().size());
			return GetPositions()[VertexIndex];
		}
		auto GetPositions() const -> std::span<const FVector3f>
		{
			return MeshStreamPrivate::Read(Positions, SharedPositions);
		}
		auto VertexPosition(uint32 VertexIndex) -> FVector3f&
		{
			check(VertexIndex < GetPositions().size());
			return static_cast<FVector3f*>(GetVertexData())[VertexIndex];
		}
		auto GetVertexData() const -> const void*
		{
			return GetPositions().data();
		}
		auto GetVertexData() -> void*
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Detach(Positions, SharedPositions).data();
		}

		auto FreezePositions() -> FSharedByteBuffer
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Freeze(Positions, SharedPositions);
		}
		auto SetSharedPositions(FSharedByteBuffer Value, bool bInNeedsCPUAccess = true) -> bool
		{
			check(!IsInitialized());
			if (!MeshStreamPrivate::Retain(Positions, SharedPositions, std::move(Value))) return false;
			NumVertices = static_cast<uint32>(GetPositions().size());
			bNeedsCPUAccess = bInNeedsCPUAccess;
			return true;
		}
		auto GetPositionCapacity() const -> size_t { return MeshStreamPrivate::Capacity(Positions, SharedPositions); }

	private:
		std::vector<FVector3f> Positions;
		FSharedByteBuffer SharedPositions;
		uint32 NumVertices = 0;
		bool bNeedsCPUAccess = true;
	};
}
