#include "GPUScene.h"
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
		auto Info(uint32 Index = 0) -> FPrimitiveSceneInfo& { return *(*Scene)->GetPrimitiveSceneInfos()[Index]; }
		auto Dirty(EGPUSceneDirtyState Value) -> void
		{
			EnqueueRenderCommand<FTestCommand>([&, Value](FRHICommandListImmediate&) { Info().RequestGPUSceneUpdate(Value); });
			FlushRenderingCommands();
		}
		auto Request(FGPUScenePrimitiveHandle Handle, EGPUSceneDirtyState Value) -> bool
		{
			bool Accepted = false;
			EnqueueRenderCommand<FTestCommand>([&](FRHICommandListImmediate&) { Accepted = (*Scene)->GPUScene->RequestUpdate(Handle, Value); });
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
			auto Data = std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene)->Prepare(Executor.GetImmediateCommandList(), Allocator, Prepared);
			ASSERT_EQ(Data.size(), 1u); ASSERT_TRUE(Data[0].Buffers);
			EXPECT_EQ(Data[0].Buffers->SceneData, (*Scene)->GPUScene->GetResourceBuffers());
			EXPECT_EQ(Data[0].InstanceId, Info().InstanceSceneDataOffset);
			Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Ids = static_cast<FSceneBuffer*>(Data[0].Buffers->InstanceIds.GetReference())->Bytes;
			ASSERT_EQ(Ids.size(), 3 * sizeof(uint32));
			for (uint32 Index = 0; Index < 3; ++Index)
			{
				uint32 Value; std::memcpy(&Value, Ids.data() + Index * sizeof(uint32), sizeof(uint32));
				EXPECT_EQ(Value, Data[0].InstanceId);
			}
			Prepared.Primitives[0].PersistentPrimitiveIndex = {};
			EXPECT_TRUE(std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene)->Prepare(Executor.GetImmediateCommandList(), Allocator, Prepared).empty());
			Prepared.Primitives[0].PrimitiveIdMode = EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData;
			Prepared.Primitives[0].LocalToWorld = Math::TranslationMatrix(FVector3(13.0, 0.0, 0.0));
			auto Scoped = std::make_shared<FGPUSceneDynamicContext>()->Prepare(Executor.GetImmediateCommandList(), Allocator, Prepared);
			ASSERT_EQ(Scoped.size(), 1u); ASSERT_TRUE(Scoped[0].Buffers);
			EXPECT_NE(Scoped[0].Buffers->SceneData[0], Data[0].Buffers->SceneData[0]);
			Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Packed = static_cast<FSceneBuffer*>(Scoped[0].Buffers->SceneData[1].GetReference())->Bytes;
			FInstanceSceneShaderData Instance; std::memcpy(&Instance, Packed.data(), sizeof(Instance));
			EXPECT_FLOAT_EQ(Instance.Data[12].x, 13.f);
			Description->PipelineKey.FactoryKey = {};
			auto Unsupported = std::make_shared<FGPUSceneDynamicContext>()->Prepare(Executor.GetImmediateCommandList(), Allocator, Prepared);
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
			FPreparedStaticMeshView Prepared;
			FPreparedStaticMeshPrimitive Primitive; Primitive.LocalToWorld = FMatrix(1.0);
			Primitive.WorldBounds = FBox(FVector3(-1.0), FVector3(1.0));
			Primitive.PrimitiveIdMode = EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData;
			Primitive.DynamicPrimitiveData = std::make_shared<FGPUSceneDynamicPrimitiveData>(FGPUSceneDynamicPrimitiveData{
				Primitive.LocalToWorld, Primitive.WorldBounds, Primitive.CustomPrimitiveData, Primitive.CustomInstanceData});
			Prepared.Primitives.push_back(Primitive); Prepared.Primitives.push_back(Primitive);
			auto Description = std::make_shared<FMeshDrawDescription>();
			Description->PipelineKey.FactoryKey = FStaticMeshBatchBinding{}.GetFactoryKey(); Description->Geometry.InstanceCount = 1;
			for (uint32 Index = 0; Index < 2; ++Index) { FPreparedStaticMeshDraw Draw; Draw.Command = Description; Draw.PrimitiveIndex = Index; Prepared.Opaque.push_back(Draw); }
			const auto First = Context->CreatePrimitiveCollector()->Commit(Prepared);
			ASSERT_EQ(First.size(), 2u); EXPECT_FALSE(First[0].Buffers->SceneData[1]); EXPECT_EQ(First[0].InstanceId, First[1].InstanceId);
			const auto Count = GPU->GetInstanceSceneData().size(); const auto Bytes = GPU->GetStatistics().UploadBytes;
			const auto Again = Context->CreatePrimitiveCollector()->Commit(Prepared);
			ASSERT_EQ(Again.size(), 2u); EXPECT_EQ(Again[0].InstanceId, First[0].InstanceId);
			EXPECT_EQ(Again[0].Buffers->PrimitiveCollector->GetDynamicContext(), First[0].Buffers->PrimitiveCollector->GetDynamicContext());
			EXPECT_EQ(GPU->GetInstanceSceneData().size(), Count); EXPECT_EQ(GPU->GetStatistics().UploadBytes, Bytes);
			const auto Old = GPU->GetInstanceSceneData()[First[0].InstanceId];
			Prepared.Primitives[0].CustomInstanceData.Values[0] = 7.f;
			auto Revision = std::make_shared<FGPUSceneDynamicPrimitiveData>(*Primitive.DynamicPrimitiveData);
			Revision->CustomInstanceData.Values[0] = 7.f; Prepared.Primitives[0].DynamicPrimitiveData = Revision;
			const auto Changed = Context->CreatePrimitiveCollector()->Commit(Prepared);
			ASSERT_EQ(Changed.size(), 2u); EXPECT_NE(Changed[0].InstanceId, First[0].InstanceId);
			EXPECT_EQ(Changed[1].InstanceId, First[1].InstanceId);
			EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(&Old, 1)), std::as_bytes(GPU->GetInstanceSceneData().subspan(First[0].InstanceId, 1))));
			EXPECT_FLOAT_EQ(GPU->GetInstancePayloadData()[Changed[0].InstanceId].Values[0], 7.f);
			// Equal transported values do not identify a collector record.
			Prepared.Primitives[0].DynamicPrimitiveData = std::make_shared<FGPUSceneDynamicPrimitiveData>(*Revision);
			const auto Distinct = Context->CreatePrimitiveCollector()->Commit(Prepared);
			ASSERT_EQ(Distinct.size(), 2u); EXPECT_NE(Distinct[0].InstanceId, Changed[0].InstanceId);
			EXPECT_EQ(GPU->GetStatistics().UploadBytes, Bytes);
			ASSERT_TRUE(Context->Upload(Commands, Allocator));
			EXPECT_EQ(First[0].Buffers->SceneData, Changed[0].Buffers->SceneData);
			EXPECT_EQ(Again[0].Buffers->SceneData, Distinct[0].Buffers->SceneData);
			const auto Uploaded = GPU->GetStatistics().UploadBytes;
			ASSERT_TRUE(Context->Upload(Commands, Allocator)); EXPECT_EQ(GPU->GetStatistics().UploadBytes, Uploaded);
			const auto Independent = std::make_shared<FGPUSceneDynamicContext>()->Prepare(Commands, Allocator, Prepared);
			ASSERT_EQ(Independent.size(), 2u); EXPECT_NE(Independent[0].Buffers->PrimitiveCollector->GetDynamicContext(), Context);
			EXPECT_NE(Independent[0].Buffers->SceneData[1], Changed[0].Buffers->SceneData[1]);
			auto OtherGPU = CreateRenderResource<FGPUScene>();
			auto OtherContext = std::make_shared<FGPUSceneDynamicContext>(OtherGPU);
			const auto Other = OtherContext->CreatePrimitiveCollector()->Commit(Prepared);
			ASSERT_EQ(Other.size(), 2u); EXPECT_NE(Other[0].Buffers->SceneData[1], Changed[0].Buffers->SceneData[1]);
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
			auto A = std::make_shared<FMeshPassPipeline>(); A->VertexDeclaration = new FDeclaration;
			auto B = std::make_shared<FMeshPassPipeline>(*A); B->VertexDeclaration = new FDeclaration;
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
			B = std::make_shared<FMeshPassPipeline>(*A); Second.Pipeline = B.get();
			B->Description.Rasterizer.FrontFace = A->Description.Rasterizer.FrontFace == ERHIFrontFace::Clockwise ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
			EXPECT_FALSE(First.MatchesForDynamicInstancing(Second));
			B = std::make_shared<FMeshPassPipeline>(*A); Second.Pipeline = B.get();

			FPreparedStaticMeshView Prepared;
			FResolvedStaticMeshView Resolved;
			Resolved.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();
			auto Binding = std::make_shared<FStaticMeshBatchBinding>(); Binding->Declaration = A->VertexDeclaration;
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
				Record.Commands[{EMeshPipelinePass::Forward}] = Resolved.DrawCommandStorage->Publish({EMeshPipelinePass::Forward}, Command);
			}
			ASSERT_TRUE(SetupMeshDrawCommandDynamicInstancing(Commands, Prepared, Resolved));
			const auto Submission = Resolved.DrawCommandSubmissions.at({EMeshPipelinePass::Forward});
			ASSERT_EQ(Submission->Runs.size(), 3u);
			EXPECT_EQ(Submission->Runs[0].Draws, (std::vector<uint32>{0, 1}));
			const auto* Leader = Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[0], {EMeshPipelinePass::Forward});
			const auto* Follower = Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[1], {EMeshPipelinePass::Forward});
			ASSERT_TRUE(Leader && Follower);
			EXPECT_EQ(Leader->NumInstances, 2u); EXPECT_FALSE(Leader->bSkipDraw);
			EXPECT_TRUE(Follower->bSkipDraw);
			EXPECT_EQ(Leader->StateBucketId, Follower->StateBucketId);
			EXPECT_EQ(Leader->Command, Resolved.GetMeshDrawCommand(Prepared.Opaque[0], {EMeshPipelinePass::Forward}).get());
			EXPECT_FALSE(Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[2], {EMeshPipelinePass::Forward}));
			EXPECT_FALSE(Resolved.GetVisibleMeshDrawCommand(Prepared.Opaque[3], {EMeshPipelinePass::Forward}));
			EXPECT_FALSE(Resolved.GetVisibleMeshDrawCommand(Prepared.Translucent[0], {EMeshPipelinePass::Forward}));
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Bytes = static_cast<FSceneBuffer*>(Resolved.DynamicInstancingInstanceIds.GetReference())->Bytes;
			ASSERT_EQ(Bytes.size(), 8u);
			std::array<uint32, 2> Ids; std::memcpy(Ids.data(), Bytes.data(), Bytes.size());
			EXPECT_EQ(Ids, (std::array<uint32, 2>{0, 1}));
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
			auto A = std::make_shared<FMeshPassPipeline>();
			A->FactoryKey = FStaticMeshBatchBinding{}.GetFactoryKey();
			A->VertexDeclaration = new FDeclaration;
			auto B = std::make_shared<FMeshPassPipeline>(*A); B->VertexDeclaration = new FDeclaration;
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
			auto ChangedState = *B; ChangedState.Description.Rasterizer.LineWidth = 3.f;
			EXPECT_NE(*KeyA.Pipeline, *ChangedState.GetIdentity());
			ChangedState = *B; ChangedState.LayoutKey = FXxHash64{123};
			EXPECT_NE(*KeyA.Pipeline, *ChangedState.GetIdentity());
			EXPECT_EQ(Cache.MakeKey(&Description, FPrimitiveComponentId{1}, 1, *B).Pipeline, KeyB.Pipeline);
			auto MakeCommand = [](const FMeshPassPipeline* Pipeline) {
				FMeshDrawCommand Command; Command.Pipeline = Pipeline;
				Command.bFinalized = true; Command.bCached = true;
				Command.DrawParameters = FRHIDrawIndexedArguments{3, 1, 0, 0, 0};
				return Command;
			};
			const auto First = Cache.Publish(KeyA, MakeCommand(A.get()), *SourceA, nullptr, {}, A);
			const auto Second = Cache.Publish(KeyB, MakeCommand(B.get()), *SourceB, nullptr, {}, B);
			const auto InfoA = Cache.FindInfo(KeyA, *SourceA), InfoB = Cache.FindInfo(KeyB, *SourceB);
			ASSERT_EQ(First, Second); ASSERT_NE(InfoA, InfoB);
			EXPECT_EQ(InfoA->StateBucketId, InfoB->StateBucketId);
			FDynamicMeshDrawCommandStorage Frame;
			const auto VisibleA = Frame.PublishVisible(First, InfoA, std::make_shared<FMeshPassBindings>(), {});
			const auto VisibleB = Frame.PublishVisible(Second, InfoB, std::make_shared<FMeshPassBindings>(), {});
			A.reset(); B.reset();
			SourceA->RemoveCachedMeshDrawCommands();
			EXPECT_FALSE(Frame.GetVisible(VisibleA)); EXPECT_TRUE(Frame.GetVisible(VisibleB));
			EXPECT_FALSE(LifeA.expired()); EXPECT_TRUE(LifeB.expired());
			EXPECT_EQ(Cache.Find(KeyB, *SourceB), Second);
			EXPECT_EQ(Second->Pipeline, LifeA.lock().get());
			// A binding replacement must retire B's old visible handle without
			// reviving A, even when the scene cache reuses an existing slot.
			auto ReplacementPipeline = std::make_shared<FMeshPassPipeline>(*LifeA.lock());
			auto Replacement = MakeCommand(ReplacementPipeline.get()); Replacement.DepthBias[0] = 1.f;
			auto ReplacementKey = KeyB; ++ReplacementKey.Bindings[1];
			const auto Rebuilt = Cache.Publish(ReplacementKey, Replacement, *SourceB, nullptr, {}, ReplacementPipeline);
			EXPECT_NE(Rebuilt, Second); EXPECT_EQ(Cache.Num(), 1u);
			EXPECT_FALSE(Frame.GetVisible(VisibleA)); EXPECT_FALSE(Frame.GetVisible(VisibleB));
			EXPECT_FALSE(Cache.Find(KeyB, *SourceB)); EXPECT_EQ(Cache.Find(ReplacementKey, *SourceB), Rebuilt);
			EXPECT_TRUE(LifeA.expired()); EXPECT_TRUE(LifeB.expired());
			SourceB->RemoveCachedMeshDrawCommands();
			EXPECT_EQ(Cache.Num(), 0u); EXPECT_FALSE(Cache.Find(ReplacementKey, *SourceB));
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
            auto Pipeline = std::make_shared<FMeshPassPipeline>(); Pipeline->Pass = EMeshPipelinePass::GBuffer;
            Pipeline->VertexDeclaration = new FDeclaration;
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
                Record.Commands[{EMeshPipelinePass::GBuffer}] = Resolved.DrawCommandStorage->Publish({EMeshPipelinePass::GBuffer}, Command);
            }
            FInstanceCullingManager Manager(Coordinator);
            FSceneView View;
            const auto FirstView = Manager.RegisterView(View);
            View.ViewLocation = FVector3(1099511627776.0, 0.0, 0.0);
            const auto SecondView = Manager.RegisterView(View);
            auto MakeContext = [&](uint32 Id) {
                auto Context = Manager.CreateContext(Id, {EMeshPipelinePass::GBuffer});
                EXPECT_TRUE(Context->SetupDrawCommands(CollectMeshDrawCommands(Prepared, Resolved, {EMeshPipelinePass::GBuffer}, false), true, true));
                EXPECT_EQ(Context->Candidates.size(), 128u);
                EXPECT_EQ(Context->Candidates[0].InstanceId, 0u);
                EXPECT_EQ(Context->Candidates[64].InstanceId, 64u);
                return Context;
            };
            auto First = MakeContext(FirstView), Second = MakeContext(SecondView);
            FInstanceCullingTelemetry FirstTelemetry, SecondTelemetry;
            // Abandon a built graph. Neither view publishes an executable indirect list.
            {
                FRDGBuilder Abandoned;
                First->BuildRenderingCommands(Abandoned, FirstTelemetry, true, false, EGPUCullingFailure::CommandAdmission);
            }
            EXPECT_FALSE(First->Submission->bDispatched); EXPECT_FALSE(Second->Submission->bDispatched);
            EXPECT_EQ(FirstTelemetry.GPUCullingBufferBytes, 128u * 12u + 2u * 20u);
            EXPECT_GT(FirstTelemetry.GPUCullingSceneCapacityBytes, 0u);
            EXPECT_EQ(FirstTelemetry.GPUCullingStandaloneSceneCapacityBytes, 0u);
            RHI.EnableIndependentComputeForCompilation();
            {
                FRDGBuilder Rejected; Rejected.SetAsyncComputeEnabled(true);
                const auto Params = Second->BuildRenderingCommands(Rejected, SecondTelemetry, true, false, EGPUCullingFailure::CommandAdmission);
                auto Reads = Rejected.AllocParameters<FGPUSceneReadParameters>();
                Reads->Buffers[0] = FRDGBufferAccess{*Params.InstanceIdOffsetBuffer, 0, 128 * sizeof(uint32), ERDGUse::Read, ERHIAccess::GraphicsShaderRead};
                Reads->Buffers[1] = FRDGBufferAccess{*Params.DrawIndirectArgsBuffer, 0, 2 * sizeof(FRHIDrawIndexedIndirectArguments), ERDGUse::Read, ERHIAccess::IndirectArgumentRead};
                Rejected.AddPass("Culling.Consumer", ERDGPassType::Graphics, std::move(Reads),
                    [](FRHICommandListImmediate&, const FGPUSceneReadParameters&, const FRDGParameterResolver&) {});
                FRejectGPUSceneAllocator Reject;
                EXPECT_FALSE(Rejected.Execute(Executor.GetImmediateCommandList(), &Reject));
                EXPECT_TRUE(Rejected.HasCompiledPlan());
                EXPECT_FALSE(Rejected.GetGPUCompletion()); EXPECT_FALSE(Second->Submission->bDispatched);
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
                EXPECT_FALSE(Graph.GetGPUCompletion()); EXPECT_FALSE(CompileRejected->Submission->bDispatched);
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
                EXPECT_FALSE(Retry->Submission->bDispatched);
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
			auto Reads = Drawing.AllocParameters<FGPUSceneReadParameters>();
			Reads->Buffers[0] = FRDGBufferAccess{Handle, 0, Buffer->GetSize(), ERDGUse::Read, ERHIAccess::GraphicsShaderRead};
			Drawing.AddPass("DrawingUsesDynamicRange", ERDGPassType::Graphics, std::move(Reads),
				[](FRHICommandListImmediate&, const FGPUSceneReadParameters&, const FRDGParameterResolver&) {});
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
			Data = std::make_shared<FGPUSceneDynamicContext>((*Scene)->GPUScene)->Prepare(Executor.GetImmediateCommandList(), Allocator, Prepared);
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
