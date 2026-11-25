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
	class FSkyBackgroundRenderer final
	{
	public:
		FSkyBackgroundRenderer(
			FDefaultTextureResources& InDefaultTextures, RendererPrivate::FSurfaceMaterialResources& InMaterials
		);
		~FSkyBackgroundRenderer();

		FSkyBackgroundRenderer(const FSkyBackgroundRenderer&) = delete;
		auto operator=(const FSkyBackgroundRenderer&) -> FSkyBackgroundRenderer& = delete;

		auto EnsureResources_RenderThread() -> bool;
		auto PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
			FScene* Scene, FPreparedEnvironment& Environment) -> bool;
		auto Draw_RenderThread(FRHICommandListImmediate& Commands,
			const FPreparedEnvironment& Environment, bool bHybridBootstrap = false) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FDefaultTextureResources& DefaultTextures;
		RendererPrivate::FSurfaceMaterialResources& Materials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
