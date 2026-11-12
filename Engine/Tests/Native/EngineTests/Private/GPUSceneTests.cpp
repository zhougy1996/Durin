#include "GPUScene.h"
#include "SceneInfo.h"
#include "SceneTestAccess.h"
#include "Renderers/GPUSceneRendering.h"
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
		EXPECT_GT(Run().UploadBytes, 0u);
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
			Primitive.PrimitiveId = FPrimitiveComponentId{1}; Primitive.bSourceBound = true; Primitive.BatchId = 901;
			Primitive.PrimitiveIdMode = EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo;
			Primitive.PersistentPrimitiveIndex = Info().GetPersistentIndex();
			Primitive.LocalToWorld = FMatrix(1.0); Primitive.WorldBounds = Info().GetWorldBounds();
			Prepared.Primitives.push_back(Primitive);
			auto Description = std::make_shared<FMeshDrawDescription>();
			Description->PipelineKey.FactoryKey = FStaticMeshBatchBinding{}.GetFactoryKey();
			Description->Geometry.InstanceCount = 3;
			FPreparedStaticMeshDraw Draw; Draw.Command = Description;
			Prepared.Opaque.push_back(Draw);
			auto Data = FGPUSceneDynamicContext::Prepare(Executor.GetImmediateCommandList(), Prepared, (*Scene)->GPUScene.get());
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
			EXPECT_TRUE(FGPUSceneDynamicContext::Prepare(Executor.GetImmediateCommandList(), Prepared, (*Scene)->GPUScene.get()).empty());
			Prepared.Primitives[0].PrimitiveIdMode = EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData;
			Prepared.Primitives[0].LocalToWorld = Math::TranslationMatrix(FVector3(13.0, 0.0, 0.0));
			auto Scoped = FGPUSceneDynamicContext::Prepare(Executor.GetImmediateCommandList(), Prepared);
			ASSERT_EQ(Scoped.size(), 1u); ASSERT_TRUE(Scoped[0].Buffers);
			EXPECT_NE(Scoped[0].Buffers->SceneData[0], Data[0].Buffers->SceneData[0]);
			Executor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			const auto& Packed = static_cast<FSceneBuffer*>(Scoped[0].Buffers->SceneData[1].GetReference())->Bytes;
			FInstanceSceneShaderData Instance; std::memcpy(&Instance, Packed.data(), sizeof(Instance));
			EXPECT_FLOAT_EQ(Instance.Data[12].x, 13.f);
			Description->PipelineKey.FactoryKey = {};
			auto Unsupported = FGPUSceneDynamicContext::Prepare(Executor.GetImmediateCommandList(), Prepared);
			ASSERT_EQ(Unsupported.size(), 1u); EXPECT_FALSE(Unsupported[0].Buffers);
		});
		FlushRenderingCommands();
		EXPECT_EQ(Run().UploadBytes, 0u);
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
			B->Description.Rasterizer.FrontFace = A->Description.Rasterizer.FrontFace == ERHIFrontFace::Clockwise ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
			EXPECT_FALSE(First.MatchesForDynamicInstancing(Second));
			B->Description = A->Description;

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
			ASSERT_EQ(Resolved.DynamicInstancingGroups.size(), 1u);
			EXPECT_EQ(Resolved.DynamicInstancingGroups[0].Draws, (std::vector<uint32>{0, 1}));
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
