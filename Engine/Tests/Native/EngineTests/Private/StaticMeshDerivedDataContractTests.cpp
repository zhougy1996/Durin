#include <gtest/gtest.h>

#include "StaticMesh/StaticMeshDerivedData.h"
#include "Runtime/Engine/Private/StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshResources.h"

namespace
{
	auto MakeKeyInput() -> Durin::FStaticMeshBuildKeyInput
	{
		Durin::FStaticMeshBuildKeyInput Input;
		Input.ImportedDataHash = Durin::FXxHash128{
			0x0123456789abcdefull,
			0xfedcba9876543210ull};
		Input.ReconciliationHash = Durin::FXxHash128{
			0x1111111111111111ull,
			0x2222222222222222ull};
		Input.TargetPlatform = Durin::EStaticMeshTargetPlatform::Win64;
		return Input;
	}

	auto MakeCollisionKeyInput() -> Durin::FStaticMeshCollisionBuildKeyInput
	{
		return {
			.GeometryHash = {0x0123456789abcdefull, 0xfedcba9876543210ull},
			.SourceMode = Durin::EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
			.QueryPolicy = Durin::EBodySetupCollisionQueryPolicy::SimpleAndComplex,
			.WeldToleranceBits = 0x3a83126fu,
			.TargetPlatform = Durin::EStaticMeshTargetPlatform::Win64};
	}
}

TEST(FStaticMeshDerivedDataContractTests, KeyEncodingIsCanonicalAndDeterministic)
{
	const Durin::FStaticMeshBuildKeyInput Input = MakeKeyInput();
	std::string Error;
	const Durin::FByteArray First =
		Durin::BuildStaticMeshDerivedDataKeyBytes(Input, Error);
	ASSERT_TRUE(Error.empty()) << Error;
	const Durin::FByteArray Second =
		Durin::BuildStaticMeshDerivedDataKeyBytes(Input, Error);
	const Durin::FByteArray Expected = [] {
		const uint8 Values[]{
		0x04, 0x00, 0x00, 0x00,
		0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
		0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
		0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
		0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
		0x04, 0x00, 0x00, 0x00,
		0x05, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00};
		const std::span<const std::byte> Bytes = std::as_bytes(std::span{Values});
		return Durin::FByteArray(Bytes.begin(), Bytes.end());
	}();

	EXPECT_EQ(First, Second);
	EXPECT_EQ(First, Expected);
	EXPECT_EQ(Durin::BuildStaticMeshDerivedDataKey(Input, Error),
		"373d527e05a47be00505fd636fd724a2");
}

TEST(FStaticMeshDerivedDataContractTests, EverySemanticInputChangesTheKey)
{
	const Durin::FStaticMeshBuildKeyInput Baseline = MakeKeyInput();
	std::string Error;
	const std::string BaselineKey =
		Durin::BuildStaticMeshDerivedDataKey(Baseline, Error);

	auto ExpectChanged = [&](auto Mutate)
	{
		Durin::FStaticMeshBuildKeyInput Changed = Baseline;
		Mutate(Changed);
		EXPECT_NE(Durin::BuildStaticMeshDerivedDataKey(Changed, Error), BaselineKey);
	};

	ExpectChanged([](auto& Value) { ++Value.ImportedDataHash.HashLow; });
	ExpectChanged([](auto& Value) { ++Value.ReconciliationHash.HashLow; });
	ExpectChanged([](auto& Value) { ++Value.BuilderVersion; });
	ExpectChanged([](auto& Value) { ++Value.PayloadSchemaVersion; });
	ExpectChanged([](auto& Value) { Value.TargetPlatform = Durin::EStaticMeshTargetPlatform::Unknown; });
}

TEST(FStaticMeshDerivedDataContractTests, CollisionKeyCoversCanonicalGeometryAndRecipe)
{
	const Durin::FStaticMeshCollisionBuildKeyInput Baseline =
		MakeCollisionKeyInput();
	std::string Error;
	const Durin::FByteArray Bytes =
		Durin::BuildStaticMeshCollisionDerivedDataKeyBytes(Baseline, Error);
	ASSERT_TRUE(Error.empty()) << Error;
	const Durin::FByteArray Expected = [] {
		const uint8 Values[]{
			0x03, 0x00, 0x00, 0x00,
			0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
			0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
			0x02, 0x02,
			0x6f, 0x12, 0x83, 0x3a,
			0x02, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00};
		const std::span<const std::byte> View = std::as_bytes(std::span{Values});
		return Durin::FByteArray(View.begin(), View.end());
	}();
	EXPECT_EQ(Bytes, Expected);
	const std::string BaselineKey =
		Durin::BuildStaticMeshCollisionDerivedDataKey(Baseline, Error);
	EXPECT_EQ(BaselineKey, "2f83321f2ed9af9cbd52d38467d40155");

	auto ExpectChanged = [&](auto Mutate)
	{
		Durin::FStaticMeshCollisionBuildKeyInput Changed = Baseline;
		Mutate(Changed);
		EXPECT_NE(Durin::BuildStaticMeshCollisionDerivedDataKey(Changed, Error),
			BaselineKey);
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
	ExpectChanged([](auto& Value) { ++Value.PayloadSchemaVersion; });
	ExpectChanged([](auto& Value) {
		Value.TargetPlatform = Durin::EStaticMeshTargetPlatform::Unknown;
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
