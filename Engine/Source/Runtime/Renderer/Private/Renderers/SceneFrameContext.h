#pragma once

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRenderGraphTypes.h"

namespace Durin
{
	// Owns all submission-local state and keeps policy, resources, transaction,
	// and observation lifetimes distinct.
	struct FSceneFrameContext final
	{
		struct FLogical final
		{
			FScene* Scene = nullptr;
			const FSceneView* CallerView = nullptr;
			FSceneView RenderView;
			FRHITexture* OutputTarget = nullptr;
			FSceneViewRenderOptions Options;
			FRendererQualificationPolicy Qualification;
			std::optional<FSceneRenderPlan> PreparedView;
			RendererEditorAssistance::FPrepared EditorAssistance;
			uint32 Width = 0;
			uint32 Height = 0;
			bool bPresentOutput = false;
		};

		struct FResolved final
		{
			FResolvedSceneResources Scene;
			FRHITexture* CloudWeatherTexture = nullptr;
			bool bHybridRetainedResourcesReady = false;
		};

		struct FTransaction final
		{
			FSceneViewTemporalContext Temporal;
			FSceneViewState* ViewState = nullptr;
			FSceneRenderGraphComposition Composition;
		};

		struct FObservation final
		{
			FSceneRenderTelemetry Telemetry;
		};

		FLogical Logical;
		FResolved Resolved;
		FSceneFrameFeaturePlan Features;
		FTransaction Transaction;
		FObservation Observation;
	};

} // namespace Durin
