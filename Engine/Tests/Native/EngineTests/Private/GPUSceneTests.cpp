#include "MeshDrawCommandTestSupport.h"
#include "GPUScene.h"
#include "../../RDGTestAccess.h"
#include "SceneInfo.h"
#include "SceneTestAccess.h"
#include "Renderers/GPUSceneRendering.h"
#include "Renderers/InstanceCulling.h"
#include "Renderers/ViewRenderTelemetry.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Backend/RHICompletionBackend.h"
#include "DynamicRHI.h"
#include "RHICapabilities.h"
#include "RHICommandList.h"
#include "RHIContext.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RenderingThread.h"
#include "Threading/TaskRuntime.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Resources/RendererResourceCoordinator.h"
#include <gtest/gtest.h>

namespace Durin::RendererPrivate
{
    struct FMeshDrawCommandPassTestAccess
    {
        static auto SetCommands(FParallelMeshDrawCommandPass& Pass, FMeshCommandOneFrameArray Commands) -> void
        { Pass.VisibleMeshDrawCommands = std::move(Commands); Pass.bFinalized = true; }
    };
}

namespace Durin
{
namespace
{
    class FSceneBuffer final : public FRHIBuffer
    {
    public:
        explicit FSceneBuffer(const FRHIBufferCreateDesc& Desc) : FRHIBuffer(Desc), Bytes(Desc.Size) {}
        FByteBuffer Bytes;
    };
		class FGPUSceneRecordingContext : public IRHICommandContext
		{
		public:
            uint64 WrittenBytes = 0;
            FRHIGPUQueueTimeline Timeline{AllocateRHIDeviceGeneration(), {}};
            std::vector<FRHIGPUSyncPointRef> Signals;
            bool bCompleteImmediately = true;
            auto RHIBeginGPUSubmission(const FRHIGPUSubmissionDesc&) -> void override {}
            auto RHIEndGPUSubmission(const FRHIGPUSyncPointRef& Signal) -> void override
            { auto Reservation = Timeline.Reserve(); EXPECT_TRUE(FRHIGPUSyncPointBackend::Attach(Signal, Reservation)); EXPECT_TRUE(Timeline.MarkSubmitted(Reservation)); Signals.push_back(Reservation); if (bCompleteImmediately) EXPECT_TRUE(Timeline.ObserveCompleted(Reservation)); }
			std::vector<std::string> BarrierOperations;
			auto RHIBeginRenderPass(const FRHIRenderPassInfo&, FName) -> void override
			{ ADD_FAILURE() << "Unexpected render pass"; }
			auto RHIBindVertexBuffer(uint32, FRHIBuffer*, uint32) -> void override
			{ ADD_FAILURE() << "Unexpected vertex buffer binding"; }
			std::vector<std::vector<FRHIBufferTransition>> BufferBatches;
			std::vector<std::vector<FRHITextureTransition>> TextureBatches;
			auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBeginFrame"; }
			auto RHISubmitCommands() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISubmitCommands"; }
			auto RHIEndFrame() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndFrame"; }
			auto RHIBeginDiagnosticRegion(std::string_view Name) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBeginDiagnosticRegion"; }
			auto RHIEndDiagnosticRegion() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndDiagnosticRegion"; }
			auto RHIEndRenderPass() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndRenderPass"; }
			auto RHIBeginDrawingViewport(FRHIViewport* InViewport, FRHITexture* InRenderTargetRHI) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBeginDrawingViewport"; }
			auto RHIEndDrawingViewport(FRHIViewport* InViewport, bool bInPresent, bool bInLockToVsync) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIEndDrawingViewport"; }
			auto RHISetViewport(float InMinX, float InMinY, float InMinZ, float InMaxX, float InMaxY, float InMaxZ) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetViewport"; }
			auto RHISetScissor(float InMinX, float InMinY, float InWidth, float InHeight) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetScissor"; }
			auto RHISetDepthBias(float InConstantFactor, float InClamp, float InSlopeFactor) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetDepthBias"; }
			auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& InGraphicsPipelineState) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetGraphicsPipelineState"; }
			auto RHIBindIndexBuffer(FRHIBuffer* IndexBuffer, uint32 Offset) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBindIndexBuffer"; }
			auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions) -> void override
			{ BarrierOperations.emplace_back("FullBuffer"); BufferBatches.emplace_back(Transitions.begin(), Transitions.end()); }
			auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void override
			{ BarrierOperations.emplace_back("FullTexture"); TextureBatches.emplace_back(Transitions.begin(), Transitions.end()); }
			auto RHIBeginTransition(const std::shared_ptr<FRHITransition>&) -> void override
			{ BarrierOperations.emplace_back("BeginSplit"); }
			auto RHIEndTransition(const std::shared_ptr<FRHITransition>& Transition) -> void override
			{
				BarrierOperations.emplace_back("EndSplit");
				IRHICommandContext::RHIEndTransition(Transition);
			}
			auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination, std::span<const FRHIBufferCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyBuffer"; }
			auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyBufferToTexture"; }
			auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyTextureToBuffer"; }
			auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination, std::span<const FRHITextureCopyRegion> Regions) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHICopyTexture"; }
			auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void override
			{ auto& Bytes = static_cast<FSceneBuffer*>(Buffer)->Bytes; std::ranges::copy(Data, Bytes.begin() + Offset); WrittenBytes += Data.size(); }
			auto RHIInitializeTexture(FRHITexture* Texture) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIInitializeTexture"; }
			auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, FByteView SourceData) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIUpdateTexture2D"; }
			auto RHIUpdateTexture3D(FRHITexture* Texture, uint32 MipIndex, const FUpdateTextureRegion3D& UpdateRegion, uint32 SourceRowPitch, uint32 SourceDepthPitch, FByteView SourceData) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIUpdateTexture3D"; }
			auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, FByteBuffer& OutData) -> bool override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIReadTexture2D"; return {}; }
			auto RHIAcquireBackBuffer(FRHITexture* BackBuffer) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIAcquireBackBuffer"; }
			auto RHIBlockUntilGPUIdle() -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIBlockUntilGPUIdle"; }
			auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIPushConstants"; }
			auto RHISetShaderParameters(FRHIShader* InShader, const std::span<const FRHIShaderParameterResource>& InResourceParameters) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHISetShaderParameters"; }
			auto RHIDraw(const FRHIDrawArguments& Arguments) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIDraw"; }
			auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Arguments) -> void override
			{ ADD_FAILURE() << "Unexpected backend operation: RHIDrawIndexed"; }
		};
		class FGPUSceneTestRHI final : public FDynamicRHI
		{
		public:
			explicit FGPUSceneTestRHI(bool bExplicitSubmissions = false)
			{
				Queues.bSplitBarriers = true;
				if (bExplicitSubmissions) Queues.Queues.push_back({.Id = {}, .bGraphics = true});
				FRHICapabilities Capabilities;
				Capabilities.SupportedTextureDimensions =
					ERHITextureDimensionFlags::Texture2D
					| ERHITextureDimensionFlags::TextureCube;
				Capabilities.MaxTextureDimension2D = 4096;
				Capabilities.MaxTextureDimensionCube = 4096;
				Capabilities.MaxTextureArrayLayers = 256;
				Capabilities.ColorSampleCounts = ERHISampleCountFlags::Samples1;
				Capabilities.DepthSampleCounts = ERHISampleCountFlags::Samples1;
				Capabilities.MaxComputeWorkGroupCount = {65535, 65535, 65535};
				Capabilities.bSupportsIndirectDraw = true;
				Capabilities.bSupportsIndirectDispatch = true;
				PublishCapabilities(Capabilities);
			}
            auto EnableIndependentComputeForCompilation() -> void
            {
                Queues.bIndependentCompute = true; Queues.Compute = {1};
                Queues.Queues.push_back({.Id = {1}, .OwnershipDomain = 1, .bCompute = true});
            }
			~FGPUSceneTestRHI() override { ClearCapabilities(); }
			auto Init(const FRHIInitializationContext&) -> void override {}
			auto Shutdown() -> void override {}
			auto RHIGetQueueCapabilities() const -> const FRHIQueueCapabilities& override { return Queues; }
			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override {}
			auto RHIEndFrame() -> void override {}
			auto RHICreateViewport(const FRHIViewportCreateInfo&) -> TRefCountPtr<FRHIViewport> override { return {}; }
			auto RHIResizeViewport(FRHIViewport*, uint32, uint32, bool) -> void override {}
			auto RHICreateGraphicsPipelineState(FName, const FGraphicsPipelineStateInitializer&) noexcept -> TRefCountPtr<FRHIGraphicsPipelineState> override { return {}; }
			auto RHIGetDefaultContext() -> IRHICommandContext* override { return nullptr; }
			auto RHIGetViewportBackBuffer(FRHIViewport*) -> TRefCountPtr<FRHITexture> override { return {}; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&) noexcept -> TRefCountPtr<FRHIVertexDeclaration> override { return {}; }
			auto RHIIsTextureSupported(const FRHITextureCreateDesc&) const -> bool override { return false; }
			auto RHICreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc&) noexcept -> FTextureRHIRef override
			{ return {}; }
			auto RHICreateSampler(const FRHISamplerDesc&) noexcept -> TRefCountPtr<FRHISampler> override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&) noexcept -> TRefCountPtr<FRHIShader> override { return {}; }
			auto RHICreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc& Desc) noexcept -> FBufferRHIRef override
			{ return MakeRefCount<FSceneBuffer>(Desc); }
		private:
			FRHIQueueCapabilities Queues;
		};

	class FRejectGPUSceneAllocator final : public FRDGAllocator
	{
	public:
		auto Allocate(std::span<const FRDGAllocationRequest>, FRDGAllocatedResources&) -> FRDGAllocationResult override
		{ return std::unexpected(FRDGAllocationBudgetError{1, 0}); }
	};

	class FSnapshotProxy final : public FPrimitiveSceneProxy
	{
	public:
		std::shared_ptr<const FInstanceSceneDataBuffers> Source;
		auto GetLocalBounds() const -> FBox override { return FBox(FVector3(-1.0), FVector3(1.0)); }
		auto GetInstanceSceneDataBuffers() const -> std::shared_ptr<const FInstanceSceneDataBuffers> override { return Source; }
	};

	class FGPUSceneTests : public ::testing::Test
	{
	protected:
		FGPUSceneTestRHI RHI{true};
		FGPUSceneRecordingContext Backend;
		FDynamicRHI* Previous = GDynamicRHI;
		FRendererResourceCoordinator Coordinator;
		FRendererRDGAllocator Allocator{Coordinator};
		FRHICommandListExecutor Executor{Backend};
		std::unique_ptr<FSceneTestOwner> Scene;
		bool bStartedScheduler = false;
		struct FTestCommand { static constexpr auto GetName() -> const char* { return "GPUSceneTest"; } };
		auto SetUp() -> void override
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			if (!IsTaskSchedulerRunning()) { ASSERT_TRUE(InitializeTaskScheduler(2)); bStartedScheduler = true; }
			InitRenderingThread();
			GDynamicRHI = &RHI;
			Scene = std::make_unique<FSceneTestOwner>();
		}
		auto TearDown() -> void override
		{
			Scene.reset();
			EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { Allocator.Release_RenderThread(); RHIFlushDeferredResources(); });
			FlushRenderingCommands();
			GDynamicRHI = Previous;
			ShutdownRenderingThread();
			if (bStartedScheduler) ShutdownTaskScheduler();
		}
		auto Add(uint64 Id, std::shared_ptr<const FInstanceSceneDataBuffers> Source = {}) -> FSnapshotProxy*
		{
			auto Proxy = std::make_unique<FSnapshotProxy>();
			Proxy->Source = std::move(Source);
			auto* Raw = Proxy.get();
			FSceneInterfaceTestAccess::AddPrimitiveProxy(**Scene, FPrimitiveComponentId{Id}, std::move(Proxy), FMatrix(1.0), false);
			FlushRenderingCommands();
			return Raw;
		}
		auto PrepareStaticGPUScene(const FPreparedStaticMeshView& Prepared, const std::shared_ptr<FGPUSceneDynamicContext>& Context)
			-> std::vector<FGPUScenePrimitiveData>
		{
			auto Result = CollectGPUScenePrimitives(Prepared, Context);
			return Result.size() == Prepared.Primitives.size() && Context->Upload(Executor.GetImmediateCommandList(), Allocator)
				? Result : std::vector<FGPUScenePrimitiveData>{};
		}
		auto Info(uint32 Index = 0) -> FPrimitiveSceneInfo& { return *(*Scene)->GetPrimitiveSceneInfos()[Index]; }
		auto Dirty(EGPUSceneDirtyState Value) -> void
		{
			EnqueueRenderCommand<FTestCommand>([&, Value](FRHICommandListImmediate&) { Info().RequestGPUSceneUpdate(Value); });
			FlushRenderingCommands();
		}
		auto Request(FGPUScenePrimitiveHandle Handle, EGPUSceneDirtyState Value) -> bool
		{
			bool Accepted = false;
			EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
				auto& GPU = *(*Scene)->GPUScene;
				Accepted = GPU.ResolvePrimitive(Handle) && GPU.AddPrimitiveToUpdate(Handle.PersistentPrimitiveIndex, Value);
			});
			FlushRenderingCommands(); return Accepted;
		}
		auto Run() -> FGPUSceneUpdateResult
		{
			FGPUSceneUpdateResult Result;
			EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			FRDGBuilder Graph;
			Graph.EnablePassCulling();
			Result = (*Scene)->GPUScene->Update(Graph);
			const auto Written = Backend.WrittenBytes;
			const auto Executed = Graph.Execute(Executor.GetImmediateCommandList(), &Allocator);
			Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			EXPECT_TRUE(Executed) << (Executed ? "" : ToString(Executed.error()));
			EXPECT_TRUE((*Scene)->GPUScene->CompleteUpdate(Graph, Result));
			EXPECT_EQ(Backend.WrittenBytes - Written, Result.UploadBytes);
			EXPECT_TRUE(Graph.GetGPUCompletion());
			});
			FlushRenderingCommands();
			return Result;
		}
		static auto Sources(std::initializer_list<uint32> Ids) -> std::shared_ptr<FInstanceSceneDataBuffers>
		{
			auto Source = std::make_shared<FInstanceSceneDataBuffers>();
			for (const auto Id : Ids) Source->Instances.push_back({{Id}, Math::TranslationMatrix(FVector3(Id, 0.0, 0.0)), FBox(FVector3(-1.0), FVector3(1.0)), {}});
			return Source;
		}
	};

	TEST_F(FGPUSceneTests, PacksSemanticParametersWithSplitOriginAndTypedIds)
	{
		const FVector3 Origin(1e12 + 0.125, -1e12 + 0.25, 1e12 + 0.5);
		auto Params = FPrimitiveUniformShaderParametersBuilder().LocalToWorld(Math::TranslationMatrix(Origin))
			.WorldBounds(FBox(Origin - FVector3(2.0), Origin + FVector3(2.0))).InstanceSceneData(7, 3).Build();
		ASSERT_TRUE(Params);
		FPrimitiveSceneShaderData Primitive(*Params);
		EXPECT_EQ(std::bit_cast<uint32>(Primitive.Data[16].x), 7u);
		EXPECT_EQ(std::bit_cast<uint32>(Primitive.Data[16].y), 3u);
		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			EXPECT_DOUBLE_EQ(double(Primitive.Data[12][Axis]) + double(Primitive.Data[13][Axis]), Origin[Axis]);
			EXPECT_FLOAT_EQ(Primitive.Data[14][Axis], -2.f);
			EXPECT_FLOAT_EQ(Primitive.Data[15][Axis], 2.f);
		}
		FInstanceSceneShaderData Instance;
		ASSERT_TRUE(Instance.Build(17, 2, 24, {88}, Params->LocalToWorld, Params->WorldBounds));
		EXPECT_EQ(std::bit_cast<uint32>(Instance.Data[16].x), 17u);
		EXPECT_EQ(std::bit_cast<uint32>(Instance.Data[16].w), 24u);
		EXPECT_EQ(std::bit_cast<uint32>(Instance.Data[17].y), 88u);
		EXPECT_FALSE(FPrimitiveUniformShaderParametersBuilder().LocalToWorld(FMatrix(0.0)).Build());
	}

	TEST_F(FGPUSceneTests, PublishesHiddenOrdinaryEmptyAndStableInstances)
	{
		Add(1); Add(2, Sources({})); Add(3, Sources({30, 10, 20}));
		const auto Uploaded = Run();
        EXPECT_GT(Uploaded.UploadBytes, 0u);
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            const auto Stats = (*Scene)->GPUScene->GetStatistics();
            EXPECT_EQ(Stats.UploadBytes, Uploaded.UploadBytes);
            EXPECT_EQ(Stats.UploadRanges, Uploaded.UploadRanges);
            EXPECT_EQ(Stats.BufferRebuilds, Uploaded.BufferRebuilds);
            EXPECT_GT(Stats.CapacityBytes, 0u);
        });
        FlushRenderingCommands();
		EXPECT_EQ(Info(0).NumInstanceSceneDataEntries, 1u);
		EXPECT_EQ(Info(1).NumInstanceSceneDataEntries, 0u);
		EXPECT_EQ(Info(2).NumInstanceSceneDataEntries, 3u);
		const auto Data = (*Scene)->GPUScene->GetInstanceSceneData();
		const auto Offset = Info(2).InstanceSceneDataOffset;
		EXPECT_EQ(std::bit_cast<uint32>(Data[Offset].Data[17].y), 10u);
		EXPECT_EQ(std::bit_cast<uint32>(Data[Offset + 2].Data[17].y), 30u);
		EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, SparseUpdateAdmissionDeduplicatesAndDoesNotRescanUnchangedScene)
	{
		auto* Target = Add(1, Sources({10, 20}));
		for (uint64 Index = 2; Index <= 64; ++Index) Add(Index);
		Run();
		FGPUSceneStatistics Before;
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { Before = (*Scene)->GPUScene->GetStatistics(); });
		FlushRenderingCommands();
		auto Changed = Sources({10, 20}); Changed->Instances[1].CustomData.Values[3] = 42.f;
		Target->Source = Changed;
		for (uint32 Index = 0; Index < 16; ++Index) Dirty(EGPUSceneDirtyState::InstanceData);
		const auto Patch = Run();
		EXPECT_EQ(Patch.UploadBytes, sizeof(FPrimitiveCustomData));
		EXPECT_EQ(Patch.UploadRanges, 1u);
		EXPECT_FLOAT_EQ((*Scene)->GPUScene->GetInstancePayloadData()[Info().InstanceSceneDataOffset + 1].Values[3], 42.f);
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto& GPU = *(*Scene)->GPUScene;
			EXPECT_EQ(GPU.GetStatistics().CPUPrimitiveUpdateVisits, Before.CPUPrimitiveUpdateVisits + 1);
			for (uint32 Index = 0; Index < 16; ++Index) { GPU.UpdateCPUData(); EXPECT_FALSE(GPU.IsDirty()); }
			EXPECT_EQ(GPU.GetStatistics().CPUPrimitiveUpdateVisits, Before.CPUPrimitiveUpdateVisits + 1);
			// A later collector phase in the same frame must still admit work.
			EXPECT_TRUE(GPU.AddPrimitiveToUpdate(Info().GetPersistentIndex()));
			GPU.UpdateCPUData();
			EXPECT_EQ(GPU.GetStatistics().CPUPrimitiveUpdateVisits, Before.CPUPrimitiveUpdateVisits + 2);
			EXPECT_FALSE(GPU.IsDirty());
		});
		FlushRenderingCommands();
	}

	TEST_F(FGPUSceneTests, QueuedRemovalAndReusedSlotDoNotDropTheNewPrimitiveUpdate)
	{
		Add(1); const auto Old = Info().GPUSceneHandle;
		FSceneInterfaceTestAccess::RemovePrimitiveProxy(**Scene, FPrimitiveComponentId{1});
		FlushRenderingCommands();
		Add(2, Sources({20, 30}));
		EXPECT_EQ(Info().GPUSceneHandle.PersistentPrimitiveIndex, Old.PersistentPrimitiveIndex);
		EXPECT_NE(Info().GPUSceneHandle.Generation, Old.Generation);
		Run();
		EXPECT_EQ(Info().NumInstanceSceneDataEntries, 2u);
		EXPECT_FALSE(Request(Old, EGPUSceneDirtyState::Transform));
		EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, GraphRegistrationUsesCommittedGenerationOutsideUpdateAndAcrossAbandonment)
	{
		Add(1); Run();
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto& GPU = *(*Scene)->GPUScene;
			const auto OldBuffers = GPU.GetResourceBuffers();
			FRDGBuilder FirstGraph, SecondGraph;
			const auto First = GPU.GetShaderParameters(FirstGraph);
			const auto Again = GPU.GetShaderParameters(FirstGraph);
			const auto Second = GPU.GetShaderParameters(SecondGraph);
			ASSERT_TRUE(First.GPUSceneInstanceSceneData); ASSERT_TRUE(Second.GPUSceneInstanceSceneData);
			EXPECT_EQ(First.GPUSceneInstanceSceneData->Buffer, Again.GPUSceneInstanceSceneData->Buffer);
			EXPECT_NE(First.GPUSceneInstanceSceneData->Buffer, Second.GPUSceneInstanceSceneData->Buffer);
			const auto Allocation = GPU.AllocateDynamicPrimitiveRange(32);
			{
				FRDGBuilder Abandoned;
				const auto Pending = GPU.Update(Abandoned);
				EXPECT_EQ(GPU.GetShaderParameters(Abandoned).GPUSceneResourceGeneration, Pending.Parameters.GPUSceneResourceGeneration);
				FRDGBuilder Reader;
				const auto Committed = GPU.GetShaderParameters(Reader);
				EXPECT_EQ(Committed.GPUSceneNumAllocatedInstances, First.GPUSceneNumAllocatedInstances);
				EXPECT_EQ(Committed.GPUSceneResourceGeneration, First.GPUSceneResourceGeneration);
				EXPECT_LT(Committed.GPUSceneNumAllocatedInstances, Pending.Parameters.GPUSceneNumAllocatedInstances);
			}
			EXPECT_TRUE(GPU.IsDirty());
			FRDGBuilder UpdateGraph;
			const auto Update = GPU.Update(UpdateGraph);
			ASSERT_TRUE(UpdateGraph.Execute(Executor.GetImmediateCommandList(), &Allocator));
			ASSERT_TRUE(GPU.CompleteUpdate(UpdateGraph, Update));
			FRDGBuilder NewGraph;
			const auto New = GPU.GetShaderParameters(NewGraph);
			EXPECT_GT(New.GPUSceneResourceGeneration, First.GPUSceneResourceGeneration);
			EXPECT_NE(GPU.GetResourceBuffers()[1], OldBuffers[1]);
			EXPECT_EQ(FirstGraph.FindExternalBuffer(OldBuffers[1]), First.GPUSceneInstanceSceneData->Buffer);
			GPU.ReleaseDynamicPrimitiveRange(Allocation);
			GPU.ReleaseRHI();
			FRDGBuilder EmptyGraph;
			const auto Empty = GPU.GetShaderParameters(EmptyGraph);
			EXPECT_FALSE(Empty.GPUSceneInstanceSceneData);
			EXPECT_EQ(Empty.GPUSceneNumAllocatedInstances, 0u);
			EXPECT_EQ(FirstGraph.FindExternalBuffer(OldBuffers[1]), First.GPUSceneInstanceSceneData->Buffer);
		});
		FlushRenderingCommands();
	}

	TEST_F(FGPUSceneTests, TypedReadParametersShareSceneAttributesAndValidateExternalContracts)
	{
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto MakeBuffer = [] { return MakeRefCount<FSceneBuffer>(FRHIBufferCreateDesc::Create("SceneRead", 64, 4,
				EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::DestinationCopy)); };
			auto Set = std::make_shared<FGPUSceneBufferSet>();
			for (auto& Buffer : Set->SceneData) Buffer = MakeBuffer();
			Set->InstanceIds = MakeBuffer();
			FRDGBuilder Graph;
			FResolvedStaticMeshView View; View.GPUSceneBuffers = Set; View.DynamicInstancingInstanceIds = Set->InstanceIds;
			const auto First = View.GetInstanceCullingDrawParams(Graph);
			const auto Again = Set->GetShaderParameters(Graph);
			ASSERT_TRUE(First.Scene.GPUScenePrimitiveSceneData); ASSERT_TRUE(First.InstanceIdsBuffer);
			ASSERT_TRUE(First.DynamicInstanceIdsBuffer);
			EXPECT_EQ(First.DynamicInstanceIdsBuffer->Buffer, First.InstanceIdsBuffer->Buffer);
			EXPECT_EQ(First.Scene.GPUScenePrimitiveSceneData->Buffer, Again.GPUScenePrimitiveSceneData->Buffer);
			EXPECT_EQ(First.InstanceIdsBuffer->Buffer, Graph.FindExternalBuffer(Set->InstanceIds));
			// Independent pass ID lists do not accumulate into a receiver/cascade-shaped fixed array.
			for (int Index = 0; Index < 64; ++Index)
			{
				const auto Other = MakeBuffer(); View.DynamicInstancingInstanceIds = Other;
				const auto Parameters = View.GetInstanceCullingDrawParams(Graph);
				ASSERT_TRUE(Parameters.DynamicInstanceIdsBuffer);
				EXPECT_EQ(Parameters.Scene.GPUSceneInstanceSceneData->Buffer, First.Scene.GPUSceneInstanceSceneData->Buffer);
				EXPECT_EQ(Parameters.DynamicInstanceIdsBuffer->Buffer, Graph.FindExternalBuffer(Other));
			}
			FRDGBuilder Conflict;
			Conflict.RegisterExternalBuffer(Set->InstanceIds, "ConflictingRead", ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead);
			(void)View.GetInstanceCullingDrawParams(Conflict);
			EXPECT_FALSE(Conflict.Execute(Executor.GetImmediateCommandList(), &Allocator));
		});
		FlushRenderingCommands();
	}

	TEST_F(FGPUSceneTests, DirectIdsReusePersistentBuffersAndDynamicFactsOwnTheirStorage)
	{
		Add(1); Run();
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			FPreparedStaticMeshView Prepared;
			FPreparedStaticMeshPrimitive Primitive;
			Primitive.PrimitiveId = FPrimitiveComponentId{1}; Primitive.BatchId = 901;
			Primitive.PrimitiveIdMode = EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo;
			Primitive.PersistentPrimitiveIndex = Info().GetPersistentIndex();
			Primitive.LocalToWorld = FMatrix(1.0); Primitive.WorldBounds = Info().GetWorldBounds();
			Prepared.Primitives.push_back(Primitive);
			auto Description = std::make_shared<FMeshDrawDescription>();
			Description->PipelineKey.FactoryKey = FStaticMeshBatchBinding{}.GetFactoryKey();
			Description->Geometry.InstanceCount = 3;
			FPreparedStaticMeshDraw Draw; Draw.Command = Description;
			Prepared.Opaque.push_back(Draw);
			auto Data = PrepareStaticGPUScene(Prepared, std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene));
			ASSERT_EQ(Data.size(), 1u); ASSERT_TRUE(Data[0].Buffers);
			EXPECT_EQ(Data[0].Buffers->SceneData, (*Scene)->GPUScene->GetResourceBuffers());
			EXPECT_EQ(Data[0].InstanceId, Info().InstanceSceneDataOffset);
			FRDGBuilder ParametersGraph; const auto Parameters = Data[0].Buffers->GetShaderParameters(ParametersGraph);
			EXPECT_EQ(Parameters.GPUSceneNumAllocatedInstances, (*Scene)->GPUScene->GetInstanceSceneData().size());
			EXPECT_EQ(Parameters.GPUSceneResourceGeneration, (*Scene)->GPUScene->GetResourceGeneration());
			Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Ids = static_cast<FSceneBuffer*>(Data[0].Buffers->InstanceIds.GetReference())->Bytes;
			ASSERT_GE(Ids.size(), 3 * sizeof(uint32));
			for (uint32 Index = 0; Index < 3; ++Index)
			{
				uint32 Value; std::memcpy(&Value, Ids.data() + Index * sizeof(uint32), sizeof(uint32));
				EXPECT_EQ(Value, Data[0].InstanceId);
			}
			Prepared.Primitives[0].PersistentPrimitiveIndex = {};
			EXPECT_TRUE(PrepareStaticGPUScene(Prepared, std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene)).empty());
			Prepared.Primitives[0].PrimitiveIdMode = EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData;
			Prepared.Primitives[0].LocalToWorld = Math::TranslationMatrix(FVector3(13.0, 0.0, 0.0));
			auto Scoped = PrepareStaticGPUScene(Prepared, std::make_shared<FGPUSceneDynamicContext>());
			ASSERT_EQ(Scoped.size(), 1u); ASSERT_TRUE(Scoped[0].Buffers);
			EXPECT_NE(Scoped[0].Buffers->SceneData[0], Data[0].Buffers->SceneData[0]);
			Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Packed = static_cast<FSceneBuffer*>(Scoped[0].Buffers->SceneData[1].GetReference())->Bytes;
			FInstanceSceneShaderData Instance; std::memcpy(&Instance, Packed.data(), sizeof(Instance));
			EXPECT_FLOAT_EQ(Instance.Data[12].x, 13.f);
			Description->PipelineKey.FactoryKey = {};
			auto Unsupported = PrepareStaticGPUScene(Prepared, std::make_shared<FGPUSceneDynamicContext>());
			ASSERT_EQ(Unsupported.size(), 1u); EXPECT_FALSE(Unsupported[0].Buffers);
		});
		FlushRenderingCommands();
		EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, SubmissionCollectorUsesExplicitRecordsAndPreservesEarlierSnapshots)
	{
		Add(1); Run();
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto& Commands = Executor.GetImmediateCommandList();
			auto GPU = (*Scene)->GPUScene;
			auto Context = std::make_shared<FGPUSceneDynamicContext>(GPU);
			auto Record = std::make_shared<FGPUSceneDynamicPrimitiveData>();
			Record->WorldBounds = FBox(FVector3(-1.0), FVector3(1.0));
			std::vector<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>> Records{Record, Record};
			auto Collect = [](const std::shared_ptr<FGPUSceneDynamicContext>& Owner, const auto& Inputs) {
				FGPUScenePrimitiveCollector Collector(Owner);
				for (const auto& Input : Inputs) Collector.Add(Input);
				require(Collector.Commit());
				std::vector<FGPUScenePrimitiveData> Result;
				for (uint32 Index = 0; Index < Collector.Num(); ++Index) Result.push_back(Collector.GetPrimitiveData(Index));
				return Result;
			};
			const auto First = Collect(Context, Records);
			ASSERT_EQ(First.size(), 2u); EXPECT_FALSE(First[0].Buffers->SceneData[1]); EXPECT_EQ(First[0].InstanceId, First[1].InstanceId);
			const auto Count = GPU->GetInstanceSceneData().size(); const auto Bytes = GPU->GetStatistics().UploadBytes;
			const auto Again = Collect(Context, Records);
			ASSERT_EQ(Again.size(), 2u); EXPECT_EQ(Again[0].InstanceId, First[0].InstanceId);
			EXPECT_EQ(Again[0].Buffers->DynamicContext, First[0].Buffers->DynamicContext);
			EXPECT_EQ(GPU->GetInstanceSceneData().size(), Count); EXPECT_EQ(GPU->GetStatistics().UploadBytes, Bytes);
			const auto Old = GPU->GetInstanceSceneData()[First[0].InstanceId];
			auto Revision = std::make_shared<FGPUSceneDynamicPrimitiveData>(*Record);
			Revision->CustomInstanceData.Values[0] = 7.f; Records[0] = Revision;
			const auto Changed = Collect(Context, Records);
			ASSERT_EQ(Changed.size(), 2u); EXPECT_NE(Changed[0].InstanceId, First[0].InstanceId);
			EXPECT_EQ(Changed[1].InstanceId, First[1].InstanceId);
			EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(&Old, 1)), std::as_bytes(GPU->GetInstanceSceneData().subspan(First[0].InstanceId, 1))));
			EXPECT_FLOAT_EQ(GPU->GetInstancePayloadData()[Changed[0].InstanceId].Values[0], 7.f);
			// Equal transported values do not identify a collector record.
			Records[0] = std::make_shared<FGPUSceneDynamicPrimitiveData>(*Revision);
			const auto Distinct = Collect(Context, Records);
			ASSERT_EQ(Distinct.size(), 2u); EXPECT_NE(Distinct[0].InstanceId, Changed[0].InstanceId);
			EXPECT_EQ(GPU->GetStatistics().UploadBytes, Bytes);
			ASSERT_TRUE(Context->Upload(Commands, Allocator));
			EXPECT_EQ(First[0].Buffers->SceneData, Changed[0].Buffers->SceneData);
			EXPECT_EQ(Again[0].Buffers->SceneData, Distinct[0].Buffers->SceneData);
			EXPECT_EQ(First[0].Buffers->InstanceIds, Changed[0].Buffers->InstanceIds);
			EXPECT_EQ(Again[0].Buffers->InstanceIds, Distinct[0].Buffers->InstanceIds);
			EXPECT_NE(First[0].InstanceIdOffset, Again[0].InstanceIdOffset);
			EXPECT_EQ(GPU->GetStatistics().InstanceIdBufferCreates, 1u);
			const auto Uploaded = GPU->GetStatistics().UploadBytes;
			ASSERT_TRUE(Context->Upload(Commands, Allocator)); EXPECT_EQ(GPU->GetStatistics().UploadBytes, Uploaded);
			const auto IndependentContext = std::make_shared<FGPUSceneDynamicContext>();
			const auto Independent = Collect(IndependentContext, Records);
			ASSERT_TRUE(IndependentContext->Upload(Commands, Allocator));
			ASSERT_EQ(Independent.size(), 2u); EXPECT_NE(Independent[0].Buffers->DynamicContext, Context);
			EXPECT_NE(Independent[0].Buffers->SceneData[1], Changed[0].Buffers->SceneData[1]);
			auto OtherGPU = CreateRenderResource<FGPUScene>();
			auto OtherContext = std::make_shared<FGPUSceneDynamicContext>(OtherGPU);
			const auto Other = Collect(OtherContext, Records);
			ASSERT_EQ(Other.size(), 2u); EXPECT_NE(Other[0].Buffers->SceneData[1], Changed[0].Buffers->SceneData[1]);
		});
		FlushRenderingCommands();
	}

    TEST_F(FGPUSceneTests, GenericCollectorBatchesMixedSourcesAndBuffersOwnTheContext)
    {
        Add(1); Run();
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            auto Context = std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene);
            std::weak_ptr<FGPUSceneDynamicContext> Lifetime = Context;
            FGPUScenePrimitiveData First, Persistent, Shared;
            {
                FGPUScenePrimitiveCollector Collector(Context);
                auto Record = std::make_shared<FGPUSceneDynamicPrimitiveData>();
                Record->WorldBounds = FBox(FVector3(-1.0), FVector3(1.0));
                const auto A = Collector.Add(Record, 2);
                const auto B = Collector.Add(Info().GetPersistentIndex(), FPrimitiveInstanceId{}, 1);
                const auto C = Collector.Add(Record, 1);
                EXPECT_EQ(Collector.Num(), 3u);
                ASSERT_TRUE(Collector.Commit());
                First = Collector.GetPrimitiveData(A); Persistent = Collector.GetPrimitiveData(B); Shared = Collector.GetPrimitiveData(C);
            }
            EXPECT_EQ(First.InstanceId, Shared.InstanceId);
            EXPECT_EQ(Persistent.InstanceId, Info().InstanceSceneDataOffset);
            EXPECT_NE(First.InstanceId, Persistent.InstanceId);
            EXPECT_EQ(Persistent.InstanceIdOffset, First.InstanceIdOffset + 2);
            EXPECT_EQ(Shared.InstanceIdOffset, First.InstanceIdOffset + 3);
            Context.reset(); EXPECT_FALSE(Lifetime.expired());
            // Collectors are gone before the single shared upload phase.
            ASSERT_TRUE(First.Buffers->DynamicContext->Upload(Executor.GetImmediateCommandList(), Allocator));
            Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
            ASSERT_TRUE(First.Buffers->SceneData[1]);
            const auto& Bytes = static_cast<FSceneBuffer*>(First.Buffers->InstanceIds.GetReference())->Bytes;
            const std::array Expected{First.InstanceId, First.InstanceId, Persistent.InstanceId, Shared.InstanceId};
            ASSERT_GE(Bytes.size(), sizeof(Expected));
            EXPECT_TRUE(std::ranges::equal(std::span(Bytes).first(sizeof(Expected)), std::as_bytes(std::span(Expected))));
            First = {}; Persistent = {}; Shared = {};
            EXPECT_TRUE(Lifetime.expired());
        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, GenericCollectorRejectsInvalidPublicationBeforeAllocating)
    {
        Add(1); Run();
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            auto GPU = (*Scene)->GPUScene;
            auto Context = std::make_shared<FGPUSceneDynamicContext>(GPU);
            const auto Before = GPU->GetInstanceSceneData().size();
            auto Record = std::make_shared<FGPUSceneDynamicPrimitiveData>();
            FGPUScenePrimitiveCollector Empty(Context); EXPECT_TRUE(Empty.Commit()); EXPECT_EQ(Empty.Num(), 0u);
            FGPUScenePrimitiveCollector Overflow(Context); Overflow.Add(Record, UINT32_MAX);
            EXPECT_FALSE(Overflow.Commit()); EXPECT_EQ(GPU->GetInstanceSceneData().size(), Before);
            FGPUScenePrimitiveCollector Missing(Context); Missing.Add(Record);
            Missing.Add(Info().GetPersistentIndex(), FPrimitiveInstanceId{999});
            EXPECT_FALSE(Missing.Commit()); EXPECT_EQ(GPU->GetInstanceSceneData().size(), Before);
            // A rejected mixed collector did not publish a dynamic identity or ID offset.
            FGPUScenePrimitiveCollector Retry(Context); const auto Index = Retry.Add(Record);
            ASSERT_TRUE(Retry.Commit()); EXPECT_EQ(Retry.GetPrimitiveData(Index).InstanceIdOffset, 0u);
            EXPECT_EQ(GPU->GetInstanceSceneData().size(), Before + 1);
        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, PersistentCollectorRejectsRemovedAndReusedSceneSlots)
    {
        Add(1); Run();
        std::shared_ptr<FGPUSceneDynamicContext> Context;
        std::unique_ptr<FGPUScenePrimitiveCollector> Collector;
        const auto Old = Info().GPUSceneHandle;
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            Context = std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene);
            Collector = std::make_unique<FGPUScenePrimitiveCollector>(Context);
            Collector->Add(Old.PersistentPrimitiveIndex, FPrimitiveInstanceId{});
        });
        FlushRenderingCommands();
        FSceneInterfaceTestAccess::RemovePrimitiveProxy(**Scene, FPrimitiveComponentId{1});
        FlushRenderingCommands(); Add(2); Run();
        ASSERT_EQ(Info().GetPersistentIndex(), Old.PersistentPrimitiveIndex);
        EXPECT_NE(Info().GPUSceneHandle.Generation, Old.Generation);
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            EXPECT_EQ(Context->GetGPUScene().GetPrimitiveSceneInfo(Old.PersistentPrimitiveIndex), &Info());
            EXPECT_FALSE(Collector->Commit());
            EXPECT_TRUE(Context->Upload(Executor.GetImmediateCommandList(), Allocator));
            Collector.reset(); Context.reset();
        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, ParallelMeshPassDispatchAggregatesSourcesAndValidatesRanges)
    {
        using namespace RendererPrivate;
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            FMeshCommandOneFrameArray Commands(2);
            Commands[0].NumSourceDraws = 64; Commands[0].SourceDrawIndex = 10;
            Commands[1].NumSourceDraws = 3; Commands[1].SourceDrawIndex = 42;
            FParallelMeshDrawCommandPass Pass;
            FMeshDrawCommandPassTestAccess::SetCommands(Pass, std::move(Commands));
            FMeshDrawCommandPassStats Statistics;
            const auto Plain = Pass.Draw(Executor.GetImmediateCommandList());
            const auto Result = Pass.Draw(Executor.GetImmediateCommandList(), {}, nullptr, &Statistics);
            EXPECT_FALSE(Result.bComplete); EXPECT_EQ(Result.Attempted, 67u); EXPECT_EQ(Result.Rejected, 67u);
            EXPECT_EQ(Plain.Attempted, Result.Attempted); EXPECT_EQ(Plain.Rejected, Result.Rejected);
            EXPECT_EQ(Result.NativeSubmissions, 0u);
            ASSERT_EQ(Statistics.DrawData.size(), 2u);
            EXPECT_EQ(Statistics.DrawData[0].SourceDrawIndex, 10u); EXPECT_EQ(Statistics.DrawData[0].NumSourceDraws, 64u);
            EXPECT_EQ(Statistics.DrawData[1].SourceDrawIndex, 42u); EXPECT_FALSE(Statistics.DrawData[1].bSucceeded);
            FMeshDrawCommandPassStats SubsetStats;
            const auto Subset = Pass.Draw(Executor.GetImmediateCommandList(), 1, 1, {}, nullptr, &SubsetStats);
            EXPECT_EQ(Subset.Attempted, 3u); ASSERT_EQ(SubsetStats.DrawData.size(), 1u);
            EXPECT_EQ(SubsetStats.DrawData[0].SourceDrawIndex, 42u);
            EXPECT_FALSE(Pass.Draw(Executor.GetImmediateCommandList(), 2, 1, {}, nullptr, &SubsetStats).bComplete);
            EXPECT_TRUE(Pass.Draw(Executor.GetImmediateCommandList(), 2, 0).bComplete);
            EXPECT_EQ(SubsetStats.DrawData.size(), 1u);
            FRendererResourceCoordinator Coordinator;
            FInstanceCullingManager Manager(Coordinator);
            auto Context = Manager.CreateContext(Manager.RegisterView(FSceneView{}), {});
            FMeshDrawCommandPassStats CullingStats;
            auto CullingCommands = FMeshCommandOneFrameArray(Pass.GetMeshDrawCommands().begin(), Pass.GetMeshDrawCommands().end());
            Context->SetupDrawCommands(CullingCommands, {});
            const auto CullingResult = Context->SubmitDrawCommands(Pass.GetMeshDrawCommands(), {}, 0, 2,
                Executor.GetImmediateCommandList(), nullptr, &CullingStats);
            EXPECT_EQ(CullingResult.Rejected, Plain.Rejected); ASSERT_EQ(CullingStats.DrawData.size(), 2u);
            EXPECT_EQ(CullingStats.DrawData[1].SourceDrawIndex, 42u);

        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, ParallelDrawRecordingMatchesSerialRangesSourcesAndCounters)
    {
        using namespace RendererPrivate;
        FRHIPipelineCompileBackend CompileBackend{
            .FindGraphics = [](const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
            .FindCompute = [](const auto&) -> FComputePipelineStateRHIRef { return {}; },
            .CreateGraphics = [](const auto&, const auto&) -> FGraphicsPipelineStateRHIRef { return MakeRefCount<FRHIGraphicsPipelineState>(); },
            .CreateCompute = [](const auto&, const auto&) -> FComputePipelineStateRHIRef { return {}; }
        };
        auto Capabilities = *RHI.RHIGetCapabilities();
        Capabilities.MaxColorAttachments = 1;
        FRHIPipelineStateCache Cache(Capabilities, std::move(CompileBackend));
        FGraphicsPipelineStateInitializer Description;
        const auto VertexShader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Vertex, FXxHash128{}));
        const auto FragmentShader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Fragment, FXxHash128{}));
        Description.BoundShaders = {VertexShader, FragmentShader};
        class FDeclaration final : public FRHIVertexDeclaration
        {
        public:
            FVertexDeclarationElementList Elements{};
            auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
        };
        const auto Declaration = MakeRefCount<FDeclaration>();
        Description.VertexDeclaration = Declaration;
        Description.DepthStencilState.bEnableTest = false;
        Description.DepthStencilState.bEnableWrite = false;
        Description.RenderTargetLayout.NumColorRenderTargets = 1;
        Description.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::RGBA8_UNORM;
        auto Key = BuildGraphicsPipelineStateKey(Description, &Capabilities);
        ASSERT_TRUE(Key) << ToString(Key.error());
        auto Cached = Cache.GetGraphics(Description, "parallel recording");
        ASSERT_TRUE(Cached); ASSERT_EQ(Cached->Wait(), ERHIPipelineState::Ready);
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            constexpr uint32 Count = 513;
            const auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Vertex, FXxHash128{}));
            FMeshPassPipelineInitializer Initializer;
            Initializer.PipelineState = Cached;
            const auto Pipeline = std::make_shared<const FMeshPassPipeline>(std::move(Initializer));
            FMeshPassBindings Bindings; Bindings.Vertex.Shader = Shader;
            std::vector<FMeshDrawCommand> Draws(Count);
            FMeshCommandOneFrameArray Visible;
            for (uint32 Index = 0; Index < Count; ++Index) {
                auto& Draw = Draws[Index]; Draw.bFinalized = true; Draw.Pipeline = Pipeline.get();
                Draw.VertexBindings.Shader = Shader;
                Draw.DrawParameters = FRHIDrawArguments{.VertexCount = 3, .FirstVertex = Index * 3};
                FVisibleMeshDrawCommand Item;
                Item.Command = &Draw; Item.Pass = &Bindings; Item.SourceDrawIndex = Index;
                Item.NumSourceDraws = Index % 5 + 1; Item.bSourceReady = Index % 17 != 0;
                Visible.push_back(Item);
            }
            FParallelMeshDrawCommandPass Pass;
            FMeshDrawCommandPassTestAccess::SetCommands(Pass, std::move(Visible));
            FSceneView View; View.ViewportX = 7; View.ViewportY = 9; View.ViewportWidth = 64; View.ViewportHeight = 32;
            for (const bool Parallel : {false, true}) {
                FRHICommandList Parent; Parent.SwitchPipeline(ERHIPipeline::Graphics); Parent.BeginRenderPass({}, "DrawTest");
                FParallelCommandListSet Set(View, Parent); Set.Width = 4; Set.MinDrawsPerCommandList = 64;
                FMeshDrawCommandPassStats Statistics;
                const auto Result = Pass.DispatchDraw(Parallel ? &Set : nullptr, Parent, 3, Count - 7, {}, &Statistics);
                size_t Attempted = 0, Successful = 0, Native = 0;
                ASSERT_EQ(Statistics.DrawData.size(), Count - 7);
                for (uint32 Offset = 0; Offset < Statistics.DrawData.size(); ++Offset) {
                    const uint32 Source = Offset + 3;
                    const auto& Data = Statistics.DrawData[Offset];
                    EXPECT_EQ(Data.SourceDrawIndex, Source); EXPECT_EQ(Data.NumSourceDraws, Source % 5 + 1);
                    EXPECT_EQ(Data.bSucceeded, Source % 17 != 0);
                    Attempted += Source % 5 + 1;
                    if (Data.bSucceeded) { Successful += Source % 5 + 1; ++Native; }
                }
                EXPECT_EQ(Result.Attempted, Attempted); EXPECT_EQ(Result.Successful, Successful);
                EXPECT_EQ(Result.Rejected, Attempted - Successful); EXPECT_EQ(Result.NativeSubmissions, Native);
                EXPECT_EQ(Parent.GetNumRecordedDrawCommands(), Native);
                EXPECT_TRUE(Parent.IsInsideRenderPass());
                Parent.EndRenderPass(); Parent.FinishRecording();
            }
        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, ParallelRecordingGroupHonorsPrerequisites)
    {
        using namespace RendererPrivate;
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            FRHICommandList Parent; Parent.SwitchPipeline(ERHIPipeline::Graphics); Parent.BeginRenderPass({}, "Prerequisites");
            const auto Before = Parent.GetNumRecordedCommands();
            FParallelCommandListSet Set(FSceneView{}, Parent);
            auto* Child = Set.NewParallelCommandList();
            bool bPrerequisiteRan = false, bVisited = false;
            auto Prerequisite = Tasks::LaunchIndependentTask("Test.DrawPrerequisite", [&] { bPrerequisiteRan = true; });
            Tasks::FTaskExecutionOptions Options; Options.Prerequisites.push_back(Prerequisite);
            auto Task = Tasks::LaunchIndependentTask("Test.ParallelDraw", [Child, &bVisited, &bPrerequisiteRan] {
                EXPECT_TRUE(bPrerequisiteRan); bVisited = true; Child->FinishRecording();
            }, Options);
            Set.AddParallelCommandList(Child, Task);
            EXPECT_TRUE(Set.Dispatch()); EXPECT_TRUE(bVisited);
            EXPECT_GT(Parent.GetNumRecordedCommands(), Before);
            Parent.EndRenderPass(); Parent.FinishRecording();
        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, ParallelRecordingFailureDoesNotInsertSuccessfulSiblings)
    {
        using namespace RendererPrivate;
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            FRHICommandList Parent; Parent.SwitchPipeline(ERHIPipeline::Graphics); Parent.BeginRenderPass({}, "FailedGroup");
            const auto Before = Parent.GetNumRecordedCommands();
            FParallelCommandListSet Set(FSceneView{}, Parent);
            auto* First = Set.NewParallelCommandList();
            auto* Second = Set.NewParallelCommandList();
            Set.AddParallelCommandList(First, Tasks::LaunchIndependentTask("Test.DrawSuccess", [First] { First->FinishRecording(); }));
            Set.AddParallelCommandList(Second, Tasks::LaunchIndependentTask("Test.DrawFailure", [] { throw std::runtime_error("Expected recording failure"); }));
            EXPECT_FALSE(Set.Dispatch());
            EXPECT_EQ(Parent.GetNumRecordedCommands(), Before);
            Parent.EndRenderPass(); Parent.FinishRecording();
        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, CullingDrawParametersResolveDeclaredRangesAndOffsetsOnce)
    {
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate& Commands) {
            auto Ids = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("Ids", 128, 4, EBufferUsageFlags::StructuredBuffer));
            auto Arguments = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("Args", 128, 4, EBufferUsageFlags::DrawIndirect));
            FRDGBuilder Graph;
            auto P = Graph.AllocParameters<FInstanceCullingDrawParams>();
            P->InstanceIdOffsetBuffer = FRDGBufferParameter{Graph.RegisterExternalBuffer(Ids, "Ids", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead), 16, 64};
            P->DrawIndirectArgsBuffer = FRDGBufferParameter{Graph.RegisterExternalBuffer(Arguments, "Args", ERHIAccess::IndirectArgumentRead, ERHIAccess::IndirectArgumentRead), 20, 80};
            P->InstanceDataByteOffset = 4; P->IndirectArgsByteOffset = 20;
            const auto Pass = Graph.AddPass("DrawParams", ERDGPassType::Graphics, std::move(P), [&](FRHICommandListImmediate&, const FInstanceCullingDrawParams& Data, const FRDGParameterResolver& Resolver) {
                const auto Native = GetMeshDrawCommandOverrideArgs(Data, Resolver);
                EXPECT_EQ(Native.InstanceBuffer, Ids.GetReference()); EXPECT_EQ(Native.IndirectArgsBuffer, Arguments.GetReference());
                EXPECT_EQ(Native.InstanceDataByteOffset, 20u); EXPECT_EQ(Native.IndirectArgsByteOffset, 40u);
                const auto Empty = GetMeshDrawCommandOverrideArgs({}, Resolver); EXPECT_FALSE(Empty.InstanceBuffer); EXPECT_FALSE(Empty.IndirectArgsBuffer);
            });
            Graph.MarkPassRoot(Pass, "Parameter boundary test");
            EXPECT_TRUE(Graph.Execute(Executor.GetImmediateCommandList(), &Allocator));
        });
        FlushRenderingCommands();
    }

        TEST_F(FGPUSceneTests, MeshDrawStatisticsKeepCPUAndGPURequestsIndependentAndCancelFailedPasses)
    {
        using namespace RendererPrivate;
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            FRDGBuilder Graph; Graph.EnablePassCulling();
            const auto Completion = Graph.CreateValue<bool>("DrawComplete", "bool");
            std::array Expected{FRHIDrawIndexedIndirectArguments{3, 0, 0, 0, 0}};
            FMeshDrawCommandStatsManager Disabled;
            EXPECT_FALSE(Disabled.CollectStats()); EXPECT_FALSE(Disabled.CollectGPUVisibilityStats());
            EXPECT_FALSE(Disabled.CreatePassStats({}));
            FMeshDrawCommandStatsManager CPUOnly({.bDrawStats = true});
            EXPECT_TRUE(CPUOnly.CreatePassStats({})); EXPECT_FALSE(CPUOnly.CollectGPUVisibilityStats());
            EXPECT_FALSE(CPUOnly.QueueDrawRDGIndirectArgsReadback(Graph, {}, {}, 64, Expected, Completion, [](bool) { return true; }));
            auto Buffer = MakeRefCount<FSceneBuffer>(FRHIBufferCreateDesc::Create("StatsArgs", sizeof(Expected), 4, EBufferUsageFlags::SourceCopy));
            const auto Handle = Graph.RegisterExternalBuffer(Buffer, "StatsArgs", ERHIAccess::TransferRead, ERHIAccess::TransferRead);
            const auto Producer = FRDGBuilderTestAccessor::AddPass(Graph, "DrawProducer", ERDGPassType::Copy,
                [Completion](FRHICommandListImmediate&, const FRDGPassResources& Resources) { Resources.WriteValue(Completion) = false; });
            FRDGBuilderTestAccessor::UseValue(Graph, Producer, Completion, ERDGUse::Write);
            std::shared_ptr<const FGPUCullingVisibilityQuery> Query;
            {
                FMeshDrawCommandStatsManager GPUOnly({.bGPUVisibility = true});
                EXPECT_FALSE(GPUOnly.CreatePassStats({})); EXPECT_FALSE(GPUOnly.CollectStats());
                Query = GPUOnly.QueueDrawRDGIndirectArgsReadback(Graph, {}, {Handle, 0, sizeof(Expected)}, 64, Expected, Completion, [](bool Ready) { return Ready; });
                ASSERT_TRUE(Query); EXPECT_EQ(Query->GetState(), ERHIGPUReadbackState::Pending);
            }
            EXPECT_TRUE(Graph.Execute(Executor.GetImmediateCommandList(), &Allocator));
            EXPECT_EQ(Query->GetState(), ERHIGPUReadbackState::Canceled);
            EXPECT_FALSE(Query->GetCounts());
            EXPECT_EQ(Graph.GetPasses().size(), 2u);
        });
        FlushRenderingCommands();
    }

    TEST_F(FGPUSceneTests, PreparedViewRequiresOneSceneAndDirectIdDomain)
    {
        using namespace RendererPrivate;
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            FPreparedStaticMeshView Prepared; FResolvedStaticMeshView Resolved;
            Resolved.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();
            auto A = std::make_shared<FGPUSceneBufferSet>(); auto B = std::make_shared<FGPUSceneBufferSet>();
            auto Make = [] { return MakeRefCount<FSceneBuffer>(FRHIBufferCreateDesc::Create("Domain", 64, 4, EBufferUsageFlags::StructuredBuffer)); };
            A->SceneData[1] = Make(); *B = *A; B->InstanceIds = Make();
            Resolved.GPUScenePrimitives = {{A, 0, 0}, {B, 0, 0}};
            EXPECT_FALSE(SetupMeshDrawCommands(Executor.GetImmediateCommandList(), Prepared, Resolved, false));
            B->InstanceIds = A->InstanceIds; B->SceneData[1] = Make();
            EXPECT_FALSE(SetupMeshDrawCommands(Executor.GetImmediateCommandList(), Prepared, Resolved, false));
            B->SceneData = A->SceneData;
            EXPECT_TRUE(SetupMeshDrawCommands(Executor.GetImmediateCommandList(), Prepared, Resolved, false));
            EXPECT_EQ(Resolved.GPUSceneBuffers, A);
        });
        FlushRenderingCommands();
    }

	TEST_F(FGPUSceneTests, InstanceIdUploadsShareImmutableContentsAndProtectPendingReaders)
	{
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto& Commands = Executor.GetImmediateCommandList();
			auto GPU = CreateRenderResource<FGPUScene>();
			auto First = std::make_shared<FGPUSceneDynamicContext>(GPU);
			auto Shared = std::make_shared<FGPUSceneDynamicContext>(GPU);
			const std::array<uint32, 3> A{1, 2, 3}, B{4, 5, 6}, C{7, 8, 9}, D{10, 11, 12};
			const auto Original = First->UploadInstanceIds(Commands, A);
			ASSERT_TRUE(Original);
			EXPECT_EQ(Shared->UploadInstanceIds(Commands, A), Original);
			EXPECT_EQ(GPU->GetStatistics().InstanceIdBufferCreates, 1u);
			EXPECT_EQ(GPU->GetStatistics().InstanceIdUploadBytes, sizeof(A));
			auto Changed = std::make_shared<FGPUSceneDynamicContext>(GPU);
			EXPECT_NE(Changed->UploadInstanceIds(Commands, B), Original);
			Backend.bCompleteImmediately = false;
			FRDGBuilder Graph;
			const auto Handle = Graph.RegisterExternalBuffer(Original, "Ids.Reader", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto Pass = Graph.QueueBufferUpload(Handle, 0, std::as_bytes(std::span(A)));
			Graph.MarkPassRoot(Pass, "pending instance ID use");
			ASSERT_TRUE(Graph.Execute(Commands, &Allocator));
			First->RegisterGraphUse(Graph); Shared->RegisterGraphUse(Graph);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			First.reset(); Shared.reset();
			auto Pending = std::make_shared<FGPUSceneDynamicContext>(GPU);
			EXPECT_NE(Pending->UploadInstanceIds(Commands, C), Original);
			EXPECT_EQ(GPU->GetStatistics().InstanceIdBufferCreates, 3u);
			for (const auto& Signal : Backend.Signals) ASSERT_TRUE(Backend.Timeline.ObserveCompleted(Signal));
			auto Recycled = std::make_shared<FGPUSceneDynamicContext>(GPU);
			EXPECT_EQ(Recycled->UploadInstanceIds(Commands, D), Original);
			EXPECT_EQ(GPU->GetStatistics().InstanceIdBufferCreates, 3u);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Bytes = static_cast<FSceneBuffer*>(Original.GetReference())->Bytes;
			EXPECT_TRUE(std::ranges::equal(std::span(Bytes).first(sizeof(D)), std::as_bytes(std::span(D))));
			GPU->ReleaseRHI();
			auto Recovered = std::make_shared<FGPUSceneDynamicContext>(GPU);
			EXPECT_NE(Recovered->UploadInstanceIds(Commands, D), Original);
			Backend.bCompleteImmediately = true;
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		});
		FlushRenderingCommands();
	}

	TEST_F(FGPUSceneTests, DynamicInstancingMatchesEffectiveStateAndPreservesOrderedRuns)
	{
		using namespace RendererPrivate;
		class FDeclaration final : public FRHIVertexDeclaration
		{
		public:
			FVertexDeclarationElementList Elements;
			auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
		};
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto& Commands = Executor.GetImmediateCommandList();
			FMeshPassPipelineInitializer AInitializer; AInitializer.VertexDeclaration = new FDeclaration;
			auto A = std::make_shared<const FMeshPassPipeline>(AInitializer);
			auto BInitializer = A->GetInitializer(); BInitializer.VertexDeclaration = new FDeclaration;
			auto B = std::make_shared<const FMeshPassPipeline>(BInitializer);
			FMeshDrawCommand First; First.Pipeline = A.get(); First.bFinalized = true;
			First.DrawParameters = FRHIDrawIndexedArguments{3, 1, 2, -3, 17};
			First.VertexStreams.push_back({0, nullptr, 12, 32});
			auto Second = First; Second.Pipeline = B.get();
			std::get<FRHIDrawIndexedArguments>(Second.DrawParameters).FirstInstance = 23;
			EXPECT_TRUE(First.MatchesForDynamicInstancing(Second));
			EXPECT_EQ(First.GetDynamicInstancingHash(), Second.GetDynamicInstancingHash());
			auto Different = Second; Different.VertexStreams[0].Offset++;
			EXPECT_FALSE(First.MatchesForDynamicInstancing(Different));
			Different = Second; Different.DepthBias[0] = 1.f;
			EXPECT_FALSE(First.MatchesForDynamicInstancing(Different));
			Different = Second; Different.FragmentBindings.Parameters.push_back({.BindingIndex = 2, .Offset = 16});
			EXPECT_FALSE(First.MatchesForDynamicInstancing(Different));
			BInitializer = A->GetInitializer();
			BInitializer.Description.Rasterizer.FrontFace = A->GetInitializer().Description.Rasterizer.FrontFace == ERHIFrontFace::Clockwise ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
			B = std::make_shared<const FMeshPassPipeline>(BInitializer); Second.Pipeline = B.get();
			EXPECT_FALSE(First.MatchesForDynamicInstancing(Second));
			B = std::make_shared<FMeshPassPipeline>(*A); Second.Pipeline = B.get();

			FPreparedStaticMeshView Prepared;
			FResolvedStaticMeshView Resolved;
			Resolved.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();
			Resolved.GPUSceneDynamicContext = std::make_shared<FGPUSceneDynamicContext>();
			auto Binding = std::make_shared<FStaticMeshBatchBinding>(); Binding->Declaration = A->GetInitializer().VertexDeclaration;
			auto SceneData = std::make_shared<FGPUSceneBufferSet>();
			for (uint32 Index = 0; Index < 5; ++Index)
			{
				FPreparedStaticMeshPrimitive Primitive; Primitive.CollectedBinding = Binding;
				Primitive.LocalToWorld = Math::TranslationMatrix(FVector3(Index, 0.0, 0.0));
				Prepared.Primitives.push_back(Primitive);
				auto Description = std::make_shared<FMeshDrawDescription>(); Description->Geometry.InstanceCount = 1;
				FPreparedStaticMeshDraw Draw; Draw.Command = Description; Draw.PrimitiveIndex = Index; Draw.ResolvedIndex = Index;
				if (Index == 4) Prepared.Translucent.push_back(Draw); else Prepared.Opaque.push_back(Draw);
				Resolved.GPUScenePrimitives.push_back({SceneData, Index, Index});
				auto Command = Index == 1 ? Second : First;
				if (Index == 2) Command.DepthBias[0] = 1.f;
				auto& Record = Resolved.Draws.emplace_back(); Record.bReady = true;
				AddTestMeshDrawCommand(*Resolved.DrawCommandStorage, Resolved.GetMeshDrawCommandPass({EMeshPipelinePass::Forward}), Command, Index);
			}
			ASSERT_TRUE(SetupMeshDrawCommands(Commands, Prepared, Resolved, true));
			const auto Runs = BuildMeshDrawCommandRuns(Resolved.GetMeshDrawCommandPass({EMeshPipelinePass::Forward}).GetMeshDrawCommands(),
                BuildMeshDrawCommandCullingPayloads(Prepared, Resolved, {EMeshPipelinePass::Forward}));
			ASSERT_EQ(Runs.size(), 3u);
			EXPECT_EQ(Runs[0].FirstCommand, 0u);
			EXPECT_EQ(Runs[0].NumCommands, 1u);
			EXPECT_EQ(Resolved.GetMeshDrawCommandPass({EMeshPipelinePass::Forward}).GetMeshDrawCommands().size(), 4u);
			const auto* Leader = Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[0], {EMeshPipelinePass::Forward});
			const auto* Follower = Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[1], {EMeshPipelinePass::Forward});
			ASSERT_TRUE(Leader && Follower);
			EXPECT_EQ(Leader->NumInstances, 2u); EXPECT_EQ(Leader, Follower);
			EXPECT_EQ(Leader->SourceDrawIndex, 0u);
			EXPECT_EQ(Leader->StateBucketId, Follower->StateBucketId);
			EXPECT_TRUE(Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[2], {EMeshPipelinePass::Forward}));
			EXPECT_TRUE(Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[3], {EMeshPipelinePass::Forward}));
			EXPECT_TRUE(Resolved.GetVisibleMeshDrawCommand(Prepared.Translucent[0], {EMeshPipelinePass::Forward}));
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Bytes = static_cast<FSceneBuffer*>(Resolved.DynamicInstancingInstanceIds.GetReference())->Bytes;
			ASSERT_GE(Bytes.size(), 2 * sizeof(uint32));
			std::array<uint32, 2> Ids; std::memcpy(Ids.data(), Bytes.data(), sizeof(Ids));
			EXPECT_EQ(Ids, (std::array<uint32, 2>{0, 1}));
		});
		FlushRenderingCommands();
	}

    TEST_F(FGPUSceneTests, PassCompactionUsesAdmittedSourcesAndPreservesInstanceIds)
    {
        using namespace RendererPrivate;
        class FDeclaration final : public FRHIVertexDeclaration
        {
            FVertexDeclarationElementList Elements;
            auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
        };
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            auto& Commands = Executor.GetImmediateCommandList();
            FMeshPassPipeline Pipeline(FMeshPassPipelineInitializer{.VertexDeclaration = new FDeclaration});
            FMeshDrawCommand Command; Command.Pipeline = &Pipeline; Command.bFinalized = true;
            Command.DrawParameters = FRHIDrawIndexedArguments{3, 1, 0, 0, 0};
            FPreparedStaticMeshView Prepared; FResolvedStaticMeshView Resolved;
            Resolved.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();
            Resolved.GPUSceneDynamicContext = std::make_shared<FGPUSceneDynamicContext>();
            auto Binding = std::make_shared<FStaticMeshBatchBinding>(); Binding->Declaration = Pipeline.GetInitializer().VertexDeclaration;
            auto Buffers = std::make_shared<FGPUSceneBufferSet>();
            for (uint32 Index = 0; Index < 6; ++Index)
            {
                FPreparedStaticMeshPrimitive Primitive; Primitive.CollectedBinding = Binding;
                Prepared.Primitives.push_back(Primitive);
                auto Description = std::make_shared<FMeshDrawDescription>(); Description->Geometry.InstanceCount = 1;
                Prepared.Opaque.push_back({.Command = Description, .ResolvedIndex = Index, .PrimitiveIndex = Index});
                Resolved.Draws.emplace_back().bReady = Index != 1;
                Resolved.GPUScenePrimitives.push_back({Buffers, Index, Index});
                AddTestMeshDrawCommand(*Resolved.DrawCommandStorage, Resolved.GetMeshDrawCommandPass(Pipeline.GetPassKey()), Command, Index);
            }
            // Final admission removes a source without renumbering ResolvedIndex.
            Prepared.Opaque.erase(Prepared.Opaque.begin() + 1);
            ASSERT_TRUE(SetupMeshDrawCommands(Commands, Prepared, Resolved, true));
            const auto& Pass = Resolved.GetMeshDrawCommandPass(Pipeline.GetPassKey());
            ASSERT_EQ(Pass.GetMeshDrawCommands().size(), 2u);
            EXPECT_EQ(Resolved.GetMeshDrawCommandPass(Pipeline.GetPassKey()).GetVisibleForSource(0), &Pass.GetMeshDrawCommands()[0]);
            EXPECT_FALSE(Resolved.GetMeshDrawCommandPass(Pipeline.GetPassKey()).GetVisibleForSource(1));
            for (uint32 Source = 2; Source < 6; ++Source)
                EXPECT_EQ(Resolved.GetMeshDrawCommandPass(Pipeline.GetPassKey()).GetVisibleForSource(Source), &Pass.GetMeshDrawCommands()[1]);
            EXPECT_EQ(Pass.GetMeshDrawCommands()[0].SourceDrawIndex, 0u);
            const auto& Group = Pass.GetMeshDrawCommands()[1];
            EXPECT_EQ(Group.SourceDrawIndex, 2u); EXPECT_EQ(Group.NumInstances, 4u);
            EXPECT_EQ(Group.NumSourceDraws, 4u);
			FRendererResourceCoordinator Coordinator;
			FInstanceCullingManager Manager(Coordinator);
			auto Culling = Manager.CreateContext(Manager.RegisterView(FSceneView{}), Pipeline.GetPassKey());
			FInstanceCullingAdmissionDiagnostics Diagnostics;
			auto CullingCommands = FMeshCommandOneFrameArray(Pass.GetMeshDrawCommands().begin(), Pass.GetMeshDrawCommands().end());
            EXPECT_FALSE(Culling->SetupDrawCommands(CullingCommands, BuildMeshDrawCommandCullingPayloads(Prepared, Resolved, Pipeline.GetPassKey()),
				nullptr, FInstanceCullingContext::MaximumGeneratedBytes, &Diagnostics));
			EXPECT_EQ(Diagnostics.IncompatibleCandidates, 5u);
            const std::weak_ptr<const FGPUSceneBufferSet> OwnerLife = Buffers;
            Buffers.reset(); Resolved.GPUScenePrimitives.clear(); Resolved.GPUSceneBuffers.reset();
            EXPECT_FALSE(OwnerLife.expired()); // The pass owns its direct/fallback domain after preparation is released.
            const auto* Data = Pass.GetMeshDrawCommands().data();
            EXPECT_EQ(Resolved.GetMeshDrawCommandPass(Pipeline.GetPassKey()).GetMeshDrawCommands().data(), Data);
            const auto OldFrame = GRenderFrameCounterRenderThread;
            ++GRenderFrameCounterRenderThread;
            const auto Expired = Pass.Draw(Commands);
            EXPECT_FALSE(Expired.bComplete); EXPECT_EQ(Expired.NativeSubmissions, 0u);
            EXPECT_FALSE(Pass.GetVisibleForSource(0));
            GRenderFrameCounterRenderThread = OldFrame;
            Resolved.MeshDrawCommandPasses.clear();
            EXPECT_TRUE(OwnerLife.expired());
            Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
        });
        FlushRenderingCommands();
    }

	TEST_F(FGPUSceneTests, SharedCommandsRetainOnlyCanonicalPipelineAndInvalidateIndependently)
	{
		using namespace RendererPrivate;
		class FDeclaration final : public FRHIVertexDeclaration
		{
		public:
			auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
		private:
			FVertexDeclarationElementList Elements;
		};
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			FMeshPassPipelineInitializer Initializer;
			Initializer.FactoryKey = FStaticMeshBatchBinding{}.GetFactoryKey();
			Initializer.VertexDeclaration = new FDeclaration;
			auto A = std::make_shared<const FMeshPassPipeline>(Initializer);
			Initializer.VertexDeclaration = new FDeclaration;
			auto B = std::make_shared<const FMeshPassPipeline>(Initializer);
			const std::weak_ptr<const FMeshPassPipeline> LifeA = A, LifeB = B;
			auto SourceA = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
			auto SourceB = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(1), std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
			FCachedMeshDrawCommandStorage Cache;
			FMeshDrawDescription Description;
			const auto KeyA = Cache.MakeKey(&Description, FPrimitiveComponentId{1}, 1, *A);
			const auto KeyB = Cache.MakeKey(&Description, FPrimitiveComponentId{2}, 1, *B);
			EXPECT_EQ(*KeyA.Pipeline, *KeyB.Pipeline);
			EXPECT_EQ(A->GetIdentity(), A->GetIdentity());
			EXPECT_EQ(KeyA.Pipeline, A->GetIdentity());
			// Mutating construction input cannot change a published pipeline or its cached key.
			Initializer.Description.Rasterizer.LineWidth = 3.f;
			EXPECT_NE(A->GetInitializer().Description.Rasterizer.LineWidth, Initializer.Description.Rasterizer.LineWidth);
			EXPECT_EQ(*KeyA.Pipeline, *A->GetIdentity());
			EXPECT_NE(*KeyA.Pipeline, *FMeshPassPipeline(Initializer).GetIdentity());
			auto ChangedState = B->GetInitializer(); ChangedState.LayoutKey = FXxHash64{123};
			EXPECT_NE(*KeyA.Pipeline, *FMeshPassPipeline(ChangedState).GetIdentity());
			EXPECT_EQ(Cache.MakeKey(&Description, FPrimitiveComponentId{1}, 1, *B).Pipeline, KeyB.Pipeline);
			auto MakeCommand = [](const FMeshPassPipeline* Pipeline) {
				FMeshDrawCommand Command; Command.Pipeline = Pipeline;
				Command.bFinalized = true; Command.bCached = true;
				Command.DrawParameters = FRHIDrawIndexedArguments{3, 1, 0, 0, 0};
				return Command;
			};
			const auto First = Cache.Publish(KeyA, MakeCommand(A.get()), *SourceA, A, nullptr, {});
			const auto Second = Cache.Publish(KeyB, MakeCommand(B.get()), *SourceB, B, nullptr, {});
			const auto InfoA = Cache.FindCachedCommand(KeyA, *SourceA).Info, InfoB = Cache.FindCachedCommand(KeyB, *SourceB).Info;
			ASSERT_EQ(First, Second); ASSERT_NE(InfoA, InfoB);
			EXPECT_EQ(InfoA->StateBucketId, InfoB->StateBucketId);
			FParallelMeshDrawCommandPass Frame({EMeshPipelinePass::Forward});
			Frame.AddCachedMeshDrawCommand(First, InfoA, std::make_shared<FMeshPassBindings>(), {});
			Frame.AddCachedMeshDrawCommand(Second, InfoB, std::make_shared<FMeshPassBindings>(), {});
			auto InstanceA = Frame.GetMeshDrawCommands()[0], InstanceB = Frame.GetMeshDrawCommands()[1];
			EXPECT_TRUE(InstanceA.MatchesForDynamicInstancing(InstanceB));
			InstanceB.DepthBias[0] = 1.f;
			EXPECT_FALSE(InstanceA.MatchesForDynamicInstancing(InstanceB));
			InstanceB.DepthBias[0] = 0.f;
			auto DifferentPass = std::make_shared<FMeshPassBindings>(*InstanceB.Pass);
			DifferentPass->Vertex.Shader = reinterpret_cast<FRHIShader*>(uintptr_t(1));
			InstanceB.Pass = DifferentPass.get();
			EXPECT_FALSE(InstanceA.MatchesForDynamicInstancing(InstanceB));
			InstanceB.Pass = Frame.GetMeshDrawCommands()[1].Pass;
			{
				FCachedMeshDrawCommandStorage OtherScene;
				auto OtherSource = std::make_shared<FPrimitiveSceneInfo>(FPrimitiveComponentId(3),
					std::make_shared<FStaticMeshSceneProxy>(nullptr, std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
				auto DifferentGeometry = MakeCommand(A.get());
				std::get<FRHIDrawIndexedArguments>(DifferentGeometry.DrawParameters).IndexCount = 6;
				const auto OtherCommand = OtherScene.Publish(KeyA, std::move(DifferentGeometry), *OtherSource, A, nullptr, {});
				const auto OtherInfo = OtherScene.FindCachedCommand(KeyA, *OtherSource).Info;
				EXPECT_EQ(OtherInfo->StateBucketId, InfoA->StateBucketId);
				Frame.AddCachedMeshDrawCommand(OtherCommand, OtherInfo, std::make_shared<FMeshPassBindings>(), {});
				auto OtherVisible = Frame.GetMeshDrawCommands().back();
				EXPECT_FALSE(InstanceA.MatchesForDynamicInstancing(OtherVisible));
			}
			A.reset(); B.reset();
			SourceA->RemoveCachedMeshDrawCommands();
			EXPECT_FALSE(InstanceA.MatchesForDynamicInstancing(InstanceB));
			EXPECT_EQ(InfoA->CommandIndex, INDEX_NONE); EXPECT_NE(InfoB->CommandIndex, INDEX_NONE);
			EXPECT_FALSE(LifeA.expired()); EXPECT_TRUE(LifeB.expired());
			EXPECT_EQ(Cache.FindCachedCommand(KeyB, *SourceB).Command, Second);
			EXPECT_EQ(Second->Pipeline, LifeA.lock().get());
			// A binding replacement must retire B's old visible handle without
			// reviving A, even when the scene cache reuses an existing slot.
			auto ReplacementPipeline = std::make_shared<FMeshPassPipeline>(*LifeA.lock());
			auto Replacement = MakeCommand(ReplacementPipeline.get()); Replacement.DepthBias[0] = 1.f;
			auto ReplacementKey = KeyB; ++ReplacementKey.Bindings[1];
			const auto Rebuilt = Cache.Publish(ReplacementKey, Replacement, *SourceB, ReplacementPipeline, nullptr, {});
			EXPECT_NE(Rebuilt, Second); EXPECT_EQ(Cache.Num(), 1u);
			EXPECT_EQ(InfoA->CommandIndex, INDEX_NONE); EXPECT_EQ(InfoB->CommandIndex, INDEX_NONE);
			EXPECT_FALSE(Cache.FindCachedCommand(KeyB, *SourceB).Command); EXPECT_EQ(Cache.FindCachedCommand(ReplacementKey, *SourceB).Command, Rebuilt);
			EXPECT_TRUE(LifeA.expired()); EXPECT_TRUE(LifeB.expired());
			EXPECT_FALSE(InstanceA.MatchesForDynamicInstancing(InstanceB));
			SourceB->RemoveCachedMeshDrawCommands();
			EXPECT_EQ(Cache.Num(), 0u); EXPECT_FALSE(Cache.FindCachedCommand(ReplacementKey, *SourceB).Command);
		});
		FlushRenderingCommands();
	}

	TEST_F(FGPUSceneTests, InstanceCullingViewsOwnIndependentGraphsAndSurviveCancellationAndAllocationFailure)
    {
        using namespace RendererPrivate;
        auto Source = std::make_shared<FInstanceSceneDataBuffers>();
        for (uint32 Index = 0; Index < 64; ++Index)
            Source->Instances.push_back({{Index + 1}, FMatrix(1.0), FBox(FVector3(-1.0), FVector3(1.0)), {}});
        Add(1, Source); Run();
            class FDeclaration final : public FRHIVertexDeclaration {
                FVertexDeclarationElementList Elements;
                auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
            };
        EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
            FPreparedStaticMeshView Prepared;
            FResolvedStaticMeshView Resolved;
            Resolved.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();
            auto Persistent = std::make_shared<FGPUSceneBufferSet>();
            Persistent->SceneData = (*Scene)->GPUScene->GetResourceBuffers();
            auto Dynamic = Persistent;
            Dynamic->SceneData[1] = MakeRefCount<FSceneBuffer>(FRHIBufferCreateDesc::Create("ScopedCullingInstances",
                128 * sizeof(FInstanceSceneShaderData), 16, EBufferUsageFlags::StructuredBuffer));
            auto Binding = std::make_shared<FStaticMeshBatchBinding>();
            auto Pipeline = std::make_shared<const FMeshPassPipeline>(FMeshPassPipelineInitializer{
                .VertexDeclaration = new FDeclaration, .Pass = EMeshPipelinePass::GBuffer});
            auto IndexBuffer = MakeRefCount<FSceneBuffer>(FRHIBufferCreateDesc::Create("CullingIndices", 12, 4, EBufferUsageFlags::IndexBuffer));
            const auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Fragment, FXxHash128{}));
            auto A = std::make_shared<FMeshDrawDescription>(); A->Geometry = {.bIndexed = true, .ElementCount = 3};
            auto B = std::make_shared<FMeshDrawDescription>(*A);
            for (uint32 Index = 0; Index < 128; ++Index)
            {
                FPreparedStaticMeshPrimitive Primitive; Primitive.CollectedBinding = Binding;
                Primitive.WorldBounds = FBox(FVector3(-1.0), FVector3(1.0));
                Primitive.PrimitiveIdMode = Index < 64 ? EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo : EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData;
                Prepared.Primitives.push_back(Primitive);
                Prepared.Opaque.push_back({.Command = Index < 64 ? A : B, .ResolvedIndex = Index, .PrimitiveIndex = Index});
                Resolved.GPUScenePrimitives.push_back({Persistent, Index, Index});
                FMeshDrawCommand Command; Command.Pipeline = Pipeline.get(); Command.bFinalized = true;
                Command.IndexBuffer = IndexBuffer; Command.DrawParameters = FRHIDrawIndexedArguments{3, 1, 0, 0, 0};
                Command.DepthBias[0] = Index < 64 ? 0.f : 1.f;
                Command.FragmentBindings.Shader = Shader;
                auto& Record = Resolved.Draws.emplace_back(); Record.bReady = true;
                AddTestMeshDrawCommand(*Resolved.DrawCommandStorage, Resolved.GetMeshDrawCommandPass({EMeshPipelinePass::GBuffer}), Command, Index);
            }
            ASSERT_TRUE(SetupMeshDrawCommands(Executor.GetImmediateCommandList(), Prepared, Resolved, false));
            FInstanceCullingManager Manager(Coordinator);
            FSceneView View;
            const auto FirstView = Manager.RegisterView(View);
            View.ViewLocation = FVector3(1099511627776.0, 0.0, 0.0);
            const auto SecondView = Manager.RegisterView(View);
            auto MakeContext = [&](uint32 Id) {
                auto Context = Manager.CreateContext(Id, {EMeshPipelinePass::GBuffer});
                auto Visible = Resolved.GetMeshDrawCommandPass({EMeshPipelinePass::GBuffer}).GetMeshDrawCommands();
                FMeshCommandOneFrameArray Commands(Visible.begin(), Visible.end());
                auto Sources = BuildMeshDrawCommandCullingPayloads(Prepared, Resolved, {EMeshPipelinePass::GBuffer});
                auto TemporaryOwner = std::make_shared<FGPUSceneBufferSet>(*Sources.front().SceneData.Buffers);
                const std::weak_ptr<const FGPUSceneBufferSet> PreparationLife = TemporaryOwner;
                for (auto& Source : Sources) Source.SceneData.Buffers = TemporaryOwner;
                EXPECT_TRUE(Context->SetupDrawCommands(Commands, Sources));
                Sources.clear(); TemporaryOwner.reset();
                EXPECT_TRUE(PreparationLife.expired()); // Only submission leases survive Setup, not preparation records.
                EXPECT_EQ(Context->GetCandidates().size(), 128u);
                EXPECT_EQ(Context->GetCandidates()[0].InstanceId, 0u);
                EXPECT_EQ(Context->GetCandidates()[64].InstanceId, 64u);
                return Context;
            };
            auto First = MakeContext(FirstView), Second = MakeContext(SecondView);
            FInstanceCullingTelemetry FirstTelemetry, SecondTelemetry;
            // Abandon a built graph. Neither view publishes an executable indirect list.
            {
                FRDGBuilder Abandoned;
                First->BuildRenderingCommands(Abandoned, FirstTelemetry, true, false, EGPUCullingFailure::CommandAdmission);
            }
            EXPECT_FALSE(First->IsCullingDispatched()); EXPECT_FALSE(Second->IsCullingDispatched());
            EXPECT_EQ(FirstTelemetry.GPUCullingBufferBytes, 128u * 12u + 2u * 20u);
            EXPECT_GT(FirstTelemetry.GPUCullingSceneCapacityBytes, 0u);
            EXPECT_EQ(FirstTelemetry.GPUCullingStandaloneSceneCapacityBytes, 0u);
            RHI.EnableIndependentComputeForCompilation();
            {
                FRDGBuilder Rejected; Rejected.SetAsyncComputeEnabled(true);
                const auto Params = Second->BuildRenderingCommands(Rejected, SecondTelemetry, true, false, EGPUCullingFailure::CommandAdmission);
                auto Reads = Rejected.AllocParameters<FInstanceCullingDrawParams>();
                Reads->InstanceIdOffsetBuffer = Params.InstanceIdOffsetBuffer;
                Reads->DrawIndirectArgsBuffer = Params.DrawIndirectArgsBuffer;
                Rejected.AddPass("Culling.Consumer", ERDGPassType::Graphics, std::move(Reads),
                    [](FRHICommandListImmediate&, const FInstanceCullingDrawParams&, const FRDGParameterResolver&) {});
                FRejectGPUSceneAllocator Reject;
                EXPECT_FALSE(Rejected.Execute(Executor.GetImmediateCommandList(), &Reject));
                EXPECT_TRUE(Rejected.HasCompiledPlan());
                EXPECT_FALSE(Rejected.GetGPUCompletion()); EXPECT_FALSE(Second->IsCullingDispatched());
                EXPECT_TRUE(std::ranges::any_of(Rejected.GetExecutionPlan().Batches, [](const auto& Batch) { return Batch.Queue == ERDGQueueAssignment::AsyncCompute; }));
                const auto Capture = Rejected.Capture();
                EXPECT_EQ(std::ranges::count_if(Capture.Uses, [](const auto& Use) {
                    return Use.ShaderBindingName == "InstanceSceneData" && Use.Access == ERHIAccess::ComputeShaderRead;
                }), 1);
                const auto Passes = Rejected.GetPasses();
                const auto Consumer = std::ranges::find(Passes, "Culling.Consumer", &FRDGCompiledPass::Name);
                ASSERT_NE(Consumer, Passes.end());
                const auto ConsumerIndex = static_cast<uint32>(Consumer - Passes.begin());
                EXPECT_TRUE(std::ranges::any_of(Rejected.GetDependencies(), [&](const auto& Dependency) { return Dependency.AfterPass == ConsumerIndex; }));
                EXPECT_FALSE(Rejected.GetExecutionPlan().Dependencies.empty());
            }
            {
                auto CompileRejected = MakeContext(FirstView);
                FInstanceCullingTelemetry Telemetry;
                FRDGBuilder Graph;
                CompileRejected->BuildRenderingCommands(Graph, Telemetry, false, false, EGPUCullingFailure::CommandAdmission);
                FRDGBudget Budget; Budget.MaxPasses = 0; Graph.SetBudget(Budget);
                EXPECT_FALSE(Graph.Execute(Executor.GetImmediateCommandList(), &Allocator));
                EXPECT_FALSE(Graph.GetGPUCompletion()); EXPECT_FALSE(CompileRejected->IsCullingDispatched());
            }
            auto Retry = MakeContext(FirstView);
            FInstanceCullingTelemetry RetryTelemetry;
            {
                FRDGBuilder Graph;
                Retry->BuildRenderingCommands(Graph, RetryTelemetry, false, false, EGPUCullingFailure::CommandAdmission);
                const auto Result = Graph.Execute(Executor.GetImmediateCommandList(), &Allocator);
                EXPECT_TRUE(Result) << (Result ? "" : ToString(Result.error()));
                Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
                EXPECT_TRUE(Graph.GetGPUCompletion());
                EXPECT_FALSE(Retry->IsCullingDispatched());
                EXPECT_EQ(RetryTelemetry.GPUCullingDispatchFailureViews, 1u);
                EXPECT_EQ(Retry->GetArgumentIndex(0), FInstanceCullingContext::InvalidArgument);
            }
        });
        FlushRenderingCommands();
        EXPECT_EQ(Run().UploadBytes, 0u);
    }

	TEST_F(FGPUSceneTests, ReorderIsFreeAndSingleInstancePayloadIsOnePatch)
	{
		auto* Proxy = Add(1, Sources({10, 20, 30})); Run();
		const auto Offset = Info().InstanceSceneDataOffset;
		Proxy->Source = Sources({30, 10, 20});
		Dirty(EGPUSceneDirtyState::InstanceData);
		EXPECT_EQ(Run().UploadBytes, 0u);
		auto Changed = Sources({30, 10, 20});
		Changed->Instances[2].CustomData.Values[11] = 42.f;
		Proxy->Source = Changed;
		Dirty(EGPUSceneDirtyState::InstanceData);
		const auto Result = Run();
		EXPECT_EQ(Result.UploadBytes, sizeof(FPrimitiveCustomData));
		EXPECT_EQ(Result.UploadRanges, 1u);
		EXPECT_EQ(Info().InstanceSceneDataOffset, Offset);
		EXPECT_EQ((*Scene)->GPUScene->GetInstancePayloadData()[Offset + 1].Values[11], 42.f);
		EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, StaticProxySnapshotHooksPreserveEmptyAndClearedSemantics)
	{
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->LocalBounds = FBox(FVector3(-1.0), FVector3(1.0));
		FSceneInterfaceTestAccess::AddPrimitiveProxy(**Scene, FPrimitiveComponentId{1},
			std::make_unique<FStaticMeshSceneProxy>(RenderData.get(), std::vector<FMaterialRenderProxyRef>{}), FMatrix(1.0));
		Run(); EXPECT_EQ(Info().NumInstanceSceneDataEntries, 1u);
		auto Instances = std::make_shared<std::vector<FStaticMeshRenderInstance>>();
		Instances->push_back({20, Math::TranslationMatrix(FVector3(2.0, 0.0, 0.0)), {}});
		Instances->push_back({10, FMatrix(1.0), {}});
		(*Scene)->UpdateStaticMeshRenderInstances(FPrimitiveComponentId{1}, Instances);
		Run(); EXPECT_EQ(Info().NumInstanceSceneDataEntries, 2u);
		(*Scene)->UpdateStaticMeshRenderInstances(FPrimitiveComponentId{1}, std::make_shared<const std::vector<FStaticMeshRenderInstance>>());
		Run(); EXPECT_EQ(Info().NumInstanceSceneDataEntries, 0u);
		(*Scene)->UpdateStaticMeshRenderInstances(FPrimitiveComponentId{1}, {});
		Run(); EXPECT_EQ(Info().NumInstanceSceneDataEntries, 1u);
		FSceneInterfaceTestAccess::RemovePrimitiveProxy(**Scene, FPrimitiveComponentId{1});
	}

	TEST_F(FGPUSceneTests, SceneMutationHooksPatchTransformAndPrimitiveData)
	{
		Add(1); Run();
		(*Scene)->UpdatePrimitiveTransform(FPrimitiveComponentId{1}, Math::TranslationMatrix(FVector3(12.0, 0.0, 0.0)));
		EXPECT_EQ(Run().UploadBytes, sizeof(FPrimitiveSceneShaderData) + sizeof(FInstanceSceneShaderData));
		FPrimitiveCustomData Custom; Custom.Values[3] = 9.f;
		(*Scene)->UpdatePrimitiveCustomData(FPrimitiveComponentId{1}, Custom);
		EXPECT_EQ(Run().UploadBytes, sizeof(FPrimitiveSceneShaderData));
		EXPECT_EQ((*Scene)->GPUScene->GetPrimitiveSceneData()[Info().GetPersistentIndex().Index].Data[17].w, 9.f);
		EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, FailedAndAbandonedGraphsRetryWithoutAcknowledgingDirtyData)
	{
		Add(1);
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
		{
			FRDGBuilder Graph;
			auto Pending = (*Scene)->GPUScene->Update(Graph);
			EXPECT_FALSE(Graph.GetGPUCompletion());
			EXPECT_GT(Pending.UploadBytes, 0u);
		}
		{
			FRDGBuilder Graph;
			auto Pending = (*Scene)->GPUScene->Update(Graph);
			FRDGBudget Budget; Budget.MaxPasses = 0; Graph.SetBudget(Budget);
			EXPECT_FALSE(Graph.Execute(Executor.GetImmediateCommandList(), &Allocator));
			EXPECT_FALSE(Graph.GetGPUCompletion());
			EXPECT_FALSE((*Scene)->GPUScene->CompleteUpdate(Graph, Pending));
		}
		});
		FlushRenderingCommands();
		EXPECT_GT(Run().UploadBytes, 0u);
		EXPECT_EQ(Run().UploadBytes, 0u);
		(*Scene)->UpdatePrimitiveCustomData(FPrimitiveComponentId{1}, [] { FPrimitiveCustomData Data; Data.Values[0] = 99.f; return Data; }());
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
		{
			FRDGBuilder Graph;
			auto Pending = (*Scene)->GPUScene->Update(Graph);
			FRDGBudget Budget; Budget.MaxPasses = 0; Graph.SetBudget(Budget);
			EXPECT_FALSE(Graph.Execute(Executor.GetImmediateCommandList(), &Allocator));
			EXPECT_FALSE((*Scene)->GPUScene->CompleteUpdate(Graph, Pending));
		}
		});
		FlushRenderingCommands();
		EXPECT_EQ(Run().UploadBytes, sizeof(FPrimitiveSceneShaderData));
	}

	TEST_F(FGPUSceneTests, GrowthAndRecoveryPreserveMirrorsAndRestoreAllStorage)
	{
		auto* Proxy = Add(1, Sources({10})); Run();
		const auto Before = (*Scene)->GPUScene->GetInstanceSceneData()[0];
		Add(2); EXPECT_GT(Run().BufferRebuilds, 0u);
		EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(&Before, 1)),
			std::as_bytes((*Scene)->GPUScene->GetInstanceSceneData().first(1))));
		Proxy->Source = Sources({10, 20, 30}); Dirty(EGPUSceneDirtyState::InstanceData);
		EXPECT_GT(Run().BufferRebuilds, 0u);
		EXPECT_EQ(Info().NumInstanceSceneDataEntries, 3u);
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { (*Scene)->GPUScene->ReleaseRHI(); });
		FlushRenderingCommands();
		EXPECT_EQ(Run().BufferRebuilds, 3u);
		EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, AllocationRejectionRebuildsNativeStorageBeforeRetry)
	{
		Add(1); Run();
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			FRejectGPUSceneAllocator Reject;
			FRDGBuilder Graph;
			auto Pending = (*Scene)->GPUScene->Update(Graph);
			const auto Buffer = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(16, 4, EBufferUsageFlags::DestinationCopy)}, "RejectedBuffer");
			const auto Upload = Graph.QueueBufferUploadOwned(Buffer, 0, FByteBuffer(16));
			Graph.MarkPassRoot(Upload);
			EXPECT_FALSE(Graph.Execute(Executor.GetImmediateCommandList(), &Reject));
			EXPECT_FALSE((*Scene)->GPUScene->CompleteUpdate(Graph, Pending));
		});
		FlushRenderingCommands();
		EXPECT_EQ(Run().BufferRebuilds, 3u);
		EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, SceneMembershipReferenceRejectsRemovalAndSlotReuse)
	{
		Add(1); Run();
		FPrimitiveSceneInfoRef Borrow;
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { Borrow = Info().GetSceneInfoRef(); EXPECT_EQ(Borrow.Resolve(), &Info()); });
		FlushRenderingCommands();
		FSceneInterfaceTestAccess::RemovePrimitiveProxy(**Scene, FPrimitiveComponentId{1}); FlushRenderingCommands();
		Add(2); Run();
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { EXPECT_TRUE(Borrow.IsSceneBound()); EXPECT_FALSE(Borrow.Resolve()); });
		FlushRenderingCommands();
		Scene.reset(); FlushRenderingCommands();
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { EXPECT_FALSE(Borrow.Resolve()); });
		FlushRenderingCommands();
	}

	TEST_F(FGPUSceneTests, DynamicRangesShareStorageWaitForDrawingAndSurviveResourceRecovery)
	{
		Add(1); Run();
		FGPUSceneDynamicAllocation Reused;
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto& GPU = *(*Scene)->GPUScene;
			const auto Range = GPU.AllocateDynamicPrimitiveRange(2);
            auto OtherScene = CreateRenderResource<FGPUScene>();
            const auto OtherRange = OtherScene->AllocateDynamicPrimitiveRange(1);
            EXPECT_FALSE(OtherScene->WriteDynamicPrimitive(Range, 0, FMatrix(1.0), FBox(FVector3(-1.0), FVector3(1.0)), {}, {}));
            OtherScene->ReleaseDynamicPrimitiveRange(OtherRange);
			EXPECT_NE(Range.PrimitiveOffset, Info().GetPersistentIndex().Index);
			EXPECT_NE(Range.InstanceOffset, Info().InstanceSceneDataOffset);
			EXPECT_TRUE(GPU.WriteDynamicPrimitive(Range, 0, Math::TranslationMatrix(FVector3(13.0, 0.0, 0.0)), FBox(FVector3(-1.0), FVector3(1.0)), {}, {}));
			FRDGBuilder Upload; auto Transaction = GPU.Update(Upload);
			EXPECT_TRUE(Upload.Execute(Executor.GetImmediateCommandList(), &Allocator)); EXPECT_TRUE(GPU.CompleteUpdate(Upload, Transaction));
			const auto& Packed = GPU.GetInstanceSceneData()[Range.InstanceOffset];
			EXPECT_EQ(std::bit_cast<uint32>(Packed.Data[FInstanceSceneShaderData::HeaderOffset].x), Range.PrimitiveOffset);
			EXPECT_FLOAT_EQ(Packed.Data[12].x, 13.f);
			Backend.bCompleteImmediately = false;
			FRDGBuilder Drawing;
			const auto Buffer = GPU.GetResourceBuffers()[1];
			const auto Handle = Drawing.RegisterExternalBuffer(Buffer, "DynamicSharedRead", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			auto Reads = Drawing.AllocParameters<FInstanceCullingDrawParams>();
			Reads->InstanceIdsBuffer = FRDGBufferParameter{Handle, 0, Buffer->GetSize()};
			Drawing.AddPass("DrawingUsesDynamicRange", ERDGPassType::Graphics, std::move(Reads),
				[](FRHICommandListImmediate&, const FInstanceCullingDrawParams&, const FRDGParameterResolver&) {});
			EXPECT_TRUE(Drawing.Execute(Executor.GetImmediateCommandList(), &Allocator));
			Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			GPU.RegisterGraphUse(Drawing); ASSERT_TRUE(Drawing.GetGPUCompletion()); EXPECT_FALSE(Drawing.GetGPUCompletion().IsComplete());
			GPU.ReleaseDynamicPrimitiveRange(Range);
			const auto InFlight = GPU.AllocateDynamicPrimitiveRange(2);
			EXPECT_NE(InFlight.PrimitiveOffset, Range.PrimitiveOffset); EXPECT_NE(InFlight.InstanceOffset, Range.InstanceOffset);
			for (const auto& Signal : Backend.Signals) if (!Signal.IsComplete()) EXPECT_TRUE(Backend.Timeline.ObserveCompleted(Signal));
			Reused = GPU.AllocateDynamicPrimitiveRange(2);
			EXPECT_EQ(Reused.PrimitiveOffset, Range.PrimitiveOffset); EXPECT_EQ(Reused.InstanceOffset, Range.InstanceOffset);
			EXPECT_TRUE(GPU.WriteDynamicPrimitive(Reused, 0, Math::TranslationMatrix(FVector3(47.0, 0.0, 0.0)), FBox(FVector3(-1.0), FVector3(1.0)), {}, {}));
			GPU.ReleaseDynamicPrimitiveRange(InFlight); GPU.ReleaseRHI(); Backend.bCompleteImmediately = true;
		});
		FlushRenderingCommands(); EXPECT_EQ(Run().BufferRebuilds, 3u);
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			EXPECT_FLOAT_EQ((*Scene)->GPUScene->GetInstanceSceneData()[Reused.InstanceOffset].Data[12].x, 47.f);
			(*Scene)->GPUScene->ReleaseDynamicPrimitiveRange(Reused);
		});
		FlushRenderingCommands(); EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, DynamicReleaseDuringAbandonedUpdateDefersReuseWithoutClearingUploads)
	{
		Add(1); Run();
		FGPUSceneDynamicAllocation Reused;
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			auto& GPU = *(*Scene)->GPUScene;
			const auto Range = GPU.AllocateDynamicPrimitiveRange(1);
			EXPECT_TRUE(GPU.WriteDynamicPrimitive(Range, 0, FMatrix(1.0), FBox(FVector3(-1.0), FVector3(1.0)), {}, {}));
			{
				FRDGBuilder Abandoned; const auto Pending = GPU.Update(Abandoned);
				GPU.ReleaseDynamicPrimitiveRange(Range);
				EXPECT_FALSE(Abandoned.GetGPUCompletion());
			}
			Reused = GPU.AllocateDynamicPrimitiveRange(1);
			EXPECT_EQ(Reused.PrimitiveOffset, Range.PrimitiveOffset); EXPECT_EQ(Reused.InstanceOffset, Range.InstanceOffset);
			EXPECT_TRUE(GPU.WriteDynamicPrimitive(Reused, 0, Math::TranslationMatrix(FVector3(19.0, 0.0, 0.0)), FBox(FVector3(-1.0), FVector3(1.0)), {}, {}));
		});
		FlushRenderingCommands(); EXPECT_GT(Run().UploadBytes, 0u);
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { (*Scene)->GPUScene->ReleaseDynamicPrimitiveRange(Reused); });
		FlushRenderingCommands(); EXPECT_EQ(Run().UploadBytes, 0u);
	}

	TEST_F(FGPUSceneTests, DynamicContextKeepsSceneStorageAliveAfterSceneTeardownAndReleasesFromGameThread)
	{
		Add(1); Run();
		std::vector<FGPUScenePrimitiveData> Data;
		std::weak_ptr<FGPUScene> Lifetime;
		EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) {
			FPreparedStaticMeshView Prepared;
			FPreparedStaticMeshPrimitive Primitive; Primitive.LocalToWorld = FMatrix(1.0); Primitive.WorldBounds = FBox(FVector3(-1.0), FVector3(1.0));
			Primitive.PrimitiveIdMode = EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData; Prepared.Primitives.push_back(Primitive);
			auto Description = std::make_shared<FMeshDrawDescription>(); Description->PipelineKey.FactoryKey = FStaticMeshBatchBinding{}.GetFactoryKey();
			Prepared.Opaque.push_back({.Command = Description});
			Lifetime = (*Scene)->GPUScene;
			Data = PrepareStaticGPUScene(Prepared, std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene));
			ASSERT_EQ(Data.size(), 1u); ASSERT_TRUE(Data[0].Buffers); EXPECT_FALSE(Data[0].Buffers->bStandaloneScene);
			EXPECT_EQ(Data[0].Buffers->SceneData, (*Scene)->GPUScene->GetResourceBuffers());
			EXPECT_NE(Data[0].InstanceId, Info().InstanceSceneDataOffset);
		});
		FlushRenderingCommands(); Scene.reset(); FlushRenderingCommands();
		EXPECT_FALSE(Lifetime.expired());
		Data.clear(); FlushRenderingCommands(); FlushRenderingCommands(); EXPECT_TRUE(Lifetime.expired());
	}

	TEST_F(FGPUSceneTests, ReuseWaitsForGPUCompletionAndRejectsStaleGenerations)
	{
		Backend.bCompleteImmediately = false;
		Add(1); Run();
		const auto OldHandle = Info().GPUSceneHandle;
		const auto OldOffset = Info().InstanceSceneDataOffset;
		FSceneInterfaceTestAccess::RemovePrimitiveProxy(**Scene, FPrimitiveComponentId{1});
		Add(2); Run();
		EXPECT_NE(Info().GetPersistentIndex(), OldHandle.PersistentPrimitiveIndex);
		EXPECT_NE(Info().InstanceSceneDataOffset, OldOffset);
		EXPECT_FALSE(Request(OldHandle, EGPUSceneDirtyState::Transform));
		for (const auto& Signal : Backend.Signals) EXPECT_TRUE(Backend.Timeline.ObserveCompleted(Signal));
		Add(3); Run();
		EXPECT_EQ(Info(1).GetPersistentIndex(), OldHandle.PersistentPrimitiveIndex);
		EXPECT_EQ(Info(1).InstanceSceneDataOffset, OldOffset);
		EXPECT_NE(Info(1).GPUSceneHandle.Generation, OldHandle.Generation);
		EXPECT_FALSE(Request(OldHandle, EGPUSceneDirtyState::All));
		for (const auto& Signal : Backend.Signals) if (!Signal.IsComplete()) EXPECT_TRUE(Backend.Timeline.ObserveCompleted(Signal));
	}
}
}
