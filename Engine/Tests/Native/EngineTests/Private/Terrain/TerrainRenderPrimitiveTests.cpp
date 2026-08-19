#include "Actors/TerrainActor.h"
#include "Components/TerrainComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"
#include "Engine/TerrainSceneProxy.h"
#include "Materials/Material.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainLOD.h"
#include "Terrain/TerrainTopology.h"

#include <gtest/gtest.h>

#include <set>

namespace
{
	auto MakeTerrainPerspectiveProjection(double NearClip, double FarClip) -> Durin::FMatrix
	{
		Durin::FMatrix Projection(0.0);
		Projection[1][0] = 1.0;
		Projection[2][1] = -1.0;
		Projection[0][2] = FarClip / (FarClip - NearClip);
		Projection[3][2] = -NearClip * FarClip / (FarClip - NearClip);
		Projection[0][3] = 1.0;
		return Projection;
	}

	auto MakeTerrainOrthographicProjection(
		double HalfWidth, double HalfHeight, double NearClip, double FarClip) -> Durin::FMatrix
	{
		Durin::FMatrix Projection(0.0);
		Projection[1][0] = 1.0 / HalfWidth;
		Projection[2][1] = -1.0 / HalfHeight;
		Projection[0][2] = 1.0 / (FarClip - NearClip);
		Projection[3][2] = -NearClip / (FarClip - NearClip);
		Projection[3][3] = 1.0;
		return Projection;
	}

	TEST(TerrainRenderPrimitive, ReflectedActorOwnsTerrainComponent)
	{
		auto* Actor = Durin::NewObject<Durin::ATerrainActor>(nullptr, "TerrainActorContract");
		ASSERT_NE(Actor, nullptr);
		ASSERT_NE(Actor->GetTerrainComponent(), nullptr);
		EXPECT_EQ(Actor->GetRootComponent(), Actor->GetTerrainComponent());
		EXPECT_NE(Durin::ATerrainActor::StaticClass(), Durin::AActor::StaticClass());
	}

	TEST(TerrainRenderPrimitive, RejectsInvalidComponentParameters)
	{
		auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "TerrainValidation");
		ASSERT_NE(Component, nullptr);
		EXPECT_FALSE(Component->SetSampleSpacing(0.0, 1.0));
		EXPECT_FALSE(Component->SetSampleSpacing(1.0, std::numeric_limits<double>::infinity()));
		EXPECT_FALSE(Component->SetHeightRange(std::numeric_limits<double>::quiet_NaN(), 0.0));
		EXPECT_TRUE(Component->SetSampleSpacing(2.0, 3.0));
		EXPECT_TRUE(Component->SetHeightRange(-100.0, 25.0));
	}

	TEST(TerrainRenderPrimitive, ExposesSingleMaterialSlotThroughMeshContract)
	{
		auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "TerrainMaterialContract");
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "TerrainMaterialContractValue");
		Durin::DMeshComponent* MeshComponent = Component;
		ASSERT_NE(Material, nullptr);
		EXPECT_EQ(MeshComponent->GetNumMaterials(), 1u);
		EXPECT_FALSE(MeshComponent->SetMaterial(1, Material));
		EXPECT_TRUE(MeshComponent->SetMaterial(0, Material));
		EXPECT_EQ(MeshComponent->GetMaterial(0), Material);
		EXPECT_TRUE(MeshComponent->SetMaterial(0, nullptr));
		EXPECT_EQ(MeshComponent->GetMaterial(0), nullptr);
	}

	TEST(TerrainRenderPrimitive, BuildsExactYMajorEdgePatchesAndBounds)
	{
		constexpr Durin::uint32 Width = 130;
		constexpr Durin::uint32 Height = 70;
		std::vector<Durin::uint16> Samples(static_cast<size_t>(Width) * Height, 0);
		Samples[0] = 65535;
		auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(nullptr, "TerrainPatchHeightmap");
		std::string Error;
		ASSERT_TRUE(Heightmap->InitializeFromSamples(Width, Height, Samples, Error)) << Error;
		auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "TerrainPatchComponent");
		Component->SetHeightmap(Heightmap);
		ASSERT_TRUE(Component->SetSampleSpacing(2.0, 3.0));
		ASSERT_TRUE(Component->SetHeightRange(-100.0, 25.0));
		std::unique_ptr<Durin::FPrimitiveSceneProxy> Base = Component->CreateSceneProxy();
		ASSERT_NE(Base, nullptr);
		ASSERT_EQ(Base->GetKind(), Durin::EPrimitiveSceneProxyKind::Terrain);
		auto& Proxy = static_cast<Durin::FTerrainSceneProxy&>(*Base);
		ASSERT_EQ(Proxy.GetPatches().size(), 6u);
		EXPECT_EQ(Proxy.GetPatches()[0].OriginX, 0u);
		EXPECT_EQ(Proxy.GetPatches()[1].OriginX, 64u);
		EXPECT_EQ(Proxy.GetPatches()[2].OriginX, 128u);
		EXPECT_EQ(Proxy.GetPatches()[3].OriginY, 64u);
		EXPECT_EQ(Proxy.GetPatches()[2].CellCountX, 1u);
		EXPECT_EQ(Proxy.GetPatches()[3].CellCountY, 5u);
		EXPECT_EQ(Proxy.GetPatches()[0].GridX, 0u);
		EXPECT_EQ(Proxy.GetPatches()[3].GridY, 1u);
		EXPECT_EQ(Proxy.GetPatches()[0].LODSteps,
			(std::vector<Durin::uint32>{1, 2, 4, 8, 16, 32, 64}));
		EXPECT_EQ(Proxy.GetPatches()[2].LODSteps,
			(std::vector<Durin::uint32>{1}));
		EXPECT_LE(Proxy.GetLODMetadataBytes(), 64u * 1024u);
		const Durin::FBox Bounds = Proxy.GetLocalBounds();
		EXPECT_DOUBLE_EQ(Bounds.Min.x, 0.0);
		EXPECT_DOUBLE_EQ(Bounds.Max.x, 258.0);
		EXPECT_DOUBLE_EQ(Bounds.Max.y, 207.0);
		EXPECT_DOUBLE_EQ(Bounds.Min.z, -75.0);
		EXPECT_DOUBLE_EQ(Bounds.Max.z, 25.0);
	}

	TEST(TerrainRenderPrimitive, BuildsConservativeMonotonicPatchErrors)
	{
		constexpr Durin::uint32 Size = 65;
		std::vector<Durin::uint16> Samples(Size * Size, 0);
		Samples[32u * Size + 32u] = 65535;
		auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(nullptr, "TerrainLODErrorHeightmap");
		std::string Error;
		ASSERT_TRUE(Heightmap->InitializeFromSamples(Size, Size, Samples, Error)) << Error;
		auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "TerrainLODErrorComponent");
		Component->SetHeightmap(Heightmap);
		ASSERT_TRUE(Component->SetHeightRange(100.0, -25.0));
		auto Base = Component->CreateSceneProxy();
		ASSERT_NE(Base, nullptr);
		const auto& Patch = static_cast<Durin::FTerrainSceneProxy&>(*Base).GetPatches().front();
		ASSERT_EQ(Patch.LODSteps.size(), Patch.LODErrors.size());
		EXPECT_DOUBLE_EQ(Patch.LODErrors.front(), 0.0);
		EXPECT_TRUE(std::ranges::is_sorted(Patch.LODErrors));
		EXPECT_GT(Patch.LODErrors.back(), 0.0);
	}

	TEST(TerrainRenderPrimitive, ReusesDerivedPatchesAndInvalidatesChangedParameters)
	{
		constexpr Durin::uint32 Size = 65;
		std::vector<Durin::uint16> Samples(Size * Size, 0);
		Samples.back() = 65'535;
		auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(nullptr,
			"TerrainDerivedCacheHeightmap");
		std::string Error;
		ASSERT_TRUE(Heightmap->InitializeFromSamples(Size, Size, Samples, Error)) << Error;
		auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr,
			"TerrainDerivedCacheComponent");
		Component->SetHeightmap(Heightmap);
		ASSERT_TRUE(Component->SetSampleSpacing(2.0, 3.0));
		ASSERT_TRUE(Component->SetHeightRange(100.0, 10.0));

		auto First = Component->CreateSceneProxy();
		ASSERT_NE(First, nullptr);
		EXPECT_EQ(Component->GetRenderDerivedDataBuildCount(), 1u);
		auto Second = Component->CreateSceneProxy();
		ASSERT_NE(Second, nullptr);
		EXPECT_EQ(Component->GetRenderDerivedDataBuildCount(), 1u);
		EXPECT_EQ(static_cast<Durin::FTerrainSceneProxy&>(*First).GetPatches().size(),
			static_cast<Durin::FTerrainSceneProxy&>(*Second).GetPatches().size());
#if DURIN_WITH_EDITOR
		Durin::FBox PickingBounds;
		Durin::EEditorPickingPrimitiveFamily Family{};
		ASSERT_TRUE(Component->GetEditorPickingLocalBounds(PickingBounds, Family));
		EXPECT_EQ(Component->GetRenderDerivedDataBuildCount(), 1u);
		EXPECT_EQ(Family, Durin::EEditorPickingPrimitiveFamily::Terrain);
		EXPECT_EQ(PickingBounds.Min, First->GetLocalBounds().Min);
		EXPECT_EQ(PickingBounds.Max, First->GetLocalBounds().Max);
#endif

		ASSERT_TRUE(Component->SetHeightRange(200.0, -50.0));
		auto Changed = Component->CreateSceneProxy();
		ASSERT_NE(Changed, nullptr);
		EXPECT_EQ(Component->GetRenderDerivedDataBuildCount(), 2u);
		EXPECT_NE(Changed->GetLocalBounds().Min.z, First->GetLocalBounds().Min.z);
		EXPECT_NE(Changed->GetLocalBounds().Max.z, First->GetLocalBounds().Max.z);
	}

	TEST(TerrainRenderPrimitive, ResolvesAdjacencyByStableCoarsePromotion)
	{
		std::vector<Durin::FTerrainPatchDescriptor> Patches(3);
		for (Durin::uint16 X = 0; X < 3; ++X)
		{
			Patches[X].GridX = X;
			Patches[X].CellCountX = 64;
			Patches[X].CellCountY = 64;
			Patches[X].LODSteps = {1, 2, 4, 8};
			Patches[X].LODErrors = {0.0, 1.0, 2.0, 4.0};
		}
		const std::array<Durin::uint32, 3> Requested{0, 3, 3};
		const auto Result = Durin::ResolveTerrainPatchAdjacency(Patches, Requested);
		ASSERT_TRUE(Result.bValid);
		EXPECT_EQ(Result.ResolvedLODs, (std::vector<Durin::uint32>{0, 1, 2}));
		EXPECT_EQ(Result.Promotions, 3u);
		EXPECT_EQ(Result.StitchMasks[0],
			static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::East));
		EXPECT_EQ(Result.StitchMasks[1],
			static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::East));
		EXPECT_EQ(Result.StitchMasks[2], 0u);
	}

	TEST(TerrainRenderPrimitive, SelectsFlatCoarsestAndHonorsForceLOD0)
	{
		Durin::FTerrainPatchDescriptor Patch;
		Patch.CellCountX = 64;
		Patch.CellCountY = 64;
		Patch.LODSteps = {1, 2, 4, 8, 16, 32, 64};
		Patch.LODErrors.assign(Patch.LODSteps.size(), 0.0);
		Patch.LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {64.0, 64.0, 0.0});
		Durin::FSceneView View;
		View.ViewportWidth = 128;
		View.ViewportHeight = 128;
		EXPECT_EQ(Durin::SelectTerrainPatchLOD(View, Durin::FMatrix(1.0), Patch).LODIndex, 6u);
		View.Settings.Mode.LODMode = Durin::EViewLODMode::ForceLOD0;
		EXPECT_EQ(Durin::SelectTerrainPatchLOD(View, Durin::FMatrix(1.0), Patch).LODIndex, 0u);
	}

	TEST(TerrainRenderPrimitive, SelectsPerspectiveAndOrthographicErrorWithFineEquality)
	{
		Durin::FTerrainPatchDescriptor Patch;
		Patch.CellCountX = 2;
		Patch.CellCountY = 2;
		Patch.LODSteps = {1, 2};
		Patch.LODErrors = {0.0, 5.0 / 64.0};
		Patch.LocalBounds = Durin::FBox({5.0, -1.0, 0.0}, {5.0, 1.0, 0.0});
		Durin::FSceneView View;
		View.ProjectionMatrix = MakeTerrainPerspectiveProjection(1.0, 200.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		View.ViewportWidth = 128;
		View.ViewportHeight = 128;
		EXPECT_EQ(Durin::SelectTerrainPatchLOD(View, Durin::FMatrix(1.0), Patch).LODIndex, 0u);
		Patch.LODErrors[1] = 0.07;
		EXPECT_EQ(Durin::SelectTerrainPatchLOD(View, Durin::FMatrix(1.0), Patch).LODIndex, 1u);

		Patch.LODErrors[1] = 1.0;
		View.ProjectionMatrix = MakeTerrainOrthographicProjection(64.0, 64.0, 1.0, 200.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix;
		EXPECT_EQ(Durin::SelectTerrainPatchLOD(View, Durin::FMatrix(1.0), Patch).LODIndex, 0u);
		Patch.LODErrors[1] = 0.9;
		EXPECT_EQ(Durin::SelectTerrainPatchLOD(View, Durin::FMatrix(1.0), Patch).LODIndex, 1u);
		View.ViewportWidth = 0;
		const auto Fallback = Durin::SelectTerrainPatchLOD(View, Durin::FMatrix(1.0), Patch);
		EXPECT_EQ(Fallback.LODIndex, 0u);
		EXPECT_TRUE(Fallback.bFallback);
	}

	TEST(TerrainRenderPrimitive, GeneratesAllStitchMasksWithoutInvalidTriangles)
	{
		for (Durin::uint16 Step : {1u, 2u, 4u, 8u, 16u, 32u})
		for (Durin::uint8 Mask = 0; Mask < 16; ++Mask)
		{
			const Durin::FTerrainTopologyKey Key{64, 64, Step, Mask};
			Durin::FTerrainTopologyData Data;
			ASSERT_TRUE(Durin::BuildTerrainTopology(Key, Data))
				<< "step=" << Step << " mask=" << static_cast<int>(Mask);
			ASSERT_EQ(Data.Indices.size() / 3,
				Durin::GetTerrainTopologyTriangleCount(Key));
			Durin::uint64 TotalDoubleArea = 0;
			for (size_t Index = 0; Index < Data.Indices.size(); Index += 3)
			{
				const auto& A = Data.Vertices[Data.Indices[Index]];
				const auto& B = Data.Vertices[Data.Indices[Index + 1]];
				const auto& C = Data.Vertices[Data.Indices[Index + 2]];
				const int Area = (static_cast<int>(B[0]) - A[0])
					* (static_cast<int>(C[1]) - A[1])
					- (static_cast<int>(B[1]) - A[1])
					* (static_cast<int>(C[0]) - A[0]);
				EXPECT_GT(Area, 0);
				TotalDoubleArea += static_cast<Durin::uint64>(Area);
				EXPECT_LE(A[0], 64u); EXPECT_LE(A[1], 64u);
				EXPECT_LE(B[0], 64u); EXPECT_LE(B[1], 64u);
				EXPECT_LE(C[0], 64u); EXPECT_LE(C[1], 64u);
			}
			EXPECT_EQ(TotalDoubleArea, 2u * 64u * 64u)
				<< "step=" << Step << " mask=" << static_cast<int>(Mask);
			auto CheckEdge = [&](Durin::ETerrainStitchEdge Edge) {
				std::set<std::pair<Durin::uint16, Durin::uint16>> Segments;
				for (size_t Index = 0; Index < Data.Indices.size(); Index += 3)
					for (size_t Side = 0; Side < 3; ++Side)
					{
						const auto& A = Data.Vertices[Data.Indices[Index + Side]];
						const auto& B = Data.Vertices[Data.Indices[Index + (Side + 1) % 3]];
						const bool Horizontal = Edge == Durin::ETerrainStitchEdge::North
							|| Edge == Durin::ETerrainStitchEdge::South;
						const Durin::uint16 Boundary = (Edge == Durin::ETerrainStitchEdge::South
							|| Edge == Durin::ETerrainStitchEdge::East) ? 64 : 0;
						if ((Horizontal && A[1] == Boundary && B[1] == Boundary && A[0] != B[0])
							|| (!Horizontal && A[0] == Boundary && B[0] == Boundary && A[1] != B[1]))
						{
							const Durin::uint16 V0 = Horizontal ? A[0] : A[1];
							const Durin::uint16 V1 = Horizontal ? B[0] : B[1];
							Segments.emplace(std::min(V0, V1), std::max(V0, V1));
						}
					}
				const bool Stitched = (Mask & static_cast<Durin::uint8>(Edge)) != 0;
				ASSERT_EQ(Segments.size(), 64u / (Step * (Stitched ? 2u : 1u)));
				Durin::uint16 Cursor = 0;
				for (const auto& Segment : Segments)
				{
					EXPECT_EQ(Segment.first, Cursor);
					EXPECT_EQ(Segment.second - Segment.first, Step * (Stitched ? 2u : 1u));
					Cursor = Segment.second;
				}
				EXPECT_EQ(Cursor, 64u);
			};
			CheckEdge(Durin::ETerrainStitchEdge::North);
			CheckEdge(Durin::ETerrainStitchEdge::East);
			CheckEdge(Durin::ETerrainStitchEdge::South);
			CheckEdge(Durin::ETerrainStitchEdge::West);
		}
		Durin::FTerrainTopologyData Coarsest;
		EXPECT_TRUE(Durin::BuildTerrainTopology({64, 64, 64, 0}, Coarsest));
		Durin::FTerrainTopologyData Partial;
		EXPECT_TRUE(Durin::BuildTerrainTopology({6, 8, 2,
			static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::East)}, Partial));
		EXPECT_FALSE(Durin::BuildTerrainTopology({6, 8, 2,
			static_cast<Durin::uint8>(Durin::ETerrainStitchEdge::North)}, Partial));
	}

	TEST(TerrainRenderPrimitive, ProxyRetainsExactPayloadRevision)
	{
		const std::array<Durin::uint16, 4> Samples{0, 1, 32768, 65535};
		auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(nullptr, "TerrainRevisionHeightmap");
		std::string Error;
		ASSERT_TRUE(Heightmap->InitializeFromSamples(2, 2, Samples, Error));
		auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "TerrainRevisionComponent");
		Component->SetHeightmap(Heightmap);
		auto Proxy = Component->CreateSceneProxy();
		ASSERT_NE(Proxy, nullptr);
		auto& Terrain = static_cast<Durin::FTerrainSceneProxy&>(*Proxy);
		EXPECT_EQ(Terrain.GetRevision(), Heightmap->GetRevision());
		EXPECT_EQ(Terrain.GetPayload(), Heightmap->GetPayload());
		Durin::uint16 Value = 0;
		ASSERT_TRUE(Terrain.GetPayload()->GetSample(1, 1, Value));
		EXPECT_EQ(Value, 65535);
	}

	TEST(TerrainRenderPrimitive, ValidAssetBeyondT1CeilingIsNotRendered)
	{
		std::vector<Durin::uint16> Samples(1026u * 2u, 1u);
		auto* Heightmap = Durin::NewObject<Durin::DTerrainHeightmap>(nullptr, "TerrainOversizeHeightmap");
		std::string Error;
		ASSERT_TRUE(Heightmap->InitializeFromSamples(1026, 2, Samples, Error));
		auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "TerrainOversizeComponent");
		Component->SetHeightmap(Heightmap);
		EXPECT_EQ(Component->CreateSceneProxy(), nullptr);
		EXPECT_EQ(Component->GetRenderStatus(), Durin::ETerrainRenderStatus::ExtentRejected);
		EXPECT_NE(Component->GetLastRenderDiagnostic().find("1026x2"), std::string::npos);
		EXPECT_EQ(Heightmap->GetStatus(), Durin::ETerrainHeightmapStatus::Ready);
	}
}
