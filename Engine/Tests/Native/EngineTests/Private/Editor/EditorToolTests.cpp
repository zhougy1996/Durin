#include "Tools/EditorToolSubsystem.h"
#include "Tools/NativeToolset.h"
#include "Editor/EditorEngineTestSupport.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "NativeDObjectTestSupport.h"
#include "Transactions/Transactor.h"

#include <gtest/gtest.h>


namespace
{
	using namespace Durin;
	// Service tests own their test namespaces; built-in PIE discovery is covered by Console tests.
	auto WithoutBuiltins(FEditorToolCatalog Catalog) -> FEditorToolCatalog
	{
		std::erase_if(Catalog.Toolsets, [](const auto& Entry) { return Entry.Descriptor.Name == "editor.pie"; });
		return Catalog;
	}

	auto Descriptor(std::string Name = "echo") -> FToolDescriptor
	{
		FToolDescriptor Result;
		Result.Name = std::move(Name);
		Result.bExposeExternally = true;
		Result.Input.Properties.emplace("enabled", FToolSchema(EToolValueType::Boolean));
		Result.Input.Properties.at("enabled").Default = true;
		Result.Input.Required = {"enabled"};
		Result.Output = Result.Input;
		return Result;
	}
	class FTestToolset final : public IToolset
	{
	public:
		std::string Name = "tests";
		std::string Description = "Test tools";
		std::vector<FToolDescriptor> Descriptors{Descriptor()};
		mutable int Descriptions = 0;
		int Executions = 0;
		bool ThrowDescription = false;
		std::function<FToolResult(const FToolRequest&, const FToolContext&)> Action;
		std::function<FToolFuture(const FToolRequest&, const FToolContext&)> AsyncAction;
		std::function<FToolAvailability(const FToolRequest&, const FToolContext&)> Availability;
		auto DescribeToolset() const -> FToolsetDescriptor override
		{
			++Descriptions;
			if (ThrowDescription) throw std::runtime_error("description failure");
			return {Name, Description, Descriptors};
		}
		auto CheckAvailability(const FToolRequest& Request, const FToolContext& Context) -> FToolAvailability override
		{
			return Availability ? Availability(Request, Context) : FToolAvailability{};
		}
		auto Call(const FToolRequest& Request, const FToolContext& Context) -> FToolFuture override
		{
			++Executions;
			return AsyncAction ? AsyncAction(Request, Context) : ReadyToolResult(Action ? Action(Request, Context) : FToolResult{EToolStatus::Succeeded, Request.Arguments});
		}
	};
	class FEditorToolTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override { Testing::InitializeDObjectSystemForTests(); }
		auto TearDown() -> void override
		{
			for (auto& Host : Hosts) { Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); }
			Hosts.clear(); CollectGarbage();
		}
		auto MakeHost() -> DEditorEngine*
		{
			auto* Host = NewObject<DEditorEngine>(nullptr, {});
			Hosts.emplace_back(Host);
			EXPECT_TRUE(FEditorEngineTestAccess::StartEditor(*Host));
			return Host;
		}
		static auto Request(std::string Name = "echo") -> FToolRequest { return {{"tests", std::move(Name)}, 1}; }
		std::vector<TStrongObjectPtr<DEditorEngine>> Hosts;
	};
}

TEST_F(FEditorToolTests, PublishesSortedOwnedCatalogsAndRefreshesOnlyAfterRegistrationChanges)
{
	FTestToolset Toolset;
	Toolset.Descriptors = {Descriptor("zeta"), Descriptor("alpha")};
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Host = MakeHost();
	auto* Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
	ASSERT_NE(nullptr, Tools);
	auto Catalog = WithoutBuiltins(Tools->ListToolsets());
	ASSERT_EQ(1u, Catalog.Toolsets.size());
	ASSERT_EQ(2u, Catalog.Toolsets[0].Descriptor.Tools.size());
	EXPECT_EQ("alpha", Catalog.Toolsets[0].Descriptor.Tools[0].Name);
	EXPECT_EQ("zeta", Catalog.Toolsets[0].Descriptor.Tools[1].Name);
	const auto Descriptions = Toolset.Descriptions;
	EXPECT_EQ(Catalog.Revision, WithoutBuiltins(Tools->ListToolsets()).Revision);
	EXPECT_EQ(Descriptions, Toolset.Descriptions);
	Token.Reset();
	EXPECT_TRUE(WithoutBuiltins(Tools->ListToolsets()).Toolsets.empty());
	EXPECT_GT(WithoutBuiltins(Tools->ListToolsets()).Revision, Catalog.Revision);
	EXPECT_EQ("alpha", Catalog.Toolsets.front().Descriptor.Tools.front().Name);
	EXPECT_FALSE(Tools->DescribeTool({"tests", "alpha"}));
}

TEST_F(FEditorToolTests, RefusesDuplicateNamesIncludingInvalidCandidatesAndContainsDescriptionExceptions)
{
	FTestToolset First, Second, Broken;
	Second.Descriptors.front().Input.Type = EToolValueType::Boolean;
	Broken.ThrowDescription = true;
	auto A = FModularFeatureRegistry::Get().RegisterFeature(First);
	auto B = FModularFeatureRegistry::Get().RegisterFeature(Second);
	auto C = FModularFeatureRegistry::Get().RegisterFeature(Broken);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Catalog = WithoutBuiltins(Tools->ListToolsets());
	EXPECT_TRUE(Catalog.Toolsets.empty());
	EXPECT_FALSE(Catalog.Diagnostics.empty());
	EXPECT_EQ(EToolStatus::Failed, Tools->CallNow(Request()).Status);
	EXPECT_EQ(0, First.Executions);
	EXPECT_EQ(0, Second.Executions);
	A.Reset(); B.Reset(); C.Reset();
	B = FModularFeatureRegistry::Get().RegisterFeature(Second);
	A = FModularFeatureRegistry::Get().RegisterFeature(First);
	EXPECT_TRUE(WithoutBuiltins(Tools->ListToolsets()).Toolsets.empty());
	B.Reset();
	EXPECT_EQ(1u, WithoutBuiltins(Tools->ListToolsets()).Toolsets.size());
}

TEST_F(FEditorToolTests, NativeAndCrossThreadCallsShareNormalizationAndExplicitHostContext)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* First = MakeHost();
	auto* Tools = First->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FToolContext Copy;
	Toolset.Action = [&](const auto& Input, const auto& Context) {
		EXPECT_EQ(First, GetToolEditor(Context));
		Copy = Context;
		EXPECT_EQ(nullptr, GetToolEditor(Copy));
		EXPECT_EQ(EToolStatus::Failed, Tools->CallNow(Request()).Status);
		return FToolResult{EToolStatus::Succeeded, Input.Arguments};
	};
	auto Direct = Tools->CallNow(Request());
	ASSERT_EQ(EToolStatus::Succeeded, Direct.Status);
	EXPECT_EQ(nullptr, GetToolEditor(Copy));
	std::optional<FEditorToolInvocationId> Id;
	std::thread Worker([&] {
		EXPECT_EQ(1u, WithoutBuiltins(Endpoint->GetCatalog()).Toolsets.size());
		auto Submitted = Endpoint->Submit(Request(), EToolSource::External);
		EXPECT_TRUE(Submitted.has_value());
		if (Submitted) Id = *Submitted;
	});
	Worker.join(); ASSERT_TRUE(Id.has_value());
	EXPECT_EQ(EToolInvocationState::Queued, Endpoint->GetInvocation(*Id)->State);
	First->Tick(0.0f, false); // Exercise the production editor tick safe point.
	auto Completed = Endpoint->GetInvocation(*Id);
	ASSERT_TRUE(Completed.has_value());
	ASSERT_TRUE(Completed->Result);
	EXPECT_EQ(Direct.Data, Completed->Result->Data);
	EXPECT_EQ(2, Toolset.Executions);
	EXPECT_EQ(Endpoint->GetInvocation(*Id)->State, EToolInvocationState::Completed);
}

TEST_F(FEditorToolTests, AvailabilityAndArgumentFailuresNeverExecuteAndWrongThreadIsRejected)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	Toolset.Availability = [](const auto&, const auto&) { return FToolAvailability{false, {}}; };
	EXPECT_FALSE(Tools->CheckAvailability(Request()).bAvailable);
	EXPECT_EQ(EToolError::ToolUnavailable, Tools->CallNow(Request()).Diagnostics.front().Code);
	Toolset.Availability = {};
	auto Invalid = Request(); Invalid.Arguments = FToolValue::FObject{{"enabled", "wrong"}};
	EXPECT_EQ(EToolError::InvalidArguments, Tools->CallNow(Invalid).Diagnostics.front().Code);
	Invalid = Request(); Invalid.ContractVersion = 2;
	EXPECT_EQ(EToolError::VersionMismatch, Tools->GetService()->Submit(Invalid).error().Code);
	std::thread Worker([&] {
		EXPECT_EQ(EToolError::WrongThread, Tools->CallNow(Request()).Diagnostics.front().Code);
		EXPECT_EQ(EToolError::WrongThread, Tools->CheckAvailability(Request()).Diagnostics.front().Code);
		EXPECT_EQ(EToolError::WrongThread, WithoutBuiltins(Tools->ListToolsets()).Diagnostics.front().Code);
	});
	Worker.join();
	EXPECT_EQ(0, Toolset.Executions);
}

TEST_F(FEditorToolTests, RejectsExternalExposureAndBindsQueuedCallsToTheOriginalRegistration)
{
	FTestToolset Toolset;
	Toolset.Descriptors.front().bExposeExternally = false;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	EXPECT_EQ(EToolError::ToolUnavailable, Endpoint->Submit(Request(), EToolSource::External).error().Code);
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id.has_value());
	Token.Reset(); Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	Tools->TickTools();
	auto Result = Endpoint->GetInvocation(*Id);
	ASSERT_TRUE(Result && Result->Result);
	EXPECT_EQ(EToolError::ToolUnavailable, Result->Result->Diagnostics.front().Code);
	EXPECT_EQ(0, Toolset.Executions);
	EXPECT_EQ(EToolStatus::Succeeded, Tools->CallNow(Request()).Status);
}

TEST_F(FEditorToolTests, EnforcesQueueCapacityCancellationAndPerTickDispatchBudget)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxQueued = 2; Limits.MaxDispatchPerTick = 1; Limits.TickBudget = std::chrono::seconds(1);
	ASSERT_TRUE(Endpoint->Configure(Limits));
	(void)WithoutBuiltins(Tools->ListToolsets());
	auto First = Endpoint->Submit(Request()); auto Second = Endpoint->Submit(Request());
	ASSERT_TRUE(First && Second);
	EXPECT_EQ(EToolError::Busy, Endpoint->Submit(Request()).error().Code);
	EXPECT_TRUE(Endpoint->Cancel(*First));
	EXPECT_EQ(EToolInvocationState::Cancelled, Endpoint->GetInvocation(*First)->State);
	auto Third = Endpoint->Submit(Request()); ASSERT_TRUE(Third);
	Tools->TickTools();
	EXPECT_EQ(1, Toolset.Executions);
	EXPECT_EQ(EToolInvocationState::Queued, Endpoint->GetInvocation(*Third)->State);
	Tools->TickTools();
	EXPECT_EQ(2, Toolset.Executions);
	Limits.MaxQueuedBytes = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)WithoutBuiltins(Tools->ListToolsets());
	EXPECT_EQ(EToolError::Busy, Endpoint->Submit(Request()).error().Code);
}

TEST_F(FEditorToolTests, ClientLifetimeIsIndependentFromInvocationCancellationAndBlocksQueuedExecution)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService(); (void)Tools->ListToolsets();
	FToolCancellationSource ClientLifetime;
	FToolClientContext Client{"client-a", ClientLifetime.GetToken()};
	FToolCancellationToken Captured;
	Toolset.Action = [&](const auto& Input, const FToolContext& Context) {
		EXPECT_EQ("client-a", Context.Client.Id); EXPECT_FALSE(Context.Client.Cancellation.IsCancellationRequested());
		Captured = Context.Client.Cancellation;
		return FToolResult{EToolStatus::Succeeded, Input.Arguments};
	};
	auto First = Endpoint->Submit(Request(), EToolSource::External, Client); ASSERT_TRUE(First);
	Tools->TickTools(); ASSERT_TRUE(Endpoint->GetInvocation(*First)->Result);
	ASSERT_TRUE(Endpoint->Cancel(*First)); EXPECT_FALSE(Captured.IsCancellationRequested());
	auto Queued = Endpoint->Submit(Request(), EToolSource::External, Client); ASSERT_TRUE(Queued);
	ClientLifetime.Cancel(); EXPECT_TRUE(Captured.IsCancellationRequested());
	Tools->TickTools(); EXPECT_EQ(1, Toolset.Executions);
	EXPECT_EQ(EToolError::Cancelled, Endpoint->GetInvocation(*Queued)->Result->Diagnostics.front().Code);
	EXPECT_EQ(EToolError::Cancelled, Endpoint->Submit(Request(), EToolSource::External, Client).error().Code);
}

TEST_F(FEditorToolTests, DistinguishesExpiredUnknownAndActiveInvocationsWithoutTombstones)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxCompleted = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)WithoutBuiltins(Tools->ListToolsets());
	auto First = Endpoint->Submit(Request()); ASSERT_TRUE(First);
	Tools->TickTools();
	auto Second = Endpoint->Submit(Request()); ASSERT_TRUE(Second);
	Tools->TickTools();
	EXPECT_EQ(EToolInvocationState::Expired, Endpoint->GetInvocation(*First)->State);
	EXPECT_EQ(EToolError::InvocationExpired, Endpoint->Cancel(*First).error().Code);
	EXPECT_EQ(EToolInvocationState::Completed, Endpoint->GetInvocation(*Second)->State);
	EXPECT_EQ(EToolError::TargetNotFound, Endpoint->GetInvocation({Second->Session, Second->Sequence + 1}).error().Code);
	Limits.ResultTTL = std::chrono::milliseconds(0);
	ASSERT_TRUE(Endpoint->Configure(Limits));
	EXPECT_EQ(EToolInvocationState::Expired, Endpoint->GetInvocation(*Second)->State);
}

TEST_F(FEditorToolTests, BoundsOutputsAndContainsProviderExceptionsWithoutPoisoningDispatch)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	Toolset.Action = [](const auto&, const auto&) -> FToolResult { throw std::runtime_error("execute failure"); };
	EXPECT_EQ(EToolError::InternalError, Tools->CallNow(Request()).Diagnostics.front().Code);
	Toolset.Action = [](const auto&, const auto&) { return FToolResult{EToolStatus::Succeeded, "wrong output"}; };
	EXPECT_EQ(EToolError::InvalidOutput, Tools->CallNow(Request()).Diagnostics.front().Code);
	Toolset.Action = [](const auto& Input, const auto&) {
		FToolResult Result{EToolStatus::Succeeded, Input.Arguments}; Result.Diagnostics.resize(65); return Result;
	};
	EXPECT_EQ(EToolError::LimitExceeded, Tools->CallNow(Request()).Diagnostics.front().Code);
	Toolset.Action = {};
	EXPECT_EQ(EToolStatus::Succeeded, Tools->CallNow(Request()).Status);
	EXPECT_FALSE(FModularFeatureRegistry::Get().IsDispatching());
}

TEST_F(FEditorToolTests, RunningCancellationIsCooperativeAndNeverWaitsForGameThread)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id);
	Toolset.Action = [&](const auto&, const auto& Context) {
		std::thread Worker([&] {
			EXPECT_EQ(EToolInvocationState::Running, Endpoint->GetInvocation(*Id)->State);
			EXPECT_TRUE(Endpoint->Cancel(*Id));
		});
		Worker.join();
		EXPECT_TRUE(Context.Cancellation.IsCancellationRequested());
		return FToolResult{EToolStatus::Failed, {}, {{EToolError::Cancelled, "cooperative", {}}}};
	};
	Tools->TickTools();
	EXPECT_EQ(EToolError::Cancelled, Endpoint->GetInvocation(*Id)->Result->Diagnostics.front().Code);
}

TEST_F(FEditorToolTests, ImmediateCallsDoNotConsumeOrRequireAQueueSlot)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxQueued = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)WithoutBuiltins(Tools->ListToolsets());
	auto Queued = Endpoint->Submit(Request()); ASSERT_TRUE(Queued);
	EXPECT_EQ(EToolStatus::Succeeded, Tools->CallNow(Request()).Status);
	EXPECT_EQ(EToolInvocationState::Queued, Endpoint->GetInvocation(*Queued)->State);
	EXPECT_TRUE(Endpoint->Cancel(*Queued));
}

TEST_F(FEditorToolTests, BoundsCatalogAndResultStorageAndPreflightsDetachedSubmissions)
{
	FTestToolset Toolset;
	Toolset.Descriptors.push_back(Descriptor("second"));
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxCatalogTools = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits));
	auto Catalog = WithoutBuiltins(Tools->ListToolsets());
	EXPECT_TRUE(Catalog.Toolsets.empty());
	ASSERT_FALSE(Catalog.Diagnostics.empty());
	EXPECT_EQ(EToolError::LimitExceeded, Catalog.Diagnostics.front().Code);
	Limits.MaxCatalogTools = 256; Limits.MaxCatalogBytes = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits));
	EXPECT_TRUE(WithoutBuiltins(Tools->ListToolsets()).Toolsets.empty());
	Limits.MaxCatalogBytes = 16 * 1024 * 1024;
	Limits.MaxCatalogTools = 256; Limits.MaxCompletedBytes = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)WithoutBuiltins(Tools->ListToolsets());
	auto Invalid = Request(); Invalid.Arguments = FToolValue::FObject{{"enabled", std::string(65537, 'x')}};
	EXPECT_EQ(EToolError::LimitExceeded, Endpoint->Submit(Invalid).error().Code);
	EXPECT_EQ(EToolError::Busy, Endpoint->Submit(Request()).error().Code);
	EXPECT_EQ(0, Toolset.Executions);
}

TEST_F(FEditorToolTests, ChargesReservedContainerCapacityAndBoundsAdmissionDiagnostics)
{
	FTestToolset Toolset;
	Toolset.Descriptors.front().Output = FToolSchema(EToolValueType::Array);
	Toolset.Descriptors.front().Output.Items = std::make_unique<FToolSchema>(EToolValueType::Boolean);
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxCompletedBytes = 4096;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)WithoutBuiltins(Tools->ListToolsets());
	Toolset.Action = [](const auto&, const auto&) {
		FToolValue::FArray Data; Data.reserve(10000); Data.emplace_back(true);
		return FToolResult{EToolStatus::Succeeded, std::move(Data)};
	};
	EXPECT_EQ(EToolError::LimitExceeded, Tools->CallNow(Request()).Diagnostics.front().Code);
	Toolset.Action = [](const auto&, const auto&) { return FToolResult{EToolStatus::Succeeded, FToolValue::FArray{true}}; };
	EXPECT_EQ(EToolStatus::Succeeded, Tools->CallNow(Request()).Status);
	Token.Reset(); Toolset.Descriptors.front().Output = FToolSchema(EToolValueType::String);
	Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	Toolset.Action = [](const auto&, const auto&) {
		std::string Data = "x"; Data.reserve(65536);
		return FToolResult{EToolStatus::Succeeded, std::move(Data)};
	};
	EXPECT_EQ(EToolError::LimitExceeded, Tools->CallNow(Request()).Diagnostics.front().Code);
	auto Invalid = Request(); Invalid.Arguments = FToolValue::FObject{{std::string(2048, 'x'), true}};
	auto Rejected = Endpoint->Submit(Invalid);
	ASSERT_FALSE(Rejected.has_value());
	EXPECT_LE(Rejected.error().Path.size(), 1024u);
	EXPECT_LE(Rejected.error().Message.size(), 4096u);
}

TEST_F(FEditorToolTests, ClosingAdmissionDuringAvailabilityNeverInvokesTheTool)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Host = MakeHost();
	auto* Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
	Toolset.Availability = [&](const auto&, const auto& Context) {
		Host->PrepareForShutdown();
		EXPECT_EQ(Host, GetToolEditor(Context));
		return FToolAvailability{};
	};
	auto Result = Tools->CallNow(Request());
	ASSERT_EQ(EToolStatus::Failed, Result.Status);
	EXPECT_EQ(EToolError::Cancelled, Result.Diagnostics.front().Code);
	EXPECT_EQ(0, Toolset.Executions);
	Host->PrepareForShutdown();
}

TEST_F(FEditorToolTests, JoinsRecordingAndKeepsDetachedEndpointSafeAfterShutdownAndGC)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Host = MakeHost();
	auto* Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id);
	{
		Editor::FScopedTransaction Transaction(Host->GetTransactor(), {"tool test"});
		Tools->TickTools();
		EXPECT_EQ(EToolInvocationState::Completed, Endpoint->GetInvocation(*Id)->State);
		EXPECT_EQ(1, Toolset.Executions);
	}
	auto Pending = Endpoint->Submit(Request()); ASSERT_TRUE(Pending);
	Host->PrepareForShutdown();
	Hosts.clear(); MarkObjectHierarchyAsGarbage(Host); CollectGarbage();
	std::thread Worker([&] {
		EXPECT_TRUE(WithoutBuiltins(Endpoint->GetCatalog()).Toolsets.empty());
		EXPECT_EQ(EToolInvocationState::Cancelled, Endpoint->GetInvocation(*Pending)->State);
		EXPECT_FALSE(Endpoint->Submit(Request()));
	});
	Worker.join();
	EXPECT_EQ(1, Toolset.Executions);
}

TEST_F(FEditorToolTests, ShutdownDuringProviderCallbackDefersHostDestructionAndCancelsLaterWork)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Host = MakeHost();
	auto* Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	auto First = Endpoint->Submit(Request()); auto Second = Endpoint->Submit(Request()); ASSERT_TRUE(First && Second);
	Toolset.Action = [&](const auto& Input, const auto& Context) {
		Host->PrepareForShutdown();
		EXPECT_EQ(Host, GetToolEditor(Context));
		EXPECT_NE(nullptr, Host->GetEditorSubsystem<DEditorToolSubsystem>());
		return FToolResult{EToolStatus::Succeeded, Input.Arguments};
	};
	Host->Tick(0.0f, false);
	EXPECT_EQ(1, Toolset.Executions);
	EXPECT_EQ(nullptr, Host->GetEditorSubsystem<DEditorToolSubsystem>());
	EXPECT_EQ(EToolInvocationState::Cancelled, Endpoint->GetInvocation(*Second)->State);
}

TEST_F(FEditorToolTests, GroupsToolsetsAndRoutesSameLocalNameByBothAddressParts)
{
	FTestToolset Alpha, Beta;
	Alpha.Name = "alpha"; Beta.Name = "beta";
	auto B = FModularFeatureRegistry::Get().RegisterFeature(Beta);
	auto A = FModularFeatureRegistry::Get().RegisterFeature(Alpha);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Catalog = WithoutBuiltins(Tools->ListToolsets());
	ASSERT_EQ(2u, Catalog.Toolsets.size());
	EXPECT_EQ("alpha", Catalog.Toolsets[0].Descriptor.Name);
	EXPECT_EQ("beta", Catalog.Toolsets[1].Descriptor.Name);
	EXPECT_EQ("echo", Catalog.Toolsets[0].Descriptor.Tools.front().Name);
	EXPECT_TRUE(Tools->DescribeToolset("alpha"));
	EXPECT_TRUE(Tools->DescribeTool({"beta", "echo"}));
	auto Input = Request(); Input.Address.Toolset = "alpha";
	EXPECT_EQ(EToolStatus::Succeeded, Tools->CallNow(Input).Status);
	EXPECT_EQ(1, Alpha.Executions); EXPECT_EQ(0, Beta.Executions);
	Input.Address.Toolset = "beta";
	auto Id = Tools->GetService()->Submit(Input); ASSERT_TRUE(Id);
	Tools->TickTools();
	EXPECT_EQ(1, Beta.Executions);
	Input.Address.Toolset = "missing";
	EXPECT_EQ(EToolError::ToolUnavailable, Tools->CallNow(Input).Diagnostics.front().Code);
	Input.Address.Toolset = "bad/name";
	EXPECT_EQ(EToolError::InvalidArguments, Tools->GetService()->Submit(Input).error().Code);
}

TEST_F(FEditorToolTests, RefusesDuplicateLocalToolsButKeepsUnambiguousToolsInTheGroup)
{
	FTestToolset Toolset;
	auto Invalid = Descriptor(); Invalid.Input.Type = EToolValueType::Boolean;
	Toolset.Descriptors = {Invalid, Descriptor(), Descriptor("other")};
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Set = Tools->DescribeToolset("tests"); ASSERT_TRUE(Set);
	ASSERT_EQ(1u, Set->Descriptor.Tools.size());
	EXPECT_EQ("other", Set->Descriptor.Tools.front().Name);
	EXPECT_FALSE(Tools->DescribeTool({"tests", "echo"}));
	Token.Reset();
	std::reverse(Toolset.Descriptors.begin(), Toolset.Descriptors.end());
	Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	EXPECT_FALSE(Tools->DescribeTool({"tests", "echo"}));
	EXPECT_TRUE(Tools->DescribeTool({"tests", "other"}));
}

TEST_F(FEditorToolTests, RejectsDottedLocalToolsAtDiscoveryAndSubmission)
{
	FTestToolset Toolset; Toolset.Name = "editor.tests";
	Toolset.Descriptors = {Descriptor("nested.echo"), Descriptor("set_enabled")};
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Set = Tools->DescribeToolset("editor.tests"); ASSERT_TRUE(Set);
	ASSERT_EQ(1u, Set->Descriptor.Tools.size());
	EXPECT_EQ("set_enabled", Set->Descriptor.Tools.front().Name);
	auto Invalid = Tools->GetService()->Submit({{"editor.tests", "nested.echo"}, 1});
	ASSERT_FALSE(Invalid); EXPECT_EQ(EToolError::InvalidArguments, Invalid.error().Code);
	EXPECT_TRUE(Tools->GetService()->Submit({{"editor.tests", "set_enabled"}, 1}));
	EXPECT_EQ(0, Toolset.Executions);
}

TEST_F(FEditorToolTests, InvalidGroupMetadataStillBlocksDuplicateNamespaceInEitherOrder)
{
	FTestToolset Good, Bad;
	Bad.Description = std::string(65537, 'x');
	auto A = FModularFeatureRegistry::Get().RegisterFeature(Good);
	auto B = FModularFeatureRegistry::Get().RegisterFeature(Bad);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	EXPECT_TRUE(WithoutBuiltins(Tools->ListToolsets()).Toolsets.empty());
	A.Reset(); B.Reset();
	B = FModularFeatureRegistry::Get().RegisterFeature(Bad);
	A = FModularFeatureRegistry::Get().RegisterFeature(Good);
	EXPECT_TRUE(WithoutBuiltins(Tools->ListToolsets()).Toolsets.empty());
}

namespace
{
	DEditorEngine* TypedHost = nullptr;
	auto NativeHostTool(const FToolContext& Context, int32 Value) -> int32
	{
		EXPECT_EQ(TypedHost, GetToolEditor(Context));
		return Value + 1;
	}
}

TEST_F(FEditorToolTests, ExplicitNativeBindingsShareTheEditorServiceAndQueue)
{
	auto Binding = BindTool(FToolDescriptor{.Name = "increment", .bExposeExternally = true}, &NativeHostTool,
		std::array{FToolParameter{"value", "Input", FToolValue{41}}});
	FNativeToolset Set("native", "Explicit native functions", {std::move(Binding)});
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Set);
	TypedHost = MakeHost();
	auto* Tools = TypedHost->GetEditorSubsystem<DEditorToolSubsystem>();
	FToolRequest Input{{"native", "increment"}, 1};
	auto Direct = Tools->CallNow(Input);
	ASSERT_EQ(EToolStatus::Succeeded, Direct.Status);
	EXPECT_EQ(FToolValue(42), Direct.Data);
	std::optional<FEditorToolInvocationId> Id;
	auto Endpoint = Tools->GetService();
	std::thread Worker([&] { auto Queued = Endpoint->Submit(Input, EToolSource::External); ASSERT_TRUE(Queued); Id = *Queued; });
	Worker.join(); ASSERT_TRUE(Id);
	Tools->TickTools();
	auto Receipt = Endpoint->GetInvocation(*Id); ASSERT_TRUE(Receipt && Receipt->Result);
	EXPECT_EQ(Direct.Data, Receipt->Result->Data);
	EXPECT_EQ(nullptr, GetToolEditor({}));
	TypedHost = nullptr;
}

TEST_F(FEditorToolTests, BoundsEmptyToolsetCountAndPublishesEmptyGroups)
{
	FTestToolset Alpha, Beta; Alpha.Name = "alpha"; Beta.Name = "beta";
	Alpha.Descriptors.clear(); Beta.Descriptors.clear();
	auto A = FModularFeatureRegistry::Get().RegisterFeature(Alpha);
	auto B = FModularFeatureRegistry::Get().RegisterFeature(Beta);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	ASSERT_EQ(2u, WithoutBuiltins(Tools->ListToolsets()).Toolsets.size());
	FEditorToolLimits Limits; Limits.MaxCatalogToolsets = 1;
	ASSERT_TRUE(Tools->GetService()->Configure(Limits));
	auto Catalog = WithoutBuiltins(Tools->ListToolsets());
	EXPECT_TRUE(Catalog.Toolsets.empty());
	ASSERT_FALSE(Catalog.Diagnostics.empty());
	EXPECT_EQ(EToolError::LimitExceeded, Catalog.Diagnostics.front().Code);
}

TEST_F(FEditorToolTests, PendingCompletionPublishesOnlyOnTickAndSynchronousAdmissionPrecedesEffects)
{
	FTestToolset Toolset;
	Toolset.Descriptors.front().bMayPend = true;
	auto Source = FToolCompletionSource::CreateDetached();
	auto Raw = Tasks::Share(Source.TakeTask());
	FToolContext Copied;
	int AvailabilityCalls = 0;
	Toolset.Availability = [&](const auto&, const auto&) { ++AvailabilityCalls; return FToolAvailability{}; };
	Toolset.AsyncAction = [&](const auto&, const auto& Context) { Copied = Context; return Raw; };
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	EXPECT_EQ(EToolError::Busy, Tools->CallNow(Request()).Diagnostics.front().Code);
	EXPECT_EQ(0, AvailabilityCalls); EXPECT_EQ(0, Toolset.Executions);
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id);
	auto Future = Endpoint->RetainInvocation(*Id); ASSERT_TRUE(Future);
	Tools->TickTools();
	EXPECT_EQ(EToolInvocationState::Running, Endpoint->GetInvocation(*Id)->State);
	EXPECT_FALSE(Future->GetResultShared());
	EXPECT_EQ(EToolError::Busy, Endpoint->Configure({}).error().Code);
	EXPECT_TRUE(Endpoint->Cancel(*Id));
	EXPECT_TRUE(Copied.Cancellation.IsCancellationRequested());
	EXPECT_EQ(nullptr, GetToolEditor(Copied));
	std::thread Producer([Source] {
		EXPECT_TRUE(Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{{"enabled", true}}}));
		EXPECT_FALSE(Source.TrySetValue({EToolStatus::Failed}));
	});
	Producer.join();
	EXPECT_FALSE(Future->GetResultShared());
	Tools->TickTools();
	ASSERT_TRUE(Future->GetResultShared());
	EXPECT_EQ(EToolStatus::Succeeded, Future->GetResultShared()->Status);
	EXPECT_EQ(Endpoint->GetInvocation(*Id)->Result->Data, Future->GetResultShared()->Data);
	EXPECT_EQ(1, Toolset.Executions);
}

TEST_F(FEditorToolTests, SynchronousPendingViolationFailsOnceAfterEffectsWithoutRetry)
{
	FTestToolset Toolset;
	auto Source = FToolCompletionSource::CreateDetached();
	auto Raw = Tasks::Share(Source.TakeTask());
	Toolset.AsyncAction = [&](const auto&, const auto&) { return Raw; };
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id);
	Tools->TickTools();
	auto Future = Endpoint->RetainInvocation(*Id); ASSERT_TRUE(Future);
	ASSERT_TRUE(Future->GetResultShared());
	EXPECT_EQ(EToolError::InvalidOutput, Future->GetResultShared()->Diagnostics.front().Code);
	EXPECT_TRUE(Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{{"enabled", true}}}));
	Tools->TickTools();
	EXPECT_EQ(EToolError::InvalidOutput, Future->GetResultShared()->Diagnostics.front().Code);
	EXPECT_EQ(1, Toolset.Executions);
}

TEST_F(FEditorToolTests, PendingCapacityDefersDispatchAndCompletionRecoversCapacity)
{
	FTestToolset Toolset; Toolset.Descriptors.front().bMayPend = true;
	auto Source = FToolCompletionSource::CreateDetached();
	auto Raw = Tasks::Share(Source.TakeTask());
	Toolset.AsyncAction = [&](const auto&, const auto&) { return Raw; };
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxPending = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)Tools->ListToolsets();
	auto First = Endpoint->Submit(Request()), Second = Endpoint->Submit(Request());
	ASSERT_TRUE(First); ASSERT_TRUE(Second);
	Tools->TickTools();
	EXPECT_EQ(1, Toolset.Executions);
	EXPECT_EQ(EToolInvocationState::Queued, Endpoint->GetInvocation(*Second)->State);
	Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{{"enabled", false}}});
	Tools->TickTools();
	EXPECT_EQ(2, Toolset.Executions);
	EXPECT_EQ(EToolInvocationState::Completed, Endpoint->GetInvocation(*Second)->State);
}

TEST_F(FEditorToolTests, ConsumerPinsRemainChargedAfterReceiptExpiryAndReleaseRecoversAdmission)
{
	FTestToolset Toolset;
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxRetained = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)Tools->ListToolsets();
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id);
	auto Future = Endpoint->RetainInvocation(*Id); ASSERT_TRUE(Future);
	Tools->TickTools();
	auto Alias = Future->GetResultShared(); ASSERT_TRUE(Alias);
	auto Snapshot = Endpoint->GetInvocation(*Id); ASSERT_TRUE(Snapshot && Snapshot->Result);
	auto CopiedSnapshot = *Snapshot;
	EXPECT_EQ(Alias, Snapshot->Result);
	EXPECT_EQ(Alias, CopiedSnapshot.Result);
	*Future = {};
	Limits.ResultTTL = std::chrono::milliseconds(0);
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)Tools->ListToolsets();
	auto Expired = Endpoint->GetInvocation(*Id); ASSERT_TRUE(Expired);
	EXPECT_EQ(EToolInvocationState::Expired, Expired->State);
	ASSERT_TRUE(Expired->Result);
	EXPECT_EQ(EToolError::InvocationExpired, Expired->Result->Diagnostics.front().Code);
	EXPECT_EQ(EToolError::Busy, Endpoint->Submit(Request()).error().Code);
	EXPECT_EQ(EToolStatus::Succeeded, Alias->Status);
	Alias.reset();
	EXPECT_EQ(EToolError::Busy, Endpoint->Submit(Request()).error().Code);
	Snapshot->Result = {}; CopiedSnapshot.Result = {};
	EXPECT_TRUE(Endpoint->Submit(Request()));
}

TEST_F(FEditorToolTests, ClosedHostDetachesPendingCompletionAndLateProducerCannotReviveIt)
{
	FTestToolset Toolset; Toolset.Descriptors.front().bMayPend = true;
	auto Source = FToolCompletionSource::CreateDetached();
	auto Raw = Tasks::Share(Source.TakeTask());
	Toolset.AsyncAction = [&](const auto&, const auto&) { return Raw; };
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Host = MakeHost(); auto* Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id);
	auto Future = Endpoint->RetainInvocation(*Id); ASSERT_TRUE(Future);
	Tools->TickTools();
	Host->PrepareForShutdown();
	ASSERT_TRUE(Future->GetResultShared());
	EXPECT_EQ(EToolError::Cancelled, Future->GetResultShared()->Diagnostics.front().Code);
	Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{{"enabled", true}}});
	EXPECT_EQ(EToolError::Cancelled, Future->GetResultShared()->Diagnostics.front().Code);
}

TEST_F(FEditorToolTests, DelayedOutputValidationAndAbandonmentUseOneFinalBoundary)
{
	FTestToolset Toolset; Toolset.Descriptors.front().bMayPend = true;
	std::optional<FToolCompletionSource> Source = FToolCompletionSource::CreateDetached();
	auto Raw = Tasks::Share(Source->TakeTask());
	Toolset.AsyncAction = [&](const auto&, const auto&) { return Raw; };
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id); Tools->TickTools();
	Source->TrySetValue({EToolStatus::Succeeded, "invalid schema"}); Tools->TickTools();
	EXPECT_EQ(EToolError::InvalidOutput, Endpoint->GetInvocation(*Id)->Result->Diagnostics.front().Code);
	Source = FToolCompletionSource::CreateDetached(); Raw = Tasks::Share(Source->TakeTask());
	auto Abandoned = Endpoint->Submit(Request()); ASSERT_TRUE(Abandoned); Tools->TickTools();
	Source.reset(); Tools->TickTools();
	EXPECT_EQ(EToolError::InternalError, Endpoint->GetInvocation(*Abandoned)->Result->Diagnostics.front().Code);
}

TEST_F(FEditorToolTests, PendingRemovalCancellationAndByteLimitsRemainDetachedAndBounded)
{
	FTestToolset Toolset; Toolset.Descriptors.front().bMayPend = true;
	auto Source = FToolCompletionSource::CreateDetached();
	auto Raw = Tasks::Share(Source.TakeTask());
	Toolset.AsyncAction = [&](const auto&, const auto&) { return Raw; };
	auto Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	auto* Tools = MakeHost()->GetEditorSubsystem<DEditorToolSubsystem>();
	auto Endpoint = Tools->GetService();
	FEditorToolLimits Limits; Limits.MaxPendingBytes = 1;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)Tools->ListToolsets();
	EXPECT_EQ(EToolError::Busy, Endpoint->Submit(Request()).error().Code);
	EXPECT_EQ(0, Toolset.Executions);
	Limits.MaxPendingBytes = 16 * 1024 * 1024;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)Tools->ListToolsets();
	auto Id = Endpoint->Submit(Request()); ASSERT_TRUE(Id); Tools->TickTools();
	Token.Reset();
	Source.TrySetCanceled(); Tools->TickTools();
	EXPECT_EQ(EToolError::Cancelled, Endpoint->GetInvocation(*Id)->Result->Diagnostics.front().Code);
	Toolset.Descriptors.front().Output = FToolSchema(EToolValueType::String);
	Token = FModularFeatureRegistry::Get().RegisterFeature(Toolset);
	Source = FToolCompletionSource::CreateDetached(); Raw = Tasks::Share(Source.TakeTask());
	Limits.MaxCompletedBytes = 2048;
	ASSERT_TRUE(Endpoint->Configure(Limits)); (void)Tools->ListToolsets();
	auto Oversized = Endpoint->Submit(Request()); ASSERT_TRUE(Oversized); Tools->TickTools();
	Source.TrySetValue({EToolStatus::Succeeded, std::string(4096, 'x')}); Tools->TickTools();
	EXPECT_EQ(EToolError::LimitExceeded, Endpoint->GetInvocation(*Oversized)->Result->Diagnostics.front().Code);
}
