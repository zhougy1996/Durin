#pragma once

#include "Renderers/MeshRenderPreparationCommon.h"
#include "Renderers/SurfaceMaterial.h"
#include "RenderingThread.h"

#include <functional>
#include <utility>

namespace Durin::RendererPrivate
{
	template <typename TForwardShaderRef, typename TMaskedShadowShaderRef,
		typename TDrawSubmission>
	auto ExecuteMeshSurfacePass_RenderThread(
		FRHICommandListImmediate& CommandList,
		ESurfaceMaterialPass Pass,
		const FRHIUniformBufferRange& Lighting,
		const FResolvedSurfaceMaterial* Material,
		const FRHIUniformBufferRange& MaterialBuffer,
		const TForwardShaderRef& ForwardShader,
		const TMaskedShadowShaderRef& MaskedShadowShader,
		TDrawSubmission&& SubmitDraw) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		switch (Pass)
		{
		case ESurfaceMaterialPass::OpaqueShadow:
			std::invoke(std::forward<TDrawSubmission>(SubmitDraw));
			return true;
		case ESurfaceMaterialPass::MaskedShadow:
			if (Material == nullptr
				|| Material->ResolvedRoleMask != (uint8{1} << 7)) return false;
			SetShaderParameters(
				CommandList, MaskedShadowShader,
				MakeSurfaceMaskedShadowParameters(*Material, MaterialBuffer));
			std::invoke(std::forward<TDrawSubmission>(SubmitDraw));
			return true;
		case ESurfaceMaterialPass::Forward:
			if (Material == nullptr || Material->ResolvedRoleMask != 0xff)
				return false;
			SetShaderParameters(
				CommandList, ForwardShader,
				MakeSurfaceForwardParameters(*Material, MaterialBuffer, Lighting));
			std::invoke(std::forward<TDrawSubmission>(SubmitDraw));
			return true;
		case ESurfaceMaterialPass::GBuffer:
			return false;
		}
		return false;
	}

	template<typename TPreparedView>
	auto GetBasePassBucket(
		TPreparedView& PreparedView,
		EMeshBasePass Pass
	) -> decltype(auto)
	{
		return Pass == EMeshBasePass::Opaque ?
				   (PreparedView.Opaque) :
			   Pass == EMeshBasePass::Masked ?
				   (PreparedView.Masked) :
				   (PreparedView.Translucent);
	}

	template<typename TPreparedView, typename TFunction>
	auto ForEachBasePassBucket(
		TPreparedView& PreparedView,
		TFunction&& Function
	) -> void
	{
		std::invoke(Function, PreparedView.Opaque, EMeshBasePass::Opaque);
		std::invoke(Function, PreparedView.Masked, EMeshBasePass::Masked);
		std::invoke(
			Function,
			PreparedView.Translucent,
			EMeshBasePass::Translucent
		);
	}

	template<typename TPreparedView, typename TFunction>
	auto ForEachShadowBucket(
		TPreparedView& PreparedView,
		TFunction&& Function
	) -> void
	{
		std::invoke(Function, PreparedView.Opaque);
		std::invoke(Function, PreparedView.Masked);
	}

	template<typename TPreparedView, typename TPhase>
	auto FinalizeResourcePreparation(
		TPreparedView& PreparedView,
		TPhase ResourcesPreparedPhase
	) -> bool
	{
		PreparedView.ResourcePreparationRejectedDraws =
			PreparedView.ResourcePreparationAttemptedDraws
			- PreparedView.ResourcePreparationSuccessfulDraws;
		PreparedView.Phase = ResourcesPreparedPhase;
		check(
			PreparedView.ResourcePreparationAttemptedDraws
			== PreparedView.ResourcePreparationSuccessfulDraws
				   + PreparedView.ResourcePreparationRejectedDraws
		);
		return PreparedView.ResourcePreparationRejectedDraws == 0;
	}

	template<typename TPreparedView, typename TPhase>
	auto FinalizeExecution(
		TPreparedView& PreparedView,
		TPhase ExecutedPhase,
		bool bExpectAllDraws = true
	) -> void
	{
		PreparedView.Phase = ExecutedPhase;
		check(
			PreparedView.AttemptedDraws
			== PreparedView.SuccessfulDraws + PreparedView.RejectedDraws
		);
		check(!bExpectAllDraws || PreparedView.AttemptedDraws == PreparedView.GetNumSections());
	}
} // namespace Durin::RendererPrivate
