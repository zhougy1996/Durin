#include <gtest/gtest.h>

#include "Asset/MountedSource.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <fstream>

TEST(FMountedSourceTests, StagesCommitsAndRollsBackWithoutEngine)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "AssetCoreMountedSource";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path ExternalSource = Root / "External" / "Input.bin";
	std::filesystem::create_directories(ExternalSource.parent_path());
	std::filesystem::create_directories(Root / "Game" / "Content");
	{
		std::ofstream Stream(ExternalSource, std::ios::binary);
		Stream << "mounted-source";
	}
	const std::array Mounts = {
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::PathUtilities::EMountOwner::ActiveProject,
			.Root = Root / "Game",
			.ContentPath = "Content",
			.bAutoScan = true,
			.bAuthoringWritable = true}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();

	Durin::Asset::FMountedSourceFile Source;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Input.bin",
		Source, Error)) << Error;
	EXPECT_TRUE(Source.bCreatedFile);
	EXPECT_TRUE(std::filesystem::is_regular_file(Source.PhysicalPath));
	Durin::Asset::RollbackMountedSourceFile(Source);
	EXPECT_FALSE(std::filesystem::exists(
		Root / "Game" / "Content" / "Textures" / "Input.bin"));

	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
		ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Input.bin",
		Source, Error)) << Error;
	const std::filesystem::path Published = Source.PhysicalPath;
	Durin::Asset::CommitMountedSourceFile(Source);
	EXPECT_TRUE(std::filesystem::is_regular_file(Published));

	Durin::Asset::FMountedSourceRelocation Relocation;
	ASSERT_TRUE(Durin::Asset::PrepareMountedSourceRelocation(
		"/Game/Textures/Asset", "/Game/Textures/Input.bin",
		"/Game/Textures/Moved.bin", Relocation, Error)) << Error;
	Durin::Asset::RollbackMountedSourceRelocation(Relocation);
	EXPECT_TRUE(std::filesystem::is_regular_file(Published));
	EXPECT_FALSE(std::filesystem::exists(
		Root / "Game" / "Content" / "Textures" / "Moved.bin"));
}
