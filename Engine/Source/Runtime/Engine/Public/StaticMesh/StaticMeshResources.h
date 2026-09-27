#pragma once

#include <expected>

#include "EngineAPI.h"
#include "StaticMesh/StaticMeshData.h"
#include "Rendering/PositionVertexBuffer.h"
#include "StaticMesh/LocalVertexFactory.h"

#include "RenderResource.h"

namespace Durin
{
	class FMeshGeometryRecord;
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
				return static_cast<uint32>(GetNormals().size());
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
					&& GetTangents().size() == GetNormals().size()
					&& GetRHI() != nullptr;
			}
			auto GetNormals() const -> std::span<const FVector3f>
			{
				return MeshStreamPrivate::Read(Normals, SharedNormals);
			}
			auto GetTangents() const -> std::span<const FVector4f>
			{
				return MeshStreamPrivate::Read(Tangents, SharedTangents);
			}
			auto SetSharedNormals(FSharedByteBuffer Value) -> bool
			{
				check(!IsInitialized());
				return MeshStreamPrivate::Retain(Normals, SharedNormals, std::move(Value));
			}
			auto GetMutableNormals() -> std::vector<FVector3f>&
			{
				check(!IsInitialized());
				return MeshStreamPrivate::Detach(Normals, SharedNormals);
			}
			auto SetSharedTangents(FSharedByteBuffer Value) -> bool
			{
				check(!IsInitialized());
				return MeshStreamPrivate::Retain(Tangents, SharedTangents, std::move(Value));
			}
			auto GetMutableTangents() -> std::vector<FVector4f>&
			{
				check(!IsInitialized());
				return MeshStreamPrivate::Detach(Tangents, SharedTangents);
			}

		private:
			std::vector<FVector3f> Normals;
			FSharedByteBuffer SharedNormals;
			std::vector<FVector4f> Tangents;
			FSharedByteBuffer SharedTangents;
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
				return GetTexCoords()[0].empty()
					? 0
					: static_cast<uint32>(GetTexCoords()[0].size());
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
				const size_t NumVertices = GetTexCoords()[0].size();
				return NumVertices > 0
					&& std::ranges::all_of(
						GetTexCoords(),
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
				check(VertexIndex < GetTexCoords()[Channel].size());
				return GetTexCoords()[Channel][VertexIndex];
			}
			auto GetTexCoords() const -> std::array<std::span<const FVector2f>, MaxStaticMeshUVChannels>
			{
				std::array<std::span<const FVector2f>, MaxStaticMeshUVChannels> Result;
				for (size_t Index = 0; Index < Result.size(); ++Index)
					Result[Index] = MeshStreamPrivate::Read(TexCoords[Index], SharedTexCoords[Index]);
				return Result;
			}
			auto SetSharedTexCoord(uint32 Channel, FSharedByteBuffer Value) -> bool
			{
				check(!IsInitialized());
				return Channel < MaxStaticMeshUVChannels
					&& MeshStreamPrivate::Retain(TexCoords[Channel], SharedTexCoords[Channel], std::move(Value));
			}
			auto GetMutableTexCoord(uint32 Channel) -> std::vector<FVector2f>&
			{
				check(!IsInitialized() && Channel < MaxStaticMeshUVChannels);
				return MeshStreamPrivate::Detach(TexCoords[Channel], SharedTexCoords[Channel]);
			}
			auto GetMutableTexCoords() -> std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels>&
			{
				for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel) GetMutableTexCoord(Channel);
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
			std::array<FSharedByteBuffer, MaxStaticMeshUVChannels> SharedTexCoords;
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
			return static_cast<uint32>(GetColors().size());
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
			check(VertexIndex < GetColors().size());
			return GetColors()[VertexIndex];
		}
		auto GetColors() const -> std::span<const FVector4f>
		{
			return MeshStreamPrivate::Read(Colors, SharedColors);
		}
		auto SetSharedColors(FSharedByteBuffer Value) -> bool
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Retain(Colors, SharedColors, std::move(Value));
		}
		auto GetMutableColors() -> std::vector<FVector4f>&
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Detach(Colors, SharedColors);
		}

	private:
		std::vector<FVector4f> Colors;
		FSharedByteBuffer SharedColors;
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
			return static_cast<uint32>(GetIndices().size());
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
			check(Index < GetIndices().size());
			return GetIndices()[Index];
		}
		auto GetIndices() const -> std::span<const uint32>
		{
			return MeshStreamPrivate::Read(Indices, SharedIndices);
		}
		auto SetSharedIndices(FSharedByteBuffer Value) -> bool
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Retain(Indices, SharedIndices, std::move(Value));
		}
		auto GetIndicesCapacity() const -> size_t { return MeshStreamPrivate::Capacity(Indices, SharedIndices); }
		auto GetMutableIndices() -> std::vector<uint32>&
		{
			check(!IsInitialized());
			return MeshStreamPrivate::Detach(Indices, SharedIndices);
		}

	private:
		std::vector<uint32> Indices;
		FSharedByteBuffer SharedIndices;
		bool bNeedsCPUAccess = true;
	};

	// Owns named CPU and GPU buffer resources for one LOD.
	struct FStaticMeshLODResources
	{
		struct FRayQueryNode
		{
			FBox Bounds;
			uint32 First = 0;
			uint32 CountOrSecond = 0;
			bool bLeaf = false;
		};

		// Stores one immutable deterministic triangle hierarchy for the matching CPU LOD data.
		struct FRayQueryAcceleration
		{
			std::vector<FRayQueryNode> Nodes;
			std::vector<uint32> TriangleOrdinals;
			uint64 RetainedBytes = 0;
			uint64 BuildNanoseconds = 0;
			uint32 SourceVertexCount = 0;
			uint32 SourceIndexCount = 0;
		};

		FStaticMeshVertexBuffers VertexBuffers;
		FRawStaticIndexBuffer IndexBuffer;
		std::vector<FStaticMeshSection> Sections;
		FBox LocalBounds;
		float ScreenSize = 0.0f;
		uint8 NumTexCoords = 0;
		bool bHasColorVertexData = false;
		std::shared_ptr<const FRayQueryAcceleration> RayQueryAcceleration;
		// Caches aggregate GPU-buffer and matching vertex-factory readiness
		// for render-thread LOD selection.
		bool bReadyForRendering = false;
		std::shared_ptr<const FMeshGeometryRecord> GeometryRecord;

		auto GetNumVertices() const -> uint32
		{
			return VertexBuffers.PositionVertexBuffer.GetNumVertices();
		}
		auto GetNumIndices() const -> uint32
		{
			return IndexBuffer.GetNumIndices();
		}
	};

	inline constexpr uint64 MaximumStaticMeshRayQueryAccelerationBytes = 256ull * 1024ull * 1024ull;

	// Builds a complete immutable hierarchy or returns null so callers can use exact reference traversal.
	ENGINE_API auto BuildStaticMeshRayQueryAcceleration(const FStaticMeshLODResources& LOD,
		const std::function<bool()>& ShouldCancel = {})
		-> std::shared_ptr<const FStaticMeshLODResources::FRayQueryAcceleration>;

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
		// Detached construction only: false leaves partial bounds that must not be published.
		ENGINE_API auto RecalculateBounds(const std::function<bool()>& ShouldCancel) -> bool;
	};

	enum class EStaticMeshLODPolicyError : uint8 { None, Empty, InvalidScreenSize, NotDescending, MissingFinalZero };
	struct FStaticMeshLODPolicyError
	{
		EStaticMeshLODPolicyError Code = EStaticMeshLODPolicyError::None;
		uint64 LODIndex = 0;
		uint64 LODCount = 0;
		float ScreenSize = 0.0f;
		float PreviousScreenSize = 0.0f;
	};

	ENGINE_API auto FormatStaticMeshLODPolicyError(const FStaticMeshLODPolicyError& Error) -> std::string;

	// Validates the published policy: finite, [0, 1], strictly descending, and final zero.
	ENGINE_API auto ValidateStaticMeshLODScreenSizes(
		std::span<const FStaticMeshLODResources> LODResources) -> std::expected<void, FStaticMeshLODPolicyError>;
}
