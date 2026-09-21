#pragma once

#include "EngineAPI.h"
#include "SimpleElement.h"

namespace Durin
{
	struct FSceneView;

	// Copies bounded world-space simple primitives into one view submission.
	class FPrimitiveDrawInterface
	{
	public:
		ENGINE_API virtual ~FPrimitiveDrawInterface();

		virtual auto DrawLine(const FVector3& Start, const FVector3& End,
			const FVector4f& Color,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			const FSimpleElementLineStyle& Style = {}) -> void = 0;
		virtual auto DrawTranslucentLine(const FVector3& Start,
			const FVector3& End, const FVector4f& Color,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			const FSimpleElementLineStyle& Style = {}) -> void = 0;
		virtual auto DrawPoint(const FVector3& Position,
			const FVector4f& Color, float PointSizePixels,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			float DepthBias = 0.0f) -> void = 0;
		virtual auto DrawSprite(const FVector3& Position,
			const FVector2f& SizePixels, const FSimpleElementTexture& Texture,
			const FVector2f& MinUV, const FVector2f& MaxUV,
			const FVector4f& Color,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			float DepthBias = 0.0f) -> void = 0;
	};

	// Admits primitives only on its producer thread and becomes inert when sealed.
	class FViewPrimitiveDrawInterface final : public FPrimitiveDrawInterface
	{
	public:
		ENGINE_API explicit FViewPrimitiveDrawInterface(FSceneView& InView);
		ENGINE_API ~FViewPrimitiveDrawInterface() override;

		FViewPrimitiveDrawInterface(const FViewPrimitiveDrawInterface&) = delete;
		auto operator=(const FViewPrimitiveDrawInterface&)
			-> FViewPrimitiveDrawInterface& = delete;

		ENGINE_API auto DrawLine(const FVector3& Start, const FVector3& End,
			const FVector4f& Color,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			const FSimpleElementLineStyle& Style = {}) -> void override;
		ENGINE_API auto DrawTranslucentLine(const FVector3& Start,
			const FVector3& End, const FVector4f& Color,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			const FSimpleElementLineStyle& Style = {}) -> void override;
		ENGINE_API auto DrawPoint(const FVector3& Position,
			const FVector4f& Color, float PointSizePixels,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			float DepthBias = 0.0f) -> void override;
		ENGINE_API auto DrawSprite(const FVector3& Position,
			const FVector2f& SizePixels, const FSimpleElementTexture& Texture,
			const FVector2f& MinUV, const FVector2f& MaxUV,
			const FVector4f& Color,
			ESceneDepthPriorityGroup DepthPriorityGroup =
				ESceneDepthPriorityGroup::World,
			float DepthBias = 0.0f) -> void override;

		// Permanently closes the view submission and rejects later calls.
		ENGINE_API auto Seal() -> void;

	private:
		auto Add(FSimpleElement Element, uint64 PayloadBytes) -> void;
		auto CanAdmit(ESceneDepthPriorityGroup DepthPriorityGroup) -> bool;

		FSimpleElementViewSubmission* Submission = nullptr;
		uint32 ProducerThreadId = 0;
	};
} // namespace Durin
