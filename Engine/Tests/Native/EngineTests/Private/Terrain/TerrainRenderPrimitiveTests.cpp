#include "Actors/TerrainActor.h"
#include "Components/TerrainComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/FPrimitiveSceneProxy.h"
#include "Terrain/TerrainHeightmap.h"

#include <gtest/gtest.h>

namespace
{
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
		const Durin::FBox Bounds = Proxy.GetLocalBounds();
		EXPECT_DOUBLE_EQ(Bounds.Min.x, 0.0);
		EXPECT_DOUBLE_EQ(Bounds.Max.x, 258.0);
		EXPECT_DOUBLE_EQ(Bounds.Max.y, 207.0);
		EXPECT_DOUBLE_EQ(Bounds.Min.z, -75.0);
		EXPECT_DOUBLE_EQ(Bounds.Max.z, 25.0);
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
