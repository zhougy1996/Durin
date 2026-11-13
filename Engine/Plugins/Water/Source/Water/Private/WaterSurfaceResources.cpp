#include "WaterSurfaceResources.h"

#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	auto MakeWaterGrid(const FWaterSurfaceSettings& S) -> std::unique_ptr<FStaticMeshRenderData>
	{
		if (!ValidateWaterSurface(S)) return {};
		const double Step = S.Wavelength * .73 / 32;
		const uint32 NX = uint32(std::ceil(S.Extent.x / Step)), NY = uint32(std::ceil(S.Extent.y / Step));
		const uint32 Count = (NX + 1) * (NY + 1);
		auto Data = std::make_unique<FStaticMeshRenderData>();
		Data->MaterialSlots.resize(1); Data->MaterialSlots[0].Name = "Water";
		Data->LODResources.resize(1); Data->LODVertexFactories.resize(1);
		std::vector<FVector3f> Positions;
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> UVs;
		for (uint32 Y = 0; Y <= NY; ++Y) for (uint32 X = 0; X <= NX; ++X)
		{
			const FVector2f P(float(S.Extent.x * (double(X) / NX - .5)), float(S.Extent.y * (double(Y) / NY - .5)));
			Positions.emplace_back(P, S.Elevation); UVs[0].push_back(P);
		}
		std::vector<uint32> Indices;
		for (uint32 Y = 0; Y < NY; ++Y) for (uint32 X = 0; X < NX; ++X)
		{
			const uint32 A = Y * (NX + 1) + X, B = A + NX + 1;
			Indices.insert(Indices.end(), {A, A + 1, B + 1, A, B + 1, B});
		}
		auto& LOD = Data->LODResources[0];
		LOD.VertexBuffers.PositionVertexBuffer.Init(std::move(Positions));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<FVector3f>(Count, {0, 0, 1}), std::vector<FVector4f>(Count, {1, 0, 0, 1}));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), Count, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(std::vector<FVector4f>(Count, FVector4f(1)), Count);
		LOD.IndexBuffer.Init(std::move(Indices));
		const double Padding = 1.e-4 + 2.e-7 * std::max({std::abs(double(S.Elevation)), S.Extent.x, S.Extent.y});
		const double H = S.Amplitude * S.Steepness + Padding;
		const FBox Bounds({-S.Extent.x * .5 - H, -S.Extent.y * .5 - H, S.Elevation - S.Amplitude - Padding},
			{S.Extent.x * .5 + H, S.Extent.y * .5 + H, S.Elevation + S.Amplitude + Padding});
		LOD.Sections.push_back({.Name = "Water", .FirstIndex = 0, .IndexCount = NX * NY * 6,
			.MinVertexIndex = 0, .MaxVertexIndex = Count - 1, .MaterialSlotIndex = 0, .LocalBounds = Bounds});
		LOD.NumTexCoords = 1; LOD.bHasColorVertexData = true; LOD.LocalBounds = Bounds; Data->LocalBounds = Bounds;
		return Data;
	}

}
