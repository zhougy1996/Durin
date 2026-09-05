#include "Renderers/SimpleElement/SimpleElementRenderer.h"

#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/SimpleElement/EditorIconAtlas.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Misc/AssertionMacros.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	using namespace RendererEditorAssistance;

	namespace
	{
		class FSimpleLineVertexShader : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FSimpleLineVertexShader, FGlobalShader,
				"/Engine/Gizmo", EShaderFrequency::Vertex, "LineVertexMain");
		};

		class FSimpleLineFragmentShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSimpleLineFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Style);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(FSimpleLineFragmentShader, FGlobalShader,
				"/Engine/Gizmo", EShaderFrequency::Fragment, "LineFragmentMain");
		};

		class FSimpleSpriteVertexShader : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FSimpleSpriteVertexShader, FGlobalShader,
				"/Engine/Gizmo", EShaderFrequency::Vertex, "IconVertexMain");
		};

		class FSimpleSpriteFragmentShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSimpleSpriteFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(Atlas);
				DURIN_SHADER_PARAMETER_SAMPLER(AtlasSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(IconStyle);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(FSimpleSpriteFragmentShader, FGlobalShader,
				"/Engine/Gizmo", EShaderFrequency::Fragment, "IconFragmentMain");
		};

		DURIN_IMPLEMENT_GLOBAL_SHADER(FSimpleLineVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FSimpleLineFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FSimpleSpriteVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FSimpleSpriteFragmentShader);
		const FGlobalShaderSetRegistration GSimpleLineShaderSet(
			"Renderer", "EditorAssistance.SimpleElement.Line",
			EShaderRequestEligibility::EditorOnly,
			{&FSimpleLineVertexShader::StaticType(),
			 &FSimpleLineFragmentShader::StaticType()});
		const FGlobalShaderSetRegistration GSimpleSpriteShaderSet(
			"Renderer", "EditorAssistance.SimpleElement.Sprite",
			EShaderRequestEligibility::EditorOnly,
			{&FSimpleSpriteVertexShader::StaticType(),
			 &FSimpleSpriteFragmentShader::StaticType()});

		struct FSimpleStyleUniform
		{
			float AlphaScale = 1.0f;
			FVector3f Padding{0.0f};
		};

		struct FSimplePipelineKey
		{
			ESimpleElementShaderClass ShaderClass =
				ESimpleElementShaderClass::Untextured;
			ESimpleElementBlendMode BlendMode = ESimpleElementBlendMode::Opaque;
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World;
			ESceneDepthConvention DepthConvention =
				ESceneDepthConvention::ForwardZ;
			RenderTargetLayouts::EViewportOutput Output =
				RenderTargetLayouts::EViewportOutput::Offscreen;
			EPixelFormat OutputFormat = EPixelFormat::SRGBA8_UNORM;

			auto operator==(const FSimplePipelineKey&) const -> bool = default;
		};

		auto MakePipelineKey(const FSimpleElementBatchKey& Key,
			EPixelFormat OutputFormat)
			-> FSimplePipelineKey
		{
			return {
				.ShaderClass = Key.ShaderClass,
				.BlendMode = Key.BlendMode,
				.DepthPriorityGroup = Key.DepthPriorityGroup,
				.DepthConvention = Key.DepthConvention,
				.Output = Key.Output,
				.OutputFormat = OutputFormat,
			};
		}

		auto MakePipelineName(const FSimplePipelineKey& Key) -> std::string
		{
			return std::format("SimpleElement{}{}{}{}Pipeline",
				Key.ShaderClass == ESimpleElementShaderClass::Textured
					? "Textured" : "Untextured",
				Key.BlendMode == ESimpleElementBlendMode::Translucent
					? "Translucent" : "Opaque",
				Key.DepthPriorityGroup == ESceneDepthPriorityGroup::Foreground
					? "Foreground" : "World",
				Key.Output == RenderTargetLayouts::EViewportOutput::Present
					? "Present" : "Offscreen");
		}
	} // namespace

	struct FSimpleElementRenderer::FState
	{
		struct FBasePayload
		{
			FGlobalShaderSetRef LineShaderSet;
			FGlobalShaderSetRef SpriteShaderSet;
			TShaderMapRef<FSimpleLineVertexShader> LineVertexShader;
			TShaderMapRef<FSimpleLineFragmentShader> LineFragmentShader;
			TShaderMapRef<FSimpleSpriteVertexShader> SpriteVertexShader;
			TShaderMapRef<FSimpleSpriteFragmentShader> SpriteFragmentShader;
			FVertexDeclarationRHIRef LineVertexDeclaration;
			FVertexDeclarationRHIRef SpriteVertexDeclaration;
			FTextureRHIRef EditorIconAtlas;
			FSamplerRHIRef Sampler;
		};

		struct FPipelinePayload
		{
			FGraphicsPipelineStateRHIRef Pipeline;
			FGlobalShaderSetRef ShaderSet;
		};

		struct FPipelineEntry
		{
			FSimplePipelineKey Key;
			TRenderResourceCreationSlot<FPipelinePayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		TRenderResourceCreationSlot<FBasePayload> Base{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device
				| ERenderResourceGenerationDependency::Manual};
		std::vector<FPipelineEntry> Pipelines;
		FBufferRHIRef VertexBuffer;
		FBufferRHIRef IndexBuffer;
		uint32 VertexCapacity = 0;
		uint32 IndexCapacity = 0;
		std::optional<FRenderResourceGeneration> BufferFailureGeneration;
	};

	FSimpleElementRenderer::FSimpleElementRenderer(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
		, State(std::make_unique<FState>())
	{
	}

	FSimpleElementRenderer::~FSimpleElementRenderer() = default;

	auto FSimpleElementRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output,
		std::span<const FSimpleElement> AdditionalElements,
		EPixelFormat OutputFormat)
		-> FPreparedSimpleElementRendering
	{
		check(IsInRenderingThread());
		FPreparedSimpleElementRendering Prepared;
		const FPreparedSimpleElements Collected =
			FSimpleElementCollector::Collect(View, Output, AdditionalElements);
		Prepared.Statistics = Collected.Statistics;
		if (Collected.IsEmpty())
			return Prepared;
		// Resolve both fixed families together so later view demand cannot observe
		// a demand-shaped base payload from an earlier frame.
		constexpr bool bNeedsLine = true;
		constexpr bool bNeedsSprite = true;

		using FBasePayload = FState::FBasePayload;
		using FBaseResult = TRenderResourceCreateResult<FBasePayload>;
		FBasePayload* Base = State->Base.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[&CommandList, bNeedsLine, bNeedsSprite]() -> FBaseResult {
				FBasePayload Candidate;
				const std::array<const FGlobalShaderType*, 2> LineTypes = {
					&FSimpleLineVertexShader::StaticType(),
					&FSimpleLineFragmentShader::StaticType()};
				const std::array<const FGlobalShaderType*, 2> SpriteTypes = {
					&FSimpleSpriteVertexShader::StaticType(),
					&FSimpleSpriteFragmentShader::StaticType()};
				if (bNeedsLine)
				{
					Candidate.LineShaderSet = GetGlobalShaderMap().ResolveShaderSet(
						"EditorAssistance.SimpleElement.Line", LineTypes, true,
						ReportRendererResourceCreateDiagnostic);
					if (Candidate.LineShaderSet)
					{
						Candidate.LineVertexShader =
							TShaderMapRef<FSimpleLineVertexShader>(Candidate.LineShaderSet);
						Candidate.LineFragmentShader =
							TShaderMapRef<FSimpleLineFragmentShader>(Candidate.LineShaderSet);
					}
				}
				if (bNeedsSprite)
				{
					Candidate.SpriteShaderSet = GetGlobalShaderMap().ResolveShaderSet(
						"EditorAssistance.SimpleElement.Sprite", SpriteTypes, true,
						ReportRendererResourceCreateDiagnostic);
					if (Candidate.SpriteShaderSet)
					{
						Candidate.SpriteVertexShader =
							TShaderMapRef<FSimpleSpriteVertexShader>(Candidate.SpriteShaderSet);
						Candidate.SpriteFragmentShader =
							TShaderMapRef<FSimpleSpriteFragmentShader>(Candidate.SpriteShaderSet);
					}
				}

				FVertexDeclarationElementList LineElements;
				LineElements[0] = FVertexElement(0,
					static_cast<uint8>(offsetof(FSimpleElementVertex, Position)),
					EVertexElementType::Float4, 0, sizeof(FSimpleElementVertex));
				LineElements[1] = FVertexElement(0,
					static_cast<uint8>(offsetof(FSimpleElementVertex, Color)),
					EVertexElementType::Float4, 1, sizeof(FSimpleElementVertex));
				LineElements[2] = FVertexElement(0,
					static_cast<uint8>(offsetof(FSimpleElementVertex, Pattern)),
					EVertexElementType::Float2, 2, sizeof(FSimpleElementVertex));
				if (Candidate.LineShaderSet)
					Candidate.LineVertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(LineElements);

				FVertexDeclarationElementList SpriteElements;
				SpriteElements[0] = FVertexElement(0,
					static_cast<uint8>(offsetof(FSimpleElementVertex, Position)),
					EVertexElementType::Float4, 0, sizeof(FSimpleElementVertex));
				SpriteElements[1] = FVertexElement(0,
					static_cast<uint8>(offsetof(FSimpleElementVertex, UV)),
					EVertexElementType::Float2, 1, sizeof(FSimpleElementVertex));
				SpriteElements[2] = FVertexElement(0,
					static_cast<uint8>(offsetof(FSimpleElementVertex, Color)),
					EVertexElementType::Float4, 2, sizeof(FSimpleElementVertex));
				if (Candidate.SpriteShaderSet)
					Candidate.SpriteVertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(SpriteElements);

				const auto Pixels = BuildEditorIconAtlasPixels();
				const FRHITextureCreateDesc TextureDesc =
					FRHITextureCreateDesc::Create2D("EditorSimpleElementIconAtlas",
						FEditorIconAtlasLayout::Width,
						FEditorIconAtlasLayout::Height,
						EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource);
				if (Candidate.SpriteShaderSet)
					Candidate.EditorIconAtlas =
						GDynamicRHI->RHICreateTexture(CommandList, TextureDesc);
				if (Candidate.EditorIconAtlas)
				{
					const FUpdateTextureRegion2D Region(0, 0, 0, 0,
						FEditorIconAtlasLayout::Width,
						FEditorIconAtlasLayout::Height);
					GDynamicRHI->RHIUpdateTexture2D(CommandList,
						Candidate.EditorIconAtlas, 0, 0, Region,
						FEditorIconAtlasLayout::RowPitchBytes,
						std::as_bytes(std::span(Pixels)));
				}
				if (Candidate.SpriteShaderSet)
					Candidate.Sampler =
						RHICreateSampler(FRHISamplerDesc::LinearClamp());
				const bool bLineReady = bNeedsLine
					&& Candidate.LineVertexShader
						&& Candidate.LineFragmentShader
						&& Candidate.LineVertexDeclaration;
				const bool bSpriteReady = bNeedsSprite
					&& Candidate.SpriteVertexShader
						&& Candidate.SpriteFragmentShader
						&& Candidate.SpriteVertexDeclaration
						&& Candidate.EditorIconAtlas && Candidate.Sampler;
				if (Candidate.LineShaderSet && !bLineReady)
				{
					ReportRendererResourceCreateDiagnostic({
						.Error = MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"SimpleElement", "line-base",
							"Line shader or vertex declaration creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual),
					});
				}
				if (Candidate.SpriteShaderSet && !bSpriteReady)
				{
					ReportRendererResourceCreateDiagnostic({
						.Error = MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"SimpleElement", "sprite-base",
							"Sprite declaration, atlas, or sampler creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual),
					});
				}
				if (!bLineReady)
					Candidate.LineShaderSet = {};
				if (!bSpriteReady)
					Candidate.SpriteShaderSet = {};
				if (!bLineReady && !bSpriteReady)
				{
					return FBaseResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"SimpleElement", "base",
						"Every requested simple-element shader class is unavailable.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				}
				return FBaseResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);
		if (Base == nullptr)
			return Prepared;

		std::vector<FSimpleElementVertex> Vertices;
		std::vector<uint32> Indices;
		Vertices.reserve(Collected.Statistics.VertexCount);
		Indices.reserve(Collected.Statistics.IndexCount);
		struct FPendingDraw
		{
			const FPreparedSimpleElementBatch* Batch = nullptr;
			uint32 StartIndex = 0;
			int32 VertexOffset = 0;
		};
		std::vector<FPendingDraw> PendingDraws;
		PendingDraws.reserve(Collected.Batches.size());
		for (const FPreparedSimpleElementBatch& Batch : Collected.Batches)
		{
			PendingDraws.push_back({
				.Batch = &Batch,
				.StartIndex = static_cast<uint32>(Indices.size()),
				.VertexOffset = static_cast<int32>(Vertices.size()),
			});
			Vertices.insert(Vertices.end(), Batch.Vertices.begin(), Batch.Vertices.end());
			Indices.insert(Indices.end(), Batch.Indices.begin(), Batch.Indices.end());
		}

		const uint32 VertexBytes =
			static_cast<uint32>(Vertices.size() * sizeof(FSimpleElementVertex));
		const uint32 IndexBytes =
			static_cast<uint32>(Indices.size() * sizeof(uint32));
		const FRenderResourceGeneration& Generation =
			Coordinator.GetGeneration_RenderThread();
		auto ReportBufferFailure = [this, &Generation](std::string Identity) {
			if (State->BufferFailureGeneration == Generation)
				return;
			State->BufferFailureGeneration = Generation;
			ReportRendererResourceCreateDiagnostic({
				.Error = MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::RHIResource,
					"SimpleElement", std::move(Identity),
					"Dynamic buffer allocation returned null.",
					ERenderResourceGenerationDependency::Device
						| ERenderResourceGenerationDependency::Manual),
			});
		};
		if (VertexBytes > State->VertexCapacity)
		{
			const uint32 Capacity = std::bit_ceil(VertexBytes);
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex(
				"SimpleElementVertexBuffer", Capacity);
			Desc.Usage |= EBufferUsageFlags::Dynamic;
			FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			if (!Buffer)
			{
				ReportBufferFailure("vertex-buffer");
				return Prepared;
			}
			State->VertexBuffer = std::move(Buffer);
			State->VertexCapacity = Capacity;
		}
		if (IndexBytes > State->IndexCapacity)
		{
			const uint32 Capacity = std::bit_ceil(IndexBytes);
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateIndex(
				"SimpleElementIndexBuffer", Capacity, sizeof(uint32));
			Desc.Usage |= EBufferUsageFlags::Dynamic;
			FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			if (!Buffer)
			{
				ReportBufferFailure("index-buffer");
				return Prepared;
			}
			State->IndexBuffer = std::move(Buffer);
			State->IndexCapacity = Capacity;
		}
		if (!State->VertexBuffer || !State->IndexBuffer)
			return Prepared;
		CommandList.WriteBuffer(State->VertexBuffer, Vertices.data(), VertexBytes, 0);
		CommandList.WriteBuffer(State->IndexBuffer, Indices.data(), IndexBytes, 0);
		State->BufferFailureGeneration.reset();

		for (const FPendingDraw& Pending : PendingDraws)
		{
			const FPreparedSimpleElementBatch& Batch = *Pending.Batch;
			const FSimplePipelineKey Key = MakePipelineKey(
				Batch.Key, OutputFormat);
			auto EntryIt = std::ranges::find(
				State->Pipelines, Key, &FState::FPipelineEntry::Key);
			if (EntryIt == State->Pipelines.end())
				EntryIt = State->Pipelines.emplace(
					State->Pipelines.end(), FState::FPipelineEntry{.Key = Key});
			const FGlobalShaderSetRef ShaderSet =
				Key.ShaderClass == ESimpleElementShaderClass::Textured
					? Base->SpriteShaderSet : Base->LineShaderSet;
			if (!ShaderSet)
				continue;
			using FPipelineResult =
				TRenderResourceCreateResult<FState::FPipelinePayload>;
			const std::string PipelineName = MakePipelineName(Key);
			auto* Pipeline = EntryIt->Slot.Resolve(ShaderSet.GetGeneration(),
				[Base, Key, ShaderSet, PipelineName]() -> FPipelineResult {
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout =
						RenderTargetLayouts::MakeEditorAssistanceOutput(
							Key.Output, Key.OutputFormat);
					if (Key.ShaderClass == ESimpleElementShaderClass::Textured)
					{
						Initializer.BoundShaders.VertexShader =
							Base->SpriteVertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							Base->SpriteFragmentShader.GetRHIShader();
						Initializer.VertexDeclaration = Base->SpriteVertexDeclaration;
					}
					else
					{
						Initializer.BoundShaders.VertexShader =
							Base->LineVertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							Base->LineFragmentShader.GetRHIShader();
						Initializer.VertexDeclaration = Base->LineVertexDeclaration;
					}
					Initializer.ColorBlendStates[0] =
						Key.BlendMode == ESimpleElementBlendMode::Translucent
							? FRHIColorBlendState::StraightAlpha()
							: FRHIColorBlendState{};
					Initializer.RasterizerState.CullMode = ERHICullMode::None;
					Initializer.DepthStencilState.bEnableTest =
						Key.DepthPriorityGroup == ESceneDepthPriorityGroup::World;
					Initializer.DepthStencilState.CompareOp =
						GetVisibleDepthCompareOp(Key.DepthConvention);
					Initializer.PipelineLayout = ShaderSet.GetPipelineLayout();
					FGraphicsPipelineStateRHIRef Candidate =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							FName(PipelineName), Initializer);
					if (!Candidate)
					{
						return FPipelineResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::GraphicsPipeline,
							"SimpleElement", PipelineName,
							"RHI graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					}
					return FPipelineResult::Success({
						.Pipeline = std::move(Candidate), .ShaderSet = ShaderSet});
				}, ReportRendererResourceCreateDiagnostic);
			if (Pipeline == nullptr)
				continue;

			FTextureRHIRef Texture;
			if (Batch.Key.ShaderClass == ESimpleElementShaderClass::Textured)
			{
				if (Batch.Key.Texture.Kind
					== FSimpleElementTexture::EKind::EditorIconAtlas)
				{
					Texture = Base->EditorIconAtlas;
				}
				else if (Batch.Key.Texture.TextureReference)
				{
					Texture = Batch.Key.Texture.TextureReference
						->GetReferencedTexture_RenderThread();
				}
				if (!Texture)
					continue;
			}
			Prepared.Draws.push_back({
				.Key = Batch.Key,
				.Pipeline = Pipeline->Pipeline,
				.ShaderSet = Pipeline->ShaderSet,
				.Texture = std::move(Texture),
				.Sampler = Base->Sampler,
				.IndexCount = static_cast<uint32>(Batch.Indices.size()),
				.StartIndex = Pending.StartIndex,
				.VertexOffset = Pending.VertexOffset,
			});
		}
		Prepared.VertexCapacity = State->VertexCapacity;
		Prepared.IndexCapacity = State->IndexCapacity;
		return Prepared;
	}

	auto FSimpleElementRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedSimpleElementRendering& Prepared,
		ESceneDepthPriorityGroup DepthPriorityGroup) -> void
	{
		check(IsInRenderingThread());
		if (!State->VertexBuffer || !State->IndexBuffer)
			return;
		for (const FPreparedSimpleElementDraw& Draw : Prepared.Draws)
		{
			if (Draw.Key.DepthPriorityGroup != DepthPriorityGroup
				|| !Draw.Pipeline)
				continue;
			CommandList.SetGraphicsPipelineState(*Draw.Pipeline);
			CommandList.BindVertexBuffer(0, State->VertexBuffer, 0);
			CommandList.BindIndexBuffer(State->IndexBuffer, 0);
			const FSimpleStyleUniform Style;
			if (Draw.Key.ShaderClass == ESimpleElementShaderClass::Textured)
			{
				if (!Draw.Texture || !Draw.Sampler)
					continue;
				FSimpleSpriteFragmentShader::FParameters Parameters;
				Parameters.Atlas = Draw.Texture;
				Parameters.AtlasSampler = Draw.Sampler;
				Parameters.IconStyle = CommandList.AllocateDynamicUniformBuffer(
					&Style, sizeof(Style));
				SetShaderParameters(CommandList,
					TShaderMapRef<FSimpleSpriteFragmentShader>(Draw.ShaderSet),
					Parameters);
			}
			else
			{
				FSimpleLineFragmentShader::FParameters Parameters;
				Parameters.Style = CommandList.AllocateDynamicUniformBuffer(
					&Style, sizeof(Style));
				SetShaderParameters(CommandList,
					TShaderMapRef<FSimpleLineFragmentShader>(Draw.ShaderSet),
					Parameters);
			}
			CommandList.DrawIndexed(
				Draw.IndexCount, Draw.StartIndex, Draw.VertexOffset);
		}
	}

	auto FSimpleElementRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->Base.Reset();
		for (FState::FPipelineEntry& Entry : State->Pipelines)
			Entry.Slot.Reset();
		State->Pipelines.clear();
		State->VertexBuffer = nullptr;
		State->IndexBuffer = nullptr;
		State->VertexCapacity = 0;
		State->IndexCapacity = 0;
		State->BufferFailureGeneration.reset();
	}
} // namespace Durin
