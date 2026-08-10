#include "ImGuiRHIImpl.h"

#include "ThirdParty/ImGui/imgui_threaded_rendering.h"

#include "Application/MonaApplication.h"
#include "CoreGlobals.h"
#include "ImGuiMonaImpl.h"
#include "RHI.h"
#include "Rendering/MonaRenderer.h"
#include "RenderingThread.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "Widgets/MWindow.h"

namespace Durin::MonaImGui
{
	static auto MakeImGuiRenderTargetLayout() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::SRGBA8_UNORM;
		Layout.ColorAttachments[0].RenderTarget.FinalLayout = ERHITextureLayout::Present;
		Layout.ColorAttachments[0].RenderTarget.FinalAccess = ERHIAccess::Present;
		return Layout;
	}

	// Declares the vertex shader used for ImGui draw lists.
	class FImGuiVertexShader : public FShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FImGuiVertexShader)
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Projection);
		DURIN_END_SHADER_PARAMETERS();

		DURIN_DECLARE_SHADER(FImGuiVertexShader, FShader, "/Engine/ImGui", EShaderFrequency::Vertex, "VertexMain");
	};

	// Declares the fragment shader used for ImGui draw lists.
	class FImGuiFragmentShader : public FShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FImGuiFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(FontTexture);
			DURIN_SHADER_PARAMETER_SAMPLER(FontSampler);
		DURIN_END_SHADER_PARAMETERS();

		DURIN_DECLARE_SHADER(FImGuiFragmentShader, FShader, "/Engine/ImGui", EShaderFrequency::Fragment, "FragmentMain");
	};

	// Groups render-thread backend state to preserve deterministic static initialization.
	struct FImGuiRHIImplRT_BackendState
	{
		std::shared_ptr<FShaderMapBase> ShaderMap;
		TShaderRef<FImGuiVertexShader> VertexShader;
		TShaderRef<FImGuiFragmentShader> FragmentShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;

		FSamplerRHIRef LinearSampler;
	};

	static FImGuiRHIImplRT_BackendState GBackendState;

	// Carries the orthographic transform consumed by the ImGui vertex shader.
	struct FImGuiRHIImpl_ConstantBufferData
	{
		FVector2f Scale;
		FVector2f Translation;
	};

	// Retains an RHI texture while it is registered with ImGui.
	struct FImGuiRHIImpl_Texture
	{
		FTextureRHIRef Texture;
	};

	// Owns render buffers and alternating draw snapshots for an ImGui viewport.
	struct FImGuiRHIImpl_RendererViewportData
	{
		FImGuiRHIImpl_WindowRenderBuffers RenderBuffers;
		std::array<ImDrawDataSnapshot, 2> DrawDataSnapshots;
		FViewportRHIRef ViewportRHI;
		uint32 NextSnapshotIndex = 0;
	};

	auto FImGuiRHIImpl_WindowRenderBuffers::Clear() -> void
	{
		for (FImGuiRHIImpl_FrameRenderBuffers& Buffers : FrameRenderBuffers)
		{
			Buffers.VertexBuffer = nullptr;
			Buffers.IndexBuffer = nullptr;
		}
	}

	static std::unordered_map<const FRHITexture*, FImGuiRHIImpl_Texture> GRegisteredTextures;
	static std::vector<FImGuiRHIImpl_Texture> GTexturesPendingRetirement;

	static auto UnregisterTextureImpl(FRHITexture* InTexture) -> void
	{
		const auto It = GRegisteredTextures.find(InTexture);
		if (It == GRegisteredTextures.end())
		{
			return;
		}

		GTexturesPendingRetirement.push_back(std::move(It->second));
		GRegisteredTextures.erase(It);
	}

	static auto GetRendererViewportData(ImGuiViewport* Viewport) -> FImGuiRHIImpl_RendererViewportData*
	{
		return Viewport != nullptr ? static_cast<FImGuiRHIImpl_RendererViewportData*>(Viewport->RendererUserData) : nullptr;
	}

	static auto CreateRendererViewportData(ImGuiViewport* Viewport) -> FImGuiRHIImpl_RendererViewportData*
	{
		if (Viewport == nullptr)
		{
			return nullptr;
		}

		auto* ViewportData = GetRendererViewportData(Viewport);
		if (ViewportData == nullptr)
		{
			ViewportData = new FImGuiRHIImpl_RendererViewportData();
			Viewport->RendererUserData = ViewportData;
		}

		return ViewportData;
	}

	static auto DestroyRendererViewportData(ImGuiViewport* Viewport) -> void
	{
		if (Viewport == nullptr)
		{
			return;
		}

		if (auto* ViewportData = GetRendererViewportData(Viewport))
		{
			ViewportData->RenderBuffers.Clear();
			ViewportData->ViewportRHI = nullptr;
			delete ViewportData;
		}
		Viewport->RendererUserData = nullptr;
	}

	static auto SnapshotViewportDrawData(ImGuiViewport* Viewport, ImDrawData* DrawData) -> ImDrawData*
	{
		auto* ViewportData = GetRendererViewportData(Viewport);
		check(ViewportData != nullptr);

		const uint32 SnapshotIndex = ViewportData->NextSnapshotIndex;
		ImDrawDataSnapshot& Snapshot = ViewportData->DrawDataSnapshots[SnapshotIndex];
		Snapshot.SnapUsingSwap(DrawData, FTime::Seconds());
		ViewportData->NextSnapshotIndex = (SnapshotIndex + 1) % ViewportData->DrawDataSnapshots.size();
		return &Snapshot.DrawData;
	}

	static auto RenderViewport(ImGuiViewport* Viewport, ImDrawData* DrawData, FImGuiRHIImpl_RendererViewportData& ViewportData, bool bPresent) -> void
	{
		if (DrawData == nullptr)
		{
			ViewportData.ViewportRHI = nullptr;
			return;
		}

		auto& App = Mona::FMonaApplication::Get();
		Mona::FMonaRenderer* Renderer = App.GetRenderer();
		const std::shared_ptr<MWindow> Window = ImGuiMonaImpl_GetViewportWindow(Viewport);
		if (Renderer == nullptr || Window == nullptr || Window->IsMinimized())
		{
			ViewportData.ViewportRHI = nullptr;
			return;
		}

		ViewportData.ViewportRHI = Renderer->GetRHIViewport(*Window);
		if (ViewportData.ViewportRHI == nullptr)
		{
			return;
		}

		ImDrawData* SnapshotDrawData = SnapshotViewportDrawData(Viewport, DrawData);
		if (SnapshotDrawData == nullptr)
		{
			return;
		}

		ImGuiRHIImpl_RenderDrawData(ViewportData.ViewportRHI, SnapshotDrawData, &ViewportData.RenderBuffers, bPresent);
	}

	static auto ResizeViewportToMatchWindow(ImGuiViewport* Viewport, ImVec2 Size) -> void
	{
		auto& App = Mona::FMonaApplication::Get();
		Mona::FMonaRenderer* Renderer = App.GetRenderer();
		const std::shared_ptr<MWindow> Window = ImGuiMonaImpl_GetViewportWindow(Viewport);
		if (Renderer == nullptr || Window == nullptr)
		{
			return;
		}

		uint32 Width;
		uint32 Height;
		if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
		{
			const FIntPoint ViewportSize = NativeWindow->GetViewportSize();
			Width = static_cast<uint32>(FMath::Max(8, ViewportSize.x));
			Height = static_cast<uint32>(FMath::Max(8, ViewportSize.y));
		}
		else
		{
			Width = static_cast<uint32>(FMath::Max(8.0f, Size.x));
			Height = static_cast<uint32>(FMath::Max(8.0f, Size.y));
		}
		Renderer->RequestResize(Window, Width, Height);
	}

	static auto ImGuiRHIImpl_RendererCreateWindow(ImGuiViewport* Viewport) -> void
	{
		CreateRendererViewportData(Viewport);

		auto& App = Mona::FMonaApplication::Get();
		Mona::FMonaRenderer* Renderer = App.GetRenderer();
		const std::shared_ptr<MWindow> Window = ImGuiMonaImpl_GetViewportWindow(Viewport);
		if (Renderer != nullptr && Window != nullptr)
		{
			Renderer->CreateViewport(Window);
		}
	}

	static auto ImGuiRHIImpl_RendererDestroyWindow(ImGuiViewport* Viewport) -> void
	{
		DestroyRendererViewportData(Viewport);
	}

	static auto ImGuiRHIImpl_RendererSetWindowSize(ImGuiViewport* Viewport, ImVec2 Size) -> void
	{
		ResizeViewportToMatchWindow(Viewport, Size);
	}

	static auto ImGuiRHIImpl_RendererRenderWindow(ImGuiViewport* Viewport, void* RenderArg) -> void
	{
		auto* ViewportData = GetRendererViewportData(Viewport);
		check(ViewportData != nullptr);
		RenderViewport(Viewport, Viewport->DrawData, *ViewportData, false);
	}

	static auto ImGuiRHIImpl_RendererSwapBuffers(ImGuiViewport* Viewport, void* RenderArg) -> void
	{
		auto* ViewportData = GetRendererViewportData(Viewport);
		check(ViewportData != nullptr);
		if (ViewportData->ViewportRHI != nullptr)
		{
			ImGuiRHIImpl_PresentViewport(ViewportData->ViewportRHI);
		}
	}

	static auto ImGuiRHIImpl_CreateFontAtlasTexture() -> void
	{
		ENQUEUE_RENDER_COMMAND(CreateImGuiFontAtlas)([](FRHICommandListImmediate& CommandList) {
			FRHISamplerDesc SamplerCreateDesc = FRHISamplerDesc::LinearClamp();
			GBackendState.LinearSampler = RHICreateSampler(SamplerCreateDesc);
		});
	}

	static auto ImGuiRHIImpl_CreateMainPipeline()
	{
		FShaderCompileOptions CompileOptions;
		FShaderType& VertexShaderType = FImGuiVertexShader::StaticType();
		FShaderType& FragmentShaderType = FImGuiFragmentShader::StaticType();
		std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
		std::shared_ptr<FShaderMapBase> ShaderMap = std::make_shared<FShaderMapBase>();
		std::string ErrorMessage;
		if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
		{
			DURIN_ERROR("Failed to initialize ImGui shader map: {}", ErrorMessage);
			return;
		}

		auto* VertexShader = static_cast<FImGuiVertexShader*>(ShaderMap->GetShader(&VertexShaderType));
		auto* FragmentShader = static_cast<FImGuiFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType));
		check(VertexShader);
		check(FragmentShader);

		GBackendState.ShaderMap = ShaderMap;
		GBackendState.VertexShader = TShaderRef<FImGuiVertexShader>(VertexShader, ShaderMap.get());
		GBackendState.FragmentShader = TShaderRef<FImGuiFragmentShader>(FragmentShader, ShaderMap.get());

		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([ShaderMap,
														 VertexShaderRef = GBackendState.VertexShader,
														 FragmentShaderRef = GBackendState.FragmentShader](FRHICommandListImmediate& CommandList) {
			FVertexDeclarationElementList VertexDeclElements;
			constexpr uint32 VertexStride = sizeof(ImDrawVert);
			VertexDeclElements[0] = FVertexElement(0, offsetof(ImDrawVert, pos), EVertexElementType::Float2, 0, VertexStride);
			VertexDeclElements[1] = FVertexElement(0, offsetof(ImDrawVert, uv), EVertexElementType::Float2, 1, VertexStride);
			VertexDeclElements[2] = FVertexElement(0, offsetof(ImDrawVert, col), EVertexElementType::UByte4N, 2, VertexStride);
			GBackendState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = MakeImGuiRenderTargetLayout();
			Initializer.BoundShaders.VertexShader = VertexShaderRef.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = FragmentShaderRef.GetRHIShader();
			Initializer.VertexDeclaration = GBackendState.VertexDeclaration;

			Initializer.ColorBlendStates[0] =
				FRHIColorBlendState::StraightAlpha();
			Initializer.RasterizerState.CullMode = ERHICullMode::None;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			GBackendState.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("ImGuiMainPipeline", Initializer);
		});
	}

	static auto ImGuiRHIImpl_CreateRHIResources()
	{
		ENQUEUE_RENDER_COMMAND(SwitchPipeline)([](FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		});
		ImGuiRHIImpl_CreateMainPipeline();
		ImGuiRHIImpl_CreateFontAtlasTexture();
	}

	static auto ImGuiRHIImpl_DestroyTexture(ImTextureData* InTex) -> void
	{
		if (InTex->BackendUserData)
		{
			const auto* BackendTexture = static_cast<FImGuiRHIImpl_Texture*>(InTex->BackendUserData);
			delete BackendTexture;
			InTex->BackendUserData = nullptr;
			InTex->SetTexID(ImTextureID_Invalid);
			InTex->SetStatus(ImTextureStatus_Destroyed);
		}
	}

	static auto ImGuiRHIImpl_DestroyRHIResources() -> void
	{
		ImGui::GetIO().Fonts->SetTexID(ImTextureID_Invalid);

		for (ImTextureData* Tex : ImGui::GetPlatformIO().Textures)
		{
			ImGuiRHIImpl_DestroyTexture(Tex);
		}

		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([](FRHICommandListImmediate& CommandList) {
			GBackendState.ShaderMap.reset();
			GBackendState.VertexShader = {};
			GBackendState.FragmentShader = {};
			GBackendState.VertexDeclaration = nullptr;
			GBackendState.PipelineState = nullptr;
			GBackendState.LinearSampler = nullptr;
		});
	}

	auto ImGuiRHIImpl_Init() -> void
	{
		check(GDynamicRHI);
		ImGuiIO& IO = ImGui::GetIO();
		IMGUI_CHECKVERSION();

		IO.BackendRendererUserData = GDynamicRHI;
		IO.BackendRendererName = "DurinRHI";
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // We can honor ImGuiPlatformIO::Textures[] requests during render.
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.Renderer_CreateWindow = ImGuiRHIImpl_RendererCreateWindow;
		PlatformIO.Renderer_DestroyWindow = ImGuiRHIImpl_RendererDestroyWindow;
		PlatformIO.Renderer_SetWindowSize = ImGuiRHIImpl_RendererSetWindowSize;
		PlatformIO.Renderer_RenderWindow = ImGuiRHIImpl_RendererRenderWindow;
		PlatformIO.Renderer_SwapBuffers = ImGuiRHIImpl_RendererSwapBuffers;

		ImGuiRHIImpl_CreateRHIResources();
	}

	auto ImGuiRHIImpl_Shutdown() -> void
	{
		if (ImGuiViewport* MainViewport = ImGui::GetMainViewport())
		{
			DestroyRendererViewportData(MainViewport);
		}

		ImGuiRHIImpl_DestroyRHIResources();

		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendRendererUserData = nullptr;
		IO.BackendRendererName = nullptr;
		IO.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);

		ImGui::GetPlatformIO().ClearRendererHandlers();

		GRegisteredTextures.clear();
		GTexturesPendingRetirement.clear();
		FlushRenderingCommands();
	}

	auto ImGuiRHIImpl_RetireUnregisteredTextures() -> void
	{
		if (GTexturesPendingRetirement.empty()) return;

		// Draw snapshots store texture IDs as raw pointers. Transfer their final
		// registration references past every main and platform viewport draw
		// queued for this frame instead of guessing from a fixed frame delay.
		std::vector<FImGuiRHIImpl_Texture> RetiredTextures;
		RetiredTextures.swap(GTexturesPendingRetirement);
		ENQUEUE_RENDER_COMMAND(RetireImGuiTextures)(
			[RetiredTextures = std::move(RetiredTextures)](
				FRHICommandListImmediate&) mutable {
				RetiredTextures.clear();
			});
	}

	auto ImGuiRHIImpl_RegisterTexture(const FTextureRHIRef& Texture) -> void
	{
		if (Texture == nullptr)
		{
			return;
		}

		GRegisteredTextures[Texture.GetReference()].Texture = Texture;
	}

	auto ImGuiRHIImpl_UnregisterTexture(FRHITexture* InRHITexture) -> void
	{
		UnregisterTextureImpl(InRHITexture);
	}

	auto ImGuiRHIImpl_GetTextureID(const FRHITexture* InRHITexture) -> ImTextureID
	{
		return GRegisteredTextures.contains(InRHITexture) ? reinterpret_cast<ImTextureID>(InRHITexture) : ImTextureID_Invalid;
	}

	auto ImGuiRHIImpl_EnsureMainViewportData(ImGuiViewport* Viewport) -> void
	{
		CreateRendererViewportData(Viewport);
	}

	auto ImGuiRHIImpl_RenderMainViewport(ImGuiViewport* Viewport) -> void
	{
		if (Viewport == nullptr)
		{
			return;
		}

		auto* ViewportData = GetRendererViewportData(Viewport);
		check(ViewportData != nullptr);
		// The main viewport is skipped by UpdatePlatformWindows() (index 0),
		// so its swapchain must be resized here.
		ResizeViewportToMatchWindow(Viewport, Viewport->Size);
		RenderViewport(Viewport, Viewport->DrawData, *ViewportData, true);
	}

	static auto ImGuiRHIImplRT_UpdateBuffers(FRHICommandListImmediate& CommandList, const ImDrawData* DrawData, FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers) -> void
	{
		check(IsInRenderingThread());

		if (DrawData->TotalVtxCount <= 0) return;

		uint32 RequiredVertexBufferSize = DrawData->TotalVtxCount * sizeof(ImDrawVert);
		uint32 RequiredIndexBufferSize = DrawData->TotalIdxCount * sizeof(ImDrawIdx);

		if (RenderBuffers.VertexBuffer == nullptr || RenderBuffers.VertexBuffer->GetSize() < RequiredVertexBufferSize)
		{
			FRHIBufferCreateDesc VertexBufferCreateDesc = FRHIBufferCreateDesc::CreateVertex("ImGuiVertexBuffer", RequiredVertexBufferSize);
			VertexBufferCreateDesc.Usage |= EBufferUsageFlags::Dynamic;
			RenderBuffers.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, VertexBufferCreateDesc);
		}
		if (RenderBuffers.IndexBuffer == nullptr || RenderBuffers.IndexBuffer->GetSize() < RequiredIndexBufferSize)
		{
			FRHIBufferCreateDesc IndexBufferCreateDesc = FRHIBufferCreateDesc::CreateIndex("ImGuiIndexBuffer", RequiredIndexBufferSize, sizeof(ImDrawIdx));
			IndexBufferCreateDesc.Usage |= EBufferUsageFlags::Dynamic;
			RenderBuffers.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, IndexBufferCreateDesc);
		}

		void* VertexBufferData = GDynamicRHI->RHILockBuffer(CommandList, RenderBuffers.VertexBuffer, 0, RequiredVertexBufferSize, EResourceLockMode::WriteOnly);
		void* IndexBufferData = GDynamicRHI->RHILockBuffer(CommandList, RenderBuffers.IndexBuffer, 0, RequiredIndexBufferSize, EResourceLockMode::WriteOnly);

		auto VertexBufferDst = static_cast<ImDrawVert*>(VertexBufferData);
		auto IndexBufferDst = static_cast<ImDrawIdx*>(IndexBufferData);

		for (const ImDrawList* DrawList : DrawData->CmdLists)
		{
			memcpy(VertexBufferDst, DrawList->VtxBuffer.Data, DrawList->VtxBuffer.Size * sizeof(ImDrawVert));
			memcpy(IndexBufferDst, DrawList->IdxBuffer.Data, DrawList->IdxBuffer.Size * sizeof(ImDrawIdx));
			VertexBufferDst += DrawList->VtxBuffer.Size;
			IndexBufferDst += DrawList->IdxBuffer.Size;
		}

		GDynamicRHI->RHIUnlockBuffer(CommandList, RenderBuffers.VertexBuffer);
		GDynamicRHI->RHIUnlockBuffer(CommandList, RenderBuffers.IndexBuffer);
	}

	static auto ImGuiRHIImpl_UpdateTexture(ImTextureData* InTex) -> void
	{
		if (InTex->Status == ImTextureStatus_WantCreate)
		{
			check(InTex->TexID == ImTextureID_Invalid && InTex->BackendUserData == nullptr);
			check(InTex->Format == ImTextureFormat_RGBA32 || InTex->Format == ImTextureFormat_Alpha8);

			auto* BackendTexture = new FImGuiRHIImpl_Texture();
			const EPixelFormat TextureFormat = (InTex->Format == ImTextureFormat_Alpha8) ? EPixelFormat::R8_UNORM : EPixelFormat::RGBA8_UNORM;
			ENQUEUE_RENDER_COMMAND(ImGuiImpl_CreateTexture)([BackendTexture, Width = InTex->Width, Height = InTex->Height, TextureFormat](FRHICommandListImmediate& CommandList) {
				FRHITextureCreateDesc TextureCreateDesc = FRHITextureCreateDesc::Create2D("ImGuiCreatedTexture", Width, Height, TextureFormat);
				TextureCreateDesc.Flags = ETextureCreateFlags::ShaderResource |
					ETextureCreateFlags::DestinationCopy;
				BackendTexture->Texture = RHICreateTexture(TextureCreateDesc);
			});
			InTex->BackendUserData = BackendTexture;
		}

		if (InTex->Status == ImTextureStatus_WantCreate)
		{
			auto* BackendTexture = static_cast<FImGuiRHIImpl_Texture*>(InTex->BackendUserData);
			FUpdateTextureRegion2D UpdateRegion(0, 0, 0, 0, InTex->Width, InTex->Height);
			const uint32 SourcePitch = static_cast<uint32>(InTex->GetPitch());
			const uint8* TexPixels = InTex->Pixels;

			ENQUEUE_RENDER_COMMAND(ImGuiImpl_UpdateTexture)([=](FRHICommandListImmediate& CommandList) {
				GDynamicRHI->RHIUpdateTexture2D(CommandList, BackendTexture->Texture, 0, 0, UpdateRegion, SourcePitch, TexPixels);
			});

			FlushRenderingCommands(); // Make sure the texture update is processed before the texture is used for rendering in the next frame.
			InTex->SetTexID(reinterpret_cast<ImTextureID>(BackendTexture->Texture.GetReference()));
			InTex->SetStatus(ImTextureStatus_OK);
		}

		if (InTex->Status == ImTextureStatus_WantUpdates)
		{
			auto* BackendTexture = static_cast<FImGuiRHIImpl_Texture*>(InTex->BackendUserData);
			const uint32 SourcePitch = static_cast<uint32>(InTex->GetPitch());
			for (const ImTextureRect& UpdateRect : InTex->Updates)
			{
				if (UpdateRect.w <= 0 || UpdateRect.h <= 0)
				{
					continue;
				}

				FUpdateTextureRegion2D UpdateRegion(UpdateRect.x, UpdateRect.y, UpdateRect.x, UpdateRect.y, UpdateRect.w, UpdateRect.h);
				const uint8* UpdatePixels = InTex->Pixels;
				ENQUEUE_RENDER_COMMAND(ImGuiImpl_UpdateTextureRegion)([=](FRHICommandListImmediate& CommandList) {
					GDynamicRHI->RHIUpdateTexture2D(CommandList, BackendTexture->Texture, 0, 0, UpdateRegion, SourcePitch, UpdatePixels);
				});
			}

			FlushRenderingCommands(); // Ensure atlas updates are visible before rendering new glyphs.
			InTex->SetStatus(ImTextureStatus_OK);
		}

		if (InTex->Status == ImTextureStatus_WantDestroy)
		{
			// Draw-data snapshots may still be queued on the render thread. Keep the
			// indirection valid until every in-flight frame that can reference it has completed.
			if (InTex->UnusedFrames >= kFrameInFlight)
			{
				ImGuiRHIImpl_DestroyTexture(InTex);
			}
		}
	}

	static auto ImGuiRHIImplRT_CreateProjectionUniform(FRHICommandListImmediate& CommandList, const ImDrawData* DrawData) -> FRHIUniformBufferRange
	{
		FImGuiRHIImpl_ConstantBufferData ProjectionData;
		ProjectionData.Scale.x = 2.0f / DrawData->DisplaySize.x;
		ProjectionData.Scale.y = 2.0f / DrawData->DisplaySize.y;
		ProjectionData.Translation.x = -1.0f - DrawData->DisplayPos.x * ProjectionData.Scale.x;
		ProjectionData.Translation.y = -1.0f - DrawData->DisplayPos.y * ProjectionData.Scale.y;
		return CommandList.AllocateDynamicUniformBuffer(&ProjectionData, sizeof(ProjectionData));
	}

	static auto ImGuiRHIImplRT_SetupRenderState(
		FRHICommandListImmediate& CommandList,
		const ImDrawData* DrawData,
		FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers,
		FRHIUniformBufferRange ProjectionUniform
	) -> void
	{
		const int32 FrameBufferWidth = static_cast<int32>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		const int32 FrameBufferHeight = static_cast<int32>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);

		CommandList.SetGraphicsPipelineState(*GBackendState.PipelineState);

		FImGuiVertexShader::FParameters VertexShaderParameters;
		VertexShaderParameters.Projection = ProjectionUniform;
		SetShaderParameters(CommandList, GBackendState.VertexShader, VertexShaderParameters);

		CommandList.SetViewport(0.0f, 0.0f, 0.0f, FrameBufferWidth, FrameBufferHeight, 1.0f);
		CommandList.BindVertexBuffer(0, RenderBuffers.VertexBuffer, 0);
		CommandList.BindIndexBuffer(RenderBuffers.IndexBuffer, 0);

		// Reset to a full-frame scissor so the next draw command starts from known state.
		CommandList.SetScissor(0.0f, 0.0f, FrameBufferWidth, FrameBufferHeight);
	}

	static auto ImGuiRHIImplRT_RenderDrawData(
		FRHICommandListImmediate& CommandList,
		FRHITexture* InTargetFrameBuffer,
		const ImDrawData* DrawData,
		FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers,
		const FClearValueBinding& ClearValue
	) -> void
	{
		check(IsInRenderingThread());
		const ImVec2 ClipOff = DrawData->DisplayPos;
		const ImVec2 ClipScale = DrawData->FramebufferScale;

		int FrameBufferWidth = static_cast<int>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		int FrameBufferHeight = static_cast<int>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);
		if (FrameBufferWidth <= 0 || FrameBufferHeight <= 0)
			return;

		// Update vertex/index buffers
		ImGuiRHIImplRT_UpdateBuffers(CommandList, DrawData, RenderBuffers);

		// Render pass
		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout = MakeImGuiRenderTargetLayout();
		PassInfo.ColorRenderTargets[0] = InTargetFrameBuffer;
		PassInfo.ColorClearValues[0] = ClearValue;
		const FRHIUniformBufferRange ProjectionUniform = ImGuiRHIImplRT_CreateProjectionUniform(CommandList, DrawData);

		CommandList.BeginRenderPass(PassInfo, "ImGuiRenderPass");
		ImGuiRHIImplRT_SetupRenderState(CommandList, DrawData, RenderBuffers, ProjectionUniform);

		int GlobalVertexOffset = 0;
		int GlobalIndexOffset = 0;

		for (const ImDrawList* DrawList : DrawData->CmdLists)
		{
			for (int CmdIndex = 0; CmdIndex < DrawList->CmdBuffer.Size; CmdIndex++)
			{
				const ImDrawCmd* Cmd = &DrawList->CmdBuffer[CmdIndex];
				if (Cmd->UserCallback != nullptr)
				{
					// User callback, registered via ImDrawList::AddCallback()
					// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
					if (Cmd->UserCallback == ImDrawCallback_ResetRenderState)
					{
						ImGuiRHIImplRT_SetupRenderState(CommandList, DrawData, RenderBuffers, ProjectionUniform);
					}
					else
					{
						Cmd->UserCallback(DrawList, Cmd);
					}
				}
				else
				{
					ImGuiRHIImplRT_SetupRenderState(CommandList, DrawData, RenderBuffers, ProjectionUniform);

					const ImVec2 ClipMin((Cmd->ClipRect.x - ClipOff.x) * ClipScale.x, (Cmd->ClipRect.y - ClipOff.y) * ClipScale.y);
					const ImVec2 ClipMax((Cmd->ClipRect.z - ClipOff.x) * ClipScale.x, (Cmd->ClipRect.w - ClipOff.y) * ClipScale.y);

					const float ScissorMinX = std::max(0.0f, ClipMin.x);
					const float ScissorMinY = std::max(0.0f, ClipMin.y);
					const float ScissorMaxX = std::min(static_cast<float>(FrameBufferWidth), ClipMax.x);
					const float ScissorMaxY = std::min(static_cast<float>(FrameBufferHeight), ClipMax.y);
					if (ScissorMaxX <= ScissorMinX || ScissorMaxY <= ScissorMinY)
					{
						continue;
					}
					CommandList.SetScissor(ScissorMinX, ScissorMinY, ScissorMaxX - ScissorMinX, ScissorMaxY - ScissorMinY);

					FImGuiFragmentShader::FParameters ShaderParameters;
					ShaderParameters.FontTexture = reinterpret_cast<FRHITexture*>(Cmd->GetTexID());
					ShaderParameters.FontSampler = GBackendState.LinearSampler;
					SetShaderParameters(CommandList, GBackendState.FragmentShader, ShaderParameters);

					CommandList.DrawIndexed(Cmd->ElemCount, Cmd->IdxOffset + GlobalIndexOffset, Cmd->VtxOffset + GlobalVertexOffset);
				}
			}
			GlobalIndexOffset += DrawList->IdxBuffer.Size;
			GlobalVertexOffset += DrawList->VtxBuffer.Size;
		}

		CommandList.EndRenderPass();
	}

	static auto ImGuiRHIImplRT_ClearViewport(
		FRHICommandListImmediate& CommandList,
		FRHITexture* InTargetFrameBuffer,
		const FClearValueBinding& ClearValue
	) -> void
	{
		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout = MakeImGuiRenderTargetLayout();
		PassInfo.ColorRenderTargets[0] = InTargetFrameBuffer;
		PassInfo.ColorClearValues[0] = ClearValue;
		CommandList.BeginRenderPass(PassInfo, "ImGuiRenderPass");
		CommandList.EndRenderPass();
	}

	auto ImGuiRHIImpl_RenderDrawData(const FViewportRHIRef& InViewport, ImDrawData* DrawData, FImGuiRHIImpl_WindowRenderBuffers* WindowRenderBuffers, bool bPresent) -> void
	{
		const bool bHasDrawData = DrawData != nullptr;

		// Update textures
		if (bHasDrawData && DrawData->Textures != nullptr)
		{
			for (ImTextureData* Tex : *DrawData->Textures)
			{
				if (Tex->Status != ImTextureStatus_OK)
				{
					ImGuiRHIImpl_UpdateTexture(Tex);
				}
			}
		}

		const ImVec4 WindowBgColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
		const FClearValueBinding ClearValue(WindowBgColor.x, WindowBgColor.y, WindowBgColor.z, 1.0f);

		ENQUEUE_RENDER_COMMAND(RenderWindow)([ViewportRHI = InViewport, DrawData, WindowRenderBuffers, ClearValue, bPresent](FRHICommandListImmediate& CommandList) {
			check(WindowRenderBuffers != nullptr);
			auto& RenderBuffersCurrentFrame = WindowRenderBuffers->FrameRenderBuffers[GRenderFrameCounterRenderThread % kFrameInFlight];
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);

			CommandList.BeginDrawingViewport(ViewportRHI, nullptr);

			FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(ViewportRHI);
			if (BackBuffer == nullptr)
			{
				CommandList.EndDrawingViewport(ViewportRHI, false, false);
				return;
			}

			if (DrawData != nullptr
				&& DrawData->TotalVtxCount > 0
				&& DrawData->TotalIdxCount > 0
				&& GBackendState.PipelineState
				&& GBackendState.VertexShader
				&& GBackendState.FragmentShader)
			{
				ImGuiRHIImplRT_RenderDrawData(CommandList, BackBuffer, DrawData, RenderBuffersCurrentFrame, ClearValue);
			}
			else
			{
				ImGuiRHIImplRT_ClearViewport(CommandList, BackBuffer, ClearValue);
			}

			CommandList.EndDrawingViewport(ViewportRHI, bPresent, false);
		});
	}

	auto ImGuiRHIImpl_PresentViewport(const FViewportRHIRef& InViewport) -> void
	{
		ENQUEUE_RENDER_COMMAND(PresentWindow)([ViewportRHI = InViewport](FRHICommandListImmediate& CommandList) {
			CommandList.EndDrawingViewport(ViewportRHI, true, false);
		});
	}

} // namespace Durin::MonaImGui
