#include <gtest/gtest.h>

#include "Asset/MountedSource.h"
#include "AssetTools.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <fstream>

TEST(FMountedSourceTests, ResolvesDiagnosticFactsWithoutAssetFamilyPolicy)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "AssetCoreMountedSourceResolution";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path Existing =
		Root / "Game" / "Content" / "Textures" / "Existing.bin";
	std::filesystem::create_directories(Existing.parent_path());
	{
		std::ofstream Stream(Existing, std::ios::binary);
		Stream << "mounted-source";
	}
	const std::array Mounts = {
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::PathUtilities::EMountOwner::ActiveProject,
			.Root = Root / "Game",
			.ContentPath = "Content",
			.Dependencies = {"/Offline/"}},
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Offline/",
			.Owner = Durin::PathUtilities::EMountOwner::ExternalSources,
			.Root = Root / "Offline",
			.ContentPath = "Content"}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();

	Durin::Asset::FMountedSourceResolution Resolution;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::ResolveMountedSourceReference(
		"/Game/Textures/Asset", "/Game/Textures/Existing.bin",
		Durin::Asset::EMountedSourceExistencePolicy::AllowMissing,
		Resolution, Error)) << Error;
	EXPECT_TRUE(Resolution.bExists);
	EXPECT_EQ(Resolution.SourcePath.Path, "/Game/Textures/Existing.bin");
	EXPECT_EQ(Resolution.PhysicalPath, Existing);

	ASSERT_TRUE(Durin::Asset::ResolveMountedSourceReference(
		"/Game/Textures/Asset", "/Game/Textures/Missing.bin",
		Durin::Asset::EMountedSourceExistencePolicy::AllowMissing,
		Resolution, Error)) << Error;
	EXPECT_FALSE(Resolution.bExists);
	EXPECT_EQ(Resolution.SourcePath.Path, "/Game/Textures/Missing.bin");
	EXPECT_EQ(Resolution.PhysicalPath,
		Root / "Game" / "Content" / "Textures" / "Missing.bin");

	ASSERT_TRUE(Durin::Asset::ResolveMountedSourceReference(
		"/Game/Textures/Asset", "/Offline/Textures/Missing.bin",
		Durin::Asset::EMountedSourceExistencePolicy::AllowMissing,
		Resolution, Error)) << Error;
	EXPECT_FALSE(Resolution.bExists);
	EXPECT_EQ(Resolution.SourcePath.Path, "/Offline/Textures/Missing.bin");
	EXPECT_EQ(Resolution.PhysicalPath,
		Root / "Offline" / "Content" / "Textures" / "Missing.bin");

	EXPECT_FALSE(Durin::Asset::ResolveMountedSourceReference(
		"/Game/Textures/Asset", "/Game/Textures/Missing.bin",
		Durin::Asset::EMountedSourceExistencePolicy::RequireFile,
		Resolution, Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_TRUE(Resolution.SourcePath.Path.empty());

	EXPECT_FALSE(Durin::Asset::ResolveMountedSourceReference(
		"/Offline/Textures/Asset", "/Game/Textures/Existing.bin",
		Durin::Asset::EMountedSourceExistencePolicy::AllowMissing,
		Resolution, Error));
	EXPECT_NE(Error.find("may not depend"), std::string::npos);

	EXPECT_FALSE(Durin::Asset::ResolveMountedSourceReference(
		"/Game/Textures/Asset", "Game/Textures/Existing.bin",
		Durin::Asset::EMountedSourceExistencePolicy::AllowMissing,
		Resolution, Error));
	EXPECT_FALSE(Error.empty());
}

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

TEST(FMountedSourceTests, ScopedOwnerRollsBackAndTransfersExactlyOnce)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "AssetCoreScopedMountedSource";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path ExternalSource = Root / "External" / "Input.bin";
	std::filesystem::create_directories(ExternalSource.parent_path());
	std::filesystem::create_directories(Root / "Game" / "Content");
	{
		std::ofstream Stream(ExternalSource, std::ios::binary);
		Stream << "scoped-mounted-source";
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
	std::string Error;
	const std::filesystem::path Destination =
		Root / "Game" / "Content" / "Textures" / "Input.bin";

	{
		Durin::Asset::FScopedMountedSourceFile Source;
		ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
			ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Input.bin",
			Source, Error)) << Error;
		ASSERT_TRUE(Source.bCreatedFile);
	}
	EXPECT_FALSE(std::filesystem::exists(Destination));

	{
		Durin::Asset::FScopedMountedSourceFile Source;
		ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
			ExternalSource, "/Game/Textures/Asset", "/Game/Textures/Input.bin",
			Source, Error)) << Error;
		Durin::Asset::FScopedMountedSourceFile Moved(std::move(Source));
		EXPECT_FALSE(Source.bCreatedFile);
		EXPECT_TRUE(Moved.bCreatedFile);
	}
	EXPECT_FALSE(std::filesystem::exists(Destination));
}

TEST(FMountedSourceTests, ScopedOwnerNeverRemovesReferencedOrReusedFiles)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "AssetCoreNonOwningMountedSource";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path Mounted =
		Root / "Game" / "Content" / "Textures" / "Existing.bin";
	const std::filesystem::path External = Root / "External" / "Same.bin";
	std::filesystem::create_directories(Mounted.parent_path());
	std::filesystem::create_directories(External.parent_path());
	for (const std::filesystem::path& Path : {Mounted, External})
	{
		std::ofstream Stream(Path, std::ios::binary);
		Stream << "identical";
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
	std::string Error;

	{
		Durin::Asset::FScopedMountedSourceFile Source;
		ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
			Mounted, "/Game/Textures/Asset", "/Game/Unused.bin", Source, Error)) << Error;
		EXPECT_EQ(Durin::Asset::ESourceFileDisposition::ReferenceExisting,
			Source.Disposition);
	}
	EXPECT_TRUE(std::filesystem::is_regular_file(Mounted));

	{
		Durin::Asset::FScopedMountedSourceFile Source;
		ASSERT_TRUE(Durin::Asset::PrepareMountedSourceFile(
			External, "/Game/Textures/Asset", "/Game/Textures/Existing.bin",
			Source, Error)) << Error;
		EXPECT_EQ(Durin::Asset::ESourceFileDisposition::ReusedIdentical,
			Source.Disposition);
	}
	EXPECT_TRUE(std::filesystem::is_regular_file(Mounted));
}
