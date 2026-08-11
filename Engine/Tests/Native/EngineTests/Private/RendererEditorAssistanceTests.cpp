#include <gtest/gtest.h>

#include "RenderResourceCreation.h"
#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"
#include "Renderers/EditorAssistance/OverlayIconRenderer.h"

namespace Durin
{
	using RendererEditorAssistance::EDrawOperation;
	using RendererEditorAssistance::EDepthMode;
	using RendererEditorAssistance::EFeature;
	using RendererEditorAssistance::EGizmoTopology;
	using RendererEditorAssistance::FPipelineKey;
	using RendererEditorAssistance::FRequest;
	using RenderTargetLayouts::EViewportOutput;

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
		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			FSceneView{}, EViewportOutput::Offscreen);

		EXPECT_TRUE(Request.IsEmpty());
		EXPECT_TRUE(FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request).empty());
		EXPECT_TRUE(FEditorAssistanceRenderer::BuildDrawableOperations(Request, {}).empty());
	}

	TEST(FOverlayIconAtlasLayoutTests, UploadCoversEveryPackedIconTile)
	{
		EXPECT_EQ(FOverlayIconAtlasLayout::Width, 192u);
		EXPECT_EQ(FOverlayIconAtlasLayout::Height, 64u);
		EXPECT_EQ(FOverlayIconAtlasLayout::RowPitchBytes, 192u * 4u);
		EXPECT_EQ(FOverlayIconAtlasLayout::PixelByteCount,
			static_cast<size_t>(FOverlayIconAtlasLayout::RowPitchBytes)
				* FOverlayIconAtlasLayout::Height);
		EXPECT_EQ(FOverlayIconAtlasLayout::GetTileX(EViewOverlayIcon::Camera), 0u);
		EXPECT_EQ(FOverlayIconAtlasLayout::GetTileX(EViewOverlayIcon::DirectionalLight), 64u);
		EXPECT_EQ(FOverlayIconAtlasLayout::GetTileX(EViewOverlayIcon::PlayerStart)
			+ FOverlayIconAtlasLayout::IconExtent, FOverlayIconAtlasLayout::Width);
	}

	TEST(FRendererEditorAssistanceTests, GridOnlyRequestsCurrentOffscreenPipeline)
	{
		FSceneView View;
		View.EditorGrid.bVisible = true;

		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			View, EViewportOutput::Offscreen);
		const std::vector<FPipelineKey> Keys =
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);

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

		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			View, EViewportOutput::Present);
		const std::vector<FPipelineKey> Keys =
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);

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

		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			View, EViewportOutput::Offscreen);
		const std::vector<FPipelineKey> Keys =
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);

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

		const FRequest Request = FEditorAssistanceRenderer::AnalyzeRequest(
			View, EViewportOutput::Present);
		const std::vector<FPipelineKey> Keys =
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);

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
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::OverlayIcon
				|| (Key.Feature == EFeature::OverlayLine
					&& Key.DepthMode == EDepthMode::XRay)
				|| (Key.Feature == EFeature::Gizmo
					&& Key.GizmoTopology == EGizmoTopology::Wire);
		});

		const std::vector<EDrawOperation> Operations =
			FEditorAssistanceRenderer::BuildDrawableOperations(Request, Available);
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
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.Feature == EFeature::OverlayIcon;
		});

		const std::vector<EDrawOperation> Operations =
			FEditorAssistanceRenderer::BuildDrawableOperations(Request, Available);

		ASSERT_EQ(Operations.size(), 1);
		EXPECT_EQ(Operations[0], EDrawOperation::EditorGrid);
	}

	TEST(FRendererEditorAssistanceTests, FailedLineXRayLeavesVisibleLineAvailable)
	{
		FRequest Request;
		Request.Output = EViewportOutput::Present;
		Request.bOverlayLines = true;
		std::vector<FPipelineKey> Available =
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.DepthMode == EDepthMode::XRay;
		});

		const std::vector<EDrawOperation> Operations =
			FEditorAssistanceRenderer::BuildDrawableOperations(Request, Available);

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
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);
		std::erase_if(Available, [](const FPipelineKey& Key) {
			return Key.GizmoTopology == EGizmoTopology::Wire;
		});

		const std::vector<EDrawOperation> Operations =
			FEditorAssistanceRenderer::BuildDrawableOperations(Request, Available);
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
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);
		Request.Output = EViewportOutput::Present;
		const std::vector<FPipelineKey> PresentKeys =
			FEditorAssistanceRenderer::GetRequiredPipelineKeys(Request);

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

	TEST(FRendererEditorAssistanceTests, DrawOrderKeepsAllAssistanceAfterGridAndXRayBeforeVisible)
	{
		const std::span<const EDrawOperation> Order = FEditorAssistanceRenderer::GetDrawOrder();
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
		const std::span<const EDrawOperation> Order = FEditorAssistanceRenderer::GetDrawOrder();
		for (const EDrawOperation Operation : Order)
		{
			EXPECT_EQ(std::ranges::count(Order, Operation), 1);
		}
	}

	TEST(
		FRendererEditorAssistanceTests,
		FixedFeatureAndPreviewFailuresStayIndependentAndRecover)
	{
		using EDependency = ERenderResourceGenerationDependency;
		using FResult = TRenderResourceCreateResult<std::string>;
		struct FInjectedResource
		{
			std::string Name;
			TRenderResourceCreationSlot<std::string> Slot{
				EDependency::Shader | EDependency::Device};
			int Attempts = 0;
			bool bFailNextAttempt = false;
		};
		std::array<FInjectedResource, 8> Resources{{
			{.Name = "SkyBox"},
			{.Name = "PostProcess"},
			{.Name = "TextureCubeThumbnail"},
			{.Name = "Gizmo"},
			{.Name = "OverlayLine"},
			{.Name = "OverlayIcon"},
			{.Name = "EditorGrid"},
			{.Name = "TextureEditorPreview"},
		}};
		FRenderResourceGeneration Generation;
		std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
		Resources[4].bFailNextAttempt = true;

		for (FInjectedResource& Resource : Resources)
		{
			auto* Payload = Resource.Slot.Resolve(
				Generation,
				[&Resource]() -> FResult {
					++Resource.Attempts;
					if (std::exchange(
							Resource.bFailNextAttempt, false))
					{
						return FResult::Failure({
							.Category =
								ERenderResourceCreateErrorCategory::
									RHIResource,
							.Context = "FixedResource",
							.Identity = Resource.Name,
							.Message = "injected failure",
							.RetryDependencies =
								EDependency::Device
								| EDependency::Manual,
						});
					}
					return FResult::Success(Resource.Name);
				},
				[&Diagnostics](
					FRenderResourceCreateDiagnostic Diagnostic) {
					Diagnostics.push_back(std::move(Diagnostic));
				});
			if (Resource.Name == "OverlayLine")
				EXPECT_EQ(Payload, nullptr);
			else
			{
				ASSERT_NE(Payload, nullptr);
				EXPECT_EQ(*Payload, Resource.Name);
			}
		}
		ASSERT_EQ(Diagnostics.size(), 1);
		EXPECT_EQ(Diagnostics[0].Error->Identity, "OverlayLine");
		for (const FInjectedResource& Resource : Resources)
		{
			if (Resource.Name != "OverlayLine")
				EXPECT_EQ(
					Resource.Slot.GetAvailability(),
					ERenderResourceAvailability::Ready);
		}

		Generation.Advance(EDependency::Manual);
		for (FInjectedResource& Resource : Resources)
		{
			ASSERT_NE(
				Resource.Slot.Resolve(
					Generation,
					[&Resource]() {
						++Resource.Attempts;
						return FResult::Success(Resource.Name);
					},
					[&Diagnostics](
						FRenderResourceCreateDiagnostic Diagnostic) {
						Diagnostics.push_back(std::move(Diagnostic));
					}),
				nullptr);
		}
		for (const FInjectedResource& Resource : Resources)
		{
			EXPECT_EQ(
				Resource.Attempts,
				Resource.Name == "OverlayLine" ? 2 : 1);
		}
		ASSERT_EQ(Diagnostics.size(), 2);
		EXPECT_EQ(
			Diagnostics.back().Kind,
			ERenderResourceCreateDiagnosticKind::Recovery);
		EXPECT_EQ(
			Diagnostics.back().Error->Identity,
			"OverlayLine");
	}
} // namespace Durin
