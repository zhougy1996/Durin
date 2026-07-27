#include "Widgets/TexturePreview.h"

#include "DynamicRHI.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "Texture/Texture2D.h"

namespace Durin
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
			uint32 Padding[3]{};
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

		struct FTexturePreviewRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FTexturePreviewVertexShader> VertexShader;
			TShaderRef<FTexturePreviewFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			FSamplerRHIRef Sampler;
			bool bCreateAttempted = false;
		};

		FTexturePreviewRendererState GTexturePreviewRendererState;

		auto MakeTexturePreviewRenderTargetLayout() -> FRHIRenderTargetLayout
		{
			FRHIRenderTargetLayout Layout;
			Layout.NumColorRenderTargets = 1;
			FRHIAttachmentLayout& ColorAttachment = Layout.ColorAttachments[0].RenderTarget;
			ColorAttachment.Format = EPixelFormat::RGBA8_UNORM;
			ColorAttachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
			ColorAttachment.StoreAction = ERHIRenderTargetStoreAction::Store;
			ColorAttachment.InitialLayout = ERHITextureLayout::Undefined;
			ColorAttachment.InitialAccess = ERHIAccess::None;
			ColorAttachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			ColorAttachment.FinalAccess = ERHIAccess::ShaderRead;
			return Layout;
		}

		auto EnsureTexturePreviewRendererResources(FRHICommandListImmediate& CommandList) -> bool
		{
			FTexturePreviewRendererState& State = GTexturePreviewRendererState;
			if (State.bCreateAttempted)
			{
				return State.PipelineState != nullptr
					&& State.VertexBuffer != nullptr
					&& State.IndexBuffer != nullptr
					&& State.Sampler != nullptr;
			}
			State.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FTexturePreviewVertexShader::StaticType();
			FShaderType& FragmentShaderType = FTexturePreviewFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			std::shared_ptr<FShaderMapBase> ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize Texture Editor preview shader map: {}", ErrorMessage);
				return false;
			}

			auto* VertexShader = static_cast<FTexturePreviewVertexShader*>(ShaderMap->GetShader(&VertexShaderType));
			auto* FragmentShader = static_cast<FTexturePreviewFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType));
			check(VertexShader);
			check(FragmentShader);
			State.ShaderMap = ShaderMap;
			State.VertexShader = TShaderRef<FTexturePreviewVertexShader>(VertexShader, ShaderMap.get());
			State.FragmentShader = TShaderRef<FTexturePreviewFragmentShader>(FragmentShader, ShaderMap.get());

			constexpr uint32 VertexStride = sizeof(FTexturePreviewVertex);
			FVertexDeclarationElementList VertexElements;
			VertexElements[0] = FVertexElement(
				0,
				offsetof(FTexturePreviewVertex, Position),
				EVertexElementType::Float2,
				0,
				VertexStride);
			VertexElements[1] = FVertexElement(
				0,
				offsetof(FTexturePreviewVertex, UV),
				EVertexElementType::Float2,
				1,
				VertexStride);
			State.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexElements);

			const std::array<FTexturePreviewVertex, 3> Vertices = {
				FTexturePreviewVertex{FVector2f{-1.0f, -1.0f}, FVector2f{0.0f, 0.0f}},
				FTexturePreviewVertex{FVector2f{3.0f, -1.0f}, FVector2f{2.0f, 0.0f}},
				FTexturePreviewVertex{FVector2f{-1.0f, 3.0f}, FVector2f{0.0f, 2.0f}},
			};
			const std::array<uint32, 3> Indices = {0, 1, 2};

			FRHIBufferCreateDesc VertexBufferDesc = FRHIBufferCreateDesc::CreateVertex(
				"TexturePreviewFullscreenVertexBuffer",
				static_cast<uint32>(sizeof(Vertices)));
			VertexBufferDesc.Usage |= EBufferUsageFlags::Static;
			VertexBufferDesc.InitialData = {Vertices.data(), static_cast<uint32>(sizeof(Vertices))};
			State.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, VertexBufferDesc);

			FRHIBufferCreateDesc IndexBufferDesc = FRHIBufferCreateDesc::CreateIndex(
				"TexturePreviewFullscreenIndexBuffer",
				static_cast<uint32>(sizeof(Indices)),
				sizeof(uint32));
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData = {Indices.data(), static_cast<uint32>(sizeof(Indices))};
			State.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, IndexBufferDesc);
			State.Sampler = GDynamicRHI->RHICreateSampler(FRHISamplerDesc::LinearClamp());

			FGraphicsPipelineStateInitializer PipelineInitializer;
			PipelineInitializer.RenderTargetLayout = MakeTexturePreviewRenderTargetLayout();
			PipelineInitializer.BoundShaders.VertexShader = State.VertexShader.GetRHIShader();
			PipelineInitializer.BoundShaders.FragmentShader = State.FragmentShader.GetRHIShader();
			PipelineInitializer.VertexDeclaration = State.VertexDeclaration;
			PipelineInitializer.bEnableAlphaBlend = false;
			PipelineInitializer.bEnableBackFaceCulling = false;
			PipelineInitializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			State.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
				"TexturePreviewChannelPipeline",
				PipelineInitializer);

			return State.PipelineState != nullptr
				&& State.VertexBuffer != nullptr
				&& State.IndexBuffer != nullptr
				&& State.Sampler != nullptr;
		}

		auto RenderTexturePreviewChannel(
			FRHICommandListImmediate& CommandList,
			FRHITexture* InputTexture,
			uint32 Width,
			uint32 Height,
			ETexturePreviewChannel Channel
		) -> FTextureRHIRef
		{
			if (!InputTexture
				|| Channel == ETexturePreviewChannel::RGBA
				|| !EnsureTexturePreviewRendererResources(CommandList))
			{
				return nullptr;
			}

			FRHITextureCreateDesc OutputDesc = FRHITextureCreateDesc::Create2D(
				"TexturePreviewChannelOutput",
				Width,
				Height,
				EPixelFormat::RGBA8_UNORM);
			OutputDesc.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
			OutputDesc.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
			FTextureRHIRef OutputTexture = GDynamicRHI->RHICreateTexture(CommandList, OutputDesc);
			if (!OutputTexture) return nullptr;

			const FRHIRenderTargetLayout RenderTargetLayout = MakeTexturePreviewRenderTargetLayout();
			FRHIRenderPassInfo PassInfo;
			PassInfo.RenderTargetLayout = RenderTargetLayout;
			PassInfo.ColorRenderTargets[0] = OutputTexture;
			PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);

			FTexturePreviewRendererState& State = GTexturePreviewRendererState;
			CommandList.BeginRenderPass(PassInfo, "TexturePreviewChannelPass");
			CommandList.SetGraphicsPipelineState(*State.PipelineState);
			CommandList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 1.0f);
			CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
			CommandList.BindVertexBuffer(0, State.VertexBuffer, 0);
			CommandList.BindIndexBuffer(State.IndexBuffer, 0);

			FTexturePreviewSettings Settings;
			Settings.Channel = static_cast<uint32>(Channel);
			FTexturePreviewFragmentShader::FParameters ShaderParameters;
			ShaderParameters.PreviewTexture = InputTexture;
			ShaderParameters.PreviewSampler = State.Sampler;
			ShaderParameters.PreviewSettings =
				CommandList.AllocateDynamicUniformBuffer(&Settings, sizeof(Settings));
			SetShaderParameters(CommandList, State.FragmentShader, ShaderParameters);
			CommandList.DrawIndexed(3, 0, 0);
			CommandList.EndRenderPass();
			return OutputTexture;
		}
	}

	FTexturePreview::~FTexturePreview()
	{
		Release();
	}

	auto FTexturePreview::UploadPixels(EPixelFormat Format, uint32 Width, uint32 Height, uint32 RowPitch, const uint8* Pixels) -> void
	{
		Release();

		if (!GDynamicRHI || !Pixels || Width == 0 || Height == 0) return;

		// Capture a snapshot of the pixel data for the render command.
		const FPixelFormatLayout Layout = GetPixelFormatLayout(Format, Width, Height);
		if (Layout.DataSize == 0 || Layout.DataSize > std::numeric_limits<size_t>::max()) return;
		const size_t PixelCount = static_cast<size_t>(Layout.DataSize);
		auto PixelSnapshot = std::make_shared<std::vector<uint8>>(Pixels, Pixels + PixelCount);

		FTextureRHIRef NewTexture;
		ENQUEUE_RENDER_COMMAND(UploadTexturePreview)([&NewTexture, Format, Width, Height, RowPitch, PixelSnapshot](FRHICommandListImmediate& CommandList) {
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("TexturePreview", Width, Height, Format);
			Desc.AddFlags(ETextureCreateFlags::ShaderResource);
			NewTexture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (NewTexture)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);
				GDynamicRHI->RHIUpdateTexture2D(CommandList, NewTexture, 0, 0, Region, RowPitch, PixelSnapshot->data());
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
		ETexturePreviewChannel Channel
	) -> void
	{
		if (!Platform.IsValid() || MipIndex >= Platform.Mips.size()) return;
		SelectedChannel = Channel;
		const FTexture2DMipData& Mip = Platform.Mips[MipIndex];
		UploadPixels(Platform.PixelFormat, Mip.Width, Mip.Height, Mip.RowPitch, Mip.Pixels.data());
	}

	auto FTexturePreview::UploadSource(
		const FTextureSourceData& Source,
		ETexturePreviewChannel Channel
	) -> void
	{
		if (!Source.IsValid()) return;
		SelectedChannel = Channel;
		// Source data is always RGBA8; preview it without color-space conversion.
		UploadPixels(EPixelFormat::RGBA8_UNORM, Source.Width, Source.Height, Source.Width * 4, Source.Pixels.data());
	}

	auto FTexturePreview::SetChannel(ETexturePreviewChannel Channel) -> void
	{
		if (SelectedChannel == Channel) return;
		SelectedChannel = Channel;
		RefreshDisplayTexture();
	}

	auto FTexturePreview::RefreshDisplayTexture() -> void
	{
		UnregisterDisplayTexture();
		FilteredTexture = nullptr;
		if (!UploadedTexture || !Mona::GActiveUIBackend) return;

		if (SelectedChannel == ETexturePreviewChannel::RGBA)
		{
			DisplayTexture = UploadedTexture;
		}
		else
		{
			FTextureRHIRef NewFilteredTexture;
			const FTextureRHIRef InputTexture = UploadedTexture;
			const uint32 Width = PreviewWidth;
			const uint32 Height = PreviewHeight;
			const ETexturePreviewChannel Channel = SelectedChannel;
			ENQUEUE_RENDER_COMMAND(RenderTexturePreviewChannel)(
				[&NewFilteredTexture, InputTexture, Width, Height, Channel](FRHICommandListImmediate& CommandList) {
					CommandList.SwitchPipeline(ERHIPipeline::Graphics);
					NewFilteredTexture = RenderTexturePreviewChannel(
						CommandList,
						InputTexture,
						Width,
						Height,
						Channel);
				});
			FlushRenderingCommands();
			FilteredTexture = std::move(NewFilteredTexture);
			DisplayTexture = FilteredTexture ? FilteredTexture : UploadedTexture;
		}

		Mona::GActiveUIBackend->RegisterTexture(DisplayTexture);
	}

	auto FTexturePreview::UnregisterDisplayTexture() -> void
	{
		if (DisplayTexture && Mona::GActiveUIBackend)
			Mona::GActiveUIBackend->UnregisterTexture(DisplayTexture);
		DisplayTexture = nullptr;
	}

	auto FTexturePreview::Release() -> void
	{
		UnregisterDisplayTexture();
		FilteredTexture = nullptr;
		UploadedTexture = nullptr;
		PreviewWidth = 0;
		PreviewHeight = 0;
	}

	auto FTexturePreview::ReleaseSharedResources() -> void
	{
		if (!GDynamicRHI)
		{
			GTexturePreviewRendererState = {};
			return;
		}

		ENQUEUE_RENDER_COMMAND(ReleaseTexturePreviewSharedResources)(
			[](FRHICommandListImmediate&) {
				GTexturePreviewRendererState = {};
			});
		FlushRenderingCommands();
	}
}
