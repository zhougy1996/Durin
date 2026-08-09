#pragma once

#include "EngineAPI.h"
#include "Math/Box.h"
#include "Rendering/PositionVertexBuffer.h"
#include "StaticMesh/LocalVertexFactory.h"

#include "RenderResource.h"

namespace Durin
{
	inline constexpr uint32 MaxStaticMeshUVChannels = 4;

	// Preserves index-ordered imported material metadata in runtime mesh data.
	struct FStaticMeshMaterialSlot
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
	};

	// Describes one indexed draw range and its local-space bounds.
	struct FStaticMeshSection
	{
		std::string Name;
		uint32 FirstIndex = 0;
		uint32 IndexCount = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		uint32 MaterialSlotIndex = 0;
		FBox LocalBounds;
	};

	// Stores one normalized 16-bit tangent frame in the tangent stream.
	struct FStaticMeshPackedTangentBasis
	{
		std::array<int16, 4> Normal{};
		std::array<int16, 4> Tangent{};
	};

	// Stores all materialized UV channels for one vertex in the texcoord stream.
	struct FStaticMeshTexcoordVertex
	{
		std::array<FVector2f, MaxStaticMeshUVChannels> TexCoords{};
	};

	// Stores one normalized 8-bit vertex color in the color stream.
	struct FStaticMeshColorVertex
	{
		std::array<uint8, 4> Color{};
	};

	static_assert(sizeof(FStaticMeshPackedTangentBasis) == 16);
	static_assert(sizeof(FStaticMeshTexcoordVertex) == 32);
	static_assert(sizeof(FStaticMeshColorVertex) == 4);

	// Groups independently bindable tangent-basis and texture-coordinate buffers.
	class FStaticMeshVertexBuffer
	{
	public:
		class FTangentsVertexBuffer : public FVertexBuffer
		{
		public:
			ENGINE_API auto Init(
				std::vector<FVector3f> InNormals,
				std::vector<FVector4f> InTangents,
				bool bInNeedsCPUAccess = true) -> void;
			ENGINE_API auto InitRHI(
				FRHICommandListBase& RHICmdList) -> void override;
			auto GetFriendlyName() const -> std::string override
			{
				return "FStaticMeshVertexBuffer::FTangentsVertexBuffer";
			}
			auto GetNumVertices() const -> uint32
			{
				return static_cast<uint32>(Normals.size());
			}
			auto GetStride() const -> uint32
			{
				return sizeof(FStaticMeshPackedTangentBasis);
			}
			auto NeedsCPUAccess() const -> bool
			{
				return bNeedsCPUAccess;
			}
			auto IsReady() const -> bool
			{
				return GetNumVertices() > 0
					&& Tangents.size() == Normals.size()
					&& GetRHI() != nullptr;
			}
			auto GetNormals() const -> const std::vector<FVector3f>&
			{
				return Normals;
			}
			auto GetTangents() const -> const std::vector<FVector4f>&
			{
				return Tangents;
			}
			auto GetMutableNormals() -> std::vector<FVector3f>&
			{
				check(!IsInitialized());
				return Normals;
			}
			auto GetMutableTangents() -> std::vector<FVector4f>&
			{
				check(!IsInitialized());
				return Tangents;
			}

		private:
			std::vector<FVector3f> Normals;
			std::vector<FVector4f> Tangents;
			bool bNeedsCPUAccess = true;
		};

		class FTexcoordVertexBuffer : public FVertexBuffer
		{
		public:
			ENGINE_API auto Init(
				std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels>
					InTexCoords,
				uint32 NumVertices,
				uint8 InNumTexCoords,
				bool bInNeedsCPUAccess = true) -> void;
			ENGINE_API auto InitRHI(
				FRHICommandListBase& RHICmdList) -> void override;
			auto GetFriendlyName() const -> std::string override
			{
				return "FStaticMeshVertexBuffer::FTexcoordVertexBuffer";
			}
			auto GetNumVertices() const -> uint32
			{
				return TexCoords[0].empty()
					? 0
					: static_cast<uint32>(TexCoords[0].size());
			}
			auto GetNumTexCoords() const -> uint8 { return NumTexCoords; }
			auto NeedsCPUAccess() const -> bool
			{
				return bNeedsCPUAccess;
			}
			auto GetStride() const -> uint32
			{
				return sizeof(FStaticMeshTexcoordVertex);
			}
			auto IsReady() const -> bool
			{
				const size_t NumVertices = TexCoords[0].size();
				return NumVertices > 0
					&& std::ranges::all_of(
						TexCoords,
						[NumVertices](const auto& Channel) {
							return Channel.size() == NumVertices;
						})
					&& GetRHI() != nullptr;
			}
			auto GetVertexUV(
				uint32 VertexIndex,
				uint32 Channel) const -> const FVector2f&
			{
				check(Channel < MaxStaticMeshUVChannels);
				check(VertexIndex < TexCoords[Channel].size());
				return TexCoords[Channel][VertexIndex];
			}
			auto GetTexCoords() const
				-> const std::array<
					std::vector<FVector2f>,
					MaxStaticMeshUVChannels>&
			{
				return TexCoords;
			}
			auto GetMutableTexCoords()
				-> std::array<
					std::vector<FVector2f>,
					MaxStaticMeshUVChannels>&
			{
				check(!IsInitialized());
				return TexCoords;
			}
			auto SetNumTexCoords(uint8 InNumTexCoords) -> void
			{
				check(!IsInitialized());
				NumTexCoords = InNumTexCoords;
			}

		private:
			std::array<
				std::vector<FVector2f>,
				MaxStaticMeshUVChannels> TexCoords;
			uint8 NumTexCoords = 0;
			bool bNeedsCPUAccess = true;
		};

		auto GetNumVertices() const -> uint32
		{
			return TangentsVertexBuffer.GetNumVertices();
		}
		auto NeedsCPUAccess() const -> bool
		{
			return TangentsVertexBuffer.NeedsCPUAccess()
				|| TexCoordVertexBuffer.NeedsCPUAccess();
		}
		auto IsReady() const -> bool
		{
			return GetNumVertices() > 0
				&& TangentsVertexBuffer.IsReady()
				&& TexCoordVertexBuffer.IsReady();
		}

		FTangentsVertexBuffer TangentsVertexBuffer;
		FTexcoordVertexBuffer TexCoordVertexBuffer;
	};

	// Owns materialized per-vertex color data and its independently bindable RHI.
	class FColorVertexBuffer : public FVertexBuffer
	{
	public:
		ENGINE_API auto Init(
			std::vector<FVector4f> InColors,
			uint32 NumVertices,
			bool bInNeedsCPUAccess = true) -> void;
		ENGINE_API auto InitRHI(
			FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FColorVertexBuffer";
		}
		auto GetNumVertices() const -> uint32
		{
			return static_cast<uint32>(Colors.size());
		}
		auto GetStride() const -> uint32
		{
			return sizeof(FStaticMeshColorVertex);
		}
		auto NeedsCPUAccess() const -> bool
		{
			return bNeedsCPUAccess;
		}
		auto IsReady() const -> bool
		{
			return GetNumVertices() > 0 && GetRHI() != nullptr;
		}
		auto GetVertexColor(uint32 VertexIndex) const -> const FVector4f&
		{
			check(VertexIndex < Colors.size());
			return Colors[VertexIndex];
		}
		auto GetColors() const -> const std::vector<FVector4f>&
		{
			return Colors;
		}
		auto GetMutableColors() -> std::vector<FVector4f>&
		{
			check(!IsInitialized());
			return Colors;
		}

	private:
		std::vector<FVector4f> Colors;
		bool bNeedsCPUAccess = true;
	};

	// Groups the UE-named semantic vertex buffers for one static-mesh LOD.
	struct FStaticMeshVertexBuffers
	{
		FPositionVertexBuffer PositionVertexBuffer;
		FStaticMeshVertexBuffer StaticMeshVertexBuffer;
		FColorVertexBuffer ColorVertexBuffer;

		ENGINE_API auto Finalize(
			uint8 NumTexCoords,
			bool bHasColorVertexData) -> void;
		ENGINE_API auto InitResources(
			FRHICommandListBase& RHICmdList) -> void;
		ENGINE_API auto ReleaseResources() -> void;
		auto IsReady() const -> bool
		{
			return PositionVertexBuffer.IsReady()
				&& StaticMeshVertexBuffer.IsReady()
				&& ColorVertexBuffer.IsReady();
		}
	};

	// Owns uint32 static-mesh indices and their RHI allocation.
	class FRawStaticIndexBuffer : public FIndexBuffer
	{
	public:
		ENGINE_API auto Init(
			std::vector<uint32> InIndices,
			bool bInNeedsCPUAccess = true) -> void;
		ENGINE_API auto InitRHI(
			FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FRawStaticIndexBuffer";
		}
		auto GetNumIndices() const -> uint32
		{
			return static_cast<uint32>(Indices.size());
		}
		auto GetStride() const -> uint32 { return sizeof(uint32); }
		auto NeedsCPUAccess() const -> bool
		{
			return bNeedsCPUAccess;
		}
		auto IsReady() const -> bool
		{
			return GetNumIndices() > 0 && GetRHI() != nullptr;
		}
		auto GetIndex(uint32 Index) const -> uint32
		{
			check(Index < Indices.size());
			return Indices[Index];
		}
		auto GetIndices() const -> const std::vector<uint32>&
		{
			return Indices;
		}
		auto GetMutableIndices() -> std::vector<uint32>&
		{
			check(!IsInitialized());
			return Indices;
		}

	private:
		std::vector<uint32> Indices;
		bool bNeedsCPUAccess = true;
	};

	// Owns named CPU and GPU buffer resources for one LOD.
	struct FStaticMeshLODResources
	{
		FStaticMeshVertexBuffers VertexBuffers;
		FRawStaticIndexBuffer IndexBuffer;
		std::vector<FStaticMeshSection> Sections;
		FBox LocalBounds;
		float ScreenSize = 0.0f;
		uint8 NumTexCoords = 0;
		bool bHasColorVertexData = false;

		auto GetNumVertices() const -> uint32
		{
			return VertexBuffers.PositionVertexBuffer.GetNumVertices();
		}
		auto GetNumIndices() const -> uint32
		{
			return IndexBuffer.GetNumIndices();
		}
	};

	class FRHICommandListImmediate;

	ENGINE_API auto PackStaticMeshTangentBasis(
		const FVector3f& Normal,
		const FVector4f& Tangent) -> FStaticMeshPackedTangentBasis;
	ENGINE_API auto PackStaticMeshColor(
		const FVector4f& Color) -> FStaticMeshColorVertex;

	// Owns all renderable LODs, material slots, and bounds for a static mesh.
	struct FStaticMeshRenderData
	{
		std::vector<FStaticMeshLODResources> LODResources;
		std::vector<FStaticMeshVertexFactories> LODVertexFactories;
		std::vector<FStaticMeshMaterialSlot> MaterialSlots;
		FBox LocalBounds;

		ENGINE_API auto InitResources(FRHICommandListImmediate& RHICmdList) -> bool;
		ENGINE_API auto ReleaseResources() -> void;
#if DURIN_BUILD_DEBUG
		ENGINE_API auto SetResourceDebugOwner(FName InOwner) -> void;
#endif
		ENGINE_API auto GetNumInitializedResources() const -> size_t;
		ENGINE_API auto IsReadyForRendering(uint32 LODIndex = 0) const -> bool;
		ENGINE_API auto RecalculateBounds() -> void;
	};

	// Produces the deterministic policy used by builders without authored thresholds.
	ENGINE_API auto GenerateDefaultStaticMeshLODScreenSizes(
		uint32 LODCount) -> std::vector<float>;

	// Validates the published policy: finite, [0, 1], strictly descending, and final zero.
	ENGINE_API auto ValidateStaticMeshLODScreenSizes(
		std::span<const FStaticMeshLODResources> LODResources,
		std::string& OutError) -> bool;
}
