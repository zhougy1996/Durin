#pragma once

#include "Renderers/MeshRenderPreparationCommon.h"
#include "Renderers/SurfaceMaterial.h"
#include "RenderingThread.h"

#include <functional>
#include <utility>

namespace Durin
{
	enum class EGeometryResolutionStatus : uint8
	{
		Complete,
		Partial
	};

	struct FGeometryResolutionResult
	{
		EGeometryResolutionStatus Status = EGeometryResolutionStatus::Complete;
		size_t AttemptedDraws = 0;
		size_t ResolvedDraws = 0;
		size_t RejectedDraws = 0;

		operator bool() const
		{
			return Status == EGeometryResolutionStatus::Complete;
		}
	};

	struct FGeometryExecutionResult
	{
		bool bComplete = true;
		bool bRenderedGeometry = false;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t SkippedDraws = 0;
	};
}

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

	template<typename TResolvedView>
	auto FinalizeResourcePreparation(
		TResolvedView& ResolvedView
	) -> FGeometryResolutionResult
	{
		ResolvedView.ResourcePreparationRejectedDraws =
			ResolvedView.ResourcePreparationAttemptedDraws
			- ResolvedView.ResourcePreparationSuccessfulDraws;
		check(
			ResolvedView.ResourcePreparationAttemptedDraws
			== ResolvedView.ResourcePreparationSuccessfulDraws
				   + ResolvedView.ResourcePreparationRejectedDraws
		);
		return {
			.Status = ResolvedView.ResourcePreparationRejectedDraws == 0
				? EGeometryResolutionStatus::Complete
				: EGeometryResolutionStatus::Partial,
			.AttemptedDraws = ResolvedView.ResourcePreparationAttemptedDraws,
			.ResolvedDraws = ResolvedView.ResourcePreparationSuccessfulDraws,
			.RejectedDraws = ResolvedView.ResourcePreparationRejectedDraws
		};
	}

	template<typename TResolvedView>
	auto FinalizeExecution(
		TResolvedView& ResolvedView,
		size_t ExpectedDraws,
		bool bExpectAllDraws = true
	) -> void
	{
		check(
			ResolvedView.AttemptedDraws
			== ResolvedView.SuccessfulDraws + ResolvedView.RejectedDraws
		);
		check(!bExpectAllDraws || ResolvedView.AttemptedDraws == ExpectedDraws);
	}

} // namespace Durin::RendererPrivate
