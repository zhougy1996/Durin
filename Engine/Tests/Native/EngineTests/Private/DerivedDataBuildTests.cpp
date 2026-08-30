#include <gtest/gtest.h>

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto GetDerivedDataBuildTestGate() -> FModuleOwnedCallbackGate
	{
		static FModuleTestOwner Context("DerivedDataBuildTests.Functions");
		static auto Registration = Context.CreateOwnedCallbackRegistration(
			"DerivedDataBuildTests.Functions");
		return Registration.GetGate();
	}

	class FScopedDerivedDataCacheDirectory
	{
	public:
		FScopedDerivedDataCacheDirectory()
			: Previous(FPaths::DerivedDataCacheDir()),
			  Root(Testing::GetTestWorkDirectory() / "DerivedDataBuildTests")
		{
			Testing::RemoveTestWorkDirectory(Root);
			FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedDerivedDataCacheDirectory()
		{
			FPaths::SetDerivedDataCacheDirForTests(Previous);
			Testing::RemoveTestWorkDirectory(Root);
		}

	private:
		std::string Previous;
		std::filesystem::path Root;
	};

	class FPolicyTestFunction final : public IBuildFunction
	{
	public:
		mutable uint32 BuildCount = 0;

		auto GetConfig() const -> FBuildFunctionConfig override
		{
			return {.CacheBucket = "DerivedDataBuildTests/Policy",
				.ExpectedValueName = "PolicyOutput", .MaximumValueBytes = 1024};
		}

		auto Validate(const FBuildDefinition&, const FBuildValue& Value,
			std::string& Error) const -> bool override
		{
			const bool bValid = Value.GetName() == "PolicyOutput"
				&& std::ranges::equal(Value.GetBytes(), Bytes({7}));
			if (!bValid) Error = "Policy output is invalid.";
			return bValid;
		}

		auto Build(const FBuildContext&, FBuildValue& Value,
			std::string&) const -> bool override
		{
			++BuildCount;
			Value = FBuildValue::FromOwned("PolicyOutput", Bytes({7}));
			return true;
		}
	};

	class FConfigurableTestFunction final : public IBuildFunction
	{
	public:
		FBuildFunctionConfig Config{
			.CacheBucket = "DerivedDataBuildTests/Config",
			.ExpectedValueName = "ConfigOutput",
			.MaximumValueBytes = 1024,
			.CleanupBudgetBytes = 4096,
			.CleanupDeleteLimit = 2};
		mutable uint32 GetConfigCount = 0;

		auto GetConfig() const -> FBuildFunctionConfig override
		{
			++GetConfigCount;
			return Config;
		}

		auto Validate(const FBuildDefinition&, const FBuildValue& Value,
			std::string& Error) const -> bool override
		{
			const bool bValid = Value.GetName() == "ConfigOutput"
				&& std::ranges::equal(Value.GetBytes(), Bytes({8}));
			if (!bValid) Error = "Config output is invalid.";
			return bValid;
		}

		auto Build(const FBuildContext&, FBuildValue& Value,
			std::string&) const -> bool override
		{
			Value = FBuildValue::FromOwned("ConfigOutput", Bytes({8}));
			return true;
		}
	};

	auto MakePolicyDefinition(char KeyCharacter) -> FBuildDefinition
	{
		FBuildDefinition Definition;
		std::string Error;
		FBuildDefinitionBuilder Builder(
			{"Durin.Tests.PolicyFunction", 1}, "PolicyOutput");
		Builder.SetKey(FBuildKey::FromString(std::string(32, KeyCharacter)))
			.AddInput(FBuildValue::FromOwned("PolicyInput", Bytes({1})));
		requiref(Builder.Build(Definition, &Error), "{}", Error);
		return Definition;
	}
}

TEST(FDerivedDataBuildTests, RegistrationRejectsInvalidCompleteFunctionConfig)
{
	auto ExpectInvalid = [](FBuildFunctionConfig Config, uint32 IdentityVersion) {
		auto Function = std::make_shared<FConfigurableTestFunction>();
		Function->Config = std::move(Config);
		std::string Error;
		EXPECT_FALSE(RegisterBuildFunction(
			{"Durin.Tests.InvalidConfigFunction", IdentityVersion}, Function,
			GetAssetBuildTestGate(), &Error).IsValid());
		EXPECT_EQ(Error, "Build function cache configuration is invalid.");
	};

	FBuildFunctionConfig Config = FConfigurableTestFunction().Config;
	Config.CacheBucket = "../escape";
	ExpectInvalid(Config, 1);
	Config = FConfigurableTestFunction().Config;
	Config.ExpectedValueName = "Invalid/Output";
	ExpectInvalid(Config, 2);
	Config = FConfigurableTestFunction().Config;
	Config.MaximumValueBytes = 0;
	ExpectInvalid(Config, 3);
	Config = FConfigurableTestFunction().Config;
	Config.CleanupDeleteLimit = 0;
	ExpectInvalid(Config, 4);
	Config = FConfigurableTestFunction().Config;
	Config.CleanupBudgetBytes = 0;
	ExpectInvalid(Config, 5);
}

TEST(FDerivedDataBuildTests, RegistrationFreezesValidatedFunctionConfig)
{
	FScopedDerivedDataCacheDirectory CacheDirectory;
	auto Function = std::make_shared<FConfigurableTestFunction>();
	std::string Error;
	auto Registration = RegisterBuildFunction(
		{"Durin.Tests.FrozenConfigFunction", 1}, Function,
		GetAssetBuildTestGate(), &Error);
	ASSERT_TRUE(Registration.IsValid()) << Error;
	ASSERT_EQ(Function->GetConfigCount, 1u);

	Function->Config = {
		.CacheBucket = "../escape",
		.ExpectedValueName = "ChangedOutput",
		.MaximumValueBytes = 1,
		.CleanupBudgetBytes = 0,
		.CleanupDeleteLimit = 0};
	FBuildDefinition Definition;
	FBuildDefinitionBuilder Builder(
		{"Durin.Tests.FrozenConfigFunction", 1}, "ConfigOutput");
	Builder.SetKey(FBuildKey::FromString(std::string(32, 'b')))
		.AddInput(FBuildValue::FromOwned("ConfigInput", Bytes({1})));
	ASSERT_TRUE(Builder.Build(Definition, &Error)) << Error;
	const FBuildOutput Output = FBuildSession().Build(Definition,
		{.bQueryCache = false, .bStoreBuildResult = false});
	EXPECT_EQ(Output.Status, EBuildStatus::Built) << Output.Diagnostic;
	EXPECT_EQ(Function->GetConfigCount, 1u);
}

TEST(FDerivedDataBuildTests, SessionOwnsColdBuildWarmHitAndQueryOnlyMiss)
{
	FScopedDerivedDataCacheDirectory CacheDirectory;
	class FSampleFunction final : public IBuildFunction
	{
	public:
		mutable uint32 BuildCount = 0;
		auto GetConfig() const -> FBuildFunctionConfig override
		{
			return {.CacheBucket = "DerivedDataBuildTests/Session",
				.ExpectedValueName = "SampleOutput", .MaximumValueBytes = 1024,
				.CleanupBudgetBytes = 4096, .CleanupDeleteLimit = 2};
		}
		auto Validate(const FBuildDefinition&, const FBuildValue& Value,
			std::string& Error) const -> bool override
		{
			const bool bValid = Value.GetName() == "SampleOutput"
				&& std::ranges::equal(Value.GetBytes(), Bytes({4, 5, 6}));
			if (!bValid) Error = "Sample output is invalid.";
			return bValid;
		}
		auto Build(const FBuildContext& Context, FBuildValue& Value,
			std::string& Error) const -> bool override
		{
			++BuildCount;
			if (!Context.GetInput("SampleInput")) { Error = "Input missing."; return false; }
			Value = FBuildValue::FromOwned("SampleOutput", Bytes({4, 5, 6}));
			return true;
		}
	};
	auto Function = std::make_shared<FSampleFunction>();
	std::string Error;
	auto Registration = RegisterBuildFunction(
		{"Durin.Tests.SampleFunction", 1}, Function, GetDerivedDataBuildTestGate(), &Error);
	ASSERT_TRUE(Registration.IsValid()) << Error;
	EXPECT_FALSE(RegisterBuildFunction(
		{"Durin.Tests.SampleFunction", 1}, Function, GetDerivedDataBuildTestGate(), &Error).IsValid());
	const std::vector<std::byte> KeyInput = Bytes({1, 2, 3});
	FBuildDefinition Definition;
	FBuildDefinitionBuilder Builder({"Durin.Tests.SampleFunction", 1}, "SampleOutput");
	Builder.SetKey(FBuildKey::FromString(FXxHash128::HashBuffer(KeyInput).ToString()), KeyInput)
		.AddTargetFact("Platform", "Test")
		.AddInput(FBuildValue::FromOwned("SampleInput", Bytes({9})));
	ASSERT_TRUE(Builder.Build(Definition, &Error)) << Error;
	const FBuildOutput Cold = FBuildSession().Build(Definition,
		{.bRequireStoreSuccess = true});
	ASSERT_TRUE(Cold.Succeeded()) << Cold.Diagnostic;
	EXPECT_EQ(Cold.Status, EBuildStatus::Built);
	EXPECT_EQ(Function->BuildCount, 1u);
	EXPECT_GT(Cold.PhaseDurations.CacheQueryNanoseconds, 0u);
	EXPECT_EQ(Cold.PhaseDurations.CachedValueValidationNanoseconds, 0u);
	EXPECT_GT(Cold.PhaseDurations.LocalBuildNanoseconds, 0u);
	EXPECT_GT(Cold.PhaseDurations.BuiltValueValidationNanoseconds, 0u);
	EXPECT_GT(Cold.PhaseDurations.CacheStoreNanoseconds, 0u);
	const FBuildCancellationToken Canceled([] { return true; });
	const FBuildOutput CanceledOutput = FBuildSession().Build(Definition, {}, &Canceled);
	EXPECT_EQ(CanceledOutput.Status, EBuildStatus::Canceled);
	EXPECT_EQ(Function->BuildCount, 1u);
	EXPECT_EQ(CanceledOutput.PhaseDurations.CacheQueryNanoseconds, 0u);
	const FBuildOutput Warm = FBuildSession().Build(Definition,
		{.bRequireStoreSuccess = true});
	ASSERT_TRUE(Warm.Succeeded()) << Warm.Diagnostic;
	EXPECT_EQ(Warm.Status, EBuildStatus::CacheHit);
	EXPECT_EQ(Function->BuildCount, 1u);
	EXPECT_GT(Warm.PhaseDurations.CacheQueryNanoseconds, 0u);
	EXPECT_GT(Warm.PhaseDurations.CachedValueValidationNanoseconds, 0u);
	EXPECT_EQ(Warm.PhaseDurations.LocalBuildNanoseconds, 0u);
	EXPECT_EQ(Warm.PhaseDurations.BuiltValueValidationNanoseconds, 0u);
	EXPECT_EQ(Warm.PhaseDurations.CacheStoreNanoseconds, 0u);

	FBuildDefinition Missing;
	FBuildDefinitionBuilder MissingBuilder({"Durin.Tests.SampleFunction", 1}, "SampleOutput");
	MissingBuilder.SetKey(FBuildKey::FromString(std::string(32, 'e')))
		.AddTargetFact("Platform", "Test");
	ASSERT_TRUE(MissingBuilder.Build(Missing, &Error)) << Error;
	const FBuildOutput QueryOnly = FBuildSession().Build(Missing,
		{.bAllowLocalBuild = false, .bStoreBuildResult = false});
	EXPECT_EQ(QueryOnly.Status, EBuildStatus::CacheMiss);
	EXPECT_GT(QueryOnly.PhaseDurations.CacheQueryNanoseconds, 0u);
	EXPECT_EQ(QueryOnly.PhaseDurations.LocalBuildNanoseconds, 0u);
}

TEST(FDerivedDataBuildTests, DefinitionRejectsDuplicateInputsAndKeyDisagreement)
{
	std::string Error;
	FBuildDefinition Definition;
	FBuildDefinitionBuilder Duplicate({"Durin.Tests.Definition", 1}, "Output");
	Duplicate.SetKey(FBuildKey::FromString(std::string(32, 'a')))
		.AddInput(FBuildValue::FromOwned("Input", Bytes({1})))
		.AddInput(FBuildValue::FromOwned("Input", Bytes({2})));
	EXPECT_FALSE(Duplicate.Build(Definition, &Error));
	FBuildDefinitionBuilder Mismatch({"Durin.Tests.Definition", 1}, "Output");
	Mismatch.SetKey(FBuildKey::FromString(std::string(32, 'a')), Bytes({7}));
	EXPECT_FALSE(Mismatch.Build(Definition, &Error));
}

TEST(FDerivedDataBuildTests, NumericTargetFactsUseExactBoundedUnsignedDecimalSyntax)
{
	uint32 Value = 99;
	EXPECT_TRUE(ParseBuildTargetFactUInt32("0", Value));
	EXPECT_EQ(Value, 0u);
	EXPECT_TRUE(ParseBuildTargetFactUInt32("001", Value));
	EXPECT_EQ(Value, 1u);
	EXPECT_TRUE(ParseBuildTargetFactUInt32("4294967295", Value));
	EXPECT_EQ(Value, std::numeric_limits<uint32>::max());
	EXPECT_FALSE(ParseBuildTargetFactUInt32("", Value));
	EXPECT_FALSE(ParseBuildTargetFactUInt32("-1", Value));
	EXPECT_FALSE(ParseBuildTargetFactUInt32("1x", Value));
	EXPECT_FALSE(ParseBuildTargetFactUInt32("4294967296", Value));
}

TEST(FDerivedDataBuildTests, SessionHonorsExplicitQueryAndStorePolicies)
{
	FScopedDerivedDataCacheDirectory CacheDirectory;
	auto Function = std::make_shared<FPolicyTestFunction>();
	std::string Error;
	auto Registration = RegisterBuildFunction(
		{"Durin.Tests.PolicyFunction", 1}, Function, GetDerivedDataBuildTestGate(), &Error);
	ASSERT_TRUE(Registration.IsValid()) << Error;
	const FBuildDefinition Definition = MakePolicyDefinition('a');
	const FBuildPolicy LocalOnly{
		.bQueryCache = false, .bStoreBuildResult = false};
	EXPECT_EQ(FBuildSession().Build(Definition, LocalOnly).Status, EBuildStatus::Built);
	EXPECT_EQ(FBuildSession().Build(Definition, LocalOnly).Status, EBuildStatus::Built);
	EXPECT_EQ(Function->BuildCount, 2u);
	const FBuildPolicy StoreOnly{
		.bQueryCache = false, .bStoreBuildResult = true, .bRequireStoreSuccess = true};
	EXPECT_EQ(FBuildSession().Build(Definition, StoreOnly).Status, EBuildStatus::Built);
	EXPECT_EQ(Function->BuildCount, 3u);
	const FBuildPolicy QueryOnly{
		.bAllowLocalBuild = false, .bStoreBuildResult = false};
	EXPECT_EQ(FBuildSession().Build(Definition, QueryOnly).Status, EBuildStatus::CacheHit);
	EXPECT_EQ(Function->BuildCount, 3u);
}

TEST(FDerivedDataBuildTests, CacheRequiredAndBestEffortWritePoliciesDiffer)
{
	FScopedDerivedDataCacheDirectory CacheDirectory;
	const std::filesystem::path BlockedRoot =
		Testing::GetTestWorkDirectory() / "DerivedDataBuildTestsBlockedRoot";
	Testing::RemoveTestWorkDirectory(BlockedRoot);
	const std::array<uint8, 1> BlockedBytes{0xff};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(BlockedBytes)), BlockedRoot));
	FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
	auto Function = std::make_shared<FPolicyTestFunction>();
	std::string Error;
	auto Registration = RegisterBuildFunction(
		{"Durin.Tests.PolicyFunction", 1}, Function, GetDerivedDataBuildTestGate(), &Error);
	ASSERT_TRUE(Registration.IsValid()) << Error;
	const FBuildOutput BestEffort = FBuildSession().Build(MakePolicyDefinition('c'),
		{.bQueryCache = false});
	EXPECT_EQ(BestEffort.Status, EBuildStatus::Built);
	EXPECT_FALSE(BestEffort.StoreDiagnostic.empty());
	const FBuildOutput Required = FBuildSession().Build(MakePolicyDefinition('d'),
		{.bQueryCache = false, .bRequireStoreSuccess = true});
	EXPECT_EQ(Required.Status, EBuildStatus::Failed);
	EXPECT_EQ(Required.FailurePhase, EBuildFailurePhase::CacheStore);
	EXPECT_FALSE(Required.Diagnostic.empty());
}
