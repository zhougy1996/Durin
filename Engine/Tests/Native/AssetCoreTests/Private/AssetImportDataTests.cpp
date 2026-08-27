#include <gtest/gtest.h>

#include "Asset/AssetImportData.h"
#include "Asset/SourceFilename.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "NativeDObjectTestSupport.h"

namespace
{
	auto MakeSource(
		std::string Identity,
		std::string Role,
		std::string Filename,
		std::string_view Bytes) -> Durin::AssetImport::FSourceFile
	{
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(Bytes);
		return {
			.StableIdentity = std::move(Identity),
			.Role = std::move(Role),
			.DisplayLabel = "Source fixture",
			.Filename = std::move(Filename),
			.ContentHashLow = Hash.HashLow,
			.ContentHashHigh = Hash.HashHigh,
			.ByteCount = Bytes.size(),
			.LastWriteTime = 123};
	}

	class FAssetImportDataTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			Durin::Testing::InitializeDObjectSystemForTests();
			(void)Durin::AssetImport::DAssetImportData::StaticClass();
		}
	};
}

TEST_F(FAssetImportDataTests, SourceInfoNormalizesValidatesLooksUpAndFingerprints)
{
	using namespace Durin::AssetImport;
	FAssetImportInfo Info;
	Info.Sources = {
		MakeSource("zeta", "dependency", "/TestSources/zeta.bin", "zeta"),
		MakeSource("root", "source", "/TestSources/root.png", "root")};
	std::string Error;
	EXPECT_FALSE(Info.Validate(Error));
	Info.Normalize();
	ASSERT_TRUE(Info.Validate(Error)) << Error;
	ASSERT_NE(Info.FindByStableIdentity("root"), nullptr);
	EXPECT_EQ(Info.FindByStableIdentity("root")->Role, "source");
	ASSERT_NE(Info.FindByRole("dependency"), nullptr);
	EXPECT_EQ(Info.FindByRole("dependency")->StableIdentity, "zeta");
	EXPECT_FALSE(Info.GetFingerprint().IsZero());

	FAssetImportInfo Same = Info;
	EXPECT_EQ(Same.GetFingerprint(), Info.GetFingerprint());
	Same.Sources[0].ContentHashHigh = 0;
	EXPECT_FALSE(Same.Validate(Error));
	Same = Info;
	Same.Sources.push_back(Same.Sources.front());
	EXPECT_FALSE(Same.Validate(Error));
	FSourceFile Partial;
	Partial.StableIdentity = "partial";
	EXPECT_FALSE(Partial.Validate(Error));
	FSourceFile Empty;
	EXPECT_TRUE(Empty.Validate(Error));
	FAssetImportInfo TooMany;
	TooMany.Sources.resize(static_cast<size_t>(MaximumAssetImportSources) + 1);
	EXPECT_FALSE(TooMany.Validate(Error));
}

TEST_F(FAssetImportDataTests, SourceFilenamesUseProjectRelativeAndExternalAbsoluteForms)
{
	using namespace Durin::AssetImport;
	const std::filesystem::path Project = Durin::FPaths::ProjectDir();
	ASSERT_FALSE(Project.empty());
	std::string Error;
	std::string Filename;
	ASSERT_TRUE(MakeSourceFilename(
		(Project / "Sources" / "Textures" / "Albedo.png").generic_string(),
		Filename, Error)) << Error;
	EXPECT_EQ(Filename, "Sources/Textures/Albedo.png");
	std::string Resolved;
	ASSERT_TRUE(ResolveSourceFilename(Filename, Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved),
		(Project / "Sources" / "Textures" / "Albedo.png").lexically_normal());

	const std::filesystem::path External =
		(std::filesystem::temp_directory_path() / "DurinExternalSource.png")
		.lexically_normal();
	ASSERT_TRUE(MakeSourceFilename(External.generic_string(), Filename, Error)) << Error;
	EXPECT_EQ(Filename, External.generic_string());
	ASSERT_TRUE(ResolveSourceFilename(Filename, Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved), External);

	EXPECT_FALSE(ResolveSourceFilename("../Escapes.png", Resolved, Error));
	EXPECT_FALSE(ResolveSourceFilename("Sources/./Invalid.png", Resolved, Error));
	EXPECT_FALSE(ResolveSourceFilename({}, Resolved, Error));
}
