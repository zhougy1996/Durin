#include "StaticMesh/StaticMeshResources.h"

#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "RHI.h"

namespace Durin
{
	auto PackStaticMeshTangentBasis(
		const FVector3f& Normal,
		const FVector4f& Tangent) -> FStaticMeshPackedTangentBasis
	{
		auto PackSnorm = [](float Value) -> int16 {
			return static_cast<int16>(
				std::lround(std::clamp(Value, -1.0f, 1.0f) * 32767.0f));
		};
		FStaticMeshPackedTangentBasis Result;
		Result.Normal = {
			PackSnorm(Normal.x), PackSnorm(Normal.y), PackSnorm(Normal.z), 0};
		Result.Tangent = {
			PackSnorm(Tangent.x), PackSnorm(Tangent.y),
			PackSnorm(Tangent.z), PackSnorm(Tangent.w)};
		return Result;
	}

	auto PackStaticMeshColor(const FVector4f& Color) -> FStaticMeshColorVertex
	{
		auto PackUnorm = [](float Value) -> uint8 {
			return static_cast<uint8>(
				std::lround(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
		};
		FStaticMeshColorVertex Result;
		Result.Color = {
			PackUnorm(Color.r), PackUnorm(Color.g),
			PackUnorm(Color.b), PackUnorm(Color.a)};
		return Result;
	}
	auto FStaticMeshVertexBuffer::FTangentsVertexBuffer::Init(
		std::vector<FVector3f> InNormals,
		std::vector<FVector4f> InTangents,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		Normals = std::move(InNormals);
		Tangents = std::move(InTangents);
		bNeedsCPUAccess = bInNeedsCPUAccess;
	}

	auto FStaticMeshVertexBuffer::FTangentsVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		if (Normals.empty()
			|| Tangents.size() != Normals.size()
			|| GetRHI() != nullptr)
		{
			return;
		}

		std::vector<FStaticMeshPackedTangentBasis> PackedTangents(
			Normals.size());
		for (size_t VertexIndex = 0;
			VertexIndex < Normals.size();
			++VertexIndex)
		{
			PackedTangents[VertexIndex] = PackStaticMeshTangentBasis(
				Normals[VertexIndex],
				Tangents[VertexIndex]);
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshTangentsVertexBuffer",
			static_cast<uint32>(
				PackedTangents.size()
				* sizeof(PackedTangents.front())));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = PackedTangents.data();
		Desc.InitialData.Size = static_cast<uint32>(
			PackedTangents.size() * sizeof(PackedTangents.front()));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	auto FStaticMeshVertexBuffer::FTexcoordVertexBuffer::Init(
		std::array<
			std::vector<FVector2f>,
			MaxStaticMeshUVChannels> InTexCoords,
		uint32 NumVertices,
		uint8 InNumTexCoords,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		TexCoords = std::move(InTexCoords);
		NumTexCoords = InNumTexCoords;
		bNeedsCPUAccess = bInNeedsCPUAccess;
		for (auto& Channel : TexCoords)
		{
			if (Channel.empty())
			{
				Channel.assign(NumVertices, FVector2f(0.0f));
			}
		}
	}

	auto FStaticMeshVertexBuffer::FTexcoordVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		const size_t NumVertices = TexCoords[0].size();
		if (NumVertices == 0
			|| !std::ranges::all_of(
				TexCoords,
				[NumVertices](const auto& Channel) {
					return Channel.size() == NumVertices;
				})
			|| GetRHI() != nullptr)
		{
			return;
		}

		std::vector<FStaticMeshTexcoordVertex>
			InterleavedTexCoords(NumVertices);
		for (size_t VertexIndex = 0;
			VertexIndex < NumVertices;
			++VertexIndex)
		{
			for (uint32 Channel = 0;
				Channel < MaxStaticMeshUVChannels;
				++Channel)
			{
				InterleavedTexCoords[VertexIndex].TexCoords[Channel] =
					TexCoords[Channel][VertexIndex];
			}
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshTexCoordVertexBuffer",
			static_cast<uint32>(
				InterleavedTexCoords.size()
				* sizeof(InterleavedTexCoords.front())));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = InterleavedTexCoords.data();
		Desc.InitialData.Size = static_cast<uint32>(
			InterleavedTexCoords.size()
				* sizeof(InterleavedTexCoords.front()));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	auto FColorVertexBuffer::Init(
		std::vector<FVector4f> InColors,
		uint32 NumVertices,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		Colors = std::move(InColors);
		if (Colors.empty())
		{
			Colors.assign(NumVertices, FVector4f(1.0f));
		}
		bNeedsCPUAccess = bInNeedsCPUAccess;
	}

	auto FColorVertexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		if (Colors.empty() || GetRHI() != nullptr) return;
		std::vector<FStaticMeshColorVertex> PackedColors(Colors.size());
		for (size_t VertexIndex = 0;
			VertexIndex < Colors.size();
			++VertexIndex)
		{
			PackedColors[VertexIndex] =
				PackStaticMeshColor(Colors[VertexIndex]);
		}
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshColorVertexBuffer",
			static_cast<uint32>(
				PackedColors.size() * sizeof(PackedColors.front())));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = PackedColors.data();
		Desc.InitialData.Size = static_cast<uint32>(
			PackedColors.size() * sizeof(PackedColors.front()));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	auto FStaticMeshVertexBuffers::Finalize(
		uint8 NumTexCoords,
		bool bHasColorVertexData) -> void
	{
		const uint32 NumVertices =
			PositionVertexBuffer.GetNumVertices();
		auto& TexCoords =
			StaticMeshVertexBuffer.TexCoordVertexBuffer
				.GetMutableTexCoords();
		for (auto& Channel : TexCoords)
		{
			if (Channel.empty())
			{
				Channel.assign(NumVertices, FVector2f(0.0f));
			}
		}
		StaticMeshVertexBuffer.TexCoordVertexBuffer.SetNumTexCoords(
			NumTexCoords);
		auto& Colors = ColorVertexBuffer.GetMutableColors();
		if (Colors.empty() && !bHasColorVertexData)
		{
			Colors.assign(NumVertices, FVector4f(1.0f));
		}
	}

	namespace
	{
		template<typename ResourceType>
		auto InitStaticMeshResource(
			ResourceType& Resource,
			FRHICommandListBase& RHICmdList) -> void
		{
			if (!Resource.IsInitialized())
			{
				Resource.InitResource(RHICmdList);
			}
			else if constexpr (requires { Resource.GetRHI(); })
			{
				if (Resource.GetRHI() == nullptr)
				{
					Resource.UpdateRHI(RHICmdList);
				}
			}
			else if (!Resource.IsReady())
			{
				Resource.UpdateRHI(RHICmdList);
			}
		}

		auto ReleaseStaticMeshResource(
			FRenderResource& Resource) -> void
		{
			if (Resource.IsInitialized())
			{
				Resource.ReleaseResource();
			}
			else
			{
				Resource.ReleaseRHI();
			}
		}
	}

	auto FStaticMeshVertexBuffers::InitResources(
		FRHICommandListBase& RHICmdList) -> void
	{
		InitStaticMeshResource(PositionVertexBuffer, RHICmdList);
		InitStaticMeshResource(
			StaticMeshVertexBuffer.TangentsVertexBuffer,
			RHICmdList);
		InitStaticMeshResource(
			StaticMeshVertexBuffer.TexCoordVertexBuffer,
			RHICmdList);
		InitStaticMeshResource(ColorVertexBuffer, RHICmdList);
	}

	auto FStaticMeshVertexBuffers::ReleaseResources() -> void
	{
		ReleaseStaticMeshResource(ColorVertexBuffer);
		ReleaseStaticMeshResource(
			StaticMeshVertexBuffer.TexCoordVertexBuffer);
		ReleaseStaticMeshResource(
			StaticMeshVertexBuffer.TangentsVertexBuffer);
		ReleaseStaticMeshResource(PositionVertexBuffer);
	}

	auto FRawStaticIndexBuffer::Init(
		std::vector<uint32> InIndices,
		bool bInNeedsCPUAccess) -> void
	{
		check(!IsInitialized());
		Indices = std::move(InIndices);
		bNeedsCPUAccess = bInNeedsCPUAccess;
	}

	auto FRawStaticIndexBuffer::InitRHI(
		FRHICommandListBase& RHICmdList) -> void
	{
		if (Indices.empty() || GetRHI() != nullptr) return;
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateIndex(
			"StaticMeshIndexBuffer",
			static_cast<uint32>(Indices.size() * sizeof(uint32)),
			sizeof(uint32));
		Desc.Usage |= EBufferUsageFlags::Static;
		Desc.InitialData.Data = Indices.data();
		Desc.InitialData.Size =
			static_cast<uint32>(Indices.size() * sizeof(uint32));
		SetRHI(GDynamicRHI->RHICreateBuffer(
			static_cast<FRHICommandListImmediate&>(RHICmdList),
			Desc));
	}

	namespace
	{
		auto IsStaticMeshLODGeometryValid(
			const FStaticMeshLODResources& LOD,
			size_t MaterialSlotCount) -> bool
		{
			const auto& Positions =
				LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
			const auto& TangentsVertexBuffer =
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer;
			const auto& Normals =
				TangentsVertexBuffer.GetNormals();
			const auto& Tangents =
				TangentsVertexBuffer.GetTangents();
			const auto& TexCoords =
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer.GetTexCoords();
			const auto& Colors =
				LOD.VertexBuffers.ColorVertexBuffer.GetColors();
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			const size_t NumVertices = Positions.size();
			const bool bValidStreams = NumVertices > 0
				&& Normals.size() == NumVertices
				&& Tangents.size() == NumVertices
				&& Colors.size() == NumVertices
				&& std::ranges::all_of(
					TexCoords,
					[NumVertices](const auto& Channel) {
						return Channel.size() == NumVertices;
					});
			const bool bValidIndices =
				!Indices.empty()
				&& Indices.size() % 3 == 0
				&& std::ranges::all_of(
					Indices,
					[NumVertices](uint32 Index) {
						return Index < NumVertices;
					});
			const bool bValidSections =
				!LOD.Sections.empty()
				&& std::ranges::all_of(
					LOD.Sections,
					[&Indices, &Positions, MaterialSlotCount](
						const FStaticMeshSection& Section) {
						return Section.IndexCount > 0
							&& Section.IndexCount % 3 == 0
							&& static_cast<uint64>(Section.FirstIndex)
								+ Section.IndexCount
								<= Indices.size()
							&& Section.MinVertexIndex
								<= Section.MaxVertexIndex
							&& Section.MaxVertexIndex
								< Positions.size()
							&& Section.MaterialSlotIndex
								< MaterialSlotCount;
					});
			return bValidStreams && bValidIndices && bValidSections;
		}
	}

	auto FStaticMeshRenderData::InitResources(FRHICommandListImmediate& RHICmdList) -> bool
	{
		check(IsInRenderingThread());
		if (LODVertexFactories.empty())
		{
			LODVertexFactories.resize(LODResources.size());
		}
		if (LODResources.empty()
			|| LODVertexFactories.size() != LODResources.size())
		{
			ReleaseResources();
			return false;
		}
		std::string LODPolicyError;
		if (!ValidateStaticMeshLODScreenSizes(
			LODResources, LODPolicyError))
		{
			ReleaseResources();
			return false;
		}

		for (FStaticMeshLODResources& LOD : LODResources)
		{
			if (!LOD.VertexBuffers.PositionVertexBuffer.IsInitialized()
				&& !LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer.IsInitialized()
				&& !LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer.IsInitialized()
				&& !LOD.VertexBuffers.ColorVertexBuffer.IsInitialized()
				&& !LOD.IndexBuffer.IsInitialized())
			{
				LOD.VertexBuffers.Finalize(
					LOD.NumTexCoords,
					LOD.bHasColorVertexData);
			}
			if (!IsStaticMeshLODGeometryValid(
				LOD, MaterialSlots.size()))
			{
				ReleaseResources();
				return false;
			}
		}
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			LOD.VertexBuffers.InitResources(RHICmdList);
			InitStaticMeshResource(LOD.IndexBuffer, RHICmdList);
		}
		if (!std::ranges::all_of(
				LODResources,
				[this](const FStaticMeshLODResources& LOD) {
					return LOD.VertexBuffers.IsReady()
						&& LOD.IndexBuffer.IsReady()
						&& IsStaticMeshLODGeometryValid(
							LOD, MaterialSlots.size());
				}))
		{
			ReleaseResources();
			return false;
		}
		for (size_t LODIndex = 0;
			LODIndex < LODResources.size();
			++LODIndex)
		{
			FLocalVertexFactory& VertexFactory =
				LODVertexFactories[LODIndex].VertexFactory;
			if (!VertexFactory.IsInitialized()
				&& !VertexFactory.SetData(
					LODResources[LODIndex].VertexBuffers))
			{
				ReleaseResources();
				return false;
			}
			InitStaticMeshResource(VertexFactory, RHICmdList);
		}
		if (!std::ranges::all_of(
				LODVertexFactories,
				[](const FStaticMeshVertexFactories& Factories) {
					return Factories.VertexFactory.IsReady();
				}))
		{
			ReleaseResources();
			return false;
		}
		return true;
	}

	auto FStaticMeshRenderData::ReleaseResources() -> void
	{
		check(IsInRenderingThread());
		for (FStaticMeshVertexFactories& Factories
			: LODVertexFactories | std::views::reverse)
		{
			ReleaseStaticMeshResource(Factories.VertexFactory);
		}
		for (FStaticMeshLODResources& LOD
			: LODResources | std::views::reverse)
		{
			ReleaseStaticMeshResource(LOD.IndexBuffer);
			LOD.VertexBuffers.ReleaseResources();
		}
	}

#if DURIN_BUILD_DEBUG
	auto FStaticMeshRenderData::SetResourceDebugOwner(FName InOwner) -> void
	{
		if (LODVertexFactories.empty())
		{
			LODVertexFactories.resize(LODResources.size());
		}
		auto SetOwner = [InOwner](FRenderResource& Resource) {
			Resource.SetDebugOwner(InOwner);
		};
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			SetOwner(LOD.VertexBuffers.PositionVertexBuffer);
			SetOwner(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer);
			SetOwner(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer);
			SetOwner(LOD.VertexBuffers.ColorVertexBuffer);
			SetOwner(LOD.IndexBuffer);
		}
		for (FStaticMeshVertexFactories& Factories : LODVertexFactories)
		{
			SetOwner(Factories.VertexFactory);
		}
	}
#endif

	auto FStaticMeshRenderData::GetNumInitializedResources() const -> size_t
	{
		size_t Count = 0;
		auto CountResource = [&Count](const FRenderResource& Resource) {
			if (Resource.IsInitialized()) ++Count;
		};
		for (const FStaticMeshLODResources& LOD : LODResources)
		{
			CountResource(LOD.VertexBuffers.PositionVertexBuffer);
			CountResource(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TangentsVertexBuffer);
			CountResource(
				LOD.VertexBuffers.StaticMeshVertexBuffer
					.TexCoordVertexBuffer);
			CountResource(LOD.VertexBuffers.ColorVertexBuffer);
			CountResource(LOD.IndexBuffer);
		}
		for (const FStaticMeshVertexFactories& Factories
			: LODVertexFactories)
		{
			CountResource(Factories.VertexFactory);
		}
		return Count;
	}

	auto FStaticMeshRenderData::IsReadyForRendering(uint32 LODIndex) const -> bool
	{
		if (LODVertexFactories.size() != LODResources.size()
			|| LODIndex >= LODResources.size())
		{
			return false;
		}
		const FStaticMeshLODResources& LOD = LODResources[LODIndex];
		return LOD.VertexBuffers.IsReady()
			&& LOD.IndexBuffer.IsReady()
			&& LODVertexFactories[LODIndex].VertexFactory.IsReady()
			&& IsStaticMeshLODGeometryValid(
				LOD, MaterialSlots.size());
	}

	auto FStaticMeshRenderData::RecalculateBounds() -> void
	{
		LocalBounds.Reset();
		for (FStaticMeshLODResources& LOD : LODResources)
		{
			const auto& Positions =
				LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			LOD.LocalBounds.Reset();
			for (const FVector3f& Position : Positions)
			{
				LOD.LocalBounds.AddPoint(FVector3(Position));
			}
			for (FStaticMeshSection& Section : LOD.Sections)
			{
				Section.LocalBounds.Reset();
				const uint64 EndIndex = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
				if (EndIndex > Indices.size()) continue;
				for (uint32 IndexOffset = 0; IndexOffset < Section.IndexCount; ++IndexOffset)
				{
					const uint32 VertexIndex =
						Indices[Section.FirstIndex + IndexOffset];
					if (VertexIndex < Positions.size())
					{
						Section.LocalBounds.AddPoint(
							FVector3(Positions[VertexIndex]));
					}
				}
			}
			for (const FVector3f& Position : Positions)
			{
				LocalBounds.AddPoint(FVector3(Position));
			}
		}
	}

	auto GenerateDefaultStaticMeshLODScreenSizes(
		uint32 LODCount) -> std::vector<float>
	{
		std::vector<float> Result(LODCount, 0.0f);
		for (uint32 LODIndex = 0; LODIndex + 1 < LODCount; ++LODIndex)
		{
			Result[LODIndex] = std::ldexp(
				1.0f, -static_cast<int>(LODIndex + 1));
		}
		return Result;
	}

	auto ValidateStaticMeshLODScreenSizes(
		std::span<const FStaticMeshLODResources> LODResources,
		std::string& OutError) -> bool
	{
		if (LODResources.empty())
		{
			OutError = "Static-mesh LOD policy requires at least one LOD.";
			return false;
		}
		for (size_t LODIndex = 0; LODIndex < LODResources.size(); ++LODIndex)
		{
			const float ScreenSize = LODResources[LODIndex].ScreenSize;
			if (!std::isfinite(ScreenSize)
				|| ScreenSize < 0.0f || ScreenSize > 1.0f
				|| (ScreenSize == 0.0f && std::signbit(ScreenSize)))
			{
				OutError = std::format(
					"Static-mesh LOD {} screen size must be finite and in [0, 1].",
					LODIndex);
				return false;
			}
			if (LODIndex > 0
				&& ScreenSize >= LODResources[LODIndex - 1].ScreenSize)
			{
				OutError = "Static-mesh LOD screen sizes must be strictly descending.";
				return false;
			}
		}
		if (LODResources.back().ScreenSize != 0.0f)
		{
			OutError = "Static-mesh lowest-detail LOD screen size must be exactly zero.";
			return false;
		}
		OutError.clear();
		return true;
	}
}
