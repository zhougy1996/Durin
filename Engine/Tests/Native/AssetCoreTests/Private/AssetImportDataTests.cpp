#include <gtest/gtest.h>

#include "Asset/AssetImportData.h"
#include "Asset/SourceHint.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "NativeDObjectTestSupport.h"

namespace
{
	auto MakeSource(
		Durin::FName Role,
		std::string Hint,
		std::string_view Bytes,
		Durin::ESourceHintBase HintBase =
			Durin::ESourceHintBase::ProjectRelative)
		-> Durin::FSourceFile
	{
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(Bytes);
		return {
			.Role = Role,
			.DisplayLabel = "Source fixture",
			.Hint = std::move(Hint),
			.HintBase = HintBase,
			.ContentHashLow = Hash.HashLow,
			.ContentHashHigh = Hash.HashHigh,
			.ByteCount = Bytes.size()};
	}

	class FAssetImportDataTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			Durin::Testing::InitializeDObjectSystemForTests();
			(void)Durin::DAssetImportData::StaticClass();
		}
	};
}

TEST_F(FAssetImportDataTests, SourceInfoNormalizesValidatesLooksUpAndFingerprints)
{
	using Durin::FAssetImportInfo;
	using Durin::FSourceFile;
	using Durin::MaximumAssetImportSources;
	FAssetImportInfo Info;
	Info.Sources = {
		MakeSource("source", "TestSources/root.png", "root"),
		MakeSource("dependency", "TestSources/zeta.bin", "zeta")};
	std::string Error;
	EXPECT_FALSE(Info.Validate(Error));
	Info.Normalize();
	ASSERT_TRUE(Info.Validate(Error)) << Error;
	ASSERT_NE(Info.FindByRole("dependency"), nullptr);
	EXPECT_EQ(Info.FindByRole("dependency")->Hint, "TestSources/zeta.bin");
	EXPECT_FALSE(Info.GetFingerprint().IsZero());

	FAssetImportInfo Same = Info;
	EXPECT_EQ(Same.GetFingerprint(), Info.GetFingerprint());
	Same.Sources[0].ContentHashHigh = 0;
	EXPECT_FALSE(Same.Validate(Error));
	Same = Info;
	Same.Sources.push_back(Same.Sources.front());
	EXPECT_FALSE(Same.Validate(Error));
	FSourceFile Partial;
	Partial.Role = "partial";
	EXPECT_FALSE(Partial.Validate(Error));
	FSourceFile UppercaseRole = Info.Sources.front();
	UppercaseRole.Role = "Dependency";
	EXPECT_TRUE(UppercaseRole.Validate(Error)) << Error;
	EXPECT_EQ(UppercaseRole.Role, Durin::FName("dependency"));
	FSourceFile NumberedRole = Info.Sources.front();
	NumberedRole.Role = "dependency_1";
	EXPECT_FALSE(NumberedRole.Validate(Error));
	FSourceFile Empty;
	EXPECT_TRUE(Empty.Validate(Error));
	FAssetImportInfo TooMany;
	TooMany.Sources.resize(static_cast<size_t>(MaximumAssetImportSources) + 1);
	EXPECT_FALSE(TooMany.Validate(Error));
}

TEST_F(FAssetImportDataTests, SourceHintsSupportAssetProjectAndAbsoluteBases)
{
	using Durin::ESourceHintBase;
	using Durin::MakeSourceHint;
	using Durin::ResolveSourceHint;
	const std::filesystem::path Project = Durin::FPaths::ProjectDir();
	ASSERT_FALSE(Project.empty());
	const std::filesystem::path Package =
		(Project / "Sandbox" / "Content" / "Textures" / "UI" / "Icon.dasset")
		.lexically_normal();
	std::string Error;
	std::string Hint;
	std::string Resolved;
	ESourceHintBase Base = ESourceHintBase::Absolute;

	const std::filesystem::path SameDirectory =
		(Package.parent_path() / "Icon.png").lexically_normal();
	ASSERT_TRUE(MakeSourceHint(
		SameDirectory.generic_string(), Package.generic_string(), Base, Hint, Error)) << Error;
	EXPECT_EQ(Base, ESourceHintBase::AssetRelative);
	EXPECT_EQ(Hint, "Icon.png");
	ASSERT_TRUE(ResolveSourceHint(
		Base, Hint, Package.generic_string(), Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved), SameDirectory);

	const std::filesystem::path ParentRelative =
		(Project / "Sandbox" / "Sources" / "Albedo.png").lexically_normal();
	ASSERT_TRUE(MakeSourceHint(ParentRelative.generic_string(), Package.generic_string(),
		Base, Hint, Error, ESourceHintBase::ProjectRelative)) << Error;
	EXPECT_EQ(Base, ESourceHintBase::ProjectRelative);
	EXPECT_EQ(Hint, "Sandbox/Sources/Albedo.png");
	ASSERT_TRUE(ResolveSourceHint(
		Base, Hint, Package.generic_string(), Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved), ParentRelative);

	ASSERT_TRUE(MakeSourceHint(SameDirectory.generic_string(), Package.generic_string(),
		Base, Hint, Error, ESourceHintBase::Absolute)) << Error;
	EXPECT_EQ(Base, ESourceHintBase::Absolute);
	EXPECT_EQ(Hint, SameDirectory.generic_string());

	const std::filesystem::path External =
		(std::filesystem::temp_directory_path() / "DurinExternalHint.png")
		.lexically_normal();
	ASSERT_TRUE(MakeSourceHint(
		External.generic_string(), Package.generic_string(), Base, Hint, Error)) << Error;
	EXPECT_EQ(Base, ESourceHintBase::Absolute);
	EXPECT_EQ(Hint, External.generic_string());
	ASSERT_TRUE(ResolveSourceHint(
		Base, Hint, Package.generic_string(), Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved), External);

	// Moving or duplicating a package copies the hint bytes. Relative hints are
	// intentionally rebound against the destination package directory.
	const std::filesystem::path MovedPackage =
		(Project / "Sandbox" / "Content" / "Moved" / "Icon.dasset")
		.lexically_normal();
	ASSERT_TRUE(ResolveSourceHint(ESourceHintBase::AssetRelative,
		"Icon.png", MovedPackage.generic_string(), Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved),
		MovedPackage.parent_path() / "Icon.png");
	ASSERT_TRUE(ResolveSourceHint(ESourceHintBase::ProjectRelative,
		"Sandbox/Sources/Albedo.png", MovedPackage.generic_string(), Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved), ParentRelative);
	ASSERT_TRUE(ResolveSourceHint(ESourceHintBase::Absolute,
		External.generic_string(), MovedPackage.generic_string(), Resolved, Error)) << Error;
	EXPECT_EQ(std::filesystem::path(Resolved), External);

	EXPECT_FALSE(ResolveSourceHint(ESourceHintBase::ProjectRelative,
		"../Escapes.png", Package.generic_string(), Resolved, Error));
	EXPECT_FALSE(ResolveSourceHint(ESourceHintBase::AssetRelative,
		SameDirectory.generic_string(), Package.generic_string(), Resolved, Error));
	EXPECT_FALSE(ResolveSourceHint(ESourceHintBase::AssetRelative,
		"Icon/./Invalid.png", Package.generic_string(), Resolved, Error));
	EXPECT_FALSE(MakeSourceHint(External.generic_string(), Package.generic_string(),
		Base, Hint, Error, ESourceHintBase::ProjectRelative));
}
