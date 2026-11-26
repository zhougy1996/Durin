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

	// Owns SkyBox shaders, pipelines, geometry, and draw submission.
	class FSkydomeMeshPass final
	{
	public:
		FSkydomeMeshPass(
			FDefaultTextureResources& InDefaultTextures, RendererPrivate::FSurfaceMaterialResources& InMaterials
		);
		~FSkydomeMeshPass();

		FSkydomeMeshPass(const FSkydomeMeshPass&) = delete;
		auto operator=(const FSkydomeMeshPass&) -> FSkydomeMeshPass& = delete;

		auto EnsureResources_RenderThread() -> bool;
		auto PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
			FScene* Scene, FPreparedEnvironment& Environment, bool bHybrid) -> bool;
		auto Draw_RenderThread(FRHICommandListImmediate& Commands,
			const FPreparedEnvironment& Environment) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FDefaultTextureResources& DefaultTextures;
		RendererPrivate::FSurfaceMaterialResources& Materials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
