#include "Renderers/EditorAssistance/OverlayIconRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "CoreGlobals.h"
#include "Misc/AssertionMacros.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	using namespace RendererEditorAssistance;

	namespace
	{
		class FOverlayIconVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FOverlayIconVertexShader,
				FShader,
				"/Engine/Gizmo",
				EShaderFrequency::Vertex,
				"IconVertexMain");
		};

		class FOverlayIconFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FOverlayIconFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(Atlas);
				DURIN_SHADER_PARAMETER_SAMPLER(AtlasSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(IconStyle);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FOverlayIconFragmentShader,
				FShader,
				"/Engine/Gizmo",
				EShaderFrequency::Fragment,
				"IconFragmentMain");
		};

		struct FOverlayIconStyleUniform
		{
			float AlphaScale = 1.0f;
			FVector3f Padding{0.0f};
		};

		struct FOverlayIconVertex
		{
			FVector4f Position{0.0f};
			FVector2f UV{0.0f};
			FVector4f Color{1.0f};
		};

		auto BuildAtlasPixels()
			-> std::array<uint8, FOverlayIconAtlasLayout::PixelByteCount>
		{
			constexpr uint32 Size = FOverlayIconAtlasLayout::IconExtent;
			constexpr uint32 SamplesPerAxis = 4;
			constexpr uint32 AtlasWidth = FOverlayIconAtlasLayout::Width;
			std::array<uint8, FOverlayIconAtlasLayout::PixelByteCount> Pixels{};
			auto InsideCircle = [](
				float X,
				float Y,
				float CenterX,
				float CenterY,
				float Radius) {
				const float DX = X - CenterX;
				const float DY = Y - CenterY;
				return DX * DX + DY * DY <= Radius * Radius;
			};
			auto InsideLens = [](float X, float Y) {
				if (X < 42.0f || X > 57.0f)
					return false;
				const float T = (X - 42.0f) / 15.0f;
				const float Top = 29.0f * (1.0f - T) + 20.0f * T;
				const float Bottom = 43.0f * (1.0f - T) + 52.0f * T;
				return Y >= Top && Y <= Bottom;
			};
			for (uint32 Y = 0; Y < Size; ++Y)
			{
				for (uint32 X = 0; X < Size; ++X)
				{
					uint32 CoveredSamples = 0;
					for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
					{
						for (uint32 SampleX = 0;
							SampleX < SamplesPerAxis;
							++SampleX)
						{
							const float PX = static_cast<float>(X)
								+ (static_cast<float>(SampleX) + 0.5f)
									/ SamplesPerAxis;
							const float PY = static_cast<float>(Y)
								+ (static_cast<float>(SampleY) + 0.5f)
									/ SamplesPerAxis;
							const bool bBody =
								PX >= 9.0f && PX <= 44.0f
								&& PY >= 27.0f && PY <= 49.0f;
							const bool bReels =
								InsideCircle(PX, PY, 19.0f, 21.0f, 10.0f)
								|| InsideCircle(
									PX, PY, 37.0f, 20.0f, 9.0f);
							const bool bReelHole =
								InsideCircle(PX, PY, 19.0f, 21.0f, 3.5f)
								|| InsideCircle(
									PX, PY, 37.0f, 20.0f, 3.0f);
							if ((bBody || bReels || InsideLens(PX, PY))
								&& !bReelHole)
							{
								++CoveredSamples;
							}
						}
					}
					const size_t Offset =
						(static_cast<size_t>(Y) * AtlasWidth + X) * 4;
					Pixels[Offset + 0] = 255;
					Pixels[Offset + 1] = 255;
					Pixels[Offset + 2] = 255;
					Pixels[Offset + 3] = static_cast<uint8>(
						CoveredSamples * 255
						/ (SamplesPerAxis * SamplesPerAxis));
				}
			}
			for (uint32 Y = 0; Y < Size; ++Y)
			{
				for (uint32 X = 0; X < Size; ++X)
				{
					uint32 CoveredSamples = 0;
					for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
					{
						for (uint32 SampleX = 0;
							SampleX < SamplesPerAxis;
							++SampleX)
						{
							const float PX = static_cast<float>(X)
								+ (static_cast<float>(SampleX) + 0.5f)
									/ SamplesPerAxis
								- 32.0f;
							const float PY = static_cast<float>(Y)
								+ (static_cast<float>(SampleY) + 0.5f)
									/ SamplesPerAxis
								- 32.0f;
							const float Radius = std::sqrt(PX * PX + PY * PY);
							const float Angle = std::atan2(PY, PX);
							const float RayAxisDistance =
								std::abs(std::sin(Angle * 4.0f)) * Radius;
							const bool bDisc = Radius <= 12.0f;
							const bool bRay = Radius >= 17.0f
								&& Radius <= 27.0f
								&& RayAxisDistance <= 2.2f;
							if (bDisc || bRay)
								++CoveredSamples;
						}
					}
					const size_t Offset =
						(static_cast<size_t>(Y) * AtlasWidth + Size + X) * 4;
					Pixels[Offset + 0] = 255;
					Pixels[Offset + 1] = 255;
					Pixels[Offset + 2] = 255;
					Pixels[Offset + 3] = static_cast<uint8>(
						CoveredSamples * 255
						/ (SamplesPerAxis * SamplesPerAxis));
				}
			}
			for (uint32 Y = 0; Y < Size; ++Y)
			{
				for (uint32 X = 0; X < Size; ++X)
				{
					uint32 CoveredSamples = 0;
					for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
					{
						for (uint32 SampleX = 0; SampleX < SamplesPerAxis; ++SampleX)
						{
							const float PX = static_cast<float>(X)
								+ (static_cast<float>(SampleX) + 0.5f) / SamplesPerAxis - 32.0f;
							const float PY = static_cast<float>(Y)
								+ (static_cast<float>(SampleY) + 0.5f) / SamplesPerAxis - 26.0f;
							const float Radius = std::sqrt(PX * PX + PY * PY);
							const bool bPinHead = Radius <= 16.0f;
							const float TailHalfWidth = std::max(0.0f, (20.0f - PY) * 0.32f);
							const bool bPinTail = PY >= 8.0f && PY <= 24.0f && std::abs(PX) <= TailHalfWidth;
							const bool bCenterHole = Radius <= 5.5f;
							if ((bPinHead || bPinTail) && !bCenterHole) ++CoveredSamples;
						}
					}
					const size_t Offset =
						(static_cast<size_t>(Y) * AtlasWidth + Size * 2 + X) * 4;
					Pixels[Offset + 0] = 255;
					Pixels[Offset + 1] = 255;
					Pixels[Offset + 2] = 255;
					Pixels[Offset + 3] = static_cast<uint8>(
						CoveredSamples * 255 / (SamplesPerAxis * SamplesPerAxis));
				}
			}
			return Pixels;
		}

		auto BuildGeometry(
			const FSceneView& View,
			std::vector<FOverlayIconVertex>& OutVertices,
			std::vector<uint32>& OutIndices) -> void
		{
			for (const FViewOverlayIcon& Icon : View.OverlayIcons)
			{
				if (!std::isfinite(Icon.SizePixels) || Icon.SizePixels <= 0.0f)
					continue;
				const float MinU = FOverlayIconAtlasLayout::GetMinU(Icon.Icon);
				const float MaxU = MinU
					+ static_cast<float>(FOverlayIconAtlasLayout::IconExtent)
						/ FOverlayIconAtlasLayout::Width;
				const FVector4 Clip =
					View.ViewProjectionMatrix
					* FVector4(Icon.WorldPosition, 1.0);
				if (!std::isfinite(Clip.x)
					|| !std::isfinite(Clip.y)
					|| !std::isfinite(Clip.z)
					|| !std::isfinite(Clip.w)
					|| Clip.w <= 1.e-8
					|| Clip.z < 0.0)
				{
					continue;
				}
				const double HalfNdcX =
					static_cast<double>(Icon.SizePixels)
					/ std::max(1u, View.ViewportWidth);
				const double HalfNdcY =
					static_cast<double>(Icon.SizePixels)
					/ std::max(1u, View.ViewportHeight);
				auto MakePosition = [&](double X, double Y) {
					return FVector4f(
						static_cast<float>(Clip.x + X * Clip.w),
						static_cast<float>(Clip.y + Y * Clip.w),
						static_cast<float>(Clip.z),
						static_cast<float>(Clip.w));
				};
				const uint32 Base = static_cast<uint32>(OutVertices.size());
				OutVertices.push_back({
					MakePosition(-HalfNdcX, -HalfNdcY),
					{MinU, 0.0f},
					Icon.Color,
				});
				OutVertices.push_back({
					MakePosition(HalfNdcX, -HalfNdcY),
					{MaxU, 0.0f},
					Icon.Color,
				});
				OutVertices.push_back({
					MakePosition(-HalfNdcX, HalfNdcY),
					{MinU, 1.0f},
					Icon.Color,
				});
				OutVertices.push_back({
					MakePosition(HalfNdcX, HalfNdcY),
					{MaxU, 1.0f},
					Icon.Color,
				});
				OutIndices.insert(OutIndices.end(), {
					Base,
					Base + 1,
					Base + 2,
					Base + 2,
					Base + 1,
					Base + 3,
				});
			}
		}

		auto GetDepthName(EDepthMode DepthMode) -> std::string_view
		{
			return DepthMode == EDepthMode::XRay ? "XRay" : "Visible";
		}

		auto GetOutputName(RenderTargetLayouts::EViewportOutput Output)
			-> std::string_view
		{
			return Output == RenderTargetLayouts::EViewportOutput::Present
				? "Present"
				: "Offscreen";
		}

		auto FindPreparedPipeline(
			const FPrepared& Prepared,
			EDepthMode DepthMode) -> FGraphicsPipelineStateRHIRef
		{
			const auto It = std::ranges::find_if(
				Prepared.Pipelines,
				[DepthMode](const FPreparedPipeline& Pipeline) {
					return Pipeline.Key.Feature == EFeature::OverlayIcon
						&& Pipeline.Key.DepthMode == DepthMode;
				});
			return It != Prepared.Pipelines.end() ? It->Pipeline : nullptr;
		}
	} // namespace

	struct FOverlayIconRenderer::FState
	{
		struct FBasePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FOverlayIconVertexShader> VertexShader;
			TShaderRef<FOverlayIconFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FTextureRHIRef Atlas;
			FSamplerRHIRef AtlasSampler;
		};

		struct FPipelineEntry
		{
			FPipelineKey Key;
			TRenderResourceCreationSlot<FGraphicsPipelineStateRHIRef> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		TRenderResourceCreationSlot<FBasePayload> Base{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		std::vector<FPipelineEntry> Pipelines;
		FBufferRHIRef VertexBuffer;
		FBufferRHIRef IndexBuffer;
		uint32 VertexCapacity = 0;
		uint32 IndexCapacity = 0;
	};

	FOverlayIconRenderer::FOverlayIconRenderer(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
		, State(std::make_unique<FState>())
	{
	}

	FOverlayIconRenderer::~FOverlayIconRenderer() = default;

	auto FOverlayIconRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output,
		FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		std::vector<FOverlayIconVertex> Vertices;
		std::vector<uint32> Indices;
		Vertices.reserve(View.OverlayIcons.size() * 4);
		Indices.reserve(View.OverlayIcons.size() * 6);
		BuildGeometry(View, Vertices, Indices);
		if (Vertices.empty() || Indices.empty())
			return;

		using FBasePayload = FState::FBasePayload;
		using FBaseResult = TRenderResourceCreateResult<FBasePayload>;
		FBasePayload* Base = State->Base.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FBaseResult {
				FShaderCompileOptions CompileOptions;
				CompileOptions.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexShaderType =
					FOverlayIconVertexShader::StaticType();
				FShaderType& FragmentShaderType =
					FOverlayIconFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> ShaderTypes = {
					&VertexShaderType,
					&FragmentShaderType};

				FBasePayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string ErrorMessage;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(
						ShaderTypes, CompileOptions, ErrorMessage))
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"OverlayIcon",
							"base",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				auto* VertexShader =
					static_cast<FOverlayIconVertexShader*>(
						Candidate.ShaderMap->GetShader(&VertexShaderType));
				auto* FragmentShader =
					static_cast<FOverlayIconFragmentShader*>(
						Candidate.ShaderMap->GetShader(&FragmentShaderType));
				if (VertexShader == nullptr || FragmentShader == nullptr)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"OverlayIcon",
							"base",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.VertexShader =
					TShaderRef<FOverlayIconVertexShader>(
						VertexShader, Candidate.ShaderMap.get());
				Candidate.FragmentShader =
					TShaderRef<FOverlayIconFragmentShader>(
						FragmentShader, Candidate.ShaderMap.get());
				FVertexDeclarationElementList Elements;
				Elements[0] = FVertexElement(
					0,
					static_cast<uint8>(
						offsetof(FOverlayIconVertex, Position)),
					EVertexElementType::Float4,
					0,
					sizeof(FOverlayIconVertex));
				Elements[1] = FVertexElement(
					0,
					static_cast<uint8>(offsetof(FOverlayIconVertex, UV)),
					EVertexElementType::Float2,
					1,
					sizeof(FOverlayIconVertex));
				Elements[2] = FVertexElement(
					0,
					static_cast<uint8>(
						offsetof(FOverlayIconVertex, Color)),
					EVertexElementType::Float4,
					2,
					sizeof(FOverlayIconVertex));
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(Elements);
				const auto Pixels = BuildAtlasPixels();
				FRHITextureCreateDesc TextureDesc =
					FRHITextureCreateDesc::Create2D(
						"EditorOverlayIconAtlas",
						FOverlayIconAtlasLayout::Width,
						FOverlayIconAtlasLayout::Height,
						EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource);
				Candidate.Atlas =
					GDynamicRHI->RHICreateTexture(CommandList, TextureDesc);
				if (Candidate.Atlas != nullptr)
				{
					const FUpdateTextureRegion2D Region(
						0, 0, 0, 0,
						FOverlayIconAtlasLayout::Width,
						FOverlayIconAtlasLayout::Height);
					GDynamicRHI->RHIUpdateTexture2D(
						CommandList,
						Candidate.Atlas,
						0,
						0,
						Region,
						FOverlayIconAtlasLayout::RowPitchBytes,
						Pixels.data());
				}
				Candidate.AtlasSampler =
					RHICreateSampler(FRHISamplerDesc::LinearClamp());
				if (Candidate.VertexShader.GetRHIShader(false) == nullptr
					|| Candidate.FragmentShader.GetRHIShader(false) == nullptr
					|| Candidate.VertexDeclaration == nullptr
					|| Candidate.Atlas == nullptr
					|| Candidate.AtlasSampler == nullptr)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"OverlayIcon",
							"base",
							"RHI creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FBaseResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Base == nullptr)
			return;

		const uint32 VertexBytes =
			static_cast<uint32>(Vertices.size() * sizeof(FOverlayIconVertex));
		const uint32 IndexBytes =
			static_cast<uint32>(Indices.size() * sizeof(uint32));
		if (VertexBytes > State->VertexCapacity)
		{
			const uint32 Capacity = std::bit_ceil(VertexBytes);
			FRHIBufferCreateDesc Desc =
				FRHIBufferCreateDesc::CreateVertex(
					"OverlayIconVertexBuffer", Capacity);
			Desc.Usage |= EBufferUsageFlags::Dynamic;
			FBufferRHIRef Buffer =
				GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			if (Buffer == nullptr)
				return;
			State->VertexBuffer = std::move(Buffer);
			State->VertexCapacity = Capacity;
		}
		if (IndexBytes > State->IndexCapacity)
		{
			const uint32 Capacity = std::bit_ceil(IndexBytes);
			FRHIBufferCreateDesc Desc =
				FRHIBufferCreateDesc::CreateIndex(
					"OverlayIconIndexBuffer", Capacity, sizeof(uint32));
			Desc.Usage |= EBufferUsageFlags::Dynamic;
			FBufferRHIRef Buffer =
				GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			if (Buffer == nullptr)
				return;
			State->IndexBuffer = std::move(Buffer);
			State->IndexCapacity = Capacity;
		}
		if (State->VertexBuffer == nullptr || State->IndexBuffer == nullptr)
			return;
		CommandList.WriteBuffer(
			State->VertexBuffer, Vertices.data(), VertexBytes, 0);
		CommandList.WriteBuffer(
			State->IndexBuffer, Indices.data(), IndexBytes, 0);
		Prepared.OverlayIconIndexCount =
			static_cast<uint32>(Indices.size());

		for (const EDepthMode DepthMode : {
				EDepthMode::XRay,
				EDepthMode::Visible})
		{
			const FPipelineKey Key{
				.Feature = EFeature::OverlayIcon,
				.Output = Output,
				.DepthMode = DepthMode,
			};
			auto EntryIt = std::ranges::find(
				State->Pipelines, Key, &FState::FPipelineEntry::Key);
			if (EntryIt == State->Pipelines.end())
			{
				EntryIt = State->Pipelines.emplace(
					State->Pipelines.end(),
					FState::FPipelineEntry{.Key = Key});
			}
			FRenderResourceGeneration PipelineGeneration =
				Coordinator.GetGeneration_RenderThread();
			PipelineGeneration.Shader =
				State->Base.GetPayloadGeneration().Shader;
			const std::string PipelineName = std::format(
				"OverlayIcon{}{}Pipeline",
				GetDepthName(DepthMode),
				GetOutputName(Output));
			using FPipelineResult =
				TRenderResourceCreateResult<FGraphicsPipelineStateRHIRef>;
			auto* Pipeline = EntryIt->Slot.Resolve(
				PipelineGeneration,
				[Base, Key, PipelineName]() -> FPipelineResult {
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout =
						RenderTargetLayouts::MakeEditorAssistanceOutput(
							Key.Output);
					Initializer.BoundShaders.VertexShader =
						Base->VertexShader.GetRHIShader();
					Initializer.BoundShaders.FragmentShader =
						Base->FragmentShader.GetRHIShader();
					Initializer.VertexDeclaration =
						Base->VertexDeclaration;
					Initializer.ColorBlendStates[0] =
						FRHIColorBlendState::StraightAlpha();
					Initializer.RasterizerState.CullMode = ERHICullMode::None;
					Initializer.DepthStencilState.bEnableTest =
						Key.DepthMode == EDepthMode::Visible;
					Initializer.PipelineLayout =
						Base->ShaderMap->GetMergedPipelineLayout();
					FGraphicsPipelineStateRHIRef Candidate =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							FName(PipelineName), Initializer);
					if (Candidate == nullptr)
					{
						return FPipelineResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::
									GraphicsPipeline,
								"OverlayIcon",
								PipelineName,
								"RHI graphics pipeline creation returned null.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::
										Device
									| ERenderResourceGenerationDependency::
										Manual));
					}
					return FPipelineResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic);
			if (Pipeline != nullptr)
			{
				Prepared.Pipelines.push_back({
					.Key = Key,
					.Pipeline = *Pipeline,
				});
			}
		}
	}

	auto FOverlayIconRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPrepared& Prepared,
		EDepthMode DepthMode) -> void
	{
		check(IsInRenderingThread());
		const FState::FBasePayload* Base = State->Base.GetPayload();
		const FGraphicsPipelineStateRHIRef Pipeline =
			FindPreparedPipeline(Prepared, DepthMode);
		if (Prepared.OverlayIconIndexCount == 0
			|| Base == nullptr
			|| Pipeline == nullptr
			|| State->VertexBuffer == nullptr
			|| State->IndexBuffer == nullptr
			|| Base->Atlas == nullptr
			|| Base->AtlasSampler == nullptr)
		{
			return;
		}
		CommandList.SetGraphicsPipelineState(*Pipeline);
		CommandList.BindVertexBuffer(0, State->VertexBuffer, 0);
		CommandList.BindIndexBuffer(State->IndexBuffer, 0);
		const FOverlayIconStyleUniform Style{
			DepthMode == EDepthMode::XRay ? 0.3f : 1.0f};
		FOverlayIconFragmentShader::FParameters Parameters;
		Parameters.Atlas = Base->Atlas;
		Parameters.AtlasSampler = Base->AtlasSampler;
		Parameters.IconStyle = CommandList.AllocateDynamicUniformBuffer(
			&Style, sizeof(Style));
		SetShaderParameters(
			CommandList, Base->FragmentShader, Parameters);
		CommandList.DrawIndexed(Prepared.OverlayIconIndexCount, 0, 0);
	}

	auto FOverlayIconRenderer::ReleaseResources_RenderThread() -> void
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
	}
} // namespace Durin
