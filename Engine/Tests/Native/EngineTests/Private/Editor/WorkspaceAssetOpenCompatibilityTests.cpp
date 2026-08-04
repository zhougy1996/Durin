#include "Asset/WorkspaceAssetOpenCompatibility.h"
#include "Misc/Paths.h"

#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto MakePath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Path));
		return Path;
	}

	class FWorkspaceAssetOpenCompatibilityTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			Root = Durin::Testing::GetTestWorkDirectory() / "WorkspaceAssetOpenCompatibility";
			std::filesystem::create_directories(Root);
			const std::array Definitions{
				Durin::PathUtilities::FMountPoint{
					.VirtualRoot = "/CompatibilityTests/",
					.Owner = Durin::PathUtilities::EMountOwner::ActiveProject,
					.Root = Root,
					.bAutoScan = true}};
			Registry = std::make_unique<Durin::PathUtilities::FScopedMountRegistryFixture>(
				Definitions);
			ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
		}

		std::filesystem::path Root;
		std::unique_ptr<Durin::PathUtilities::FScopedMountRegistryFixture> Registry;
	};
}

TEST_F(FWorkspaceAssetOpenCompatibilityTests, AcceptsACompatibleLoadWithoutReleasingPackages)
{
	const Durin::FAssetPath Path = MakePath("/CompatibilityTests/Materials/Compatible");
	Durin::uint32 ReleaseCount = 0;
	Durin::FWorkspaceAssetOpenCompatibility Policy(
		Path,
		[&ReleaseCount] {
			++ReleaseCount;
			return Durin::Asset::FAssetResult{};
		});
	Durin::Asset::FAssetLoadReport Report{.PackagePath = Path};
	std::string Diagnostic;

	EXPECT_FALSE(Policy.RejectIfIncompatible(Report, Diagnostic));
	EXPECT_TRUE(Diagnostic.empty());
	EXPECT_EQ(ReleaseCount, 0u);
}

TEST_F(FWorkspaceAssetOpenCompatibilityTests, RejectsAnIncompatibleLoadWithStablePolicy)
{
	const Durin::FAssetPath Path = MakePath("/CompatibilityTests/Textures/Incompatible");
	Durin::uint32 ReleaseCount = 0;
	Durin::FWorkspaceAssetOpenCompatibility Policy(
		Path,
		[&ReleaseCount] {
			++ReleaseCount;
			return Durin::Asset::FAssetResult{};
		});
	Durin::Asset::FAssetLoadReport Report{
		.PackagePath = Path,
		.CompatibilityIssues = {{.ObjectPath = Path.ToString()}}};
	std::string Diagnostic;

	EXPECT_TRUE(Policy.RejectIfIncompatible(Report, Diagnostic));
	EXPECT_EQ(
		Diagnostic,
		"Asset /CompatibilityTests/Textures/Incompatible is incompatible with the current authored baseline and "
		"was not opened. Run Asset Compatibility Audit for complete details.");
	EXPECT_EQ(ReleaseCount, 1u);
}

TEST_F(FWorkspaceAssetOpenCompatibilityTests, LevelMaterialAndTextureUseTheSameOpenSafetyContract)
{
	for (const std::string_view Workspace : {"Levels", "Materials", "Textures"})
	{
		const Durin::FAssetPath Path = MakePath(std::format(
			"/CompatibilityTests/{}/Qualification", Workspace));
		Durin::uint32 ReleaseCount = 0;
		Durin::FWorkspaceAssetOpenCompatibility CompatiblePolicy(
			Path,
			[&ReleaseCount] {
				++ReleaseCount;
				return Durin::Asset::FAssetResult{};
			});
		Durin::Asset::FAssetLoadReport CompatibleReport{.PackagePath = Path};
		std::string Diagnostic;
		EXPECT_FALSE(CompatiblePolicy.RejectIfIncompatible(CompatibleReport, Diagnostic))
			<< Workspace;
		EXPECT_EQ(ReleaseCount, 0u) << Workspace;

		Durin::FWorkspaceAssetOpenCompatibility IncompatiblePolicy(
			Path,
			[&ReleaseCount] {
				++ReleaseCount;
				return Durin::Asset::FAssetResult{};
			});
		Durin::Asset::FAssetLoadReport IncompatibleReport{
			.PackagePath = Path,
			.CompatibilityIssues = {{.ObjectPath = Path.ToString()}}};
		EXPECT_TRUE(IncompatiblePolicy.RejectIfIncompatible(IncompatibleReport, Diagnostic))
			<< Workspace;
		EXPECT_EQ(ReleaseCount, 1u) << Workspace;
		EXPECT_NE(Diagnostic.find("was not opened"), std::string::npos) << Workspace;
		EXPECT_NE(Diagnostic.find("Asset Compatibility Audit"), std::string::npos) << Workspace;
	}
}
