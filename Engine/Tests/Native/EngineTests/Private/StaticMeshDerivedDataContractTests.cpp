#include "Runtime/Engine/Private/Physics/PhysicsCookDerivedDataKey.h"
#include <gtest/gtest.h>

#include "StaticMesh/StaticMeshDerivedData.h"
#include "Runtime/Engine/Private/StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshResources.h"

namespace
{
	auto MakeKeyInput() -> Durin::FStaticMeshBuildKeyInput
	{
		Durin::FStaticMeshBuildKeyInput Input;
		Input.SourceHash = Durin::FXxHash128{
			0x0123456789abcdefull,
			0xfedcba9876543210ull};
		Input.ReconciliationHash = Durin::FXxHash128{
			0x1111111111111111ull,
			0x2222222222222222ull};
		Input.TargetPlatform = Durin::EAssetPayloadTargetPlatform::Win64;
		return Input;
	}

	auto MakeCollisionKeyInput() -> Durin::FPhysicsCookKeyInput
	{
		return {
			.GeometryHash = {0x0123456789abcdefull, 0xfedcba9876543210ull},
			.SourceMode = Durin::EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
			.QueryPolicy = Durin::EBodySetupCollisionQueryPolicy::SimpleAndComplex,
			.WeldToleranceBits = 0x3a83126fu,
			.TargetPlatform = Durin::EAssetPayloadTargetPlatform::Win64};
	}
}

TEST(FStaticMeshDerivedDataContractTests, KeyEncodingIsCanonicalAndDeterministic)
{
	const Durin::FStaticMeshBuildKeyInput Input = MakeKeyInput();
	const Durin::FByteBuffer First =
		Durin::BuildStaticMeshDerivedDataKeyBytes(Input).value();
	ASSERT_FALSE(First.empty());
	const Durin::FByteBuffer Second =
		Durin::BuildStaticMeshDerivedDataKeyBytes(Input).value();


	EXPECT_EQ(First, Second);
	EXPECT_EQ(Durin::BuildStaticMeshDerivedDataKey(Input).value().ToString(),
		"99e5d7ba219a5a117ea74d151845fcef");
}

TEST(FStaticMeshDerivedDataContractTests, EverySemanticInputChangesTheKey)
{
	const Durin::FStaticMeshBuildKeyInput Baseline = MakeKeyInput();
	const Durin::FCacheKeyProxy BaselineKey =
		Durin::BuildStaticMeshDerivedDataKey(Baseline).value();

	auto ExpectChanged = [&](auto Mutate)
	{
		Durin::FStaticMeshBuildKeyInput Changed = Baseline;
		Mutate(Changed);
		const auto Key = Durin::BuildStaticMeshDerivedDataKey(Changed);
		if (Changed.TargetPlatform == Durin::EAssetPayloadTargetPlatform::Unknown)
		{
			ASSERT_FALSE(Key);
			EXPECT_EQ(Key.error().Code, Durin::EStaticMeshBuildKeyError::UnsupportedTarget);
		}
		else
		{
			ASSERT_TRUE(Key);
			EXPECT_NE(*Key, BaselineKey);
		}
	};

	ExpectChanged([](auto& Value) { ++Value.SourceHash.HashLow; });
	ExpectChanged([](auto& Value) { ++Value.ReconciliationHash.HashLow; });
	ExpectChanged([](auto& Value) { ++Value.BuilderVersion; });
	ExpectChanged([](auto& Value) { ++Value.OutputSchemaVersion; });
	ExpectChanged([](auto& Value) { ++Value.MaterialSlotCount; });
	ExpectChanged([](auto& Value) { Value.TargetPlatform = Durin::EAssetPayloadTargetPlatform::Unknown; });
}

TEST(FStaticMeshDerivedDataContractTests, CollisionKeyCoversCanonicalGeometryAndRecipe)
{
	const Durin::FPhysicsCookKeyInput Baseline =
		MakeCollisionKeyInput();
	const Durin::FByteBuffer Bytes =
		Durin::BuildPhysicsCookDerivedDataKeyBytes(Baseline).value();
	ASSERT_FALSE(Bytes.empty());

	EXPECT_EQ(Bytes, Durin::BuildPhysicsCookDerivedDataKeyBytes(Baseline).value());
	const Durin::FCacheKeyProxy BaselineKey =
		Durin::BuildPhysicsCookDerivedDataKey(Baseline).value();
	EXPECT_EQ(BaselineKey.ToString(), "0c1836c76d6968f748cc33a0d91f5ead");

	auto ExpectChanged = [&](auto Mutate)
	{
		Durin::FPhysicsCookKeyInput Changed = Baseline;
		Mutate(Changed);
		const auto Key = Durin::BuildPhysicsCookDerivedDataKey(Changed);
		if (Changed.TargetPlatform == Durin::EAssetPayloadTargetPlatform::Unknown)
		{
			ASSERT_FALSE(Key);
			EXPECT_EQ(Key.error().Code, Durin::EPhysicsCookKeyError::UnsupportedTarget);
		}
		else
		{
			ASSERT_TRUE(Key);
			EXPECT_NE(*Key, BaselineKey);
		}
	};
	ExpectChanged([](auto& Value) { ++Value.GeometryHash.HashLow; });
	ExpectChanged([](auto& Value) {
		Value.SourceMode = Durin::EBodySetupCollisionSourceMode::None;
	});
	ExpectChanged([](auto& Value) {
		Value.QueryPolicy = Durin::EBodySetupCollisionQueryPolicy::ComplexOnly;
	});
	ExpectChanged([](auto& Value) { ++Value.WeldToleranceBits; });
	ExpectChanged([](auto& Value) { ++Value.BuilderVersion; });
	ExpectChanged([](auto& Value) { ++Value.OutputSchemaVersion; });
	ExpectChanged([](auto& Value) {
		Value.TargetPlatform = Durin::EAssetPayloadTargetPlatform::Unknown;
	});
}

TEST(FStaticMeshDerivedDataContractTests, FormatConstantsRemainWithinReaderLimits)
{
	EXPECT_EQ(Durin::StaticMeshPayloadHeaderSize, 64u);
	EXPECT_EQ(Durin::StaticMeshPayloadChunkEntrySize, 32u);
	EXPECT_EQ(Durin::StaticMeshPayloadAlignment, 16u);
	EXPECT_GE(Durin::MaximumStaticMeshPayloadChunks, 6u);
	EXPECT_GE(Durin::MaxStaticMeshUVChannels, 4u);
}

TEST(FStaticMeshDerivedDataContractTests, KeyRejectionOwnsTargetAndHasNoPartialOutput)
{
	using namespace Durin;
	auto Render = MakeKeyInput();
	Render.TargetPlatform = static_cast<EAssetPayloadTargetPlatform>(99);
	const auto RenderBytes = BuildStaticMeshDerivedDataKeyBytes(Render);
	const auto RenderKey = BuildStaticMeshDerivedDataKey(Render);
	Render = {};
	ASSERT_FALSE(RenderBytes);
	ASSERT_FALSE(RenderKey);
	EXPECT_EQ(RenderBytes.error().Code, EStaticMeshBuildKeyError::UnsupportedTarget);
	EXPECT_EQ(RenderKey.error().Code, EStaticMeshBuildKeyError::UnsupportedTarget);
	EXPECT_EQ(RenderBytes.error().TargetPlatform, static_cast<EAssetPayloadTargetPlatform>(99));
	EXPECT_EQ(RenderKey.error().TargetPlatform, static_cast<EAssetPayloadTargetPlatform>(99));
	auto Collision = MakeCollisionKeyInput();
	Collision.TargetPlatform = EAssetPayloadTargetPlatform::Unknown;
	const auto CollisionBytes = BuildPhysicsCookDerivedDataKeyBytes(Collision);
	const auto CollisionKey = BuildPhysicsCookDerivedDataKey(Collision);
	Collision = MakeCollisionKeyInput();
	ASSERT_FALSE(CollisionBytes);
	ASSERT_FALSE(CollisionKey);
	EXPECT_EQ(CollisionBytes.error().Code, EPhysicsCookKeyError::UnsupportedTarget);
	EXPECT_EQ(CollisionKey.error().TargetPlatform, EAssetPayloadTargetPlatform::Unknown);
	const auto Valid = BuildPhysicsCookDerivedDataKey(Collision);
	ASSERT_TRUE(Valid);
	EXPECT_TRUE(Valid->IsValid());
}
