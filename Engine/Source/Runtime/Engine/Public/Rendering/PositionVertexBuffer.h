#pragma once

#include "EngineAPI.h"

#include "RenderResource.h"

namespace Durin
{
	// Stores one local-space vertex position.
	struct FPositionVertex
	{
		FVector3f Position;
	};

	// Owns retained CPU position data and its render-thread vertex buffer.
	class FPositionVertexBuffer : public FVertexBuffer
	{
	public:
		/** Default constructor. */
		ENGINE_API FPositionVertexBuffer();

		/** Destructor. */
		ENGINE_API ~FPositionVertexBuffer() override;

		ENGINE_API auto Init(
			uint32 InNumVertices,
			bool bInNeedsCPUAccess = true) -> void;
		ENGINE_API auto Init(
			const std::vector<FVector3f>& InPositions,
			bool bInNeedsCPUAccess = true) -> void;

		// FRenderResource interface.
		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		ENGINE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FPositionVertexBuffer";
		}

		auto GetNumVertices() const -> uint32
		{
			return static_cast<uint32>(Positions.size());
		}
		auto GetStride() const -> uint32 { return sizeof(FVector3f); }
		auto NeedsCPUAccess() const -> bool { return bNeedsCPUAccess; }
		auto IsReady() const -> bool
		{
			return GetNumVertices() > 0 && GetRHI() != nullptr;
		}
		auto GetVertexPosition(uint32 VertexIndex) const -> const FVector3f&
		{
			check(VertexIndex < Positions.size());
			return Positions[VertexIndex];
		}
		auto GetPositions() const -> const std::vector<FVector3f>&
		{
			return Positions;
		}
		auto GetMutablePositions() -> std::vector<FVector3f>&
		{
			check(!IsInitialized());
			return Positions;
		}

	private:
		std::vector<FVector3f> Positions;
		bool bNeedsCPUAccess = true;
	};
}
