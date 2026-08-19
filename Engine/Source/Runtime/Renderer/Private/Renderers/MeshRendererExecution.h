#pragma once

#include "Renderers/StaticMeshRenderPreparation.h"

#include <functional>

namespace Durin::RendererPrivate
{
	template<typename TPreparedView>
	auto GetBasePassBucket(
		TPreparedView& PreparedView,
		EStaticMeshBasePass Pass
	) -> decltype(auto)
	{
		return Pass == EStaticMeshBasePass::Opaque ?
				   (PreparedView.Opaque) :
			   Pass == EStaticMeshBasePass::Masked ?
				   (PreparedView.Masked) :
				   (PreparedView.Translucent);
	}

	template<typename TPreparedView, typename TFunction>
	auto ForEachBasePassBucket(
		TPreparedView& PreparedView,
		TFunction&& Function
	) -> void
	{
		std::invoke(Function, PreparedView.Opaque, EStaticMeshBasePass::Opaque);
		std::invoke(Function, PreparedView.Masked, EStaticMeshBasePass::Masked);
		std::invoke(
			Function,
			PreparedView.Translucent,
			EStaticMeshBasePass::Translucent
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
