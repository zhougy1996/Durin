#include "StaticMesh/StaticMesh.h"

#include "AssetCore.h"
#include "Logging/LogMacros.h"
#include "StaticMesh/StaticMeshResources.h"

#include "RHICommandList.h"

namespace Durin
{
	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::GetRenderData() -> FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void
	{
		RenderData = std::move(InRenderData);
	}

	auto DStaticMesh::CreateDebugTriangle() -> std::shared_ptr<DStaticMesh>
	{
		auto Mesh = std::make_shared<DStaticMesh>();
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->Positions = {
			FVector3f(-0.65f, -0.45f, 0.0f),
			FVector3f(0.65f, -0.45f, 0.0f),
			FVector3f(0.0f, 0.65f, 0.0f)
		};
		RenderData->Indices = {0, 1, 2};
		RenderData->IndexCount = static_cast<uint32>(RenderData->Indices.size());
		Mesh->SetRenderData(std::move(RenderData));
		return Mesh;
	}

	auto DStaticMesh::CreateFromFile(std::string_view FilePath) -> std::shared_ptr<DStaticMesh>
	{
		std::vector<Asset::FTestAssetData> ImportedMeshes;
		if (!Asset::ImportFromFile(FilePath, ImportedMeshes))
		{
			DURIN_ERROR("Failed to create static mesh from file: {}", FilePath);
			return nullptr;
		}

		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		for (const Asset::FTestAssetData& ImportedMesh : ImportedMeshes)
		{
			if (ImportedMesh.Positions.empty() || ImportedMesh.Indices.empty())
			{
				continue;
			}

			const uint32 BaseVertexIndex = static_cast<uint32>(RenderData->Positions.size());
			RenderData->Positions.insert(RenderData->Positions.end(), ImportedMesh.Positions.begin(), ImportedMesh.Positions.end());
			RenderData->Indices.reserve(RenderData->Indices.size() + ImportedMesh.Indices.size());
			for (uint32 Index : ImportedMesh.Indices)
			{
				RenderData->Indices.push_back(BaseVertexIndex + Index);
			}
		}

		if (RenderData->Positions.empty() || RenderData->Indices.empty())
		{
			DURIN_ERROR("Imported static mesh has no renderable geometry: {}", FilePath);
			return nullptr;
		}

		FVector3f BoundsMin = RenderData->Positions[0];
		FVector3f BoundsMax = RenderData->Positions[0];
		for (const FVector3f& Position : RenderData->Positions)
		{
			BoundsMin = glm::min(BoundsMin, Position);
			BoundsMax = glm::max(BoundsMax, Position);
		}

		const FVector3f BoundsCenter = (BoundsMin + BoundsMax) * 0.5f;
		const FVector3f BoundsExtent = BoundsMax - BoundsMin;
		const float MaxDimension = std::max(BoundsExtent.x, std::max(BoundsExtent.y, BoundsExtent.z));
		if (MaxDimension <= 0.0f)
		{
			DURIN_ERROR("Imported static mesh has invalid bounds: {}", FilePath);
			return nullptr;
		}

		const float Scale = 1.5f / MaxDimension;
		for (FVector3f& Position : RenderData->Positions)
		{
			Position = (Position - BoundsCenter) * Scale;
		}

		RenderData->IndexCount = static_cast<uint32>(RenderData->Indices.size());

		auto Mesh = std::make_shared<DStaticMesh>();
		Mesh->SetRenderData(std::move(RenderData));
		return Mesh;
	}

	auto FStaticMeshRenderData::InitResources(FRHICommandListImmediate& RHICmdList) -> void
	{
		if (PositionVertexBufferRHI == nullptr && !Positions.empty())
		{
			FRHIBufferCreateDesc VertexBufferDesc = FRHIBufferCreateDesc::CreateVertex(
				"StaticMeshPositionVertexBuffer",
				static_cast<uint32>(Positions.size() * sizeof(FVector3f))
			);
			VertexBufferDesc.Usage |= EBufferUsageFlags::Static;
			VertexBufferDesc.InitialData.Data = Positions.data();
			VertexBufferDesc.InitialData.Size = static_cast<uint32>(Positions.size() * sizeof(FVector3f));
			PositionVertexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, VertexBufferDesc);
		}

		if (IndexBufferRHI == nullptr && !Indices.empty())
		{
			FRHIBufferCreateDesc IndexBufferDesc = FRHIBufferCreateDesc::CreateIndex(
				"StaticMeshIndexBuffer",
				static_cast<uint32>(Indices.size() * sizeof(uint32)),
				sizeof(uint32)
			);
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData.Data = Indices.data();
			IndexBufferDesc.InitialData.Size = static_cast<uint32>(Indices.size() * sizeof(uint32));
			IndexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, IndexBufferDesc);
		}

		IndexCount = static_cast<uint32>(Indices.size());
	}

	auto FStaticMeshRenderData::ReleaseResources() -> void
	{
		PositionVertexBufferRHI = nullptr;
		IndexBufferRHI = nullptr;
	}

	auto FStaticMeshRenderData::IsReadyForRendering() const -> bool
	{
		return PositionVertexBufferRHI != nullptr && IndexBufferRHI != nullptr && IndexCount > 0;
	}
}
