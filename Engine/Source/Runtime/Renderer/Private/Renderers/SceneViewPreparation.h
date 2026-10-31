#pragma once

#include "CoreMinimal.h"

#include "RendererAPI.h"

namespace Durin
{
	struct FSceneView;
	class FRHICommandListImmediate;
	struct FRHIUniformBufferRange;
	RENDERER_API auto FitSceneViewToOutput(const FSceneView& View,
		uint32 Width, uint32 Height) -> FSceneView;
} // namespace Durin

namespace Durin::RendererPrivate
{
	struct FMaterialViewUniform
	{
		FVector4f Parameters{0.0f};
		FVector4f CameraWorldPosition{0.0f};
		FVector4f Viewport{0.0f};
		FVector4f ReciprocalViewport{0.0f};
		FMatrix4f WorldToView{1.0f};
		FMatrix4f ViewToWorld{1.0f};
		FMatrix4f WorldToClip{1.0f};
		FVector4f WorldOriginHigh{0.0f};
		FVector4f WorldOriginLow{0.0f};
	};

	auto PrepareMeshViewUniform(FRHICommandListImmediate& Commands, const FSceneView& View, bool bLighting) -> FRHIUniformBufferRange;
}
