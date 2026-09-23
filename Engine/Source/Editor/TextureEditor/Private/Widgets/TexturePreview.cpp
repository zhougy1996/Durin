#include "TexturePreview.h"

#include "DynamicRHI.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderResourceCreation.h"
#include "RenderingThread.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCookedLibrary.h"
#include "Shader/ShaderCompilerCore.h"
#include "Texture/Texture2D.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		struct FTexturePreviewVertex
		{
			FVector2f Position;
			FVector2f UV;
		};

		struct alignas(16) FTexturePreviewSettings
		{
			uint32 Channel = 0;
			uint32 DecodeNormal = 0;
			uint32 InputSRGB = 0;
			uint32 Padding = 0;
		};

		class FTexturePreviewVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FTexturePreviewVertexShader,
				FShader,
				"/Engine/TexturePreview",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FTexturePreviewFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTexturePreviewFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(PreviewTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(PreviewSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(PreviewSettings);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FTexturePreviewFragmentShader,
				FShader,
				"/Engine/TexturePreview",
				EShaderFrequency::Fragment,
				"FragmentMain");
		};

		const FShaderProgramRegistration GTexturePreviewShaderProgram(
			"TextureEditor", "TextureEditor.Preview",
			EShaderRequestEligibility::EditorOnly,
			{&FTexturePreviewVertexShader::StaticType(),
			 &FTexturePreviewFragmentShader::StaticType()});

		struct FTexturePreviewRendererState
		{
			struct FPayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FTexturePreviewVertexShader> VertexShader;
				TShaderRef<FTexturePreviewFragmentShader> FragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
				FGraphicsPipelineStateRHIRef PipelineState;
				FBufferRHIRef VertexBuffer;
				FBufferRHIRef IndexBuffer;
				FSamplerRHIRef Sampler;
			};

			FRenderResourceGeneration Generation;
			TRenderResourceCreationSlot<FPayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FTexturePreviewVertexShader> VertexShader;
			TShaderRef<FTexturePreviewFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			FSamplerRHIRef Sampler;
		};

		FTexturePreviewRendererState GTexturePreviewRendererState;

		auto MakeTexturePreviewRenderTargetLayout() -> FRHIRenderTargetLayout
		{
			FRHIRenderTargetLayout Layout;
			Layout.NumColorRenderTargets = 1;
			FRHIAttachmentLayout& ColorAttachment = Layout.ColorAttachments[0].RenderTarget;
			ColorAttachment.Format = EPixelFormat::SRGBA8_UNORM;
			ColorAttachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
			ColorAttachment.StoreAction = ERHIRenderTargetStoreAction::Store;
			ColorAttachment.InitialLayout = ERHITextureLayout::Undefined;
			ColorAttachment.InitialAccess = ERHIAccess::None;
			ColorAttachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			ColorAttachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
			return Layout;
		}

		auto EnsureTexturePreviewRendererResources(FRHICommandListImmediate& CommandList) -> bool
		{
			FTexturePreviewRendererState& State = GTexturePreviewRendererState;
			using FResult = TRenderResourceCreateResult<
				FTexturePreviewRendererState::FPayload>;
			auto* Payload = State.Slot.Resolve(
				State.Generation,
				[&CommandList]() -> FResult {
					FShaderCompileOptions CompileOptions;
					FShaderType& VertexShaderType =
						FTexturePreviewVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FTexturePreviewFragmentShader::StaticType();
					std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
										auto MakeError = [](auto Category, ERenderResourceCreateErrorReason Reason, FRenderResourceCreateCause Cause = {}) {
						return FRenderResourceCreateError{
							.Category = Category,
							.Reason = Reason,
							.Context = "TextureEditorPreview",
							.Identity = "channel-filter",
							.Cause = std::move(Cause),
							.RetryDependencies =
								ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual,
						};
					};
					if (auto Result = ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions); !Result)
						return FResult::Failure(MakeError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							ERenderResourceCreateErrorReason::ShaderFailure, std::move(Result.error())));
					auto* VertexShader =
						static_cast<FTexturePreviewVertexShader*>(
							ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader =
						static_cast<FTexturePreviewFragmentShader*>(
							ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
						return FResult::Failure(MakeError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							ERenderResourceCreateErrorReason::ShaderFailure, FShaderError{.Code = EShaderError::MissingShaderType}));
					FTexturePreviewRendererState::FPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader =
						TShaderRef<FTexturePreviewVertexShader>(
							VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FTexturePreviewFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					FRHIShader* VertexRHI =
						Candidate.VertexShader.GetRHIShader(false);
					FRHIShader* FragmentRHI =
						Candidate.FragmentShader.GetRHIShader(false);
					if (VertexRHI == nullptr || FragmentRHI == nullptr)
						return FResult::Failure(MakeError(
							ERenderResourceCreateErrorCategory::RHIResource,
							ERenderResourceCreateErrorReason::ShaderCreationFailed));
					constexpr uint32 VertexStride =
						sizeof(FTexturePreviewVertex);
					FVertexDeclarationElementList VertexElements;
					VertexElements[0] = FVertexElement(
						0, offsetof(FTexturePreviewVertex, Position),
						EVertexElementType::Float2, 0, VertexStride);
					VertexElements[1] = FVertexElement(
						0, offsetof(FTexturePreviewVertex, UV),
						EVertexElementType::Float2, 1, VertexStride);
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(
							VertexElements);
					const std::array<FTexturePreviewVertex, 3> Vertices = {
						FTexturePreviewVertex{
							FVector2f{-1.0f, -1.0f},
							FVector2f{0.0f, 0.0f}},
						FTexturePreviewVertex{
							FVector2f{3.0f, -1.0f},
							FVector2f{2.0f, 0.0f}},
						FTexturePreviewVertex{
							FVector2f{-1.0f, 3.0f},
							FVector2f{0.0f, 2.0f}},
					};
					const std::array<uint32, 3> Indices = {0, 1, 2};
					FRHIBufferCreateDesc VertexBufferDesc =
						FRHIBufferCreateDesc::CreateVertex(
							"TexturePreviewFullscreenVertexBuffer",
							static_cast<uint32>(sizeof(Vertices)));
					VertexBufferDesc.Usage |= EBufferUsageFlags::Static;
					VertexBufferDesc.InitialData = {
						Vertices.data(),
						static_cast<uint32>(sizeof(Vertices))};
					Candidate.VertexBuffer =
						GDynamicRHI->RHICreateBuffer(
							CommandList, VertexBufferDesc);
					FRHIBufferCreateDesc IndexBufferDesc =
						FRHIBufferCreateDesc::CreateIndex(
							"TexturePreviewFullscreenIndexBuffer",
							static_cast<uint32>(sizeof(Indices)),
							sizeof(uint32));
					IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
					IndexBufferDesc.InitialData = {
						Indices.data(),
						static_cast<uint32>(sizeof(Indices))};
					Candidate.IndexBuffer =
						GDynamicRHI->RHICreateBuffer(
							CommandList, IndexBufferDesc);
					Candidate.Sampler =
						GDynamicRHI->RHICreateSampler(
							FRHISamplerDesc::LinearClamp());
					FGraphicsPipelineStateInitializer PipelineInitializer;
					PipelineInitializer.RenderTargetLayout =
						MakeTexturePreviewRenderTargetLayout();
					PipelineInitializer.BoundShaders.VertexShader =
						VertexRHI;
					PipelineInitializer.BoundShaders.FragmentShader =
						FragmentRHI;
					PipelineInitializer.VertexDeclaration =
						Candidate.VertexDeclaration;
					PipelineInitializer.RasterizerState.CullMode =
						ERHICullMode::None;
					PipelineInitializer.PipelineLayout =
						Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							"TexturePreviewChannelPipeline",
							PipelineInitializer);
					if (Candidate.VertexDeclaration == nullptr
						|| Candidate.VertexBuffer == nullptr
						|| Candidate.IndexBuffer == nullptr
						|| Candidate.Sampler == nullptr)
						return FResult::Failure(MakeError(
							ERenderResourceCreateErrorCategory::RHIResource,
							ERenderResourceCreateErrorReason::ResourceCreationFailed));
					if (Candidate.PipelineState == nullptr)
						return FResult::Failure(MakeError(
							ERenderResourceCreateErrorCategory::GraphicsPipeline,
							ERenderResourceCreateErrorReason::PipelineCreationFailed));
					return FResult::Success(std::move(Candidate));
				},
				[](const FRenderResourceCreateDiagnostic& Diagnostic) {
					if (!Diagnostic.Error)
						return;
					if (Diagnostic.Kind
						== ERenderResourceCreateDiagnosticKind::Recovery)
					{
						DURIN_INFO(
							"Recovered Texture Editor preview resources.");
						return;
					}
					DURIN_ERROR(
						"Texture Editor preview resource creation failed: category={}, generation={}/{}/{}, retained={}, message={}",
						static_cast<uint8>(Diagnostic.Error->Category),
						Diagnostic.Error->AttemptedGeneration.Shader,
						Diagnostic.Error->AttemptedGeneration.Device,
						Diagnostic.Error->AttemptedGeneration.Manual,
						Diagnostic.Error->bRetainedFallback,
						FormatRenderResourceCreateError(*Diagnostic.Error));
				});
			if (Payload == nullptr)
				return false;
			State.ShaderMap = Payload->ShaderMap;
			State.VertexShader = Payload->VertexShader;
			State.FragmentShader = Payload->FragmentShader;
			State.VertexDeclaration = Payload->VertexDeclaration;
			State.PipelineState = Payload->PipelineState;
			State.VertexBuffer = Payload->VertexBuffer;
			State.IndexBuffer = Payload->IndexBuffer;
			State.Sampler = Payload->Sampler;
			return true;
		}

	}

	auto RenderTexturePreview(
		FRHICommandListImmediate& CommandList,
		FRHITexture* InputTexture,
		uint32 Width,
		uint32 Height,
		FTexturePreviewOptions Options
	) -> FTextureRHIRef
	{
		if (GTexturePreviewRendererState.Slot.GetFailure() != nullptr)
		{
			GTexturePreviewRendererState.Generation.Advance(
				ERenderResourceGenerationDependency::Manual);
		}
		if (!InputTexture
			|| Width == 0 || Height == 0
			|| !EnsureTexturePreviewRendererResources(CommandList))
		{
			return nullptr;
		}

		FRHITextureCreateDesc OutputDesc = FRHITextureCreateDesc::Create2D(
			"TexturePreviewChannelOutput",
			Width,
			Height,
			EPixelFormat::SRGBA8_UNORM);
		OutputDesc.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback);
		OutputDesc.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f));
		FTextureRHIRef OutputTexture = GDynamicRHI->RHICreateTexture(CommandList, OutputDesc);
		if (!OutputTexture) return nullptr;

		const FRHIRenderTargetLayout RenderTargetLayout = MakeTexturePreviewRenderTargetLayout();
		FRHIRenderPassInfo PassInfo;
		PassInfo.RenderTargetLayout = RenderTargetLayout;
		PassInfo.ColorRenderTargets[0] = OutputTexture;
		PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);

		FTexturePreviewRendererState& State = GTexturePreviewRendererState;
		CommandList.BeginRenderPass(PassInfo, "TexturePreviewChannelPass");
		CommandList.SetGraphicsPipelineState(*State.PipelineState);
		const float Scale = std::min(static_cast<float>(Width) / InputTexture->GetSizeX(),
			static_cast<float>(Height) / InputTexture->GetSizeY());
		const float DrawWidth = std::max(1.0f, std::floor(InputTexture->GetSizeX() * Scale));
		const float DrawHeight = std::max(1.0f, std::floor(InputTexture->GetSizeY() * Scale));
		const float X = std::floor((Width - DrawWidth) * 0.5f);
		const float Y = std::floor((Height - DrawHeight) * 0.5f);
		CommandList.SetViewport(X, Y, 0.0f, X + DrawWidth, Y + DrawHeight, 1.0f);
		CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
		CommandList.BindVertexBuffer(0, State.VertexBuffer, 0);
		CommandList.BindIndexBuffer(State.IndexBuffer, 0);

		FTexturePreviewSettings Settings;
		Settings.Channel = static_cast<uint32>(Options.Channel);
		Settings.DecodeNormal = Options.DecodesNormal();
		Settings.InputSRGB = GetPixelFormatInfo(InputTexture->GetFormat()).bIsSRGB;
		FTexturePreviewFragmentShader::FParameters ShaderParameters;
		ShaderParameters.PreviewTexture = InputTexture;
		ShaderParameters.PreviewSampler = State.Sampler;
		ShaderParameters.PreviewSettings =
			CommandList.CreateUniformBufferRange(&Settings, sizeof(Settings));
		SetShaderParameters(CommandList, State.FragmentShader, ShaderParameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return OutputTexture;
	}

	FTexturePreview::~FTexturePreview()
	{
		Release();
	}

	auto FTexturePreview::UploadPixels(EPixelFormat Format, uint32 Width, uint32 Height, uint32 RowPitch,
		FByteView Pixels) -> void
	{
		Release();

		if (!GDynamicRHI || Pixels.empty() || Width == 0 || Height == 0) return;

		// Capture a snapshot of the pixel data for the render command.
		const FPixelFormatLayout Layout = GetPixelFormatLayout(Format, Width, Height);
		if (Layout.DataSize == 0 || Layout.DataSize > Pixels.size()) return;
		const size_t PixelCount = static_cast<size_t>(Layout.DataSize);
		auto PixelSnapshot = std::make_shared<FByteBuffer>(
			Pixels.begin(), Pixels.begin() + static_cast<ptrdiff_t>(PixelCount));

		FTextureRHIRef NewTexture;
		ENQUEUE_RENDER_COMMAND(UploadTexturePreview)([&NewTexture, Format, Width, Height, RowPitch, PixelSnapshot](FRHICommandListImmediate& CommandList) {
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("TexturePreview", Width, Height, Format);
			Desc.AddFlags(ETextureCreateFlags::ShaderResource);
			NewTexture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (NewTexture)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);
				GDynamicRHI->RHIUpdateTexture2D(CommandList, NewTexture, 0, 0, Region, RowPitch, *PixelSnapshot);
			}
		});
		FlushRenderingCommands();

		if (!NewTexture) return;

		UploadedTexture = std::move(NewTexture);
		PreviewWidth = Width;
		PreviewHeight = Height;
		RefreshDisplayTexture();
	}

	auto FTexturePreview::Upload(
		const FTexturePlatformData& Platform,
		uint32 MipIndex,
		FTexturePreviewOptions Options
	) -> void
	{
		if (!Platform.IsValid() || MipIndex >= Platform.Mips.size()) return;
		DisplayOptions = Options;
		const FTexture2DMipData& Mip = Platform.Mips[MipIndex];
		UploadPixels(Platform.PixelFormat, Mip.Width, Mip.Height, Mip.RowPitch, Mip.Pixels);
	}

	auto FTexturePreview::UploadSource(
		Image::FImageView Source,
		FTexturePreviewOptions Options
	) -> void
	{
		if (!Source.IsValid() || Source.GetInfo().Format != Image::ERawImageFormat::RGBA8
			|| Source.GetInfo().Depth != 1 || Source.GetInfo().SliceCount != 1) return;
		DisplayOptions = Options;
		// Source data is always RGBA8; preview it without color-space conversion.
		UploadPixels(EPixelFormat::RGBA8_UNORM, Source.GetInfo().Width,
			Source.GetInfo().Height, Source.GetInfo().Width * 4, Source.GetPixels());
	}

	auto FTexturePreview::UploadRGBA8(uint32 Width, uint32 Height,
		FByteView Pixels, FTexturePreviewOptions Options) -> void
	{
		if (Width == 0 || Height == 0
			|| Pixels.size() != static_cast<uint64>(Width) * Height * 4) return;
		DisplayOptions = Options;
		UploadPixels(EPixelFormat::RGBA8_UNORM, Width, Height, Width * 4, Pixels);
	}

	auto FTexturePreview::SetOptions(FTexturePreviewOptions Options) -> void
	{
		if (DisplayOptions == Options) return;
		DisplayOptions = Options;
		RefreshDisplayTexture();
	}

	auto FTexturePreview::SetTexture(FTextureRHIRef Texture, uint32 Width, uint32 Height,
		FTexturePreviewOptions Options) -> void
	{
		if (UploadedTexture == Texture && PreviewWidth == Width && PreviewHeight == Height
			&& DisplayOptions == Options) return;
		Release();
		UploadedTexture = std::move(Texture);
		PreviewWidth = Width;
		PreviewHeight = Height;
		DisplayOptions = Options;
		RefreshDisplayTexture();
	}

	auto FTexturePreview::RefreshDisplayTexture() -> void
	{
		UnregisterDisplayTexture();
		if (!UploadedTexture || !Mona::GetActiveUIBackend()) return;
		FTextureRHIRef Output;
		const auto Input = UploadedTexture;
		const auto Options = DisplayOptions;
		const uint32 Width = PreviewWidth, Height = PreviewHeight;
		ENQUEUE_RENDER_COMMAND(RenderTexturePreview)(
			[&Output, Input, Width, Height, Options](FRHICommandListImmediate& Commands) {
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				Output = RenderTexturePreview(Commands, Input, Width, Height, Options);
			});
		FlushRenderingCommands();
		DisplayTexture = std::move(Output);
		if (DisplayTexture)
		{
			RegisteredBackend = Mona::GetActiveUIBackend();
			RegisteredBackend->RegisterTexture(DisplayTexture);
		}
	}

	auto FTexturePreview::UnregisterDisplayTexture() -> void
	{
		if (DisplayTexture && RegisteredBackend && RegisteredBackend == Mona::GetActiveUIBackend())
			RegisteredBackend->UnregisterTexture(DisplayTexture);
		RegisteredBackend = nullptr;
		DisplayTexture = nullptr;
	}

	auto FTexturePreview::Release() -> void
	{
		UnregisterDisplayTexture();
		UploadedTexture = nullptr;
		PreviewWidth = 0;
		PreviewHeight = 0;
	}

	auto FTexturePreview::ReleaseSharedResources() -> void
	{
		if (!GDynamicRHI)
		{
			GTexturePreviewRendererState.Slot.Reset();
			GTexturePreviewRendererState = {};
			return;
		}

		ENQUEUE_RENDER_COMMAND(ReleaseTexturePreviewSharedResources)(
			[](FRHICommandListImmediate&) {
				GTexturePreviewRendererState.Slot.Reset();
				GTexturePreviewRendererState = {};
			});
		FlushRenderingCommands();
	}
}
