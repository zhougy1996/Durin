#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h"

namespace Durin
{
	namespace RendererPrivate { class FSurfaceMaterialResources; }
	class FScene;
	struct FPreparedEnvironment;
	class FDefaultTextureResources;
	class FRHICommandListImmediate;
	struct FSceneView;
	struct FSkyBoxSceneData;

	// Owns SkyBox shaders, pipelines, geometry, and draw submission.
	class FSkyBoxRenderer final
	{
	public:
		FSkyBoxRenderer(
			FDefaultTextureResources& InDefaultTextures, RendererPrivate::FSurfaceMaterialResources& InMaterials
		);
		~FSkyBoxRenderer();

		FSkyBoxRenderer(const FSkyBoxRenderer&) = delete;
		auto operator=(const FSkyBoxRenderer&) -> FSkyBoxRenderer& = delete;

		auto EnsureResources_RenderThread() -> bool;
		auto PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
			FScene* Scene, FPreparedEnvironment& Environment) -> bool;
		auto Draw_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
			const FPreparedEnvironment& Environment, bool bHybridBootstrap = false) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FDefaultTextureResources& DefaultTextures;
		RendererPrivate::FSurfaceMaterialResources& Materials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
