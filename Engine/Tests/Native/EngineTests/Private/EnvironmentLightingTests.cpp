#include "EngineTestSupport.h"
#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "Misc/FileHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Serialization/Archive.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeEnvironmentLightingFixture() -> Durin::FEnvironmentLightingData
	{
		Durin::FEnvironmentLightingData Data;
		const size_t IrradianceElements =
			static_cast<size_t>(Durin::EnvironmentIrradianceDimension)
			* Durin::EnvironmentIrradianceDimension * 4;
		for (size_t Face = 0; Face < Data.Irradiance.size(); ++Face)
			Data.Irradiance[Face].assign(IrradianceElements, static_cast<uint16>(Face + 1));
		for (uint32 Mip = 0; Mip < Durin::EnvironmentPrefilterMipCount; ++Mip)
		{
			const size_t Dimension = Durin::EnvironmentPrefilterDimension >> Mip;
			for (size_t Face = 0; Face < Data.Prefiltered[Mip].size(); ++Face)
			{
				Data.Prefiltered[Mip][Face].assign(
					Dimension * Dimension * 4,
					static_cast<uint16>(100 + Mip * 10 + Face));
			}
		}
		Data.BrdfLut.assign(
			static_cast<size_t>(Durin::EnvironmentBrdfLutDimension)
				* Durin::EnvironmentBrdfLutDimension * 4,
			555);
		return Data;
	}

	auto SerializeEnvironmentLighting(
		Durin::FEnvironmentLightingData& Data,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		Durin::FCanonicalMemoryWriter Ar(
			OutBytes, Durin::EArchivePurpose::DerivedDataPayload);
		Data.Serialize(Ar);
		if (!Ar.HasError()) return true;
		OutError = Ar.GetFailure()->Message;
		return false;
	}

	auto DeserializeEnvironmentLighting(
		std::span<const std::byte> Bytes,
		Durin::FEnvironmentLightingData& OutData,
		std::string& OutError) -> bool
	{
		Durin::FCanonicalMemoryReader Ar(
			Bytes, Durin::EArchivePurpose::DerivedDataPayload);
		OutData.Serialize(Ar);
		if (!Ar.HasError()) return true;
		OutError = Ar.GetFailure()->Message;
		return false;
	}
}

TEST(FEnvironmentLightingTests, PayloadRoundTripsDeterministicallyAndRejectsCorruption)
{
	Durin::FEnvironmentLightingData Expected = MakeEnvironmentLightingFixture();
	ASSERT_TRUE(Expected.IsValid());
	std::vector<std::byte> First;
	std::vector<std::byte> Second;
	std::string Error;
	ASSERT_TRUE(SerializeEnvironmentLighting(Expected, First, Error)) << Error;
	ASSERT_TRUE(SerializeEnvironmentLighting(Expected, Second, Error)) << Error;
	EXPECT_EQ(First, Second);

	Durin::FEnvironmentLightingData Decoded;
	ASSERT_TRUE(DeserializeEnvironmentLighting(First, Decoded, Error)) << Error;
	EXPECT_EQ(Decoded, Expected);
	std::vector<std::byte> DifferentProducer = First;
	DifferentProducer[8] ^= std::byte{0x40};
	ASSERT_TRUE(DeserializeEnvironmentLighting(DifferentProducer, Decoded, Error)) << Error;
	EXPECT_EQ(Decoded, Expected);

	First.back() ^= std::byte{0x80};
	const Durin::FEnvironmentLightingData BeforeFailure = Decoded;
	EXPECT_FALSE(DeserializeEnvironmentLighting(First, Decoded, Error));
	EXPECT_EQ(Decoded, BeforeFailure);
}

TEST(FEnvironmentLightingTests, CheckedInStudioPayloadIsValid)
{
	const std::filesystem::path PayloadPath =
		std::filesystem::path(Durin::FPaths::EngineContentDir())
		/ "Renderer/DefaultStudioEnvironment.iblbulk";
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, PayloadPath));
	Durin::FEnvironmentLightingData Data;
	std::string Error;
	ASSERT_TRUE(DeserializeEnvironmentLighting(Bytes, Data, Error)) << Error;
	EXPECT_TRUE(Data.IsValid());
}

TEST(FEnvironmentLightingTests, AssetCooksAuthoredPayloadDirectlyWithoutDdc)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "EnvironmentLightingCook";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root / "Content");
	Durin::PathUtilities::RegisterMountPointForTests(
		"/EnvironmentLightingCook/", (Root / "Content").generic_string() + "/");

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/EnvironmentLightingCook/StudioEnvironment", AssetPath));
	Durin::DEnvironmentLighting* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(AssetPath, Asset));
	ASSERT_NE(Asset, nullptr);

	std::vector<std::byte> SourceBytes;
	std::string Error;
	Durin::FEnvironmentLightingData SourceData = MakeEnvironmentLightingFixture();
	ASSERT_TRUE(SerializeEnvironmentLighting(SourceData, SourceBytes, Error)) << Error;
	Durin::FFileHelper::FAtomicFileError FileError;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(
		SourceBytes,
		Durin::DEnvironmentLighting::GetAuthoredPayloadPath(AssetPath.ToString()),
		&FileError)) << FileError.ToString();

	const std::filesystem::path CookRoot = std::filesystem::absolute(Root / "Cook");
	Durin::Asset::FCookContext Context(
		CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::Asset::ContributeEngineCookAsset(
		*Asset, "/Game/StudioEnvironment", Context, Error)) << Error;
	ASSERT_TRUE(Context.Publish(&Error)) << Error;

	Durin::Asset::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::Asset::InspectAssetPackage(
		(CookRoot / "Game/StudioEnvironment.dasset").generic_string(), Inspection));
	EXPECT_NE(Inspection.FindField("PlatformData"), nullptr);
	std::vector<std::byte> CookedBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		CookedBytes, CookRoot / "Game/StudioEnvironment.dbulk"));
	EXPECT_EQ(CookedBytes, SourceBytes);
}
