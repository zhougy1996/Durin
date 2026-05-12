#include "ImGuiRHIImpl.h"

#include "DynamicRHI.h"
#include "Shader/ShaderCompiler.h"
#include "Shader/ShaderPaths.h"

namespace Doge::Mona::MonaImGuiBackend
{
	ImGuiContext* GMonaImGuiContext = nullptr;

	static auto CalcOrthoProj(float L, float R, float B, float T) -> FMatrix
	{
		FMatrix Result(1.0f);
		Result[0][0] = 2.0f / (R - L);
		Result[1][1] = 2.0f / (T - B);
		Result[2][2] = -1.0f;
		Result[3][0] = -(R + L) / (R - L);
		Result[3][1] = -(T + B) / (T - B);
		return Result;
	}

	struct FImGuiRHIBackendState
	{
		FShaderRHIRef VertexShader;
		FShaderRHIRef PixelShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;
		FBufferRHIRef ProjectionUBO;

		// Font atlas
		FTextureRHIRef FontAtlasTexture;

		bool bInitialized = false;
	};


	auto ImGuiRHIImpl_Init() -> void
	{
		check(GDynamicRHI);
		ImGuiIO& IO = ImGui::GetIO();
		IMGUI_CHECKVERSION();

		IO.BackendRendererUserData = GDynamicRHI;
		IO.BackendRendererName = "DogeRHI";
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // We can honor ImGuiPlatformIO::Textures[] requests during render.

		// Get font atlas pixel data on main thread
		unsigned char* FontPixels = nullptr;
		int FontWidth = 0, FontHeight = 0;
		IO.Fonts->GetTexDataAsRGBA32(&FontPixels, &FontWidth, &FontHeight);
		const uint32 FontDataSize = FontWidth * FontHeight * 4;

		// Compile shader
		std::string ShaderName = "/Engine/ImGui";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {"vertexMain", "fragmentMain"};
		FShaderCompilerOutput CompileResult = GShaderCompiler->Compile(FShaderPaths::SourcePath(ShaderName), CompileOptions);
		if (!CompileResult)
		{
			DOGE_ERROR("Failed to compile ImGui shader: {}", CompileResult.ErrorMessage);
		}
	}

	auto ImGuiRHIImpl_Shutdown() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendRendererUserData = nullptr;
		IO.BackendRendererName = nullptr;
		IO.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.ClearPlatformHandlers();
	}

	auto ImGuiRHIImpl_NewFrame() -> void
	{
	}

	auto ImGuiRHIImpl_RenderDrawData(ImDrawData* DrawData) -> void
	{
		int FrameBufferWidth = static_cast<int>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		int FrameBufferHeight = static_cast<int>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);
		if (FrameBufferWidth <= 0 || FrameBufferHeight <= 0)
		{
			return;
		}

		if (DrawData->Textures != nullptr)
			for (ImTextureData* TextureData : *DrawData->Textures)
				if (TextureData->Status != ImTextureStatus_OK)
					ImGuiRHIImpl_UpdateTexture(TextureData);
	}

	auto ImGuiRHIImpl_UpdateTexture(ImTextureData* TextureData) -> void
	{
	}
} // namespace Doge::Mona::MonaImGuiBackend