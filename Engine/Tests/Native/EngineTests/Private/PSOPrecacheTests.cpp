#include "PSOPrecacheComponent.h"
#include "PSOPrecacheMaterial.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshRendererShared.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "RenderPipelineCreation.h"
#include "RHICapabilities.h"
#include "RHICommandList.h"
#include "Threading/TaskRuntime.h"

#include <gtest/gtest.h>

namespace
{
	class FMaterialPSOTestPipelines
	{
	public:
		FMaterialPSOTestPipelines() : Released(Gate.get_future().share())
		{
			using namespace Durin;
			OwnsScheduler = !IsTaskSchedulerRunning();
			if (OwnsScheduler) require(InitializeTaskScheduler(1));
			FRHICapabilities Caps;
			Caps.MaxComputeWorkGroupCount = {65535, 65535, 65535};
			FRHIPipelineCompileBackend Backend{
				.FindGraphics = [](const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
				.FindCompute = [](const auto&) -> FComputePipelineStateRHIRef { return {}; },
				.CreateGraphics = [](const auto&, const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
				.CreateCompute = [this](const FRHIComputePipelineCreationInputs& Input, const auto&) -> FComputePipelineStateRHIRef {
					if (Input.DebugName == "gated") Released.wait();
					if (Input.DebugName == "failed") return {};
					return MakeRefCount<FRHIComputePipelineState>();
				},
				.PublishTerminalFailure = [](std::exception_ptr) { ADD_FAILURE() << "Unexpected material test pipeline failure."; }};
			Cache = std::make_unique<FRHIPipelineStateCache>(Caps, std::move(Backend));
			Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		}
		~FMaterialPSOTestPipelines()
		{
			Release(); Cache.reset(); Shader = nullptr;
			if (OwnsScheduler) Durin::ShutdownTaskScheduler();
			Durin::RHIFlushDeferredResources();
		}
		auto Release() -> void { if (!GateReleased) { GateReleased = true; Gate.set_value(); } }
		auto TryRequestSlot(uint32 Slot) -> std::expected<std::shared_ptr<Durin::FComputePipelineState>, Durin::ERHIPipelineRequestRejection>
		{
			using namespace Durin;
			FComputePipelineStateInitializer Input;
			Input.ComputeShader = Shader;
			Input.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, Slot, ERHIBindingType::UniformBuffer);
			auto Result = Cache->GetCompute(Input, "slot");
			if (Result) Descriptions.insert_or_assign(Result->get(), FPSOPrecacheData(Input));
			return Result;
		}
		auto Request(bool Gated, bool Failed = false) -> std::shared_ptr<Durin::FPipelineState>
		{
			using namespace Durin;
			FComputePipelineStateInitializer Input;
			Input.ComputeShader = Shader;
			Input.PipelineLayout.PushConstantRanges.push_back({EShaderStageFlags::Compute, 0, Failed ? 12u : Gated ? 8u : 4u});
			const auto Result = Cache->GetCompute(Input, Failed ? "failed" : Gated ? "gated" : "ready");
			require(Result.has_value());
			Descriptions.insert_or_assign(Result->get(), FPSOPrecacheData(Input));
			return *Result;
		}
		auto Describe(const std::shared_ptr<Durin::FPipelineState>& Pipeline, bool Required = true)
			-> Durin::FPSOPrecacheData
		{
			auto Data = Descriptions.at(Pipeline.get()); Data.bRequired = Required; return Data;
		}
		auto DescribeSlot(uint32 Slot) -> Durin::FPSOPrecacheData
		{
			using namespace Durin;
			FComputePipelineStateInitializer Input;
			Input.ComputeShader = Shader;
			Input.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, Slot, ERHIBindingType::UniformBuffer);
			return FPSOPrecacheData(Input);
		}
		auto Submit(const Durin::FPSOPrecacheData& Data)
			-> std::expected<std::shared_ptr<Durin::FPipelineState>, Durin::ERHIPipelineRequestRejection>
		{
			using namespace Durin;
			const auto& Input = Data.ComputeInitializer;
			const uint32 Size = Input.PipelineLayout.PushConstantRanges.empty() ? 0 : Input.PipelineLayout.PushConstantRanges.front().Size;
			auto Result = Cache->GetCompute(Input, Size == 12 ? "failed" : Size == 8 ? "gated" : "ready");
			if (!Result) return std::unexpected(Result.error());
			return *Result;
		}

	private:
		std::unordered_map<const Durin::FPipelineState*, Durin::FPSOPrecacheData> Descriptions;
		std::promise<void> Gate;
		std::shared_future<void> Released;
		bool GateReleased = false, OwnsScheduler = false;
		std::unique_ptr<Durin::FRHIPipelineStateCache> Cache;
		Durin::FShaderRHIRef Shader;
	};

	auto MaterialPSOTestParams() -> Durin::FMaterialPSOPrecacheParams
	{
		using namespace Durin;
		const auto Factory = RendererPrivate::FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
		require(Factory != nullptr);
		FMaterialPSOPrecacheParams Params;
		Params.VertexFactoryData = FPSOPrecacheVertexFactoryData(&Factory->GetType());
		return Params;
	}
}

TEST(FPSOPrecacheTests, VertexFactoryPrecacheDescriptionsAreCpuOnlyAndOwnExplicitDefaultLayouts)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	const auto Local = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
	const auto Spline = FindMeshVertexFactory(FSplineMeshBatchBinding{}.GetFactoryKey());
	ASSERT_TRUE(Local);
	ASSERT_TRUE(Spline);
	EXPECT_TRUE(Local->GetType().SupportsPSOPrecaching());
	EXPECT_TRUE(Spline->GetType().SupportsPSOPrecaching());
	EXPECT_FALSE(EnumHasAnyFlags(Local->GetType().GetFlags(), EVertexFactoryFlags::SupportsManualVertexFetch));
	for (const auto Stream : {EVertexInputStreamType::Default, EVertexInputStreamType::PositionOnly, EVertexInputStreamType::PositionAndNormalOnly})
	{
		const auto Elements = FPSOPrecacheVertexFactoryData(&Local->GetType()).GetVertexElements(Stream);
		const auto SplineElements = FPSOPrecacheVertexFactoryData(&Spline->GetType()).GetVertexElements(Stream);
		ASSERT_TRUE(Elements);
		ASSERT_TRUE(SplineElements);
		EXPECT_EQ(*Elements, *SplineElements);
		EXPECT_TRUE(ValidateVertexDeclarationElements(*Elements));
		EXPECT_EQ(std::ranges::count_if(*Elements, [](const auto& Element) { return Element.Type != EVertexElementType::None; }),
			Stream == EVertexInputStreamType::Default ? 8 : Stream == EVertexInputStreamType::PositionOnly ? 1 : 2);
	}
	const auto Fixed = FPSOPrecacheVertexFactoryData(&Local->GetType()).GetVertexElements();
	ASSERT_TRUE(Fixed);
	auto Custom = *Fixed;
	Custom[0].Stride += 4;
	const FPSOPrecacheVertexFactoryData Snapshot(&Local->GetType(), Custom);
	EXPECT_TRUE(Snapshot.MatchesVertexElements(Custom));
	EXPECT_FALSE(Snapshot.MatchesVertexElements(*Fixed));
	Custom[0].Stride += 4;
	EXPECT_FALSE(Snapshot.MatchesVertexElements(Custom));
	const auto Position = Snapshot.GetVertexElements(EVertexInputStreamType::PositionOnly);
	ASSERT_TRUE(Position);
	EXPECT_EQ((*Position)[0], (*Fixed)[0]); // Default overrides do not change reduced streams.
	EXPECT_FALSE(Snapshot.GetVertexElements(EVertexInputStreamType::Count));
	FVertexDeclarationElementList Reused = *Fixed;
	Local->GetType().GetPSOPrecacheVertexFetchElements(EVertexInputStreamType::Count, Reused);
	EXPECT_TRUE(std::ranges::all_of(Reused, [](const auto& Element) { return Element.Type == EVertexElementType::None; }));
}

TEST(FPSOPrecacheTests, VertexFactoryPrecacheRejectsUnsupportedMissingAndMalformedLayouts)
{
	using namespace Durin;
	static const FVertexFactoryType Legacy("PrecacheLegacyFixture");
	static const FVertexFactoryType ExplicitOnly("PrecacheExplicitFixture", EVertexFactoryFlags::SupportsPSOPrecaching);
	static const FVertexFactoryType Manual("PrecacheManualFixture",
		EVertexFactoryFlags::SupportsPSOPrecaching | EVertexFactoryFlags::SupportsManualVertexFetch,
		+[](EVertexInputStreamType, FVertexDeclarationElementList& Elements) { Elements = {}; });
	EXPECT_EQ(FPSOPrecacheVertexFactoryData{}.GetVertexElements().error(), EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&Legacy).GetVertexElements().error(), EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&ExplicitOnly).GetVertexElements().error(), EPSOPrecacheVertexFactoryError::MissingVertexElements);
	FVertexDeclarationElementList Elements{};
	Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 16);
	EXPECT_TRUE(FPSOPrecacheVertexFactoryData(&ExplicitOnly, Elements).GetVertexElements());
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&Legacy, Elements).GetVertexElements().error(), EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&ExplicitOnly, Elements).GetVertexElements(EVertexInputStreamType::PositionOnly).error(), EPSOPrecacheVertexFactoryError::UnsupportedStream);
	EXPECT_TRUE(FPSOPrecacheVertexFactoryData(&Manual).GetVertexElements());
	EXPECT_FALSE(FPSOPrecacheVertexFactoryData(&Manual).GetVertexElements(EVertexInputStreamType::PositionAndNormalOnly));
	EXPECT_FALSE(FPSOPrecacheVertexFactoryData(&ExplicitOnly, {}).GetVertexElements());
	for (uint32 Mutation = 0; Mutation < 5; ++Mutation)
	{
		SCOPED_TRACE(Mutation);
		auto Invalid = Elements;
		switch (Mutation)
		{
		case 0: Invalid[0].Stride = 4; break;
		case 1: Invalid[1] = FVertexElement(1, 0, EVertexElementType::Float1, 0, 4); break;
		case 2: Invalid[1] = FVertexElement(0, 0, EVertexElementType::Float1, 1, 16); break;
		case 3: Invalid[1] = FVertexElement(0, 12, EVertexElementType::Float1, 1, 20); break;
		case 4: Invalid[2] = FVertexElement(1, 0, EVertexElementType::Float1, 1, 4); break;
		}
		const auto Result = FPSOPrecacheVertexFactoryData(&ExplicitOnly, Invalid).GetVertexElements();
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error(), EPSOPrecacheVertexFactoryError::InvalidVertexElements);
	}
}

TEST(FPSOPrecacheTests, PSOPrecacheDataValidatesSelectedInitializerAndPriority)
{
    using namespace Durin;
    FMaterialPSOTestPipelines Pipelines;
    auto Native = Pipelines.Request(false);
    auto Data = Pipelines.Describe(Native, false);
    EXPECT_EQ(Data.Type, FPSOPrecacheData::EType::Compute);
    EXPECT_FALSE(Data.bRequired);
    Data.Priority = EPSOPrecachePriority::Highest;
    EXPECT_TRUE(Data.IsValid());
    Data.Type = FPSOPrecacheData::EType::Graphics;
    EXPECT_FALSE(Data.IsValid());
    Data.Type = static_cast<FPSOPrecacheData::EType>(255);
    EXPECT_FALSE(Data.IsValid());
    Data.Type = FPSOPrecacheData::EType::Compute;
    Data.Priority = EPSOPrecachePriority::NotSet;
    EXPECT_FALSE(Data.IsValid());
}

TEST(FPSOPrecacheTests, MaterialPSOInitializerValidationAndInvalidationPrecedePublication)
{
    using namespace Durin;
    uint32 Submissions = 0;
    FMaterialPSORequestManager Invalid([&](const FPSOPrecacheData&)
        -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
        ++Submissions; return std::unexpected(ERHIPipelineRequestRejection::InvalidDescription);
    });
    auto Request = Invalid.PrecachePSOs(MaterialPSOTestParams());
    Invalid.Pump([](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.emplace_back(); return true;
    });
    EXPECT_EQ(Submissions, 0u);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);

    FMaterialPSOTestPipelines Pipelines;
    auto Ready = Pipelines.Request(false);
    ASSERT_TRUE(Ready->Wait());
    FMaterialPSORequestManager* Current = nullptr;
    FMaterialPSORequestManager Retiring([&](const FPSOPrecacheData& Data) {
        const auto Copy = Data; Current->Invalidate(); return Pipelines.Submit(Copy);
    });
    Current = &Retiring;
    Request = Retiring.PrecachePSOs(MaterialPSOTestParams());
    Retiring.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready)); return true;
    });
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Stale);
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
}

TEST(FPSOPrecacheTests, MaterialPSONotificationsAreOwnerDirectedAndReentrant)
{
    using namespace Durin;
    FMaterialPSORequestManager Manager;
    auto Shared = Manager.PrecachePSOs(MaterialPSOTestParams());
    FMaterialPSOPrecacheOwner FirstOwner, SecondOwner;
    auto First = FirstOwner.Observe(Shared), Second = SecondOwner.Observe(Shared);
    uint32 FirstChanges = 0, SecondChanges = 0;
    auto A = First.Subscribe([&] { ++FirstChanges; (void)Manager.GetPendingRequestCount(); });
    auto B = Second.Subscribe([&] { ++SecondChanges; (void)Manager.GetPendingRequestCount(); });
    EXPECT_EQ(FirstChanges, 1u); EXPECT_EQ(SecondChanges, 1u);
    FirstOwner.Invalidate();
    EXPECT_EQ(FirstChanges, 2u); EXPECT_EQ(SecondChanges, 1u);
    EXPECT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Stale);
    EXPECT_EQ(Second.GetProgress().State, EMaterialPSOPrecacheState::Pending);
    First.Cancel();
    EXPECT_EQ(SecondChanges, 1u);
    Manager.Invalidate();
    EXPECT_EQ(SecondChanges, 2u);
    EXPECT_EQ(Second.GetProgress().State, EMaterialPSOPrecacheState::Stale);
}

TEST(FPSOPrecacheTests, FailedOptionalPSODiagnosticsDoNotAuthorizeAnInvalidatedOwner)
{
    using namespace Durin;
    FMaterialPSOTestPipelines Pipelines;
    auto Ready = Pipelines.Request(false), Failed = Pipelines.Request(false, true);
    ASSERT_TRUE(Ready->Wait());
    EXPECT_FALSE(Failed->Wait());
    FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
    auto Shared = Manager.PrecachePSOs(MaterialPSOTestParams());
    FMaterialPSOPrecacheOwner Owner;
    auto Request = Owner.Observe(Shared);
    Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers = {Pipelines.Describe(Ready, true), Pipelines.Describe(Failed, false)};
        return true;
    });
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Request.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < Deadline)
    {
        Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
        std::this_thread::yield();
    }
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    EXPECT_TRUE(Request.IsCurrent());
    Owner.Invalidate();
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Request.IsCurrent());
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
    EXPECT_TRUE(Shared.IsCurrent());
    EXPECT_TRUE(Shared.GetProgress().bRequiredReady);
    Manager.Invalidate();
    EXPECT_EQ(Shared.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Shared.IsCurrent());
    EXPECT_FALSE(Shared.GetProgress().bRequiredReady);
}

TEST(FPSOPrecacheTests, ComponentPSONotificationsIgnoreReplacedAndDestroyedJobSets)
{
    using namespace Durin;
    FMaterialPSORequestManager Manager;
    auto First = Manager.PrecachePSOs(MaterialPSOTestParams());
    auto Params = MaterialPSOTestParams(); Params.PrecachePSOParams.bWireframe = true;
    auto Second = Manager.PrecachePSOs(Params);
    FPSOPrecacheComponentData Component;
    uint32 OldChanges = 0, NewChanges = 0, DestroyedChanges = 0;
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({First}, {}, [&] { ++OldChanges; }));
    FPSOPrecacheComponentData::DispatchOwnerNotifications();
    const auto OldBefore = OldChanges;
    std::thread CancelOld([&] { First.Cancel(); }); CancelOld.join();
    Component.Reset();
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Second}, {}, [&] { ++NewChanges; }));
    FPSOPrecacheComponentData::DispatchOwnerNotifications();
    EXPECT_EQ(OldChanges, OldBefore);
    EXPECT_EQ(NewChanges, 1u);
    {
        FPSOPrecacheComponentData Destroyed;
        ASSERT_TRUE(Destroyed.SetMaterialPSOPrecacheData({Second}, {}, [&] { ++DestroyedChanges; }));
        FPSOPrecacheComponentData::DispatchOwnerNotifications();
        std::thread CancelCurrent([&] { Second.Cancel(); }); CancelCurrent.join();
    }
    const auto BeforeDestroyDispatch = DestroyedChanges;
    FPSOPrecacheComponentData::DispatchOwnerNotifications();
    EXPECT_EQ(DestroyedChanges, BeforeDestroyDispatch);
    EXPECT_EQ(NewChanges, 2u);
}

TEST(FPSOPrecacheTests, MaterialPSORequestsShareWorkWithoutSharingCancellationOrOwnerStaleness)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false);
	ASSERT_TRUE(Ready->Wait());
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto First = Manager.PrecachePSOs(MaterialPSOTestParams());
	auto Params = MaterialPSOTestParams();
	Params.VertexFactoryData.CustomDefaultVertexElements = *Params.VertexFactoryData.GetVertexElements();
	auto Peer = Manager.PrecachePSOs(Params);
	ASSERT_NE(First.GetValue(), 0u);
	EXPECT_EQ(First.GetValue(), Peer.GetValue());
	FMaterialPSOPrecacheOwner OwnerScope;
	auto Owner = OwnerScope.Observe(Peer);
	First.Cancel();
	EXPECT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Canceled);
	uint32 Collections = 0;
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		++Collections; Out.PSOInitializers.push_back(Pipelines.Describe(Ready, true)); return true;
	});
	EXPECT_EQ(Collections, 1u);
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	OwnerScope.Invalidate();
	EXPECT_EQ(Owner.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_FALSE(Owner.GetProgress().bRequiredReady);
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	Manager.Invalidate();
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_NE(Manager.PrecachePSOs(Params).GetValue(), Peer.GetValue());
	Manager.Close();
	EXPECT_EQ(Manager.PrecachePSOs(Params).GetValue(), 0u);
}

TEST(FPSOPrecacheTests, ComponentPSOSetsSeparateRequiredReadinessFromAllWorkAndIgnoreLateBatches)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false);
	ASSERT_TRUE(Ready->Wait());
	auto Gated = Pipelines.Request(true);
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto First = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		Out.PSOInitializers = {Pipelines.Describe(Ready, true), Pipelines.Describe(Gated, false)}; return true;
	});
	ASSERT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Pending);
	EXPECT_TRUE(First.GetProgress().bRequiredReady);
	FPSOPrecacheComponentData Component, Peer;
	uint32 OldCallbacks = 0, NewCallbacks = 0;
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({First}, [&] { ++OldCallbacks; }));
	ASSERT_TRUE(Peer.SetMaterialPSOPrecacheData({First}));
	const auto OldBatch = Component.GetLatestJobSet();
	EXPECT_TRUE(Component.IsPSOPrecaching());
	EXPECT_TRUE(Component.IsReadyForProxyCreation());
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Pending);
	auto Params = MaterialPSOTestParams(); Params.PrecachePSOParams.bReversedZ = true;
	auto New = Manager.PrecachePSOs(Params);
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> { Out.PSOInitializers = {Pipelines.Describe(Ready, true)}; return true; });
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({New}, [&] { ++NewCallbacks; }));
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Ready);
	EXPECT_EQ(NewCallbacks, 1u);
	Component.Poll(); EXPECT_EQ(NewCallbacks, 1u);
	Pipelines.Release(); ASSERT_TRUE(Gated->Wait());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	EXPECT_FALSE(Component.SetLatestJobSetCompleted(OldBatch));
	EXPECT_EQ(OldCallbacks, 0u);
	EXPECT_EQ(Peer.Poll(), EMaterialPSOPrecacheState::Ready);
	Component.Reset();
	EXPECT_FALSE(Component.IsReadyForProxyCreation());
	EXPECT_EQ(Peer.Poll(), EMaterialPSOPrecacheState::Ready);
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({New}, [&] { Component.Reset(); }));
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Canceled);
	auto Ephemeral = std::make_unique<FPSOPrecacheComponentData>();
	ASSERT_TRUE(Ephemeral->SetMaterialPSOPrecacheData({New}, [&] { Ephemeral.reset(); }));
	auto* Completing = Ephemeral.get();
	EXPECT_EQ(Completing->Poll(), EMaterialPSOPrecacheState::Canceled);
	Manager.Close();
}

TEST(FPSOPrecacheTests, MaterialPSOFailuresRemainObservableAndRequestsHaveNoAggregateCountLimit)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false); ASSERT_TRUE(Ready->Wait());
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto Failure = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { return std::unexpected(std::string("fixture collection failure")); });
	EXPECT_EQ(Failure.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	EXPECT_FALSE(Failure.GetProgress().bRequiredReady);
	EXPECT_EQ(Failure.GetDiagnostic(), "fixture collection failure");
	FPSOPrecacheComponentData Component;
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Failure}));
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Failed);
	EXPECT_FALSE(Component.IsReadyForProxyCreation());
	Manager.Invalidate();
	auto NativeFailure = Pipelines.Request(false, true);
	(void)NativeFailure->Wait();
	ASSERT_EQ(NativeFailure->GetState(), ERHIPipelineRequestState::Failed);
	Failure = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> { Out.PSOInitializers = {Pipelines.Describe(NativeFailure, true)}; return true; });
	const auto NativeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Failure.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < NativeDeadline)
	{
		Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
		std::this_thread::yield();
	}
	EXPECT_EQ(Failure.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	EXPECT_FALSE(Failure.GetProgress().bRequiredReady);
	Manager.Invalidate();
	std::vector<FMaterialPSOPrecacheRequestID> Requests;
	for (size_t Index = 0; Index < 512; ++Index)
	{
		auto Params = MaterialPSOTestParams();
		FVertexDeclarationElementList Elements{};
		Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, uint16(16 + Index));
		Params.VertexFactoryData.CustomDefaultVertexElements = Elements;
		Requests.push_back(Manager.PrecachePSOs(Params));
		ASSERT_NE(Requests.back().GetValue(), 0u);
	}
	EXPECT_NE(Manager.PrecachePSOs(MaterialPSOTestParams()).GetValue(), 0u);
	uint32 Collections = 0;
	for (size_t Pump = 0; Pump < 64; ++Pump)
		Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
			++Collections; Out.PSOInitializers = {Pipelines.Describe(Ready, true)}; return true;
		});
	EXPECT_EQ(Collections, 512u);
	EXPECT_EQ(Manager.GetPendingRequestCount(), 0u);
	for (const auto& Request : Requests) EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	// Retaining all completed observations neither occupies active capacity nor
	// causes their collectors to run on later frames.
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	Requests.clear();
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	EXPECT_NE(Manager.PrecachePSOs(MaterialPSOTestParams()).GetValue(), 0u);
	Manager.Close();
}

TEST(FPSOPrecacheTests, MaterialPSOCompletionReleasesPipelinesAndResourcesButKeepsSharedStatus)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false); ASSERT_TRUE(Ready->Wait());
	std::weak_ptr<FPipelineState> WeakPipeline = Ready;
	auto Resources = std::make_shared<int>(42);
	std::weak_ptr<int> WeakResources = Resources;
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		Out.PSOInitializers.push_back(Pipelines.Describe(Ready, true)); Out.Resources = std::move(Resources); return true;
	});
	Ready.reset();
	EXPECT_TRUE(WeakPipeline.expired());
	EXPECT_TRUE(WeakResources.expired());
	EXPECT_EQ(Request.GetProgress().Pipelines, 1u);
	EXPECT_TRUE(Request.GetProgress().bRequiredReady);
	EXPECT_EQ(Manager.GetPendingRequestCount(), 0u);
	const auto Peer = Manager.PrecachePSOs(MaterialPSOTestParams());
	EXPECT_EQ(Peer.GetValue(), Request.GetValue());
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	Manager.Invalidate();
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Stale);
}

TEST(FPSOPrecacheTests, MaterialPSOCollectionConcurrencyIsBoundedAcrossPumps)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Gated = Pipelines.Request(true);
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	std::vector<FMaterialPSOPrecacheRequestID> Requests;
	for (uint32 Index = 0; Index < 24; ++Index)
	{
		auto Params = MaterialPSOTestParams(); Params.Material.ShaderMap.OpacityMaskThreshold = float(Index);
		Requests.push_back(Manager.PrecachePSOs(Params));
	}
	uint32 Collections = 0;
	auto Collect = [&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		++Collections; Out.PSOInitializers = {Pipelines.Describe(Gated, true)}; return true;
	};
	for (uint32 Frame = 0; Frame < 5; ++Frame) Manager.Pump(Collect);
	EXPECT_EQ(Collections, FMaterialPSORequestManager::MaximumActiveCollections);
	Pipelines.Release(); ASSERT_TRUE(Gated->Wait());
	for (uint32 Frame = 0; Frame < 4; ++Frame) Manager.Pump(Collect);
	EXPECT_EQ(Collections, 24u);
	EXPECT_EQ(Manager.GetPendingRequestCount(), 0u);
}

TEST(FPSOPrecacheTests, MaterialPSOResumesAfterNativeCapacityWithoutResubmittingAcceptedPrefix)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Gated = Pipelines.Request(true);
	std::vector<std::shared_ptr<FComputePipelineState>> Occupied;
	for (uint32 Slot = 0; Slot < 254; ++Slot)
	{
		auto Result = Pipelines.TryRequestSlot(Slot); ASSERT_TRUE(Result);
		Occupied.push_back(*Result);
	}
	uint32 Accepted = 0, Rejected = 0;
	std::weak_ptr<FPipelineState> Prefix;
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) {
		auto Result = Pipelines.Submit(Data);
		if (Result) { ++Accepted; if (Accepted == 1) Prefix = *Result; }
		else { EXPECT_EQ(Result.error(), ERHIPipelineRequestRejection::CapacityExceeded); ++Rejected; }
		return Result;
	});
	auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	uint32 Collections = 0;
	auto Collect = [&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		++Collections;
		Out.PSOInitializers = {Pipelines.DescribeSlot(254), Pipelines.DescribeSlot(255)};
		return true;
	};
	Manager.Pump(Collect); Manager.Pump(Collect);
	EXPECT_EQ(Accepted, 1u); EXPECT_EQ(Rejected, 2u);
	EXPECT_EQ(Collections, 1u);
	EXPECT_EQ(Request.GetProgress().Pipelines, 2u);
	EXPECT_GE(Request.GetProgress().Pending, 1u);
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Pending);
	Pipelines.Release(); ASSERT_TRUE(Gated->Wait());
	for (auto& Pipeline : Occupied) ASSERT_TRUE(Pipeline->Wait());

	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Request.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < Deadline)
	{
		Manager.Pump(Collect); std::this_thread::yield();
	}
	EXPECT_EQ(Collections, 1u);
	EXPECT_EQ(Accepted, 2u);
	EXPECT_TRUE(Prefix.expired());
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);

}

TEST(FPSOPrecacheTests, MaterialPSOFailureSurvivesObserverCancellationAndOwnerInvalidation)
{
    using namespace Durin;
    FMaterialPSOPrecacheOwner OwnerScope;
    const auto Request = OwnerScope.Observe(FMaterialPSOPrecacheRequestID::Rejected("failed collection"));
    Request.Cancel();
    OwnerScope.Invalidate();
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_EQ(Request.GetDiagnostic(), "failed collection");
}

TEST(FPSOPrecacheTests, MaterialPSOCollectionCannotPublishEmptyOrInvalidatedSuccess)
{
	using namespace Durin;
	FMaterialPSORequestManager Manager;
	auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { return true; });
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	Manager.Invalidate();
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto&) -> std::expected<bool, std::string> { Manager.Invalidate(); return true; });
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_FALSE(Request.GetProgress().bRequiredReady);
}

TEST(FPSOPrecacheTests, RequiredPSOCollectionDoesNotWaitForOptionalShaders)
{
    using namespace Durin;
    FMaterialPSOTestPipelines Pipelines;
    auto Ready = Pipelines.Request(false);
    ASSERT_TRUE(Ready->Wait());
    FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
    auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
    uint32 Pumps = 0;
    auto Collect = [&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        if (++Pumps == 1)
        {
            Out.PSOInitializers.push_back(Pipelines.Describe(Ready));
            Out.bRequiredPSOsCollected = true;
            return false;
        }
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready, false));
        return true;
    };
    Manager.Pump(Collect);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Pending);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    FPSOPrecacheComponentData Component;
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Request}));
    EXPECT_TRUE(Component.IsReadyForProxyCreation());
    EXPECT_TRUE(Component.IsPSOPrecaching());
    Manager.Pump(Collect);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);

    auto Params = MaterialPSOTestParams();
    Params.PrecachePSOParams.Participation = {false, false};
    auto Optional = Manager.PrecachePSOs(Params);
    EXPECT_FALSE(Optional.GetProgress().bRequiredReady);
    Manager.Pump([](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.bRequiredPSOsCollected = true;
        return false;
    });
    EXPECT_TRUE(Optional.GetProgress().bRequiredReady);
    EXPECT_EQ(Optional.GetProgress().State, EMaterialPSOPrecacheState::Pending);

    Manager.Invalidate();
    Request = Manager.PrecachePSOs(MaterialPSOTestParams());
    Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready));
        Out.bRequiredPSOsCollected = true;
        return std::unexpected("Optional shader collection failed.");
    });
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Request}));
    EXPECT_TRUE(Component.IsReadyForProxyCreation());

    Manager.Invalidate();
    EXPECT_FALSE(Component.IsReadyForProxyCreation());
    Request = Manager.PrecachePSOs(MaterialPSOTestParams()); Pumps = 0;
    Manager.Pump(Collect);
    Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready)); return true;
    });
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
}

TEST(FPSOPrecacheTests, EmptyApplicableMaterialPSOSetPublishesReadinessWithoutNativeWork)
{
    using namespace Durin;
    uint32 Submissions = 0;
    FMaterialPSORequestManager Manager([&](const FPSOPrecacheData&)
        -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
        ++Submissions;
        return std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded);
    });
    auto Params = MaterialPSOTestParams();
    Params.PrecachePSOParams.Participation = {true, false};
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Translucent;
    auto Request = Manager.PrecachePSOs(Params);
    ASSERT_TRUE(Request.IsValid());
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
    Manager.Pump([](const auto& In, auto& Out) -> std::expected<bool, std::string> {
        Out.bRequiredPSOsCollected = true;
        return true;
    });
    EXPECT_EQ(Submissions, 0u);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);
}

TEST(FPSOPrecacheTests, ShadowPSORequirementsUseTheNormalizedDrawingState)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    auto Params = MaterialPSOTestParams();
    Params.PrecachePSOParams.Participation = {true, false};
    Params.PrecachePSOParams.bWireframe = true;
    Params.PrecachePSOParams.bReversedZ = true;
    FSceneRenderPreparationConfig Config;
    Config.bSupportsWireframe = false;
    const auto Plan = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::ShadowDepth).GetPSOPrecacheVariants(Config, Params.PrecachePSOParams);
    ASSERT_EQ(Plan.size(), 2u);
    for (const auto& Entry : Plan) {
        EXPECT_TRUE(Entry.bRequired);
        EXPECT_FALSE(Entry.Raster.bWireframe);
        EXPECT_FALSE(Entry.Raster.bReversedZ);
        const auto State = MakeMeshPipelineState(Params.Material, Entry.Raster);
        const auto Draw = DescribeMeshPipeline(EMeshPipelinePass::ShadowDepth,
            MakeMeshPipelineState(Params.Material, {true, Entry.Raster.bReverseCulling, false, true}),
            Params.PrecachePSOParams.Topology);
        const auto Precache = DescribeMeshPipeline(EMeshPipelinePass::ShadowDepth, State, Params.PrecachePSOParams.Topology);
        EXPECT_EQ(Draw.Rasterizer, Precache.Rasterizer);
        EXPECT_EQ(Draw.Depth, Precache.Depth);
        EXPECT_EQ(Draw.RenderTargets, Precache.RenderTargets);
    }
}

TEST(FPSOPrecacheTests, OptionalPSOAdmissionFailuresDoNotWithholdRequiredPipelines)
{
    using namespace Durin;
    for (uint32 Failure = 0; Failure < 4; ++Failure)
    {
        SCOPED_TRACE(Failure);
        FMaterialPSOTestPipelines Pipelines;
        auto Ready = Pipelines.Request(false);
        ASSERT_TRUE(Ready->Wait());
        FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data)
            -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
            if (Data.bRequired) return Pipelines.Submit(Data);
            if (Failure == 1) return std::shared_ptr<FPipelineState>{};
            if (Failure == 2) throw std::runtime_error("Optional backend rejection");
            return std::unexpected(ERHIPipelineRequestRejection::Unsupported);
        });
        auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
        Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
            auto Optional = Pipelines.Describe(Ready, false);
            if (Failure == 3) Optional.Type = static_cast<FPSOPrecacheData::EType>(255);
            Out.PSOInitializers = {Pipelines.Describe(Ready), Optional};
            Out.bRequiredPSOsCollected = true;
            return true;
        });
        EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
        EXPECT_TRUE(Request.GetProgress().bRequiredReady);
        EXPECT_EQ(Request.GetProgress().RequiredPending, 0u);
        EXPECT_EQ(Request.GetProgress().Failed, 1u);
        EXPECT_FALSE(Request.GetDiagnostic().empty());
        FPSOPrecacheComponentData Component;
        ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Request}));
        EXPECT_TRUE(Component.IsReadyForProxyCreation());
        Manager.Invalidate();
        EXPECT_FALSE(Component.IsReadyForProxyCreation());
    }
}

TEST(FPSOPrecacheTests, VertexFactoryRouteEligibilityUsesTheSharedPassFilter)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    const auto Local = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
    ASSERT_TRUE(Local);
    class FLimitedFactory final : public FMeshVertexFactoryImplementation
    {
        const FMeshVertexFactoryImplementation& Base;
        uint32 Unavailable;
    public:
        FLimitedFactory(const FMeshVertexFactoryImplementation& InBase, uint32 InUnavailable)
            : Base(InBase), Unavailable(InUnavailable) {}
        auto GetType() const -> const FVertexFactoryType& override { return Base.GetType(); }
        auto GetLayoutKey() const -> FXxHash64 override { return Base.GetLayoutKey(); }
        auto GetShaderType(uint32 Pass) const -> FShaderType* override
        { return Pass == Unavailable ? nullptr : Base.GetShaderType(Pass); }
        auto Resolve(const FMeshMaterialShaderMap&, uint32) const -> std::shared_ptr<const FMeshVertexShaderBinding> override { return {}; }
    };
    auto Params = MaterialPSOTestParams();
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Lit;
    const FLimitedFactory NoShadow(*Local, MaterialMeshPassShadow);
    EXPECT_FALSE(NoShadow.Supports(MaterialMeshPassShadow, {}));
    const auto& Shadow = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::ShadowDepth);
    const auto& Forward = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::Forward);
    const auto& GBuffer = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer);
    EXPECT_FALSE(Shadow.SupportsVertexFactory(NoShadow));
    EXPECT_TRUE(Forward.SupportsVertexFactory(NoShadow));
    const FLimitedFactory NoReceiver(*Local, MaterialMeshPassForward);
    EXPECT_FALSE(Forward.SupportsVertexFactory(NoReceiver));
    EXPECT_FALSE(GBuffer.SupportsVertexFactory(NoReceiver));
    EXPECT_TRUE(Shadow.SupportsVertexFactory(NoReceiver));
    const FLimitedFactory NoGBuffer(*Local, MaterialMeshPassGBuffer);
    EXPECT_FALSE(GBuffer.SupportsVertexFactory(NoGBuffer));
    EXPECT_TRUE(Forward.SupportsVertexFactory(NoGBuffer));
    EXPECT_TRUE(FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::HybridRetainedForward)
        .IsMeshRelevant(Params.Material, Params.PrecachePSOParams.Participation, false));
}

TEST(FPSOPrecacheTests, MaterialPSORequestsDoNotShareDifferentSceneFeatureLevels)
{
    using namespace Durin;
    FMaterialPSORequestManager Manager;
    auto High = MaterialPSOTestParams();
    High.FeatureLevel = ERHIFeatureLevel::SM5;
    auto Low = High;
    Low.FeatureLevel = ERHIFeatureLevel::ES3_1;
    auto HighRequest = Manager.PrecachePSOs(High);
    auto LowRequest = Manager.PrecachePSOs(Low);
    ASSERT_TRUE(HighRequest.IsValid());
    ASSERT_TRUE(LowRequest.IsValid());
    EXPECT_NE(HighRequest.GetValue(), LowRequest.GetValue());
    EXPECT_EQ(Manager.PrecachePSOs(High).GetValue(), HighRequest.GetValue());
    EXPECT_EQ(Manager.PrecachePSOs(Low).GetValue(), LowRequest.GetValue());
    auto Invalid = High;
    Invalid.FeatureLevel = static_cast<ERHIFeatureLevel>(255);
    EXPECT_FALSE(Manager.PrecachePSOs(Invalid).IsValid());
}
TEST(FPSOPrecacheTests, ComponentPriorityBoostReachesSharedMaterialCollectionAndNativeWork)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto Ordinary = Manager.PrecachePSOs(MaterialPSOTestParams());
	auto Params = MaterialPSOTestParams(); Params.PrecachePSOParams.bWireframe = true;
	auto Urgent = Manager.PrecachePSOs(Params);
	auto Peer = Manager.PrecachePSOs(Params);
	FPSOPrecacheComponentData Component;
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Urgent}));
	EXPECT_TRUE(Component.CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority::Highest));
	Peer.BoostPriority(EPSOPrecachePriority::Low);
	EXPECT_EQ(Peer.GetPriority(), EPSOPrecachePriority::Highest);
	std::vector<bool> Order;
	auto Native = Pipelines.Request(true);
	Manager.Pump([&](const auto& Input, auto& Collection) -> std::expected<bool, std::string> {
		Order.push_back(Input.PrecachePSOParams.bWireframe);
		Collection.PSOInitializers.push_back(Pipelines.Describe(Native, true)); return true;
	});
	EXPECT_EQ(Order, (std::vector<bool>{true, false}));
	EXPECT_EQ(Native->GetPriority(), EPSOPrecachePriority::Highest);
	EXPECT_FALSE(Component.IsReadyForProxyCreation());
	Pipelines.Release(); EXPECT_TRUE(Native->Wait());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	EXPECT_TRUE(Component.IsReadyForProxyCreation());
	EXPECT_FALSE(Component.CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority::Highest));
}

TEST(FPSOPrecacheTests, OptionalAdmissionFailureStillObservesPendingAndFailedRequiredWork)
{
    using namespace Durin;
    for (bool bRequiredFails : {false, true})
    {
        SCOPED_TRACE(bRequiredFails);
        FMaterialPSOTestPipelines Pipelines;
        auto Required = Pipelines.Request(!bRequiredFails, bRequiredFails);
        if (bRequiredFails) EXPECT_FALSE(Required->Wait());
        FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data)
            -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
            if (!Data.bRequired) return std::unexpected(ERHIPipelineRequestRejection::Unsupported);
            return Pipelines.Submit(Data);
        });
        auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
        Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
            Out.PSOInitializers = {Pipelines.Describe(Required), Pipelines.Describe(Required, false)};
            Out.bRequiredPSOsCollected = true;
            return true;
        });
        EXPECT_FALSE(Request.GetProgress().bRequiredReady);
        if (!bRequiredFails) {
            EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Pending);
            EXPECT_EQ(Request.GetProgress().RequiredPending, 1u);
            Pipelines.Release();
            ASSERT_TRUE(Required->Wait());
            Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
        }
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (Request.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < Deadline) {
            Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
            std::this_thread::yield();
        }
        EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
        EXPECT_EQ(Request.GetProgress().bRequiredReady, !bRequiredFails);
        EXPECT_EQ(Request.GetProgress().Failed, bRequiredFails ? 2u : 1u);
    }
}

TEST(FPSOPrecacheTests, PreparationConfigVersionInvalidatesReadyCoverageBeforeResourceRetirement)
{
	using namespace Durin;
	FMaterialPSORequestManager Manager;
	Manager.SetPreparationConfigVersion(1);
	auto Params = MaterialPSOTestParams();
	Params.PreparationConfig.FeatureLevel = Params.FeatureLevel;
	Params.PreparationConfig.Version = 1;
	auto Old = Manager.PrecachePSOs(Params);
	Manager.Pump([](const auto&, auto& Collection) -> std::expected<bool, std::string> {
		Collection.bRequiredPSOsCollected = true;
		return true;
	});
	ASSERT_TRUE(Old.GetProgress().bRequiredReady);
	EXPECT_EQ(Old.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	Manager.SetPreparationConfigVersion(2);
	EXPECT_FALSE(Old.IsCurrent());
	EXPECT_FALSE(Old.GetProgress().bRequiredReady);
	EXPECT_EQ(Old.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_FALSE(Manager.PrecachePSOs(Params).IsValid());
	Params.PreparationConfig.Version = 2;
	Params.PreparationConfig.Settings.bHybrid = false;
	auto Current = Manager.PrecachePSOs(Params);
	ASSERT_TRUE(Current.IsValid());
	EXPECT_NE(Current.GetValue(), Old.GetValue());
	Manager.InvalidateOutdatedPreparationConfigs();
	EXPECT_TRUE(Current.IsCurrent());
	Manager.Pump([](const auto& Snapshot, auto& Collection) -> std::expected<bool, std::string> {
		EXPECT_EQ(Snapshot.PreparationConfig.Version, 2u);
		EXPECT_FALSE(Snapshot.PreparationConfig.Settings.bHybrid);
		Collection.bRequiredPSOsCollected = true;
		return true;
	});
	EXPECT_TRUE(Current.GetProgress().bRequiredReady);
}

TEST(FPSOPrecacheTests, PreparationConfigSnapshotParticipatesInSharingAndPreservesFailureDiagnostics)
{
	using namespace Durin;
	FMaterialPSORequestManager Manager;
	Manager.SetPreparationConfigVersion(1);
	auto Params = MaterialPSOTestParams();
	Params.PreparationConfig.Version = 1;
	Params.PreparationConfig.FeatureLevel = Params.FeatureLevel;
	auto First = Manager.PrecachePSOs(Params);
	Params.PreparationConfig.bSupportsWireframe = false;
	auto Second = Manager.PrecachePSOs(Params);
	EXPECT_NE(First.GetValue(), Second.GetValue());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { return std::unexpected("Fixture collection failure"); });
	ASSERT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	Manager.SetPreparationConfigVersion(2);
	Manager.InvalidateOutdatedPreparationConfigs();
	EXPECT_FALSE(First.IsCurrent());
	EXPECT_FALSE(First.GetProgress().bRequiredReady);
	EXPECT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	EXPECT_EQ(First.GetDiagnostic(), "Fixture collection failure");
}

namespace
{
    class FRegistryProbeCollector final : public Durin::IPSOCollector
    {
    public:
        Durin::ERHIFeatureLevel FeatureLevel;
        FRegistryProbeCollector(int32 Index, Durin::ERHIFeatureLevel Level) : IPSOCollector(Index), FeatureLevel(Level) {}
        auto CollectPSOInitializers(const Durin::FSceneRenderPreparationConfig&, const Durin::FMaterialResource&,
            const Durin::FPSOPrecacheVertexFactoryData&, const Durin::FPSOPrecacheParams&, Durin::FPSOPrecacheDataArray&) -> void override {}
    };
    auto CreateDeferredProbe(Durin::ERHIFeatureLevel Level) -> std::unique_ptr<Durin::IPSOCollector>
    { return std::make_unique<FRegistryProbeCollector>(Durin::FPSOCollectorCreateManager::GetIndex(Durin::EShadingPath::Deferred, "RegistryProbe"), Level); }
    auto CreateMobileProbe(Durin::ERHIFeatureLevel Level) -> std::unique_ptr<Durin::IPSOCollector>
    { return std::make_unique<FRegistryProbeCollector>(Durin::FPSOCollectorCreateManager::GetIndex(Durin::EShadingPath::Mobile, "RegistryProbe"), Level); }
}

TEST(FPSOPrecacheTests, CollectorRegistrySeparatesShadingPathsNamesAndIndicesAndRejectsInvalidContributions)
{
    using namespace Durin;
    const auto DeferredCount = FPSOCollectorCreateManager::GetPSOCollectorCount(EShadingPath::Deferred);
    const auto MobileCount = FPSOCollectorCreateManager::GetPSOCollectorCount(EShadingPath::Mobile);
    {
        FRegisterPSOCollectorCreateFunction Deferred(&CreateDeferredProbe, EShadingPath::Deferred, "RegistryProbe");
        FRegisterPSOCollectorCreateFunction Mobile(&CreateMobileProbe, EShadingPath::Mobile, "RegistryProbe");
        ASSERT_TRUE(Deferred.IsRegistered()); ASSERT_TRUE(Mobile.IsRegistered());
        EXPECT_EQ(FPSOCollectorCreateManager::GetIndex(EShadingPath::Deferred, "RegistryProbe"), Deferred.GetIndex());
        EXPECT_EQ(FPSOCollectorCreateManager::GetIndex(EShadingPath::Mobile, "RegistryProbe"), Mobile.GetIndex());
        EXPECT_EQ(FPSOCollectorCreateManager::GetName(EShadingPath::Deferred, Deferred.GetIndex()), "RegistryProbe");
        EXPECT_EQ(FPSOCollectorCreateManager::GetCreateFunction(EShadingPath::Mobile, Mobile.GetIndex()), &CreateMobileProbe);
        {
            FRegisterPSOCollectorCreateFunction Duplicate(&CreateMobileProbe, EShadingPath::Deferred, "RegistryProbe");
            EXPECT_FALSE(Duplicate.IsRegistered());
        }
        EXPECT_EQ(FPSOCollectorCreateManager::GetCreateFunction(EShadingPath::Deferred, Deferred.GetIndex()), &CreateDeferredProbe);
        FRegisterPSOCollectorCreateFunction Null(nullptr, EShadingPath::Deferred, "NullCollector");
        FRegisterPSOCollectorCreateFunction Empty(&CreateMobileProbe, EShadingPath::Mobile, "");
        FRegisterPSOCollectorCreateFunction Invalid(&CreateMobileProbe, EShadingPath::Num, "InvalidCollector");
        EXPECT_FALSE(Null.IsRegistered()); EXPECT_FALSE(Empty.IsRegistered()); EXPECT_FALSE(Invalid.IsRegistered());
        EXPECT_EQ(FPSOCollectorCreateManager::GetCreateFunction(EShadingPath::Num, 0), nullptr);
        EXPECT_EQ(FPSOCollectorCreateManager::GetCreateFunction(EShadingPath::Deferred, -1), nullptr);
        EXPECT_EQ(FPSOCollectorCreateManager::GetCreateFunction(EShadingPath::Deferred, FPSOCollectorCreateManager::MaxPSOCollectorCount), nullptr);
    }
    EXPECT_EQ(FPSOCollectorCreateManager::GetPSOCollectorCount(EShadingPath::Deferred), DeferredCount);
    EXPECT_EQ(FPSOCollectorCreateManager::GetPSOCollectorCount(EShadingPath::Mobile), MobileCount);
    EXPECT_EQ(FPSOCollectorCreateManager::GetIndex(EShadingPath::Deferred, "RegistryProbe"), -1);
}

TEST(FPSOPrecacheTests, CollectorRegistryReusesRemovedSlotsWithoutMovingLiveIndicesAndBoundsCapacity)
{
    using namespace Durin;
    const auto Before = FPSOCollectorCreateManager::GetPSOCollectorCount(EShadingPath::Mobile);
    auto First = std::make_unique<FRegisterPSOCollectorCreateFunction>(&CreateMobileProbe, EShadingPath::Mobile, "FirstSlot");
    FRegisterPSOCollectorCreateFunction Second(&CreateMobileProbe, EShadingPath::Mobile, "SecondSlot");
    const auto FirstIndex = First->GetIndex(); const auto SecondIndex = Second.GetIndex();
    ASSERT_TRUE(First->IsRegistered() && Second.IsRegistered());
    First.reset();
    EXPECT_EQ(FPSOCollectorCreateManager::GetCreateFunction(EShadingPath::Mobile, FirstIndex), nullptr);
    FRegisterPSOCollectorCreateFunction Reused(&CreateMobileProbe, EShadingPath::Mobile, "ReusedSlot");
    EXPECT_EQ(Reused.GetIndex(), FirstIndex);
    EXPECT_EQ(FPSOCollectorCreateManager::GetIndex(EShadingPath::Mobile, "SecondSlot"), SecondIndex);
    std::vector<std::unique_ptr<FRegisterPSOCollectorCreateFunction>> Full;
    for (int32 Index = Before + 2; Index < FPSOCollectorCreateManager::MaxPSOCollectorCount; ++Index) {
        auto Entry = std::make_unique<FRegisterPSOCollectorCreateFunction>(&CreateMobileProbe, EShadingPath::Mobile, std::format("Capacity{}", Index));
        ASSERT_TRUE(Entry->IsRegistered()); Full.push_back(std::move(Entry));
    }
    FRegisterPSOCollectorCreateFunction Overflow(&CreateMobileProbe, EShadingPath::Mobile, "Overflow");
    EXPECT_FALSE(Overflow.IsRegistered());
}

TEST(FPSOPrecacheTests, CollectorFactoriesCreateIndependentInstancesAcrossThreadsAndForwardFeatureLevel)
{
    using namespace Durin;
    FRegisterPSOCollectorCreateFunction Registration(&CreateDeferredProbe, EShadingPath::Deferred, "RegistryProbe");
    ASSERT_TRUE(Registration.IsRegistered());
    std::vector<std::future<std::unique_ptr<IPSOCollector>>> Created;
    for (int32 Index = 0; Index < 8; ++Index) Created.push_back(std::async(std::launch::async, [&] {
        const auto Create = FPSOCollectorCreateManager::GetCreateFunction(EShadingPath::Deferred, Registration.GetIndex());
        return Create(ERHIFeatureLevel::ES3_1); // factory queries the registry again
    }));
    std::vector<std::unique_ptr<IPSOCollector>> Instances;
    for (auto& Result : Created) {
        auto Collector = Result.get(); ASSERT_TRUE(Collector);
        EXPECT_EQ(Collector->PSOCollectorIndex, Registration.GetIndex());
        EXPECT_EQ(static_cast<FRegistryProbeCollector&>(*Collector).FeatureLevel, ERHIFeatureLevel::ES3_1);
        for (const auto& Existing : Instances) EXPECT_NE(Existing.get(), Collector.get());
        Instances.push_back(std::move(Collector));
    }
}
