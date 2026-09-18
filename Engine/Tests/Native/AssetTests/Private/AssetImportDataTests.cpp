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
	EXPECT_EQ(Info.Validate().Error.Code, Durin::EAssetImportDataError::NonCanonicalRoles);
	Info.Normalize();
	ASSERT_TRUE(Info.Validate());
	ASSERT_NE(Info.FindByRole("dependency"), nullptr);
	EXPECT_EQ(Info.FindByRole("dependency")->Hint, "TestSources/zeta.bin");
	EXPECT_FALSE(Info.GetFingerprint().IsZero());

	FAssetImportInfo Same = Info;
	EXPECT_EQ(Same.GetFingerprint(), Info.GetFingerprint());
	Same.Sources[0].ContentHashHigh = 0;
	EXPECT_FALSE(Same.Validate());
	Same = Info;
	Same.Sources.push_back(Same.Sources.front());
	EXPECT_FALSE(Same.Validate());
	FSourceFile Partial;
	Partial.Role = "partial";
	EXPECT_FALSE(Partial.Validate());
	FSourceFile UppercaseRole = Info.Sources.front();
	UppercaseRole.Role = "Dependency";
	EXPECT_TRUE(UppercaseRole.Validate());
	EXPECT_EQ(UppercaseRole.Role, Durin::FName("dependency"));
	FSourceFile NumberedRole = Info.Sources.front();
	NumberedRole.Role = "dependency_1";
	EXPECT_FALSE(NumberedRole.Validate());
	FSourceFile Empty;
	EXPECT_TRUE(Empty.Validate());
	FAssetImportInfo TooMany;
	TooMany.Sources.resize(static_cast<size_t>(MaximumAssetImportSources) + 1);
	EXPECT_FALSE(TooMany.Validate());
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
		SameDirectory.generic_string(), Package.generic_string(), Base, Hint));
	EXPECT_EQ(Base, ESourceHintBase::AssetRelative);
	EXPECT_EQ(Hint, "Icon.png");
	ASSERT_TRUE(ResolveSourceHint(
		Base, Hint, Package.generic_string(), Resolved));
	EXPECT_EQ(std::filesystem::path(Resolved), SameDirectory);

	const std::filesystem::path ParentRelative =
		(Project / "Sandbox" / "Sources" / "Albedo.png").lexically_normal();
	ASSERT_TRUE(MakeSourceHint(ParentRelative.generic_string(), Package.generic_string(),
		Base, Hint, ESourceHintBase::ProjectRelative));
	EXPECT_EQ(Base, ESourceHintBase::ProjectRelative);
	EXPECT_EQ(Hint, "Sandbox/Sources/Albedo.png");
	ASSERT_TRUE(ResolveSourceHint(
		Base, Hint, Package.generic_string(), Resolved));
	EXPECT_EQ(std::filesystem::path(Resolved), ParentRelative);

	ASSERT_TRUE(MakeSourceHint(SameDirectory.generic_string(), Package.generic_string(),
		Base, Hint, ESourceHintBase::Absolute));
	EXPECT_EQ(Base, ESourceHintBase::Absolute);
	EXPECT_EQ(Hint, SameDirectory.generic_string());

	const std::filesystem::path External =
		(std::filesystem::temp_directory_path() / "DurinExternalHint.png")
		.lexically_normal();
	ASSERT_TRUE(MakeSourceHint(
		External.generic_string(), Package.generic_string(), Base, Hint));
	EXPECT_EQ(Base, ESourceHintBase::Absolute);
	EXPECT_EQ(Hint, External.generic_string());
	ASSERT_TRUE(ResolveSourceHint(
		Base, Hint, Package.generic_string(), Resolved));
	EXPECT_EQ(std::filesystem::path(Resolved), External);

	// Moving or duplicating a package copies the hint bytes. Relative hints are
	// intentionally rebound against the destination package directory.
	const std::filesystem::path MovedPackage =
		(Project / "Sandbox" / "Content" / "Moved" / "Icon.dasset")
		.lexically_normal();
	ASSERT_TRUE(ResolveSourceHint(ESourceHintBase::AssetRelative,
		"Icon.png", MovedPackage.generic_string(), Resolved));
	EXPECT_EQ(std::filesystem::path(Resolved),
		MovedPackage.parent_path() / "Icon.png");
	ASSERT_TRUE(ResolveSourceHint(ESourceHintBase::ProjectRelative,
		"Sandbox/Sources/Albedo.png", MovedPackage.generic_string(), Resolved));
	EXPECT_EQ(std::filesystem::path(Resolved), ParentRelative);
	ASSERT_TRUE(ResolveSourceHint(ESourceHintBase::Absolute,
		External.generic_string(), MovedPackage.generic_string(), Resolved));
	EXPECT_EQ(std::filesystem::path(Resolved), External);

	EXPECT_FALSE(ResolveSourceHint(ESourceHintBase::ProjectRelative,
		"../Escapes.png", Package.generic_string(), Resolved));
	EXPECT_FALSE(ResolveSourceHint(ESourceHintBase::AssetRelative,
		SameDirectory.generic_string(), Package.generic_string(), Resolved));
	EXPECT_FALSE(ResolveSourceHint(ESourceHintBase::AssetRelative,
		"Icon/./Invalid.png", Package.generic_string(), Resolved));
	EXPECT_FALSE(MakeSourceHint(External.generic_string(), Package.generic_string(),
		Base, Hint, ESourceHintBase::ProjectRelative));
}

TEST_F(FAssetImportDataTests, ValidatesDetachedStateBeforePublication)
{
	auto* Data = Durin::NewObject<Durin::DAssetImportData>(nullptr, "ValidatedImportData");
	ASSERT_NE(Data, nullptr);
	const auto Initial = Data->GetState();
	Durin::FAssetImportDataState State;
	State.SourceData.Sources = {
		MakeSource("source", "TestSources/root.png", "root"),
		MakeSource("dependency", "TestSources/zeta.bin", "zeta")};
	EXPECT_EQ(State.Validate().Error.Code, Durin::EAssetImportDataError::NonCanonicalRoles);
	EXPECT_EQ(Data->GetState(), Initial);
	State.SourceData.Normalize();
	ASSERT_TRUE(State.Validate());
	Data->SetState(State);
	EXPECT_EQ(Data->GetState(), State);
	const auto Identity = Data->GetCompilationIdentity();
	Data->SetState(State);
	EXPECT_EQ(Data->GetCompilationIdentity(), Identity);

	++State.SchemaVersion;
	EXPECT_FALSE(State.Validate());
	EXPECT_EQ(State.Validate().Error.Code, Durin::EAssetImportDataError::UnsupportedSchema);
	EXPECT_EQ(Data->GetCompilationIdentity(), Identity);
}

TEST_F(FAssetImportDataTests, ValidationRetainsOwnedContextAndInspectionPreservesOutput)
{
	using namespace Durin;
	FAssetImportInfo Info;
	Info.Sources = {MakeSource("first", "TestSources/first.png", "first"),
		MakeSource("second", "TestSources/second.png", "second")};
	Info.Sources[1].ContentHashHigh = 0;
	const auto InvalidHash = Info.Validate();
	EXPECT_FALSE(InvalidHash);
	EXPECT_EQ(InvalidHash.Error.Code, EAssetImportDataError::IncompleteHash);
	EXPECT_EQ(InvalidHash.Error.Index, 1u);
	EXPECT_EQ(InvalidHash.Error.ContentHash.HashHigh, 0u);
	Info.Sources[1] = MakeSource("repaired", "TestSources/repaired.png", "repaired");
	EXPECT_EQ(InvalidHash.Error.Role, "second");
	EXPECT_EQ(InvalidHash.Error.Hint, "TestSources/second.png");

	auto Source = Info.Sources.front();
	Source.DisplayLabel.assign(MaximumAssetImportStringBytes + 1, 'x');
	const auto Label = Source.Validate();
	EXPECT_EQ(Label.Error.Code, EAssetImportDataError::InvalidLabel);
	EXPECT_EQ(Label.Error.Actual, MaximumAssetImportStringBytes + 1);
	EXPECT_EQ(Label.Error.Expected, MaximumAssetImportStringBytes);
	Source = Info.Sources.front();
	Source.ByteCount = 0;
	EXPECT_EQ(Source.Validate().Error.Code, EAssetImportDataError::EmptyPayload);
	Info.Sources[1] = {};
	const auto Empty = Info.Validate();
	EXPECT_EQ(Empty.Error.Code, EAssetImportDataError::EmptySource);
	EXPECT_EQ(Empty.Error.Index, 1u);

	FAssetImportDataState State;
	State.SchemaVersion = AssetImportDataSchemaVersion + 1;
	const auto Schema = State.Validate();
	EXPECT_EQ(Schema.Error.Code, EAssetImportDataError::UnsupportedSchema);
	EXPECT_EQ(Schema.Error.Actual, AssetImportDataSchemaVersion + 1);
	EXPECT_EQ(Schema.Error.Expected, AssetImportDataSchemaVersion);

	FAssetImportInfo Output;
	Output.Sources = {MakeSource("preserved", "TestSources/old.png", "old")};
	const auto Before = Output;
	const auto Inspected = InspectAssetImportInfo(FAssetPackageInspection{}, Output);
	EXPECT_FALSE(Inspected);
	EXPECT_EQ(Inspected.Error.Code, EAssetImportDataError::MissingImportReference);
	EXPECT_EQ(Output, Before);
}

TEST_F(FAssetImportDataTests, SourceHintFailuresOwnPathsAndPreserveClearedOutputContract)
{
	using namespace Durin;
	std::string Input = "../outside.png";
	const std::string Package = (std::filesystem::path(FPaths::ProjectDir()) / "Content/Test.dasset").generic_string();
	std::string Output = "stale";
	const auto Escaped = ResolveSourceHint(ESourceHintBase::ProjectRelative, Input, Package, Output);
	EXPECT_FALSE(Escaped);
	EXPECT_EQ(Escaped.Error.Code, ESourceHintError::EscapesProject);
	EXPECT_EQ(Escaped.Error.Operation, ESourceHintOperation::Resolve);
	EXPECT_EQ(Escaped.Error.PackagePath, Package);
	EXPECT_EQ(Escaped.Error.Base, ESourceHintBase::ProjectRelative);
	EXPECT_TRUE(Output.empty());
	Input = "repaired.png";
	EXPECT_EQ(Escaped.Error.Input, "../outside.png");
	const auto Invalid = ResolveSourceHint(ESourceHintBase::AssetRelative, "a/./b.png", Package, Output);
	EXPECT_EQ(Invalid.Error.Code, ESourceHintError::InvalidHint);
	const auto InvalidPackage = ResolveSourceHint(ESourceHintBase::AssetRelative, "b.png", "file.bin", Output);
	EXPECT_EQ(InvalidPackage.Error.Code, ESourceHintError::InvalidPaths);
	ESourceHintBase Base = ESourceHintBase::Absolute;
	Output = "stale";
	const auto Empty = MakeSourceHint({}, Package, Base, Output);
	EXPECT_EQ(Empty.Error.Code, ESourceHintError::EmptyPath);
	EXPECT_TRUE(Output.empty());
	EXPECT_EQ(Base, ESourceHintBase::Absolute);
	const auto InvalidBase = MakeSourceHint(Package, Package, Base, Output, static_cast<ESourceHintBase>(255));
	EXPECT_EQ(InvalidBase.Error.Code, ESourceHintError::InvalidBase);
	EXPECT_EQ(InvalidBase.Error.Base, static_cast<ESourceHintBase>(255));
	EXPECT_TRUE(Output.empty());
	const auto Outside = MakeSourceHint("/DurinExternalSource.png", Package, Base, Output, ESourceHintBase::ProjectRelative);
	EXPECT_EQ(Outside.Error.Code, ESourceHintError::OutsideProject);
	EXPECT_EQ(Outside.Error.Input, "/DurinExternalSource.png");
	EXPECT_EQ(Outside.Error.ProjectPath, FPaths::ProjectDir());
}
