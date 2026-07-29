#include <gtest/gtest.h>

#include "RendererEditorAssistance.h"

namespace Durin
{
	using RendererEditorAssistance::EDrawOperation;
	using RendererEditorAssistance::EDepthMode;
	using RendererEditorAssistance::EFeature;
	using RendererEditorAssistance::EGizmoTopology;
	using RendererEditorAssistance::EResourceAvailability;
	using RendererEditorAssistance::FGenerationScopedAttempt;
	using RendererEditorAssistance::FPipelineKey;
	using RendererEditorAssistance::FRequest;
	using RendererEditorAssistance::FResourceGeneration;
	using RendererEditorAssistance::FResourceGenerationDependencies;
	using RendererRenderTargetLayouts::EViewportOutput;

	namespace
	{
		auto MakePrimitive(EViewOverlayShape Shape) -> FViewOverlayPrimitive
		{
			FViewOverlayPrimitive Primitive;
			Primitive.Shape = Shape;
			return Primitive;
		}
	}

	TEST(FRendererEditorAssistanceTests, EmptyViewRequestsNoAssistance)
	{
		const FRequest Request = RendererEditorAssistance::AnalyzeRequest(
			FSceneView{}, EViewportOutput::Offscreen);

		EXPECT_TRUE(Request.IsEmpty());
		EXPECT_TRUE(RendererEditorAssistance::GetRequiredPipelineKeys(Request).empty());
		EXPECT_TRUE(RendererEditorAssistance::BuildDrawableOperations(Request, {}).empty());
	}

	TEST(FRendererEditorAssistanceTests, GridOnlyRequestsCurrentOffscreenPipeline)
	{
		FSceneView View;
		View.EditorGrid.bVisible = true;

		const FRequest Request = RendererEditorAssistance::AnalyzeRequest(
			View, EViewportOutput::Offscreen);
		const std::vector<FPipelineKey> Keys =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);

		ASSERT_EQ(Keys.size(), 1);
		EXPECT_EQ(Keys[0], (FPipelineKey{
			.Feature = EFeature::EditorGrid,
			.Output = EViewportOutput::Offscreen,
			.DepthMode = EDepthMode::Visible,
		}));
	}

	TEST(FRendererEditorAssistanceTests, SolidGizmoDoesNotRequestWirePipelines)
	{
		FSceneView View;
		View.OverlayPrimitives.push_back(MakePrimitive(EViewOverlayShape::Arrow));

		const FRequest Request = RendererEditorAssistance::AnalyzeRequest(
			View, EViewportOutput::Present);
		const std::vector<FPipelineKey> Keys =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);

		EXPECT_TRUE(Request.bSolidGizmos);
		EXPECT_FALSE(Request.bWireGizmos);
		ASSERT_EQ(Keys.size(), 2);
		EXPECT_TRUE(std::ranges::all_of(Keys, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::Gizmo
				&& Key.Output == EViewportOutput::Present
				&& Key.GizmoTopology == EGizmoTopology::Solid;
		}));
	}

	TEST(FRendererEditorAssistanceTests, WireGizmoDoesNotRequestSolidPipelines)
	{
		FSceneView View;
		View.OverlayPrimitives.push_back(MakePrimitive(EViewOverlayShape::WireBox));

		const FRequest Request = RendererEditorAssistance::AnalyzeRequest(
			View, EViewportOutput::Offscreen);
		const std::vector<FPipelineKey> Keys =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);

		EXPECT_FALSE(Request.bSolidGizmos);
		EXPECT_TRUE(Request.bWireGizmos);
		ASSERT_EQ(Keys.size(), 2);
		EXPECT_TRUE(std::ranges::all_of(Keys, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::Gizmo
				&& Key.Output == EViewportOutput::Offscreen
				&& Key.GizmoTopology == EGizmoTopology::Wire;
		}));
	}

	TEST(FRendererEditorAssistanceTests, LinesAndIconsRequestIndependentDepthVariants)
	{
		FSceneView View;
		View.OverlayLines.emplace_back();
		View.OverlayIcons.emplace_back();

		const FRequest Request = RendererEditorAssistance::AnalyzeRequest(
			View, EViewportOutput::Present);
		const std::vector<FPipelineKey> Keys =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);

		EXPECT_TRUE(Request.bOverlayLines);
		EXPECT_TRUE(Request.bOverlayIcons);
		ASSERT_EQ(Keys.size(), 4);
		for (const EFeature Feature : {EFeature::OverlayLine, EFeature::OverlayIcon})
		{
			EXPECT_EQ(std::ranges::count_if(Keys, [Feature](const FPipelineKey& Key) {
				return Key.Feature == Feature && Key.DepthMode == EDepthMode::XRay;
			}), 1);
			EXPECT_EQ(std::ranges::count_if(Keys, [Feature](const FPipelineKey& Key) {
				return Key.Feature == Feature && Key.DepthMode == EDepthMode::Visible;
			}), 1);
		}
	}

	TEST(FRendererEditorAssistanceTests, UnavailableOperationDoesNotSuppressIndependentDraws)
	{
		FRequest Request;
		Request.Output = EViewportOutput::Offscreen;
		Request.bEditorGrid = true;
		Request.bSolidGizmos = true;
		Request.bWireGizmos = true;
		Request.bOverlayLines = true;
		Request.bOverlayIcons = true;
		std::vector<FPipelineKey> Available =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::OverlayIcon
				|| (Key.Feature == EFeature::OverlayLine
					&& Key.DepthMode == EDepthMode::XRay)
				|| (Key.Feature == EFeature::Gizmo
					&& Key.GizmoTopology == EGizmoTopology::Wire);
		});

		const std::vector<EDrawOperation> Operations =
			RendererEditorAssistance::BuildDrawableOperations(Request, Available);
		const std::array Expected{
			EDrawOperation::EditorGrid,
			EDrawOperation::XRayGizmos,
			EDrawOperation::VisibleGizmos,
			EDrawOperation::VisibleOverlayLines,
		};

		EXPECT_TRUE(std::ranges::equal(Operations, Expected));
	}

	TEST(FRendererEditorAssistanceTests, FailedIconBaseLeavesGridAvailable)
	{
		FRequest Request;
		Request.Output = EViewportOutput::Offscreen;
		Request.bEditorGrid = true;
		Request.bOverlayIcons = true;
		std::vector<FPipelineKey> Available =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::OverlayIcon;
		});

		const std::vector<EDrawOperation> Operations =
			RendererEditorAssistance::BuildDrawableOperations(Request, Available);

		ASSERT_EQ(Operations.size(), 1);
		EXPECT_EQ(Operations[0], EDrawOperation::EditorGrid);
	}

	TEST(FRendererEditorAssistanceTests, FailedLineXRayLeavesVisibleLineAvailable)
	{
		FRequest Request;
		Request.Output = EViewportOutput::Present;
		Request.bOverlayLines = true;
		std::vector<FPipelineKey> Available =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.DepthMode == EDepthMode::XRay;
		});

		const std::vector<EDrawOperation> Operations =
			RendererEditorAssistance::BuildDrawableOperations(Request, Available);

		ASSERT_EQ(Operations.size(), 1);
		EXPECT_EQ(Operations[0], EDrawOperation::VisibleOverlayLines);
	}

	TEST(FRendererEditorAssistanceTests, FailedWireGizmoLeavesSolidGizmoAvailable)
	{
		FRequest Request;
		Request.Output = EViewportOutput::Offscreen;
		Request.bSolidGizmos = true;
		Request.bWireGizmos = true;
		std::vector<FPipelineKey> Available =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.GizmoTopology == EGizmoTopology::Wire;
		});

		const std::vector<EDrawOperation> Operations =
			RendererEditorAssistance::BuildDrawableOperations(Request, Available);
		const std::array Expected{
			EDrawOperation::XRayGizmos,
			EDrawOperation::VisibleGizmos,
		};

		EXPECT_TRUE(std::ranges::equal(Operations, Expected));
	}

	TEST(FRendererEditorAssistanceTests, OutputPipelineKeysAreCreatedIndependently)
	{
		FRequest Request;
		Request.bEditorGrid = true;
		Request.bOverlayLines = true;
		const std::vector<FPipelineKey> OffscreenKeys =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);
		Request.Output = EViewportOutput::Present;
		const std::vector<FPipelineKey> PresentKeys =
			RendererEditorAssistance::GetRequiredPipelineKeys(Request);

		ASSERT_EQ(OffscreenKeys.size(), PresentKeys.size());
		EXPECT_TRUE(std::ranges::all_of(
			OffscreenKeys,
			[](const FPipelineKey& Key) {
				return Key.Output == EViewportOutput::Offscreen;
			}));
		EXPECT_TRUE(std::ranges::all_of(
			PresentKeys,
			[](const FPipelineKey& Key) {
				return Key.Output == EViewportOutput::Present;
			}));
		EXPECT_TRUE(std::ranges::none_of(
			OffscreenKeys,
			[&PresentKeys](const FPipelineKey& Key) {
				return std::ranges::find(PresentKeys, Key)
					!= PresentKeys.end();
			}));
	}

	TEST(FRendererEditorAssistanceTests, FailedAttemptRetriesOnlyAfterRelevantGeneration)
	{
		const FResourceGenerationDependencies Dependencies{
			.bShader = true,
			.bManual = true,
		};
		FResourceGeneration Generation;
		FGenerationScopedAttempt Attempt;

		EXPECT_TRUE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));
		RendererEditorAssistance::RecordResourceAttemptFailure(
			Attempt, Generation, "compile failed");
		EXPECT_EQ(Attempt.Availability, EResourceAvailability::Failed);
		EXPECT_EQ(Attempt.FailureDetail, "compile failed");
		EXPECT_FALSE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));

		++Generation.Device;
		EXPECT_FALSE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));
		++Generation.Shader;
		EXPECT_TRUE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));
		RendererEditorAssistance::RecordResourceAttemptSuccess(
			Attempt, Generation);
		EXPECT_EQ(Attempt.Availability, EResourceAvailability::Ready);
		EXPECT_TRUE(Attempt.FailureDetail.empty());

		++Generation.Manual;
		EXPECT_FALSE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));
		++Generation.Shader;
		EXPECT_TRUE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));
		RendererEditorAssistance::RecordResourceAttemptFailure(
			Attempt, Generation, "refresh failed");
		EXPECT_EQ(Attempt.Availability, EResourceAvailability::Ready);
		EXPECT_TRUE(Attempt.bHasPayload);
		EXPECT_EQ(Attempt.PayloadGeneration.Shader, 1);
		EXPECT_FALSE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));

		++Generation.Manual;
		EXPECT_TRUE(RendererEditorAssistance::ShouldAttemptResource(
			Attempt, Generation, Dependencies));
	}

	TEST(FRendererEditorAssistanceTests, DrawOrderKeepsAllAssistanceAfterGridAndXRayBeforeVisible)
	{
		const std::span<const EDrawOperation> Order = RendererEditorAssistance::GetDrawOrder();
		const std::array Expected{
			EDrawOperation::EditorGrid,
			EDrawOperation::XRayGizmos,
			EDrawOperation::XRayOverlayLines,
			EDrawOperation::XRayOverlayIcons,
			EDrawOperation::VisibleGizmos,
			EDrawOperation::VisibleOverlayLines,
			EDrawOperation::VisibleOverlayIcons,
		};

		ASSERT_EQ(Order.size(), Expected.size());
		EXPECT_TRUE(std::ranges::equal(Order, Expected));
	}

	TEST(FRendererEditorAssistanceTests, EveryAssistanceOperationAppearsExactlyOnce)
	{
		const std::span<const EDrawOperation> Order = RendererEditorAssistance::GetDrawOrder();
		for (const EDrawOperation Operation : Order)
		{
			EXPECT_EQ(std::ranges::count(Order, Operation), 1);
		}
	}
} // namespace Durin
