#pragma once

#include "RendererAPI.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	enum class ESimpleElementShaderClass : uint8
	{
		Untextured,
		Textured,
	};

	struct FSimpleElementVertex
	{
		FVector4f Position{0.0f};
		FVector2f UV{0.0f};
		FVector4f Color{1.0f};
		FVector2f Pattern{0.0f};
	};

	struct FSimpleElementBatchKey
	{
		ESimpleElementShaderClass ShaderClass =
			ESimpleElementShaderClass::Untextured;
		ESimpleElementBlendMode BlendMode = ESimpleElementBlendMode::Opaque;
		ESceneDepthPriorityGroup DepthPriorityGroup =
			ESceneDepthPriorityGroup::World;
		ESceneDepthConvention DepthConvention = ESceneDepthConvention::ForwardZ;
		RenderTargetLayouts::EViewportOutput Output =
			RenderTargetLayouts::EViewportOutput::Offscreen;
		FSimpleElementTexture Texture;

		auto operator==(const FSimpleElementBatchKey&) const -> bool = default;
	};

	// Owns one contiguous compatible geometry span without any RHI resource.
	struct FPreparedSimpleElementBatch
	{
		FSimpleElementBatchKey Key;
		std::vector<FSimpleElementVertex> Vertices;
		std::vector<uint32> Indices;
		uint32 SourceElementCount = 0;
		uint32 DroppedElementCount = 0;
		uint64 VertexBytes = 0;
		uint64 IndexBytes = 0;
		uint64 FirstSubmissionOrder = 0;
	};

	struct FSimpleElementCollectionStatistics
	{
		uint32 SubmittedElementCount = 0;
		uint32 AcceptedElementCount = 0;
		uint32 DroppedElementCount = 0;
		uint32 BatchCount = 0;
		uint32 VertexCount = 0;
		uint32 IndexCount = 0;
		uint64 VertexBytes = 0;
		uint64 IndexBytes = 0;
	};

	struct FPreparedSimpleElements
	{
		std::vector<FPreparedSimpleElementBatch> Batches;
		FSimpleElementCollectionStatistics Statistics;

		auto IsEmpty() const -> bool { return Batches.empty(); }
	};

	// Converts one immutable view submission into bounded, stable CPU batches.
	class FSimpleElementCollector final
	{
	public:
		static constexpr uint64 MaxPreparedBytes =
			32ull * 1024ull * 1024ull;

		RENDERER_API static auto Collect(const FSceneView& View,
			RenderTargetLayouts::EViewportOutput Output)
			-> FPreparedSimpleElements;
	};
} // namespace Durin
