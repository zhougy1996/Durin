#include "Renderers/SceneViewPreparation.h"
#include "SceneView.h"
#include "Math/Operations.h"
#include "RHICommandList.h"

namespace Durin
{
	auto FitSceneViewToOutput(
		const FSceneView& View,
		uint32 Width,
		uint32 Height
	) -> FSceneView
	{
		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint <= 0.0f)
		{
			return RenderView;
		}

		uint32 ContentWidth = Width;
		uint32 ContentHeight = static_cast<uint32>(
			std::round(ContentWidth / RenderView.AspectRatioConstraint)
		);
		if (ContentHeight > Height)
		{
			ContentHeight = Height;
			ContentWidth = static_cast<uint32>(
				std::round(
					ContentHeight * RenderView.AspectRatioConstraint
				)
			);
		}
		RenderView.ViewportWidth = std::max(1u, ContentWidth);
		RenderView.ViewportHeight = std::max(1u, ContentHeight);
		RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
		RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		return RenderView;
	}

} // namespace Durin

namespace Durin::RendererPrivate
{
	auto PrepareMeshViewUniform(FRHICommandListImmediate& CommandList, const FSceneView& View,
		bool bLighting) -> FRHIUniformBufferRange
	{
		FMatrix ViewToWorld;
		if (!Math::TryInverse(View.GetTranslatedWorldToView(), ViewToWorld, 1.0e-12)) return {};
		FMaterialViewUniform Uniform;
		Uniform.Parameters = FVector4f(static_cast<float>(View.MaterialTimeSeconds), 0.0f,
			bLighting ? 1.0f : 0.0f, bLighting && View.Settings.Mode.bEnableSpecularAA ? 1.0f : 0.0f);
		Uniform.CameraWorldPosition = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
		Uniform.Viewport = FVector4f(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth), static_cast<float>(View.ViewportHeight));
		Uniform.ReciprocalViewport = FVector4f(
			View.ViewportWidth ? 1.0f / static_cast<float>(View.ViewportWidth) : 0.0f,
			View.ViewportHeight ? 1.0f / static_cast<float>(View.ViewportHeight) : 0.0f, 0.0f, 0.0f);
		Uniform.WorldToView = Math::TransposeToFloat(View.GetTranslatedWorldToView());
		Uniform.ViewToWorld = Math::TransposeToFloat(ViewToWorld);
		Uniform.WorldToClip = Math::TransposeToFloat(View.GetTranslatedWorldToClip());
		const FVector3f OriginHigh(View.ViewLocation);
		Uniform.WorldOriginHigh = FVector4f(OriginHigh, 0.0f);
		Uniform.WorldOriginLow = FVector4f(FVector3f(View.ViewLocation - FVector3(OriginHigh)), 0.0f);
		return CommandList.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
	}
}
