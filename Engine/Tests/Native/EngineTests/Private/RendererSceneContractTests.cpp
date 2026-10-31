#include "../../RDGTestAccess.h"
#include "Backend/RHICompletionBackend.h"
#include "RDG/RDG.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"
#include "Renderers/MeshPassRelevance.h"
#include "HitProxy.h"
#include "PSOPrecache.h"
#include "RenderResourceCreation.h"
#include "RenderPipelineCreation.h"
#include "PSOPrecacheComponent.h"
#include "Materials/MaterialPSOPrecache.h"
#include "Materials/MaterialParameterCollection.h"
#include "RHICapabilities.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/MeshBatch.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/LightSceneProxy.h"
#include "Actors/DirectionalLightActor.h"
#include "EngineTestSupport.h"
#include "LightSceneTestSupport.h"
#include "CoreGlobals.h"
#include "Client/SceneViewport.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/PlatformLTS.h"
#include "NativeTestSupport.h"
#include "Modules/ModuleTestSupport.h"
#include "Math/Operations.h"
#include "RenderingThread.h"
#include "Threading/Task.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/MeshRendererShared.h"
#include "Renderers/SceneRenderPipeline.h"
#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderingService.h"
#include "Renderers/AmbientOcclusionRendering.h"
#include "Renderers/BaseSceneRendering.h"
#include "Renderers/ContactShadowVisibilityRendering.h"
#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "Renderers/DirectionalShadowRendering.h"
#include "Renderers/EditorAssistanceRendering.h"
#include "Renderers/GBufferRendering.h"
#include "Renderers/PostProcessRendering.h"
#include "Renderers/SceneColorRendering.h"
#include "Renderers/VolumetricCloudRendering.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRendererProfiling.h"
#include "SceneInfo.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/SceneViewState.h"
#include "Renderers/SurfaceMaterial.h"
#include "Renderers/DirectionalShadowView.h"
#include "RendererModule.h"
#include "SceneTestAccess.h"
#include "StaticMesh/StaticMeshResources.h"
#include "ViewRenderStatistics.h"

#include <gtest/gtest.h>

#if !defined(DURIN_RENDERER_RDG_ALLOCATOR_TESTS)
namespace
{
	static_assert(!std::is_convertible_v<Durin::FPrimitiveComponentId, Durin::FLightComponentId>);
	static_assert(!std::is_convertible_v<Durin::FLightComponentId, Durin::FPrimitiveComponentId>);
	static_assert(!Durin::FPrimitiveComponentId{}.IsValid());
	static_assert(!Durin::FLightComponentId{}.IsValid());

	template<typename SceneType>
	concept CHasPublicProxyMutation = requires(
		SceneType& Scene, std::unique_ptr<Durin::FLightSceneProxy> Proxy
	) {
		Scene.TryAddLightProxy(std::move(Proxy));
	};
	template<typename SceneType>
	concept CHasPublicProxyAddOverload = requires(
		SceneType& Scene, std::unique_ptr<Durin::FLightSceneProxy> Proxy
	) {
		Scene.AddLight(std::move(Proxy));
	};

	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().AddPrimitive(static_cast<Durin::DPrimitiveComponent*>(nullptr))), void>);
	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().RemovePrimitive(static_cast<Durin::DPrimitiveComponent*>(nullptr))), void>);
	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().AddLight(static_cast<Durin::DLightComponent*>(nullptr))), void>);
	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().RemoveLight(static_cast<Durin::DLightComponent*>(nullptr))), void>);
	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().AddSkyBox(static_cast<Durin::DSkyBoxComponent*>(nullptr))), void>);
	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().RemoveSkyBox(static_cast<Durin::DSkyBoxComponent*>(nullptr))), void>);
	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().AddVolumetricCloud(static_cast<Durin::DVolumetricCloudComponent*>(nullptr))), void>);
	static_assert(std::is_same_v<decltype(std::declval<Durin::FSceneInterface&>().RemoveVolumetricCloud(static_cast<Durin::DVolumetricCloudComponent*>(nullptr))), void>);
	static_assert(!CHasPublicProxyMutation<Durin::FSceneInterface>);
	static_assert(!CHasPublicProxyMutation<Durin::FScene>);
	static_assert(!CHasPublicProxyAddOverload<Durin::FSceneInterface>);
	static_assert(!CHasPublicProxyAddOverload<Durin::FScene>);

	struct FInspectionOutputParameters final
	{
		Durin::FRDGTokenParameter Output;

		static auto GetRDGParametersMetadata()
			-> const Durin::FRDGParametersMetadata*
		{
			static const std::array Members{
				Durin::MakeRDGResourceParameterMemberMetadata<
					FInspectionOutputParameters, decltype(Output),
					Durin::FRDGTokenParameter>("Output", offsetof(FInspectionOutputParameters, Output), Durin::ERDGParameterMemberKind::Token, Durin::ERDGResourceKind::Token, Durin::ERDGParameterRangeKind::None, Durin::ERDGUse::Write, Durin::ERHIAccess::None, true),
			};
			static const auto Metadata =
				Durin::MakeInlineRDGParametersMetadata<
					FInspectionOutputParameters>(
					"FInspectionOutputParameters", Members
				);
			return &Metadata;
		}
	};

	auto HasMovedParameterArgument(std::string_view Source, size_t OpenParenthesis)
		-> bool
	{
		uint32 Depth = 1;
		uint32 TopLevelCommas = 0;
		char Quote = '\0';
		bool bEscaped = false;
		for (size_t Index = OpenParenthesis + 1; Index < Source.size(); ++Index)
		{
			const char Character = Source[Index];
			if (Quote != '\0')
			{
				if (bEscaped)
					bEscaped = false;
				else if (Character == '\\')
					bEscaped = true;
				else if (Character == Quote)
					Quote = '\0';
				continue;
			}
			if (Character == '"' || Character == '\'')
			{
				Quote = Character;
				continue;
			}
			if (Character == '(')
				++Depth;
			else if (Character == ')')
			{
				if (--Depth == 0) return false;
			}
			else if (Character == ',' && Depth == 1)
			{
				++TopLevelCommas;
				if (TopLevelCommas != 2) continue;
				const size_t Argument = Source.find_first_not_of(" \t\r\n", Index + 1);
				return Argument != std::string_view::npos
					   && Source.substr(Argument).starts_with("std::move(");
			}
		}
		return false;
	}

	auto FindManualRenderGraphAuthoring(std::string_view Source, std::string_view DisplayPath) -> std::vector<std::string>
	{
		std::vector<std::string> Violations;
		constexpr std::array ManualUses{
			"UseTexture",
			"UseBuffer",
			"UseToken",
			"UseValue",
			"UseColorAttachment",
			"UseDepthStencilAttachment",
			"UseManagedColorAttachment",
			"UseManagedDepthStencilAttachment",
			"UseManagedTexture",
		};
		for (const std::string_view Method : ManualUses)
		{
			for (const std::string_view Access : {".", "->"})
			{
				const std::string MemberCall = std::string(Access)
											   + std::string(Method) + "(";
				if (Source.find(MemberCall) != std::string_view::npos)
					Violations.push_back(std::string(DisplayPath) + ": manual " + std::string(Method));
			}
		}

		for (const std::string_view AddPass : {".AddPass(", "->AddPass("})
		{
			size_t Position = Source.find(AddPass);
			while (Position != std::string_view::npos)
			{
				const size_t OpenParenthesis = Position + AddPass.size() - 1;
				if (!HasMovedParameterArgument(Source, OpenParenthesis))
					Violations.push_back(std::string(DisplayPath) + ": AddPass without a moved parameter object");
				Position = Source.find(AddPass, OpenParenthesis + 1);
			}
		}
		return Violations;
	}
} // namespace

template<typename T>
concept CHasResolvedReceiver = requires(T Value) { Value.ResolvedReceiver; };

TEST(FRendererSceneContractTests, ProductionRendererRequiresParameterizedRenderGraphAuthoring)
{
	EXPECT_TRUE(FindManualRenderGraphAuthoring(
					"Graph.AddPass(\"Parameterized\", Type, std::move(Parameters), Callback);",
					"seeded-valid"
	)
					.empty());
	const auto SeededViolations = FindManualRenderGraphAuthoring(
		"Graph.AddPass(\"Manual\", Type); Graph.UseTexture(Pass, Texture);",
		"seeded-invalid"
	);
	ASSERT_EQ(SeededViolations.size(), 2u);

	const std::filesystem::path SourceRoot(DURIN_RENDERER_SOURCE_DIR);
	ASSERT_TRUE(std::filesystem::is_directory(SourceRoot));
	std::vector<std::filesystem::path> Sources;
	for (const auto& Entry : std::filesystem::recursive_directory_iterator(
			 SourceRoot
		 ))
	{
		if (!Entry.is_regular_file()) continue;
		const auto Extension = Entry.path().extension();
		if (Extension == ".cpp" || Extension == ".h")
			Sources.push_back(Entry.path());
	}
	std::ranges::sort(Sources);
	std::vector<std::string> Violations;
	for (const auto& Path : Sources)
	{
		std::ifstream Stream(Path, std::ios::binary);
		ASSERT_TRUE(Stream) << Path;
		const std::string Source((std::istreambuf_iterator<char>(Stream)), {});
		const auto Relative = std::filesystem::relative(Path, SourceRoot)
								  .generic_string();
		auto FileViolations = FindManualRenderGraphAuthoring(Source, Relative);
		Violations.insert(Violations.end(), FileViolations.begin(), FileViolations.end());
	}
	EXPECT_TRUE(Violations.empty()) << ::testing::PrintToString(Violations);
}

template<typename T>
concept CHasResolvedDirectionalShadow = requires(T Value) {
	Value.ResolvedDirectionalShadow;
};

template<typename T>
concept CHasTelemetry = requires(T Value) { Value.Telemetry; };

template<typename T>
concept CHasResolvedTargets = requires(T Value) { Value.Targets; };

template<typename T>
concept CHasDeferredParameters = requires(T Value) { Value.DeferredParameters; };

template<typename T>
concept CHasReadyDrawHash = requires(T Value) { Value.ReadyDraws; };

template<typename T>
concept CHasMaterialBindingHash = requires(T Value) { Value.MaterialBindings; };

template<typename T>
concept CHasResolvedDrawRecords = requires(T Value) { Value.Draws; };

template<typename T>
concept CHasRenderObservations = requires(T Value) { Value.Observations; };

template<typename T>
concept CHasDirectExecutionCounters = requires(T Value) {
	Value.AttemptedDraws;
	Value.SuccessfulDraws;
	Value.RejectedDraws;
};

TEST(FRendererSceneContractTests, MeshPipelineDescriptionsCollectAllMaterialPassStatesWithoutNativeResources)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	constexpr auto Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::LineList;
	for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
		for (const auto Policy : {EMaterialDepthWritePolicy::Automatic, EMaterialDepthWritePolicy::Enabled, EMaterialDepthWritePolicy::Disabled})
		{
			FMaterialPlanningPassIdentity Material;
			Material.ShaderMap.BlendMode = Blend;
			Material.DepthWritePolicy = Policy;
			const auto State = MakeMeshPipelineState(Material, {.bWireframe = true, .bReverseCulling = true, .bReversedZ = true});
			EXPECT_EQ(State.Rasterizer.PolygonMode, ERHIPolygonMode::Line);
			EXPECT_EQ(State.Rasterizer.CullMode, ERHICullMode::Back);
			EXPECT_EQ(State.Rasterizer.FrontFace, ERHIFrontFace::CounterClockwise);
			EXPECT_EQ(State.Depth.CompareOp, ERHIDepthCompareOp::GreaterOrEqual);
			EXPECT_EQ(State.Depth.bEnableWrite, Policy == EMaterialDepthWritePolicy::Enabled
				|| (Policy == EMaterialDepthWritePolicy::Automatic && Blend != EMaterialBlendMode::Translucent));
			for (const auto Pass : {EMeshPipelinePass::Forward, EMeshPipelinePass::HybridRetainedForward,
				EMeshPipelinePass::SortedTranslucency, EMeshPipelinePass::ShadowDepth, EMeshPipelinePass::GBuffer, EMeshPipelinePass::HitProxy})
			{
				SCOPED_TRACE(std::format("blend={},policy={},pass={}", uint8(Blend), uint8(Policy), uint8(Pass)));
				const auto Description = DescribeMeshPipeline(Pass, State, Topology, Blend, true);
				const auto Initializer = Description.MakeInitializer(nullptr, nullptr, nullptr, {});
				EXPECT_EQ(Initializer.BoundShaders.VertexShader, nullptr);
				EXPECT_EQ(Initializer.VertexDeclaration, nullptr);
				EXPECT_EQ(Initializer.PrimitiveTopology, Topology);
				EXPECT_TRUE(Initializer.RenderTargetLayout.bHasDepthStencil);
				if (Pass == EMeshPipelinePass::ShadowDepth)
				{
					EXPECT_EQ(Initializer.RenderTargetLayout.NumColorRenderTargets, 0u);
					EXPECT_EQ(Initializer.RenderTargetLayout.DepthStencilAttachment.Format, EPixelFormat::D32);
					EXPECT_EQ(Initializer.RasterizerState.PolygonMode, ERHIPolygonMode::Fill);
					EXPECT_TRUE(Initializer.RasterizerState.bEnableDepthBias);
					EXPECT_TRUE(Initializer.DepthStencilState.bEnableWrite);
					EXPECT_EQ(Initializer.DepthStencilState.CompareOp, ERHIDepthCompareOp::Less);
				}
				else if (Pass == EMeshPipelinePass::HitProxy)
				{
					EXPECT_EQ(Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format, FHitProxyPixel::Format);
					EXPECT_EQ(Initializer.RasterizerState.PolygonMode, ERHIPolygonMode::Fill);
					EXPECT_TRUE(Initializer.DepthStencilState.bEnableWrite);
					EXPECT_FALSE(Initializer.ColorBlendStates[0].bEnable);
				}
				else
				{
					EXPECT_EQ(Initializer.DepthStencilState, State.Depth);
					EXPECT_EQ(Initializer.RasterizerState, State.Rasterizer);
					EXPECT_EQ(Initializer.RenderTargetLayout.NumColorRenderTargets, Pass == EMeshPipelinePass::GBuffer ? 4u : 1u);
					EXPECT_EQ(Initializer.ColorBlendStates[0].bEnable, Pass != EMeshPipelinePass::GBuffer && Blend == EMaterialBlendMode::Translucent);
					if (Pass == EMeshPipelinePass::HybridRetainedForward || Pass == EMeshPipelinePass::SortedTranslucency)
						EXPECT_EQ(Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget.LoadAction, ERHIRenderTargetLoadAction::Load);
				}
			}
		}
}

TEST(FRendererSceneContractTests, MeshPipelineDescriptionsNormalizeShadowIdentityAndPreserveMirroredCulling)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	FMaterialPlanningPassIdentity Material;
	Material.bTwoSided = true;
	EXPECT_EQ(MakeMeshPipelineState(Material, {}).Rasterizer.CullMode, ERHICullMode::None);
	Material.bTwoSided = false;
	EXPECT_EQ(MakeMeshPipelineState(Material, {.bOrientationChanging = true}).Rasterizer.CullMode, ERHICullMode::None);
	FEffectiveMeshPipelineKey Source;
	Source.Rasterizer = MakeMeshPipelineState(Material, {.bReverseCulling = true}).Rasterizer;
	Source.Rasterizer.bEnableDepthBias = true;
	Source.Rasterizer.DepthBiasConstantFactor = 7.f;
	Source.Rasterizer.DepthBiasSlopeFactor = 3.f;
	Source.Rasterizer.DepthBiasClamp = 2.f;
	const auto Key = MakeShadowPipelineKey(Source);
	const auto Description = DescribeMeshPipeline(EMeshPipelinePass::ShadowDepth,
		{Source.Rasterizer, Source.Depth, Source.ColorBlend}, Source.Topology);
	EXPECT_EQ(Description.Rasterizer, Key.Rasterizer);
	EXPECT_EQ(Description.Depth, Key.Depth);
	EXPECT_EQ(Description.Rasterizer.FrontFace, ERHIFrontFace::CounterClockwise);
	EXPECT_EQ(Description.Rasterizer.DepthBiasConstantFactor, 0.f);
	EXPECT_EQ(Description.Rasterizer.DepthBiasSlopeFactor, 0.f);
	EXPECT_EQ(Description.Rasterizer.DepthBiasClamp, 0.f);
	const auto HitProxy = DescribeMeshPipeline(EMeshPipelinePass::HitProxy, {}, Source.Topology);
	EXPECT_EQ(HitProxy.Depth.CompareOp, ERHIDepthCompareOp::LessOrEqual);
}

TEST(FRendererSceneContractTests, MaterialProgramIdentityParticipatesInRendererKeys)
{
	Durin::FEffectiveMeshPipelineKey FirstPipeline;
	FirstPipeline.Material.ShaderMap.RenderLayout =
		Durin::MakeErrorMaterialRenderLayout().Identity;
	FirstPipeline.Material.ShaderMap.ProgramIdentity.Digest.HashLow =
		0x0123456789abcdefull;
	FirstPipeline.Material.ShaderMap.ProgramIdentity.Digest.HashHigh =
		0xfedcba9876543210ull;
	auto SecondPipeline = FirstPipeline;
	SecondPipeline.Material.ShaderMap.ProgramIdentity.Digest.HashLow =
		0x1123456789abcdefull;
	const Durin::FMaterialRenderRepresentation Representation =
		Durin::FMaterialRenderRepresentation{};
	const Durin::FVertexDeclarationElementList Elements{};
	const std::array<uint32, 6> Geometry{};
	const auto First = Durin::RendererPrivate::MakeMeshDrawSortKey(
		Durin::EMeshBasePass::Opaque, FirstPipeline, Representation,
		0, Elements, Geometry, 1, 0, 0
	);
	const auto Second = Durin::RendererPrivate::MakeMeshDrawSortKey(
		Durin::EMeshBasePass::Opaque, SecondPipeline, Representation,
		0, Elements, Geometry, 1, 0, 0
	);
	EXPECT_NE(First.Pipeline, Second.Pipeline);
	EXPECT_NE(Durin::RendererPrivate::GetIdentityText(FirstPipeline.Material.ShaderMap), Durin::RendererPrivate::GetIdentityText(SecondPipeline.Material.ShaderMap));
}

template<typename T>
concept CHasUploadRange = requires(T Value) { Value.Range; };

template<typename T>
concept CHasPublicQualificationSwitches = requires(T Value) {
	Value.bEnableGBufferQualification;
	Value.bEnableDeferredDirectionalQualification;
	Value.bEnableGroundTruthAmbientOcclusionQualification;
	Value.bForceFragmentContactVisibility;
};

template<typename T>
concept CHasPreparedView = requires(T Value) { Value.PreparedView; };

template<typename T>
concept CHasGenericSceneProxyMetadata = requires(const T& Value) {
	Value.GetMetadata();
};

static_assert(!CHasGenericSceneProxyMetadata<Durin::FLightSceneProxy>);
static_assert(!CHasGenericSceneProxyMetadata<Durin::FSkyBoxSceneProxy>);
static_assert(!CHasGenericSceneProxyMetadata<Durin::FVolumetricCloudSceneProxy>);

template<auto AddPasses, typename TInputs>
concept CAcceptsFeatureInputs = requires(
	Durin::FRDGBuilder& Graph, const TInputs& Inputs
) {
	AddPasses(Graph, Inputs);
};

template<auto AddPasses, typename TInputs, typename TOutput>
concept CFeatureReturns = requires(Durin::FRDGBuilder& Graph, const TInputs& Inputs) {
	{ AddPasses(Graph, Inputs) } -> std::same_as<TOutput>;
};

static_assert(!std::is_copy_constructible_v<Durin::FSceneViewStateOwner>);
static_assert(!std::is_copy_assignable_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_move_constructible_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_move_assignable_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_final_v<Durin::FSceneRenderPipeline>);
static_assert(!std::is_empty_v<Durin::FSceneRenderPipeline>);
static_assert(!std::is_copy_constructible_v<Durin::FSceneRenderPipeline>);
static_assert(!std::is_copy_assignable_v<Durin::FSceneRenderPipeline>);
static_assert(!std::is_move_constructible_v<Durin::FSceneRenderPipeline>);
static_assert(!std::is_move_assignable_v<Durin::FSceneRenderPipeline>);
static_assert(std::is_default_constructible_v<Durin::FSceneFrameContext>);
static_assert(!std::is_copy_constructible_v<Durin::FSceneRenderer>);
static_assert(!std::is_move_constructible_v<Durin::FSceneRenderer>);
static_assert(std::is_same_v<decltype(&Durin::FSceneRenderer::Render),
	void (Durin::FSceneRenderer::*)(Durin::FRDGBuilder&)>);
static_assert(!CHasPublicQualificationSwitches<
			  Durin::FSceneViewRenderOptions>);
static_assert(!std::is_copy_constructible_v<
			  Durin::FScopedRendererQualificationPolicy>);
static_assert(!CHasResolvedReceiver<Durin::FSceneRenderPlan>);
static_assert(!CHasResolvedDirectionalShadow<Durin::FSceneRenderPlan>);
static_assert(!CHasTelemetry<Durin::FSceneRenderPlan>);
static_assert(!CHasResolvedTargets<Durin::FSceneRenderPlan>);
static_assert(!CHasResolvedTargets<Durin::FResolvedSceneResources>);
static_assert(!CHasDeferredParameters<Durin::FSceneFrameContext::FTransaction>);
static_assert(std::is_copy_constructible_v<Durin::FSceneFrameFeaturePlan>);
static_assert(CAcceptsFeatureInputs<
			  Durin::AddDirectionalShadowPasses,
			  Durin::FDirectionalShadowFeatureInputs>);
static_assert(CAcceptsFeatureInputs<
			  Durin::AddGBufferPasses,
			  Durin::FGBufferFeatureInputs>);
static_assert(std::is_standard_layout_v<Durin::FGBufferPassParameters>);
static_assert(Durin::CRDGParameters<Durin::FGBufferPassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FDirectionalShadowPassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FAmbientOcclusionPassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FContactShadowGraphicsPassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FContactShadowComputePassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FVolumetricCloudShadowPassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FDeferredDirectionalLightingPassParameters>);
static_assert(Durin::CRDGParameters<Durin::FBaseScenePassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FVolumetricCloudSpatialPassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FVolumetricCloudCompositePassParameters>);
static_assert(Durin::CRDGParameters<Durin::FSceneColorPassParameters>);
static_assert(Durin::CRDGParameters<Durin::FPostProcessPassParameters>);
static_assert(Durin::CRDGParameters<
			  Durin::FEditorAssistancePassParameters>);
static_assert(CAcceptsFeatureInputs<
			  Durin::AddVolumetricCloudShadowPasses,
			  Durin::FCloudShadowFeatureInputs>);
static_assert(CAcceptsFeatureInputs<
			  Durin::AddBaseScenePasses,
			  Durin::FBaseSceneFeatureInputs>);
static_assert(CFeatureReturns<Durin::AddDirectionalShadowPasses, Durin::FDirectionalShadowFeatureInputs, Durin::FDirectionalShadowGraphOutput>);
static_assert(CFeatureReturns<Durin::AddGBufferPasses, Durin::FGBufferFeatureInputs, Durin::FGBufferGraphOutput>);
static_assert(CFeatureReturns<Durin::AddAmbientOcclusionPasses, Durin::FAmbientOcclusionFeatureInputs, Durin::FAmbientOcclusionGraphOutput>);
static_assert(CFeatureReturns<
			  Durin::AddContactShadowVisibilityPasses,
			  Durin::FContactShadowFeatureInputs,
			  Durin::FContactShadowGraphOutput>);
static_assert(CFeatureReturns<
			  Durin::AddVolumetricCloudShadowPasses,
			  Durin::FCloudShadowFeatureInputs,
			  Durin::FCloudShadowGraphOutput>);
static_assert(CFeatureReturns<
			  Durin::AddDeferredDirectionalLightingPasses,
			  Durin::FDeferredLightingFeatureInputs,
			  Durin::FDeferredLightingGraphOutput>);
static_assert(CFeatureReturns<Durin::AddBaseScenePasses, Durin::FBaseSceneFeatureInputs, Durin::FBaseSceneGraphOutput>);
static_assert(CFeatureReturns<
			  Durin::AddVolumetricCloudSpatialPasses,
			  Durin::FCloudSpatialFeatureInputs,
			  Durin::FCloudSpatialGraphOutput>);
static_assert(CFeatureReturns<
			  Durin::AddVolumetricCloudCompositePasses,
			  Durin::FCloudCompositeFeatureInputs,
			  Durin::FCloudCompositeGraphOutput>);
static_assert(CFeatureReturns<Durin::AddSceneColorPasses, Durin::FSceneColorFeatureInputs, Durin::FSceneColorGraphOutput>);
static_assert(CFeatureReturns<Durin::AddPostProcessPasses, Durin::FPostProcessFeatureInputs, Durin::FPostProcessGraphOutput>);
static_assert(CFeatureReturns<Durin::AddEditorAssistancePasses, Durin::FEditorAssistanceFeatureInputs, void>);
static_assert(!CHasPreparedView<Durin::FDirectionalShadowFeatureInputs>);
static_assert(!CHasPreparedView<Durin::FPostProcessFeatureInputs>);
static_assert(!CAcceptsFeatureInputs<
			  Durin::AddDirectionalShadowPasses,
			  Durin::FSceneRenderPlan>);
static_assert(CHasResolvedDrawRecords<Durin::FResolvedStaticMeshView>);
static_assert(!CHasReadyDrawHash<Durin::FResolvedStaticMeshView>);
static_assert(!CHasMaterialBindingHash<Durin::FResolvedStaticMeshView>);
static_assert(CHasRenderObservations<Durin::FResolvedStaticMeshView>);
static_assert(!CHasDirectExecutionCounters<Durin::FResolvedStaticMeshView>);
static_assert(static_cast<uint8>(Durin::ERDGAllocationObservation::Count) == 16);

TEST(FRendererSceneContractTests, RDGAllocationPolicyKeepsStructuralBoundary)
{
	using FPolicy = Durin::FRendererRDGAllocationPolicy;
	EXPECT_TRUE(FPolicy::IsBatchWithinStructuralBudget(
		FPolicy::MaximumRetainedBytes
	));
	EXPECT_FALSE(FPolicy::IsBatchWithinStructuralBudget(
		FPolicy::MaximumRetainedBytes + 1
	));
}

TEST(FRendererSceneContractTests, QualificationPolicyIsLexicallyScoped)
{
	EXPECT_FALSE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
	{
		Durin::FScopedRendererQualificationPolicy Outer({.bEnableGBuffer = true});
		EXPECT_TRUE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
		{
			Durin::FScopedRendererQualificationPolicy Inner({.bForceFragmentVolumetricCloud = true, .RasterFailure = Durin::ESceneRasterFailure::HybridBootstrap});
			const auto InnerPolicy =
				Durin::GetRendererQualificationPolicy();
			EXPECT_FALSE(InnerPolicy.bEnableGBuffer);
			EXPECT_TRUE(InnerPolicy.bForceFragmentVolumetricCloud);
			EXPECT_EQ(InnerPolicy.RasterFailure, Durin::ESceneRasterFailure::HybridBootstrap);
		}
		EXPECT_TRUE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
	}
	EXPECT_FALSE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
	EXPECT_EQ(Durin::GetRendererQualificationPolicy().RasterFailure, Durin::ESceneRasterFailure::None);
}

TEST(FRendererSceneContractTests, TypedPassResultsSeparateGraphOwnedResources)
{
	Durin::FGBufferPassResult GBuffer;
	Durin::FGroundTruthAmbientOcclusionPassResult AmbientOcclusion;
	Durin::FContactShadowVisibilityPassResult ContactShadow;
	Durin::FVolumetricCloudShadowPassResult CloudShadow;
	Durin::FSceneColorPassResult SceneColor;
	EXPECT_FALSE(GBuffer.IsComplete());
	EXPECT_FALSE(AmbientOcclusion.IsComplete());
	EXPECT_FALSE(ContactShadow.IsComplete());
	EXPECT_FALSE(CloudShadow.IsComplete());
	EXPECT_FALSE(SceneColor.IsSuccess());
	GBuffer.Status = Durin::EScenePassStatus::Complete;
	AmbientOcclusion.Status = Durin::EScenePassStatus::Complete;
	ContactShadow.Status = Durin::EScenePassStatus::Complete;
	CloudShadow.Status = Durin::EScenePassStatus::Complete;
	SceneColor.Result = Durin::ERenderViewResult::Success;
	EXPECT_TRUE(GBuffer.IsComplete());
	EXPECT_TRUE(AmbientOcclusion.IsComplete());
	EXPECT_FALSE(ContactShadow.IsComplete());
	EXPECT_FALSE(CloudShadow.IsComplete());
	EXPECT_TRUE(SceneColor.IsSuccess());
	ContactShadow.Route = Durin::EContactShadowVisibilityPassRoute::Compute;
	EXPECT_TRUE(ContactShadow.IsComplete());
	CloudShadow.Route = Durin::EVolumetricCloudShadowPassRoute::Fragment;
	EXPECT_TRUE(CloudShadow.IsComplete());
}

namespace
{
	// Keep typed-value identities and parameter metadata in the same module.
	struct FOptionalVisibilityTestParameters final
	{
		std::optional<Durin::TRDGValueRead<Durin::FGroundTruthAmbientOcclusionPassResult>> AmbientOcclusion;
		std::optional<Durin::TRDGValueRead<Durin::FContactShadowVisibilityPassResult>> ContactShadow;
		std::optional<Durin::TRDGValueRead<Durin::FVolumetricCloudShadowPassResult>> CloudShadow;

		static auto GetRDGParametersMetadata() -> const Durin::FRDGParametersMetadata*
		{
			using namespace Durin;
			using FParameters = FOptionalVisibilityTestParameters;
			static const std::array Members{
				MakeRDGValueParameterMemberMetadata<FParameters,
					decltype(AmbientOcclusion), FGroundTruthAmbientOcclusionPassResult>(
						"AmbientOcclusion", offsetof(FParameters, AmbientOcclusion)),
				MakeRDGValueParameterMemberMetadata<FParameters,
					decltype(ContactShadow), FContactShadowVisibilityPassResult>(
						"ContactShadow", offsetof(FParameters, ContactShadow)),
				MakeRDGValueParameterMemberMetadata<FParameters,
					decltype(CloudShadow), FVolumetricCloudShadowPassResult>(
						"CloudShadow", offsetof(FParameters, CloudShadow))};
			static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>(
				"FOptionalVisibilityTestParameters", Members);
			return &Metadata;
		}
	};
}

// Only declared reads may retain optional producers, including failed results.
TEST(FRendererSceneContractTests, OptionalVisibilityResultsRetainOnlyRequestedProducers)
{
	using namespace Durin;
	const auto* DeferredMetadata = FDeferredLightingInputParameters::GetRDGParametersMetadata();
	for (const auto Name : {"AmbientOcclusion", "ContactShadow", "CloudShadow"})
	{
		const auto Member = std::ranges::find_if(DeferredMetadata->Members,
			[&](const auto& Candidate) { return std::string_view(Candidate.Name) == Name; });
		ASSERT_NE(Member, DeferredMetadata->Members.end());
		EXPECT_TRUE(Member->bOptional);
		EXPECT_EQ(Member->Kind, ERDGParameterMemberKind::ValueRead);
	}
	for (const auto Status : {EScenePassStatus::NotRequested,
		EScenePassStatus::Complete, EScenePassStatus::Failed})
	{
		FRHICommandListExecutor Executor;
		FRDGBuilder Graph;
		Graph.EnablePassCulling();
		const auto Ambient = Graph.CreateValue<FGroundTruthAmbientOcclusionPassResult>(
			"Ambient", "ambient-occlusion-result");
		const auto Contact = Graph.CreateValue<FContactShadowVisibilityPassResult>(
			"Contact", "contact-shadow-result");
		const auto Cloud = Graph.CreateValue<FVolumetricCloudShadowPassResult>(
			"Cloud", "cloud-shadow-result");
		uint32 ProducerCalls = 0;
		const auto Visibility = FRDGBuilderTestAccessor::AddPass(Graph, "Visibility", ERDGPassType::Graphics,
			[&](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
				++ProducerCalls;
				Resources.WriteValue(Ambient).Status = Status;
				Resources.WriteValue(Contact).Status = Status;
				Resources.WriteValue(Cloud).Status = Status;
			});
		FRDGBuilderTestAccessor::UseValue(Graph, Visibility, Ambient, ERDGUse::Write);
		FRDGBuilderTestAccessor::UseValue(Graph, Visibility, Contact, ERDGUse::Write);
		FRDGBuilderTestAccessor::UseValue(Graph, Visibility, Cloud, ERDGUse::Write);
		auto Parameters = Graph.AllocParameters<FOptionalVisibilityTestParameters>();
		const bool bRequested = Status != EScenePassStatus::NotRequested;
		if (bRequested)
		{
			Parameters->AmbientOcclusion = TRDGValueRead<FGroundTruthAmbientOcclusionPassResult>{Ambient};
			Parameters->ContactShadow = TRDGValueRead<FContactShadowVisibilityPassResult>{Contact};
			Parameters->CloudShadow = TRDGValueRead<FVolumetricCloudShadowPassResult>{Cloud};
		}
		uint32 ConsumerCalls = 0;
		const auto Consumer = Graph.AddPass("Deferred", ERDGPassType::Graphics,
			std::move(Parameters), [&](FRHICommandListImmediate&,
				const FOptionalVisibilityTestParameters& Pass,
				const FRDGParameterResolver& Resolver) {
				++ConsumerCalls;
				const auto CheckResult = [&](const auto& Field) {
					const auto* Result = Resolver.ReadValue(Field);
					EXPECT_EQ(Result != nullptr, bRequested);
					EXPECT_EQ(Result ? Result->Status : EScenePassStatus::NotRequested, Status);
				};
				CheckResult(Pass.AmbientOcclusion);
				CheckResult(Pass.ContactShadow);
				CheckResult(Pass.CloudShadow);
			});
		Graph.MarkPassRoot(Consumer, "test output");
		const auto Execution = Graph.Execute(Executor.GetImmediateCommandList());
		ASSERT_TRUE(Execution.has_value()) << ToString(Execution.error());
		EXPECT_EQ(ProducerCalls, bRequested ? 1u : 0u);
		EXPECT_EQ(ConsumerCalls, 1u);
		EXPECT_EQ(Graph.GetStatistics().ScheduledPasses, bRequested ? 2u : 1u);
	}
}

namespace
{
	struct FDeferredResourceTestParameters final
	{
		Durin::TRDGValueRead<Durin::FDirectionalShadowPassResult> DirectionalShadow;
		std::optional<Durin::TRDGValueRead<Durin::FGBufferPassResult>> GBufferCompletion;
		std::optional<Durin::TRDGValueRead<Durin::FGroundTruthAmbientOcclusionPassResult>> AmbientOcclusion;
		std::optional<Durin::TRDGValueRead<Durin::FContactShadowVisibilityPassResult>> ContactShadow;
		std::optional<Durin::TRDGValueRead<Durin::FVolumetricCloudShadowPassResult>> CloudShadow;
		Durin::FDeferredDirectionalLightingPassResources Resources;
		static auto GetRDGParametersMetadata() -> const Durin::FRDGParametersMetadata*
		{
			using namespace Durin;
			using FParameters = FDeferredResourceTestParameters;
			#define DURIN_TEST_VALUE(Field, Type) MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::Field), Type>(#Field, offsetof(FParameters, Field))
			static const std::array Members{
				DURIN_TEST_VALUE(DirectionalShadow, FDirectionalShadowPassResult),
				DURIN_TEST_VALUE(GBufferCompletion, FGBufferPassResult),
				DURIN_TEST_VALUE(AmbientOcclusion, FGroundTruthAmbientOcclusionPassResult),
				DURIN_TEST_VALUE(ContactShadow, FContactShadowVisibilityPassResult),
				DURIN_TEST_VALUE(CloudShadow, FVolumetricCloudShadowPassResult),
				MakeRDGNestedParameterMemberMetadata<FParameters, decltype(Resources)>("Resources", offsetof(FParameters, Resources), FDeferredDirectionalLightingPassResources::GetRDGParametersMetadata())};
			#undef DURIN_TEST_VALUE
			static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>("FDeferredResourceTestParameters", Members);
			return &Metadata;
		}
	};
}

TEST(FRendererSceneContractTests, DeferredLightingResolvesDeclaredAliasesAndFailedOptionalProducersLocally)
{
	using namespace Durin;
	for (const bool bProduction : {false, true})
		for (const bool bOptionalProducers : {false, true})
		{
			FRHICommandListExecutor Executor;
			FRDGBuilder Graph;
			Graph.EnablePassCulling();
			std::vector<FTextureRHIRef> Textures;
			const auto Import = [&](const char* Name, bool bDepth = false) {
				auto Texture = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(Name, 8, 8,
					bDepth ? EPixelFormat::D32 : EPixelFormat::RGBA8_UNORM)
					.SetFlags(ETextureCreateFlags::ShaderResource));
				Textures.push_back(Texture);
				return Graph.RegisterExternalTexture(Texture, Name, ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			};
			auto Parameters = Graph.AllocParameters<FDeferredResourceTestParameters>();
			for (size_t Index = 0; Index < 4; ++Index)
			{
				const auto Name = "GBuffer" + std::to_string(Index);
				Parameters->Resources.GBuffer[Index] = {Import(Name.c_str()), {ERHITextureAspect::Color, 0, 1, 0, 1}};
			}
			Parameters->Resources.SceneDepth = {Import("Depth", true), {ERHITextureAspect::Depth, 0, 1, 0, 1}};
			const auto White = Import("White");
			const auto ShadowArray = Import("ShadowArray");
			const auto Cube = Import("CubeFallback");
			const auto Black = Import("Black");
			Parameters->Resources.DefaultWhite = {White, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.DefaultShadowArray = {ShadowArray, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.EnvironmentIrradiance = {Cube, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.EnvironmentBrdfLut = {Black, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			const auto Shadow = Graph.CreateValue<FDirectionalShadowPassResult>("Shadow", "directional-shadow-result");
			const auto GBuffer = Graph.CreateValue<FGBufferPassResult>("GBuffer", "gbuffer-result");
			const auto Ambient = Graph.CreateValue<FGroundTruthAmbientOcclusionPassResult>("Ambient", "ambient-occlusion-result");
			const auto Contact = Graph.CreateValue<FContactShadowVisibilityPassResult>("Contact", "contact-shadow-result");
			const auto Cloud = Graph.CreateValue<FVolumetricCloudShadowPassResult>("Cloud", "cloud-shadow-result");
			const auto Required = FRDGBuilderTestAccessor::AddPass(Graph, "Required", ERDGPassType::Graphics,
				[&](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
					Resources.WriteValue(Shadow).Status = EScenePassStatus::Failed;
					Resources.WriteValue(GBuffer).Status = EScenePassStatus::Complete;
				});
			FRDGBuilderTestAccessor::UseValue(Graph, Required, Shadow, ERDGUse::Write);
			FRDGBuilderTestAccessor::UseValue(Graph, Required, GBuffer, ERDGUse::Write);
			uint32 OptionalCalls = 0;
			const auto Optional = FRDGBuilderTestAccessor::AddPass(Graph, "Optional", ERDGPassType::Graphics,
				[&](FRHICommandListImmediate&, const FRDGPassResources& Resources) {
					++OptionalCalls;
					Resources.WriteValue(Ambient).Status = EScenePassStatus::Failed;
					Resources.WriteValue(Contact).Status = EScenePassStatus::Failed;
					Resources.WriteValue(Cloud).Status = EScenePassStatus::Failed;
				});
			FRDGBuilderTestAccessor::UseValue(Graph, Optional, Ambient, ERDGUse::Write);
			FRDGBuilderTestAccessor::UseValue(Graph, Optional, Contact, ERDGUse::Write);
			FRDGBuilderTestAccessor::UseValue(Graph, Optional, Cloud, ERDGUse::Write);
			Parameters->DirectionalShadow = {Shadow};
			Parameters->GBufferCompletion = TRDGValueRead<FGBufferPassResult>{GBuffer};
			if (bOptionalProducers)
			{
				Parameters->AmbientOcclusion = TRDGValueRead<FGroundTruthAmbientOcclusionPassResult>{Ambient};
				Parameters->ContactShadow = TRDGValueRead<FContactShadowVisibilityPassResult>{Contact};
				Parameters->CloudShadow = TRDGValueRead<FVolumetricCloudShadowPassResult>{Cloud};
			}
			const FDeferredLightingPolicy Policy{.PersistentAliases = {std::nullopt, White, ShadowArray, Cube, Cube, Black},
				.bProduction = bProduction};
			const auto Consumer = Graph.AddPass("Lighting", ERDGPassType::Graphics, std::move(Parameters),
				[&](FRHICommandListImmediate&, const FDeferredResourceTestParameters& Pass, const FRDGParameterResolver& Resolver) {
					FSceneView View;
					FSceneViewRenderOptions Options;
					Options.DeferredDirectionalDebugMode = EDeferredDirectionalDebugMode::Disabled;
					const FDeferredLightingOutcomes Outcomes{
						.DirectionalShadow = Resolver.ReadValue(Pass.DirectionalShadow),
						.GBuffer = *Resolver.ReadValue(Pass.GBufferCompletion),
						.AmbientOcclusion = bOptionalProducers ? *Resolver.ReadValue(Pass.AmbientOcclusion) : FGroundTruthAmbientOcclusionPassResult{},
						.ContactShadow = bOptionalProducers ? *Resolver.ReadValue(Pass.ContactShadow) : FContactShadowVisibilityPassResult{},
						.CloudShadow = bOptionalProducers ? *Resolver.ReadValue(Pass.CloudShadow) : FVolumetricCloudShadowPassResult{}};
					const auto Physical = ResolveDeferredLightingResources(Resolver, Pass.Resources, Outcomes, Policy, View, Options, {});
					ASSERT_TRUE(Physical);
					EXPECT_EQ(Physical->Material, Textures[0].GetReference());
					EXPECT_EQ(Physical->Depth, Textures[4].GetReference());
					EXPECT_EQ(Physical->EnvironmentIrradiance, Textures[7].GetReference());
					EXPECT_EQ(Physical->EnvironmentPrefiltered, Physical->EnvironmentIrradiance);
					EXPECT_EQ(Physical->EnvironmentBrdfLut, Textures[8].GetReference());
					EXPECT_EQ(Physical->DirectionalShadowTexture, Textures[6].GetReference());
					EXPECT_EQ(Physical->ContactVisibility, Textures[5].GetReference());
					EXPECT_EQ(Physical->VolumetricCloudVisibility, Physical->ContactVisibility);
					EXPECT_EQ(Physical->GroundTruthAmbientOcclusionResolved, Physical->ContactVisibility);
					EXPECT_FALSE(Physical->bGroundTruthAmbientOcclusionEnabled);
					EXPECT_FALSE(Physical->bContactVisibilityEnabled);
					EXPECT_FALSE(Physical->bVolumetricCloudVisibilityEnabled);
					auto FailedGBuffer = Outcomes;
					FailedGBuffer.GBuffer.Status = EScenePassStatus::Failed;
					EXPECT_FALSE(ResolveDeferredLightingResources(Resolver, Pass.Resources, FailedGBuffer, Policy, View, Options, {}).has_value());
				});
			Graph.MarkPassRoot(Consumer);
			const auto Execution = Graph.Execute(Executor.GetImmediateCommandList());
			ASSERT_TRUE(Execution) << ToString(Execution.error());
			EXPECT_EQ(OptionalCalls, bOptionalProducers ? 1u : 0u);
		}
}

TEST(FRendererSceneContractTests, ContactShadowPilotsComposeExactGraphicsAndComputeShaderAuthority)
{
	const auto* Graphics = Durin::FContactShadowGraphicsPassParameters::
		GetRDGParametersMetadata();
	const auto* Compute = Durin::FContactShadowComputePassParameters::
		GetRDGParametersMetadata();
	ASSERT_NE(Graphics, nullptr);
	ASSERT_NE(Compute, nullptr);
	ASSERT_EQ(Graphics->Members.size(), 9u);
	ASSERT_EQ(Compute->Members.size(), 9u);
	for (size_t Index = 3; Index < 8; ++Index)
	{
		EXPECT_TRUE(Graphics->Members[Index].bShaderBinding);
		EXPECT_TRUE(Compute->Members[Index].bShaderBinding);
		EXPECT_EQ(Graphics->Members[Index].ShaderBindingType, Durin::ERHIBindingType::Texture);
		EXPECT_EQ(Compute->Members[Index].ShaderBindingType, Durin::ERHIBindingType::Texture);
		EXPECT_EQ(Graphics->Members[Index].Access, Durin::ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Compute->Members[Index].Access, Durin::ERHIAccess::ComputeShaderRead);
	}
	EXPECT_FALSE(Graphics->Members.back().bShaderBinding);
	EXPECT_EQ(Graphics->Members.back().Kind, Durin::ERDGParameterMemberKind::ColorAttachment);
	EXPECT_TRUE(Compute->Members.back().bShaderBinding);
	EXPECT_STREQ(Compute->Members.back().ShaderBindingName, "ContactVisibilityOutput");
	EXPECT_EQ(Compute->Members.back().ShaderBindingType, Durin::ERHIBindingType::StorageImage);
	EXPECT_EQ(Compute->Members.back().Use, Durin::ERDGUse::Write);
}

TEST(FRendererSceneContractTests, GraphWarningsDeduplicateAcrossAlternatingViewsAndHealthyFrames)
{
	Durin::FSceneRenderGraphWarnings Warnings;
	Durin::FRDGBudget Budget{.RegressionMaxTextureTransitions = 32};
	Durin::FRDGStatistics Statistics;
	Statistics.TextureTransitions = 42;
	Statistics.bTextureTransitionRegressionBudgetExceeded = true;
	EXPECT_TRUE(Warnings.ShouldReport(Statistics, Budget));
	EXPECT_FALSE(Warnings.ShouldReport(Statistics, Budget));
	++Statistics.TextureTransitions;
	EXPECT_TRUE(Warnings.ShouldReport(Statistics, Budget));
	EXPECT_FALSE(Warnings.ShouldReport({}, Budget));
	--Statistics.TextureTransitions;
	EXPECT_FALSE(Warnings.ShouldReport(Statistics, Budget));
	++Budget.RegressionMaxTextureTransitions;
	EXPECT_TRUE(Warnings.ShouldReport(Statistics, Budget));
	Durin::FSceneRenderGraphWarnings OtherRenderer;
	EXPECT_TRUE(OtherRenderer.ShouldReport(Statistics, Budget));
}

TEST(FRendererSceneContractTests, GraphWarningsBoundVaryingDiagnostics)
{
	Durin::FSceneRenderGraphWarnings Warnings;
	Durin::FRDGBudget Budget{.RegressionMaxPasses = 12};
	Durin::FRDGStatistics Statistics;
	Statistics.DeclaredPasses = 13;
	Statistics.bPassRegressionBudgetExceeded = true;
	for (uint32 Index = 0; Index < Durin::FSceneRenderGraphWarnings::MaxWarnings; ++Index)
	{
		EXPECT_FALSE(Warnings.IsFull());
		EXPECT_TRUE(Warnings.ShouldReport(Statistics, Budget));
		++Statistics.DeclaredPasses;
	}
	EXPECT_TRUE(Warnings.IsFull());
	EXPECT_FALSE(Warnings.ShouldReport(Statistics, Budget));
}

TEST(FRendererSceneContractTests, SceneFeaturePlanOwnsPurposesAndExactRoutes)
{
	Durin::FSceneFrameFeaturePlan Plan;
	EXPECT_FALSE(Plan.RequiresDeferredInputs());
	Plan.Deferred.Purposes = Durin::ESceneFeaturePurpose::Production
		| Durin::ESceneFeaturePurpose::Debug;
	EXPECT_TRUE(Plan.RequiresProductionDeferred());
	EXPECT_TRUE(Plan.RequiresIsolatedDeferred());
	EXPECT_TRUE(Plan.RequiresDeferredInputs());
	Plan.ContactVisibility.Decision.Route =
		Durin::FContactShadowVisibilityRenderer::ERoute::Fragment;
	Plan.CloudShadow.Decision.Route =
		Durin::FVolumetricCloudShadowRenderer::ERoute::Compute;
	Plan.CloudSpatial.Decision.Route =
		Durin::FVolumetricCloudRenderer::ERoute::Fragment;
	EXPECT_EQ(Plan.ContactVisibility.Decision.Route,
		Durin::FContactShadowVisibilityRenderer::ERoute::Fragment);
	EXPECT_EQ(Plan.CloudShadow.Decision.Route,
		Durin::FVolumetricCloudShadowRenderer::ERoute::Compute);
	EXPECT_EQ(Plan.CloudSpatial.Decision.Route,
		Durin::FVolumetricCloudRenderer::ERoute::Fragment);
}

TEST(FRendererSceneContractTests, FeatureScheduleOrderIsStableAndUnique)
{
	const std::array<std::string_view, 12> Names{
		Durin::DirectionalShadowPassName,
		Durin::GBufferPassName,
		Durin::AmbientOcclusionPassName,
		Durin::ContactShadowVisibilityPassName,
		Durin::VolumetricCloudShadowPassName,
		Durin::DeferredDirectionalLightingPassName,
		Durin::BaseScenePassName,
		Durin::VolumetricCloudSpatialPassName,
		Durin::VolumetricCloudCompositePassName,
		Durin::SceneColorPassName,
		Durin::PostProcessPassName,
		Durin::EditorAssistancePassName
	};
	EXPECT_EQ(Names.front(), "Scene.DirectionalShadow");
	EXPECT_EQ(Names.back(), "Scene.EditorAssistance");
	for (size_t Index = 0; Index < Names.size(); ++Index)
		for (size_t Other = Index + 1; Other < Names.size(); ++Other)
			EXPECT_NE(Names[Index], Names[Other]);
}

TEST(FRendererSceneContractTests, GBufferPassParametersOwnThePilotDeclarations)
{
	const Durin::FRDGParametersMetadata* Metadata =
		Durin::FGBufferPassParameters::GetRDGParametersMetadata();
	ASSERT_NE(Metadata, nullptr);
	EXPECT_STREQ(Metadata->StructName, "FGBufferPassParameters");
	EXPECT_EQ(Metadata->StructSize, sizeof(Durin::FGBufferPassParameters));
	ASSERT_EQ(Metadata->Members.size(), 6u);

	const auto& Completion = Metadata->Members[0];
	EXPECT_STREQ(Completion.Name, "Completion");
	EXPECT_EQ(Completion.Kind, Durin::ERDGParameterMemberKind::ValueWrite);
	EXPECT_EQ(Completion.ArraySize, 1u);
	EXPECT_FALSE(Completion.bOptional);
	EXPECT_EQ(Completion.Use, Durin::ERDGUse::Write);
	EXPECT_TRUE(Completion.bDiscard);

	const auto& Colors = Metadata->Members[1];
	EXPECT_STREQ(Colors.Name, "Colors");
	EXPECT_EQ(Colors.Kind, Durin::ERDGParameterMemberKind::ManagedColorAttachment);
	EXPECT_EQ(Colors.ArraySize, 4u);
	EXPECT_TRUE(Colors.bOptional);
	EXPECT_EQ(Colors.Use, Durin::ERDGUse::ReadWrite);
	EXPECT_EQ(Colors.Access, Durin::ERHIAccess::ColorAttachmentReadWrite);
	EXPECT_TRUE(Colors.bDiscard);
	EXPECT_EQ(Colors.LoadAction, Durin::ERHIRenderTargetLoadAction::Clear);
	EXPECT_EQ(Colors.StoreAction, Durin::ERHIRenderTargetStoreAction::Store);
	EXPECT_TRUE(Colors.bPassManagedTransition);
	EXPECT_EQ(Colors.ResultAccess, Durin::ERHIAccess::GraphicsShaderRead);

	const auto& Depth = Metadata->Members[2];
	EXPECT_STREQ(Depth.Name, "Depth");
	EXPECT_EQ(Depth.Kind, Durin::ERDGParameterMemberKind::ManagedDepthStencilAttachment);
	EXPECT_EQ(Depth.ArraySize, 1u);
	EXPECT_TRUE(Depth.bOptional);
	EXPECT_EQ(Depth.Use, Durin::ERDGUse::ReadWrite);
	EXPECT_EQ(Depth.Access, Durin::ERHIAccess::DepthStencilReadWrite);
	EXPECT_TRUE(Depth.bDiscard);
	EXPECT_EQ(Depth.LoadAction, Durin::ERHIRenderTargetLoadAction::Clear);
	EXPECT_EQ(Depth.StoreAction, Durin::ERHIRenderTargetStoreAction::Store);
	EXPECT_TRUE(Depth.bPassManagedTransition);
	EXPECT_EQ(Depth.ResultAccess, Durin::ERHIAccess::GraphicsShaderRead);

	const auto& VisibleInstances = Metadata->Members[3];
	EXPECT_STREQ(VisibleInstances.Name, "VisibleInstances");
	EXPECT_EQ(VisibleInstances.Kind, Durin::ERDGParameterMemberKind::Buffer);
	EXPECT_TRUE(VisibleInstances.bOptional);
	EXPECT_EQ(VisibleInstances.Use, Durin::ERDGUse::Read);
	EXPECT_EQ(VisibleInstances.Access, Durin::ERHIAccess::GraphicsShaderRead);

	const auto& InstanceTransforms = Metadata->Members[4];
	EXPECT_STREQ(InstanceTransforms.Name, "InstanceTransforms");
	EXPECT_EQ(InstanceTransforms.Kind, Durin::ERDGParameterMemberKind::Buffer);
	EXPECT_TRUE(InstanceTransforms.bOptional);
	EXPECT_EQ(InstanceTransforms.Use, Durin::ERDGUse::Read);
	EXPECT_EQ(InstanceTransforms.Access, Durin::ERHIAccess::GraphicsShaderRead);

	const auto& Arguments = Metadata->Members[5];
	EXPECT_STREQ(Arguments.Name, "Arguments");
	EXPECT_EQ(Arguments.Kind, Durin::ERDGParameterMemberKind::Buffer);
	EXPECT_TRUE(Arguments.bOptional);
	EXPECT_EQ(Arguments.Use, Durin::ERDGUse::Read);
	EXPECT_EQ(Arguments.Access, Durin::ERHIAccess::IndirectArgumentRead);
}

TEST(FRendererSceneContractTests, GBufferGPUCullingParametersOwnExactBufferRoles)
{
	const Durin::FRDGParametersMetadata* Metadata =
		Durin::FGBufferGPUCullingPassParameters::GetRDGParametersMetadata();
	ASSERT_NE(Metadata, nullptr);
	EXPECT_STREQ(Metadata->StructName, "FGBufferGPUCullingPassParameters");
	ASSERT_EQ(Metadata->Members.size(), 3u);
	EXPECT_EQ(Metadata->Members[0].Use, Durin::ERDGUse::ReadWrite);
	EXPECT_EQ(Metadata->Members[0].Access,
		Durin::ERHIAccess::ComputeShaderReadWrite);
	EXPECT_TRUE(Metadata->Members[0].bShaderBinding);
	for (size_t Index : {1u, 2u})
	{
		EXPECT_EQ(Metadata->Members[Index].Use, Durin::ERDGUse::ReadWrite);
		EXPECT_EQ(Metadata->Members[Index].Access,
			Durin::ERHIAccess::ComputeShaderReadWrite);
		EXPECT_TRUE(Metadata->Members[Index].bShaderBinding);
	}
	EXPECT_STREQ(Metadata->Members[2].ShaderBindingName,
		"IndirectArguments");
}

TEST(FRendererSceneContractTests, ErrorMaterialUsesCompiledBindingWithoutRoleResources)
{
	const auto& Error = Durin::GetErrorMaterialRenderData();
	Durin::FMaterialRenderBinding Binding;
	Durin::FMaterialRenderValidationDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::TryGetMaterialRenderBinding(Error.Representation, Binding, Diagnostic));
	EXPECT_TRUE(Binding.bError);
	EXPECT_EQ(Binding.LayoutIdentity, Error.PlanningPassIdentity.ShaderMap.RenderLayout);
	EXPECT_EQ(Binding.LayoutIdentity.Version, Durin::CompiledMaterialRenderLayoutVersion);
	EXPECT_TRUE(Binding.CompiledTextures.empty());
	EXPECT_TRUE(Binding.CompiledSamplers.empty());
	EXPECT_EQ(Binding.CompiledUniformPayload.size(), Durin::MaterialUniformHeaderBytes);
}

namespace
{
	std::vector<Durin::FSceneViewStateId>* GReleasedViewStateIds = nullptr;

	auto ObserveReleasedViewState(Durin::FSceneViewStateId Id) -> void
	{
		if (GReleasedViewStateIds != nullptr)
			GReleasedViewStateIds->push_back(Id);
	}

	class FTestViewStateRenderer final : public Durin::IRendererModule
	{
	public:
		auto StartupModule() -> void override {}
		auto ShutdownModule() -> void override {}
		auto CreateScene() -> Durin::FScenePtr override { return {}; }
		auto CreateViewState() -> Durin::FSceneViewStateOwner override
		{
			++CreateCount;
			return Durin::FSceneViewStateOwnerTestAccess::Make(
				Durin::FSceneViewStateIdAccess::Make(NextId++),
				ObserveReleasedViewState
			);
		}
		auto InvalidateViewState(Durin::FSceneViewStateId) -> void override {}
		auto InvalidateAllViewStates() -> void override {}
		auto PrepareViewResources(Durin::FRHICommandListImmediate&, const Durin::FSceneViewRenderRequest& Request,
			const Durin::FRenderResourcePreparation& Preparation) -> Durin::FRenderResourcePreparationProgress override
		{
			++PrepareCount;
			Durin::FRenderResourcePreparationScope Scope(Preparation);
			Scope.Complete(Request.View.ViewportWidth == 64 ? Durin::ERenderResourcePreparationState::Ready
				: Durin::ERenderResourcePreparationState::Pending);
			return Preparation.GetProgress();
		}
		uint32 PrepareCount = 0;
		uint32 RenderCount = 0;
		auto RenderView(Durin::FRHICommandListImmediate&, const Durin::FSceneViewRenderRequest& Request) -> Durin::ERenderViewResult override
		{
			++RenderCount;
			return Durin::ERenderViewResult::Success;
		}

		uint64 NextId = 1000;
		uint32 CreateCount = 0;
	};

	std::vector<Durin::FViewRenderTelemetry>* GObservedViewTelemetrySnapshots = nullptr;
	std::vector<Durin::FRDGCapture>* GObservedRenderGraphCaptures = nullptr;

	auto ObserveViewTelemetrySnapshot(
		const Durin::FViewRenderTelemetry& Telemetry
	) -> void
	{
		if (GObservedViewTelemetrySnapshots != nullptr)
		{
			GObservedViewTelemetrySnapshots->push_back(Telemetry);
		}
	}

	auto ObserveRenderGraphCapture(const Durin::FRDGCapture& Capture) -> void
	{
		if (GObservedRenderGraphCaptures != nullptr)
			GObservedRenderGraphCaptures->push_back(Capture);
	}

	struct FObservedLight
	{
		bool bPresent = false;
		Durin::FDirectionalLightSceneData Data;
	};

	auto ObserveLight(Durin::FScene& Scene) -> FObservedLight
	{
		auto Result = std::make_shared<FObservedLight>();
		struct FObserveLightCommand
		{
			static constexpr auto GetName() -> const char* { return "ObserveLight"; }
		};
		Durin::EnqueueRenderCommand<FObserveLightCommand>(
			[&Scene, Result](Durin::FRHICommandListImmediate&) {
				const auto& Lights = Scene.GetDirectionalLightSceneInfos();
				Result->bPresent = !Lights.empty();
				if (Result->bPresent)
					Result->Data = Lights.front()->GetDirectionalProxy().GetData();
			}
		);
		Durin::FlushRenderingCommands();
		return *Result;
	}

	class FRenderingThreadScope final
	{
	public:
		FRenderingThreadScope()
		{
			if (!Durin::GIsGameThreadIdInitialized)
			{
				Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
				Durin::GIsGameThreadIdInitialized = true;
			}
			if (!Durin::IsTaskSchedulerRunning()) require(Durin::InitializeTaskScheduler(2));
			Durin::InitRenderingThread();
		}
		~FRenderingThreadScope() { Durin::ShutdownRenderingThread(); }
	};

	class FTrackedStaticMeshSceneProxy final
		: public Durin::FStaticMeshSceneProxy
	{
	public:
		FTrackedStaticMeshSceneProxy(
			const Durin::FStaticMeshRenderData* RenderData,
			std::shared_ptr<std::atomic<bool>> InDestroyed,
			std::shared_ptr<std::atomic<bool>> InDestroyedOnRenderingThread
		)
			: FStaticMeshSceneProxy(RenderData, {})
			, Destroyed(std::move(InDestroyed))
			, DestroyedOnRenderingThread(
				  std::move(InDestroyedOnRenderingThread)
			  )
		{
		}

		~FTrackedStaticMeshSceneProxy() override
		{
			DestroyedOnRenderingThread->store(
				Durin::IsInRenderingThread(), std::memory_order_release
			);
			Destroyed->store(true, std::memory_order_release);
		}

	private:
		std::shared_ptr<std::atomic<bool>> Destroyed;
		std::shared_ptr<std::atomic<bool>> DestroyedOnRenderingThread;
	};

	auto MakePerspectiveProjection() -> Durin::FMatrix
	{
		Durin::FMatrix Projection(0.0);
		Projection[1][0] = 0.5;
		Projection[2][1] = -1.0;
		Projection[0][2] = 11.0 / 10.0;
		Projection[3][2] = -11.0 / 10.0;
		Projection[0][3] = 1.0;
		return Projection;
	}
} // namespace

TEST(FRendererSceneContractTests, OwningScenePointerDefersDeletionBehindQueuedCommands)
{
	FRenderingThreadScope RenderingThread;
	struct FBlockedRenderCommandState
	{
		std::mutex Mutex;
		std::condition_variable CV;
		bool bStarted = false;
		bool bContinue = false;
	};
	auto BlockedRenderCommand =
		std::make_shared<FBlockedRenderCommandState>();
	struct FBlockRenderThreadCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "BlockRenderThread";
		}
	};
	Durin::EnqueueRenderCommand<FBlockRenderThreadCommand>(
		[BlockedRenderCommand](Durin::FRHICommandListImmediate&) {
			std::unique_lock Lock(BlockedRenderCommand->Mutex);
			BlockedRenderCommand->bStarted = true;
			BlockedRenderCommand->CV.notify_all();
			BlockedRenderCommand->CV.wait(Lock, [&] {
				return BlockedRenderCommand->bContinue;
			});
		}
	);
	{
		std::unique_lock Lock(BlockedRenderCommand->Mutex);
		BlockedRenderCommand->CV.wait(Lock, [&] {
			return BlockedRenderCommand->bStarted;
		});
	}

	Durin::FRendererModule RendererModule;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-1.0), Durin::FVector3(1.0)
	);
	auto Destroyed = std::make_shared<std::atomic<bool>>(false);
	auto DestroyedOnRenderingThread =
		std::make_shared<std::atomic<bool>>(false);
	auto ObservedQueuedMutation = std::make_shared<std::atomic<bool>>(false);

	Durin::FScenePtr Scene = RendererModule.CreateScene();
	auto* ConcreteScene = static_cast<Durin::FScene*>(Scene.get());
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(*ConcreteScene, Durin::FPrimitiveComponentId(1), std::make_unique<FTrackedStaticMeshSceneProxy>(&RenderData, Destroyed, DestroyedOnRenderingThread), Durin::FMatrix(1.0));
	struct FObserveQueuedSceneMutationCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveQueuedSceneMutation";
		}
	};
	Durin::EnqueueRenderCommand<FObserveQueuedSceneMutationCommand>(
		[ConcreteScene, ObservedQueuedMutation](
			Durin::FRHICommandListImmediate&
		) {
			ObservedQueuedMutation->store(
				ConcreteScene->GetPrimitiveSceneInfos().size() == 1,
				std::memory_order_release
			);
		}
	);

	Durin::FSceneInterfaceTestAccess::ReleaseScene(Scene);
	Durin::FRenderCommandFence Fence;
	Fence.BeginFence();
	{
		std::lock_guard Lock(BlockedRenderCommand->Mutex);
		BlockedRenderCommand->bContinue = true;
	}
	BlockedRenderCommand->CV.notify_all();
	Fence.Wait();

	EXPECT_TRUE(ObservedQueuedMutation->load(std::memory_order_acquire));
	EXPECT_TRUE(Destroyed->load(std::memory_order_acquire));
	EXPECT_TRUE(
		DestroyedOnRenderingThread->load(std::memory_order_acquire)
	);
}

TEST(FRendererSceneContractTests, SceneReleaseRejectsMutationAndSecondRelease)
{
	FRenderingThreadScope RenderingThread;
	EXPECT_DEATH_IF_SUPPORTED({
		auto Scene = Durin::FSceneInterfaceTestAccess::CreateScene();
		Scene->Release();
		Scene->UpdatePrimitiveVisibility(Durin::FPrimitiveComponentId(1), true); }, "");
	EXPECT_DEATH_IF_SUPPORTED({
		auto Scene = Durin::FSceneInterfaceTestAccess::CreateScene();
		Scene->Release();
		Scene->Release(); }, "");
}

TEST(FRendererSceneContractTests, SceneOwnerRequiresExplicitRelease)
{
	FRenderingThreadScope RenderingThread;
	EXPECT_DEATH_IF_SUPPORTED({
		auto Scene = Durin::FSceneInterfaceTestAccess::CreateScene();
		Scene.reset(); }, "");
}

TEST(FRendererSceneContractTests, RendererShutdownRejectsAnActiveScene)
{
	FRenderingThreadScope RenderingThread;
	Durin::FRendererModule Renderer;
	auto Scene = Renderer.CreateScene();
	EXPECT_DEATH_IF_SUPPORTED(Renderer.ShutdownModule(), "");
	Durin::FSceneInterfaceTestAccess::ReleaseScene(Scene);
}

TEST(FRendererSceneContractTests, ViewStateIdentityIsOpaqueUniqueAndMoveOwned)
{
	std::vector<Durin::FSceneViewStateId> Released;
	GReleasedViewStateIds = &Released;
	const Durin::FSceneViewStateId FirstId =
		Durin::FSceneViewStateIdAccess::Make(1);
	const Durin::FSceneViewStateId SecondId =
		Durin::FSceneViewStateIdAccess::Make(2);
	Durin::FSceneViewStateOwner First =
		Durin::FSceneViewStateOwnerTestAccess::Make(
			FirstId, ObserveReleasedViewState
		);
	Durin::FSceneViewStateOwner Second =
		Durin::FSceneViewStateOwnerTestAccess::Make(
			SecondId, ObserveReleasedViewState
		);
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	EXPECT_NE(First.GetId(), Second.GetId());
	Durin::FSceneViewStateOwner Moved = std::move(First);
	EXPECT_FALSE(First);
	EXPECT_EQ(Moved.GetId(), FirstId);
	Moved.Reset();
	Second.Reset();
	GReleasedViewStateIds = nullptr;
	ASSERT_EQ(Released.size(), 2u);
	EXPECT_EQ(Released[0], FirstId);
	EXPECT_EQ(Released[1], SecondId);
}

TEST(FRendererSceneContractTests, ViewStateIdsSurviveRendererRestartWithoutReuseAndLeakedOwnersBecomeInert)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneViewStateOwner LeakedOwner;
	Durin::FSceneViewStateId FirstId;
	{
		Durin::FRendererModule FirstRenderer;
		Durin::FModuleTestHarness FirstLifecycle("ViewStateRestartFirst");
		FirstLifecycle.Start(FirstRenderer);
		LeakedOwner = FirstRenderer.CreateViewState();
		ASSERT_TRUE(LeakedOwner);
		FirstId = LeakedOwner.GetId();
		Durin::FlushRenderingCommands();
		FirstLifecycle.Shutdown();
	}
	// The shutdown audit removed the private route, so late public release is safe.
	LeakedOwner.Reset();
	Durin::FlushRenderingCommands();

	Durin::FRendererModule SecondRenderer;
	Durin::FModuleTestHarness SecondLifecycle("ViewStateRestartSecond");
	SecondLifecycle.Start(SecondRenderer);
	Durin::FSceneViewStateOwner SecondOwner =
		SecondRenderer.CreateViewState();
	ASSERT_TRUE(SecondOwner);
	EXPECT_NE(SecondOwner.GetId(), FirstId);
	SecondOwner.Reset();
	Durin::FlushRenderingCommands();
	SecondLifecycle.Shutdown();
}

TEST(FRendererSceneContractTests, ViewStateTransactionsPreserveLastSuccessfulFittedMetadata)
{
	FRenderingThreadScope RenderingThread;
	auto Result = std::make_shared<std::vector<Durin::FSceneViewTemporalContext>>();
	auto HistoryPolicies = std::make_shared<std::vector<uint64>>();
	struct FExerciseViewStateTransactionsCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ExerciseViewStateTransactions";
		}
	};
	Durin::EnqueueRenderCommand<FExerciseViewStateTransactionsCommand>(
		[Result, HistoryPolicies](Durin::FRHICommandListImmediate&) {
			Durin::FSceneViewStateRegistry Registry;
			const Durin::FSceneViewStateId FirstId =
				Durin::FSceneViewStateIdAccess::Make(101);
			const Durin::FSceneViewStateId SecondId =
				Durin::FSceneViewStateIdAccess::Make(102);
			EXPECT_TRUE(Registry.Add(FirstId));
			EXPECT_FALSE(Registry.Add(FirstId));
			EXPECT_TRUE(Registry.Add(SecondId));
			Durin::FSceneViewState* State = Registry.Find(FirstId);
			ASSERT_NE(State, nullptr);

			Durin::FSceneView View;
			View.ViewportWidth = 640;
			View.ViewportHeight = 360;
			Durin::FSceneViewTemporalMetadata First =
				Durin::BuildSceneViewTemporalMetadata(
					View, nullptr, 640, 360
				);
			Result->push_back(State->GetVolumetricCloudHistory().Begin(First, 1, false));
			State->GetVolumetricCloudHistory().SetPendingClear(11, 1);
			State->GetVolumetricCloudHistory().Commit();
			HistoryPolicies->push_back(
				State->GetVolumetricCloudHistory().CommittedPolicyKey
			);

			Durin::FSceneViewTemporalMetadata Moved = First;
			Moved.ViewProjectionMatrix[3][0] = 5.0;
			Result->push_back(State->GetVolumetricCloudHistory().Begin(Moved, 2, false));
			State->GetVolumetricCloudHistory().SetPendingClear(22, 1);
			State->GetVolumetricCloudHistory().Abort();
			HistoryPolicies->push_back(
				State->GetVolumetricCloudHistory().CommittedPolicyKey
			);

			Result->push_back(State->GetVolumetricCloudHistory().Begin(Moved, 3, false));
			State->GetVolumetricCloudHistory().SetPendingClear(33, 1);
			State->GetVolumetricCloudHistory().Commit();
			HistoryPolicies->push_back(
				State->GetVolumetricCloudHistory().CommittedPolicyKey
			);

			(void)State->GetVolumetricCloudHistory().Begin(Moved, 4, false);
			State->GetVolumetricCloudHistory().Commit();
			HistoryPolicies->push_back(
				State->GetVolumetricCloudHistory().CommittedPolicyKey
			);

			Durin::FSceneViewTemporalMetadata ProjectionChanged = Moved;
			ProjectionChanged.ProjectionMatrix[0][0] = 2.0;
			Result->push_back(State->GetVolumetricCloudHistory().Begin(ProjectionChanged, 5, false));
			State->GetVolumetricCloudHistory().Abort();

			State->Invalidate(
				Durin::ESceneViewDiscontinuity::ManualInvalidation
			);
			Result->push_back(State->GetVolumetricCloudHistory().Begin(Moved, 6, false));
			State->GetVolumetricCloudHistory().Abort();

			Result->push_back(State->GetVolumetricCloudHistory().Begin(Moved, 200, true));
			State->GetVolumetricCloudHistory().Abort();

			EXPECT_TRUE(Registry.Remove(SecondId));
			EXPECT_FALSE(Registry.Remove(SecondId));
			EXPECT_EQ(Registry.ReleaseAll(), 1u);
		}
	);
	Durin::FlushRenderingCommands();

	ASSERT_EQ(Result->size(), 6u);
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[0].Discontinuities,
		Durin::ESceneViewDiscontinuity::FirstUse
	));
	EXPECT_FALSE((*Result)[0].bHistoryValid);
	EXPECT_TRUE((*Result)[1].bHistoryValid);
	EXPECT_EQ((*Result)[1].SuccessfulSequence, 1u);
	EXPECT_EQ((*Result)[1].PreviousRenderFrame, 1u);
	EXPECT_EQ((*Result)[1].PreviousViewProjectionMatrix, Durin::FMatrix(1.0));
	EXPECT_TRUE((*Result)[2].bHistoryValid);
	EXPECT_EQ((*Result)[2].PreviousRenderFrame, 1u);
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[3].Discontinuities,
		Durin::ESceneViewDiscontinuity::ProjectionChange
	));
	EXPECT_FALSE((*Result)[3].bHistoryValid);
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[4].Discontinuities,
		Durin::ESceneViewDiscontinuity::ManualInvalidation
	));
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[5].Discontinuities,
		Durin::ESceneViewDiscontinuity::ExplicitCameraCut
	));
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[5].Discontinuities,
		Durin::ESceneViewDiscontinuity::InactiveGapExpiry
	));
	ASSERT_EQ(HistoryPolicies->size(), 4u);
	EXPECT_EQ((*HistoryPolicies)[0], 11u);
	EXPECT_EQ((*HistoryPolicies)[1], 11u);
	EXPECT_EQ((*HistoryPolicies)[2], 33u);
	EXPECT_EQ((*HistoryPolicies)[3], 33u);
}

TEST(FRendererSceneContractTests, VolumetricCloudHistoryPublishesIndependentlyAndAbortsOnlyUnfinishedCandidates)
{
	FRenderingThreadScope RenderingThread;
	auto Observed = std::make_shared<std::vector<uint64>>();
	struct FExerciseCloudHistoryTransactionsCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ExerciseCloudHistoryTransactions";
		}
	};
	Durin::EnqueueRenderCommand<FExerciseCloudHistoryTransactionsCommand>(
		[Observed](Durin::FRHICommandListImmediate&) {
			Durin::FSceneViewState State;
			Durin::FSceneView View;
			View.ViewportWidth = 640;
			View.ViewportHeight = 360;
			const auto Metadata = Durin::BuildSceneViewTemporalMetadata(
				View, nullptr, 640, 360
			);
			(void)State.GetVolumetricCloudHistory().Begin(Metadata, 1, false);
			State.GetVolumetricCloudHistory().SetPendingClear(11, 101);
			State.GetVolumetricCloudHistory().Commit();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);

			(void)State.GetVolumetricCloudHistory().Begin(Metadata, 2, false);
			State.GetVolumetricCloudHistory().SetPendingClear(22, 202);
			State.GetVolumetricCloudHistory().Abort();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			(void)State.GetVolumetricCloudHistory().Begin(Metadata, 3, false);
			State.GetVolumetricCloudHistory().SetPendingClear(33, 303);
			State.GetVolumetricCloudHistory().Commit();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			Durin::FSceneViewTemporalMetadata Resized = Metadata;
			Resized.OutputWidth = 1280;
			const auto ResizedContext = State.GetVolumetricCloudHistory().Begin(Resized, 4, false);
			EXPECT_FALSE(ResizedContext.bHistoryValid);
			State.GetVolumetricCloudHistory().Abort();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			(void)State.GetVolumetricCloudHistory().Begin(Resized, 5, false);
			State.GetVolumetricCloudHistory().Commit();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			State.Invalidate(Durin::ESceneViewDiscontinuity::ManualInvalidation);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			EXPECT_EQ(State.GetVolumetricCloudHistory().GetRetainedBytes(), 0u);
		}
	);
	Durin::FlushRenderingCommands();

	EXPECT_EQ(*Observed, (std::vector<uint64>{11, 11, 101, 33, 303, 33, 303, 0, 0, 0}));
}

TEST(FRendererSceneContractTests, CloudHistoryKeepsPublishedCameraAfterOuterFailureAndUsesRenderFrameAge)
{
	FRenderingThreadScope RenderingThread;
	struct FExerciseCloudHistoryAgeCommand
	{
		static constexpr auto GetName() -> const char* { return "ExerciseCloudHistoryAge"; }
	};
	Durin::EnqueueRenderCommand<FExerciseCloudHistoryAgeCommand>(
		[](Durin::FRHICommandListImmediate&) {
			Durin::FVolumetricCloudViewHistory History;
			Durin::FSceneViewTemporalMetadata First;
			First.OutputWidth = 640;
			First.OutputHeight = 360;
			(void)History.Begin(First, 10, false);
			History.SetPendingClear(11, 101);
			History.Commit();

			auto Moved = First;
			Moved.ViewProjectionMatrix[3][0] = 5.0;
			(void)History.Begin(Moved, 11, false);
			History.SetPendingClear(22, 202);
			History.Commit();
			// The outer view fails after cloud publication; scope cleanup must
			// preserve both the published camera and its matching cloud policy.
			History.Abort();
			for (uint32 OtherView = 0; OtherView < 256; ++OtherView)
			{
				const auto Context = History.Begin(First, 11, false);
				EXPECT_TRUE(Context.bHistoryValid);
				EXPECT_EQ(Context.FrameGap, 0u);
				EXPECT_EQ(Context.PreviousViewProjectionMatrix, Moved.ViewProjectionMatrix);
				EXPECT_EQ(History.CommittedPolicyKey, 22u);
				History.Abort();
			}
			// Recording no cloud output cannot advance the camera alone.
			(void)History.Begin(First, 12, false);
			History.Commit();
			const auto Boundary = History.Begin(First, 131, false);
			EXPECT_EQ(Boundary.OutputWidth, First.OutputWidth);
			EXPECT_EQ(Boundary.OutputHeight, First.OutputHeight);
			EXPECT_TRUE(Boundary.bHistoryValid);
			EXPECT_EQ(Boundary.PreviousRenderFrame, 11u);
			EXPECT_EQ(Boundary.SuccessfulSequence, 2u);
			History.Abort();
			const auto Expired = History.Begin(First, 132, false);
			EXPECT_FALSE(Expired.bHistoryValid);
			EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(Expired.Discontinuities,
				Durin::ESceneViewDiscontinuity::InactiveGapExpiry));
			History.Abort();
		});
	Durin::FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, SceneViewportsOwnIsolatedOptInStateAndConsumeCutsOnce)
{
	std::vector<Durin::FSceneViewStateId> Released;
	GReleasedViewStateIds = &Released;
	FTestViewStateRenderer Renderer;
	{
		auto Main = Durin::FSceneViewport::CreateOffscreen(nullptr);
		auto Auxiliary = Durin::FSceneViewport::CreateOffscreen(nullptr);
		Main->InitializeViewState(&Renderer);
		Main->InitializeViewState(&Renderer);
		Auxiliary->InitializeViewState(&Renderer);
		EXPECT_EQ(Renderer.CreateCount, 2u);
		EXPECT_TRUE(Main->GetViewStateId().IsValid());
		EXPECT_TRUE(Auxiliary->GetViewStateId().IsValid());
		EXPECT_NE(Main->GetViewStateId(), Auxiliary->GetViewStateId());
		EXPECT_TRUE(Main->ConsumeHistoryReset());
		EXPECT_FALSE(Main->ConsumeHistoryReset());
		Main->RequestHistoryReset();
		EXPECT_TRUE(Main->ConsumeHistoryReset());
		EXPECT_TRUE(Auxiliary->ConsumeHistoryReset());
	}
	GReleasedViewStateIds = nullptr;
	EXPECT_EQ(Released.size(), 2u);
}

TEST(FRendererSceneContractTests, ViewStateReportsEveryFrozenDiscontinuityCause)
{
	FRenderingThreadScope RenderingThread;
	auto Contexts = std::make_shared<
		std::vector<Durin::FSceneViewTemporalContext>>();
	struct FExerciseViewStateDiscontinuitiesCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ExerciseViewStateDiscontinuities";
		}
	};
	Durin::EnqueueRenderCommand<FExerciseViewStateDiscontinuitiesCommand>(
		[Contexts](Durin::FRHICommandListImmediate&) {
			Durin::FSceneViewState State;
			Durin::FSceneView View;
			View.ViewportWidth = 320;
			View.ViewportHeight = 180;
			const Durin::FSceneViewTemporalMetadata Baseline =
				Durin::BuildSceneViewTemporalMetadata(
					View, nullptr, 320, 180
				);
			(void)State.GetVolumetricCloudHistory().Begin(Baseline, 1, false);
			State.GetVolumetricCloudHistory().SetPendingClear(0, 0);
			State.GetVolumetricCloudHistory().Commit();

			auto ObserveAbort = [&](Durin::FSceneViewTemporalMetadata Candidate,
									uint64 Serial) {
				Contexts->push_back(State.GetVolumetricCloudHistory().Begin(Candidate, Serial, false));
				State.GetVolumetricCloudHistory().Abort();
			};

			Durin::FSceneViewTemporalMetadata Candidate = Baseline;
			Candidate.Scene = reinterpret_cast<const Durin::FScene*>(1);
			ObserveAbort(Candidate, 2);
			Candidate = Baseline;
			Candidate.OutputWidth = 640;
			ObserveAbort(Candidate, 3);
			Candidate = Baseline;
			Candidate.ViewportX = 1;
			ObserveAbort(Candidate, 4);
			Candidate = Baseline;
			Candidate.DepthConvention =
				Durin::ESceneDepthConvention::ReversedZ;
			ObserveAbort(Candidate, 5);
			State.Invalidate(
				Durin::ESceneViewDiscontinuity::DeviceInvalidation
			);
			ObserveAbort(Baseline, 6);
			// Abort must not consume a hard invalidation.
			ObserveAbort(Baseline, 7);
			// Non-monotonic serial input is treated as an expired gap, never wrapped.
			ObserveAbort(Baseline, 0);
		}
	);
	Durin::FlushRenderingCommands();

	ASSERT_EQ(Contexts->size(), 7u);
	const std::array Causes{
		Durin::ESceneViewDiscontinuity::SceneChange,
		Durin::ESceneViewDiscontinuity::OutputExtentChange,
		Durin::ESceneViewDiscontinuity::ViewportRectChange,
		Durin::ESceneViewDiscontinuity::DepthConventionChange,
		Durin::ESceneViewDiscontinuity::DeviceInvalidation,
		Durin::ESceneViewDiscontinuity::DeviceInvalidation,
		Durin::ESceneViewDiscontinuity::InactiveGapExpiry,
	};
	for (size_t Index = 0; Index < Causes.size(); ++Index)
	{
		EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
			(*Contexts)[Index].Discontinuities, Causes[Index]
		));
		EXPECT_FALSE((*Contexts)[Index].bHistoryValid);
	}
}

TEST(FRendererSceneContractTests, GPUVisibilityQueryDistinguishesPendingZeroAndReusableCompletedCounts)
{
	using namespace Durin;
	const std::array Expected{FRHIDrawIndexedIndirectArguments{6, 0, 3, -2, 0},
		FRHIDrawIndexedIndirectArguments{6, 0, 3, -2, 64}};
	FGPUCullingVisibilityQuery Query(128, Expected);
	EXPECT_EQ(Query.GetState(), ERHITextureReadbackState::Pending);
	EXPECT_FALSE(Query.GetCounts());
	auto Actual = Expected;
	Actual[1].InstanceCount = 64;
	const auto Bytes = std::as_bytes(std::span(Actual));
	Query.GetReadback()->Complete(FByteBuffer(Bytes.begin(), Bytes.end()));
	for (int Observer = 0; Observer < 2; ++Observer)
	{
		const auto Counts = Query.GetCounts();
		ASSERT_TRUE(Counts);
		EXPECT_EQ(Query.GetState(), ERHITextureReadbackState::Ready);
		EXPECT_EQ(Counts->Candidates, 128u);
		EXPECT_EQ(Counts->Visible, 64u);
		EXPECT_EQ(Counts->Culled, 64u);
	}
	FGPUCullingVisibilityQuery Zero(128, Expected);
	const auto ZeroBytes = std::as_bytes(std::span(Expected));
	Zero.GetReadback()->Complete(FByteBuffer(ZeroBytes.begin(), ZeroBytes.end()));
	ASSERT_TRUE(Zero.GetCounts());
	EXPECT_EQ(Zero.GetCounts()->Visible, 0u);
	EXPECT_EQ(Zero.GetCounts()->Culled, 128u);
}

TEST(FRendererSceneContractTests, GPUVisibilityQueryRejectsMalformedCapacityAndIdentityWithoutPublishingCounts)
{
	using namespace Durin;
	const std::array Expected{FRHIDrawIndexedIndirectArguments{6, 0, 3, -2, 0}};
	for (int Fault = 0; Fault < 4; ++Fault)
	{
		SCOPED_TRACE(Fault);
		FGPUCullingVisibilityQuery Query(64, Expected);
		auto Actual = Expected;
		if (Fault == 0) Actual[0].InstanceCount = 65;
		if (Fault == 1) Actual[0].FirstIndex = 0;
		if (Fault == 2) Actual[0].FirstInstance = 1;
		const auto Bytes = std::as_bytes(std::span(Actual));
		FByteBuffer Payload(Bytes.begin(), Bytes.end());
		if (Fault == 3) Payload.pop_back();
		Query.GetReadback()->Complete(std::move(Payload));
		EXPECT_FALSE(Query.GetCounts());
		EXPECT_EQ(Query.GetState(), ERHITextureReadbackState::Failed);
	}
	FGPUCullingVisibilityQuery Canceled(64, Expected);
	Canceled.GetReadback()->Cancel();
	EXPECT_EQ(Canceled.GetState(), ERHITextureReadbackState::Canceled);
	EXPECT_FALSE(Canceled.GetCounts());
	FGPUCullingVisibilityQuery Failed(64, Expected);
	Failed.GetReadback()->Fail();
	EXPECT_EQ(Failed.GetState(), ERHITextureReadbackState::Failed);
	EXPECT_FALSE(Failed.GetCounts());
}

TEST(FRendererSceneContractTests, TelemetrySnapshotSeamDeliversOneImmutableValue)
{
	std::vector<Durin::FViewRenderTelemetry> Snapshots;
	GObservedViewTelemetrySnapshots = &Snapshots;
	Durin::SetViewRenderTelemetrySink(ObserveViewTelemetrySnapshot);
	Durin::FViewRenderTelemetry Telemetry;
	Telemetry.Visibility.SubmittedPrimitives = 3;
	Telemetry.StaticMesh.StaticMeshAttemptedDraws = 2;
	Durin::EmitViewRenderTelemetrySnapshot(Telemetry);
	Telemetry.Visibility.SubmittedPrimitives = 9;
	Durin::SetViewRenderTelemetrySink(nullptr);
	GObservedViewTelemetrySnapshots = nullptr;
	ASSERT_EQ(Snapshots.size(), 1u);
	EXPECT_EQ(Snapshots.front().Visibility.SubmittedPrimitives, 3u);
	EXPECT_EQ(Snapshots.front().StaticMesh.StaticMeshAttemptedDraws, 2u);
}

TEST(FRendererSceneContractTests, SceneRenderGraphInspectionPublishesOwningSnapshot)
{
	std::vector<Durin::FRDGCapture> Captures;
	Durin::FRDGCapture ExplicitCapture;
	GObservedRenderGraphCaptures = &Captures;
	Durin::SetSceneRenderGraphCaptureSink(ObserveRenderGraphCapture);
	{
		Durin::FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Output = Builder.CreateToken("Scene.Output");
		auto Parameters = Builder.AllocParameters<FInspectionOutputParameters>();
		Parameters->Output = {Output};
		const auto Final = Durin::FRDGBuilderTestAccessor::AddPass(Builder,
			"Scene.FinalOutput", Durin::ERDGPassType::Graphics,
			std::move(Parameters)
		);
		Builder.MarkPassRoot(Final, "offscreen-output");
		Durin::FRHICommandListExecutor Executor;
		const auto Result = Builder.Execute(Executor.GetImmediateCommandList());
		ASSERT_TRUE(Result.has_value()) << ToString(Result.error());
		Durin::PublishSceneRenderGraphCapture(Builder, &ExplicitCapture);
	}
	Durin::SetSceneRenderGraphCaptureSink(nullptr);
	GObservedRenderGraphCaptures = nullptr;
	ASSERT_EQ(Captures.size(), 1u);
	EXPECT_EQ(ExplicitCapture.Dump, Captures[0].Dump);
	EXPECT_TRUE(ExplicitCapture.ExecutionResult.value().has_value());
	EXPECT_TRUE(Captures[0].ExecutionResult.value().has_value());
	ASSERT_EQ(Captures[0].Passes.size(), 1u);
	EXPECT_EQ(Captures[0].Passes[0].Name, "Scene.FinalOutput");
	EXPECT_EQ(Captures[0].Passes[0].ParameterStructName, "FInspectionOutputParameters");
	ASSERT_EQ(Captures[0].Parameters.size(), 1u);
	EXPECT_EQ(Captures[0].Parameters[0].FieldPath, "FInspectionOutputParameters.Output");
	EXPECT_EQ(Captures[0].CullingDecisions[0].Reason, "offscreen-output");
	EXPECT_NE(Captures[0].Dump.find("Scene.FinalOutput"), std::string::npos);
}

TEST(FRendererSceneContractTests, SceneRenderGraphInspectionPublishesCompileFailure)
{
	std::vector<Durin::FRDGCapture> Captures;
	Durin::FRDGCapture ExplicitCapture;
	GObservedRenderGraphCaptures = &Captures;
	Durin::SetSceneRenderGraphCaptureSink(ObserveRenderGraphCapture);
	{
		Durin::FRDGBuilder Builder;
		Durin::FRDGBuilderTestAccessor::AddPass(Builder, "", Durin::ERDGPassType::Copy);
		Durin::FRHICommandListExecutor Executor;
		const auto Result = Builder.Execute(Executor.GetImmediateCommandList());
		EXPECT_EQ(Durin::GetRDGExecutionStatus(Result), Durin::ERDGExecutionStatus::CompileFailed);
		Durin::PublishSceneRenderGraphCapture(Builder, &ExplicitCapture);
	}
	Durin::SetSceneRenderGraphCaptureSink(nullptr);
	GObservedRenderGraphCaptures = nullptr;
	ASSERT_EQ(Captures.size(), 1u);
	EXPECT_FALSE(Captures[0].bCompiled);
	EXPECT_EQ(Durin::GetRDGExecutionStatus(Captures[0].ExecutionResult.value()), Durin::ERDGExecutionStatus::CompileFailed);
	EXPECT_TRUE(Durin::HasRDGTestReason(Captures[0].ExecutionResult.value().error(), Durin::ERDGIdentityError::PassNameEmpty));
	EXPECT_EQ(ToString(ExplicitCapture.ExecutionResult.value().error()), ToString(Captures[0].ExecutionResult.value().error()));
}

TEST(FRendererSceneContractTests, TelemetryPublishesOnlyAfterSuccessfulCommit)
{
	std::vector<Durin::FViewRenderTelemetry> Snapshots;
	GObservedViewTelemetrySnapshots = &Snapshots;
	Durin::SetViewRenderTelemetrySink(ObserveViewTelemetrySnapshot);
	Durin::FSceneRenderTelemetry Telemetry;
	Telemetry.View.Visibility.SubmittedPrimitives = 4;
	Durin::FSceneViewStatistics Statistics;
	{
		Durin::FSceneTelemetryPublication Aborted(Telemetry, &Statistics);
	}
	EXPECT_TRUE(Snapshots.empty());
	EXPECT_EQ(Statistics.Visibility.SubmittedPrimitives, 0u);
	Durin::FSceneTelemetryPublication Completed(Telemetry, &Statistics);
	Completed.Commit();
	Completed.Commit();
	Durin::SetViewRenderTelemetrySink(nullptr);
	GObservedViewTelemetrySnapshots = nullptr;
	ASSERT_EQ(Snapshots.size(), 1u);
	EXPECT_EQ(Snapshots.front().Visibility.SubmittedPrimitives, 4u);
	EXPECT_EQ(Statistics.Visibility.SubmittedPrimitives, 4u);
}

TEST(FRendererSceneContractTests, ViewSettingsDefaultToProductionVisibilityAndLOD)
{
	const Durin::FSceneViewSettings Settings;
	EXPECT_EQ(Settings.Mode.VisibilityMode, Durin::EViewVisibilityMode::Normal);
	EXPECT_EQ(Settings.Mode.LODMode, Durin::EViewLODMode::Automatic);
	EXPECT_EQ(Settings.DirectionalShadow.FilterQuality, Durin::EDirectionalShadowFilterQuality::Medium);
	EXPECT_EQ(Settings.DirectionalShadow.ContactRoutePreference, Durin::EContactShadowRoutePreference::Auto);
	EXPECT_TRUE(Settings.AmbientOcclusion.bEnabled);
	EXPECT_EQ(Settings.AmbientOcclusion.Quality, Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution);
	EXPECT_EQ(Settings.VolumetricCloud.Quality, Durin::EVolumetricCloudQuality::High);
	EXPECT_EQ(Settings.VolumetricCloud.DebugMode, Durin::EVolumetricCloudDebugMode::Lit);
}

TEST(FRendererSceneContractTests, ViewRenderOptionsDefaultToSafeProductionScheduling)
{
	const Durin::FSceneViewRenderOptions Options;
	EXPECT_FALSE(Options.Environment.has_value());
	EXPECT_FALSE(Options.bEnableAsyncCompute);

	const Durin::FViewEnvironmentOverride Environment;
	EXPECT_EQ(Environment.TextureReference, nullptr);
	EXPECT_EQ(Environment.Rotation, Durin::FQuat(1.0, 0.0, 0.0, 0.0));
	EXPECT_EQ(Environment.Tint, Durin::FVector3f(1.0f));
	EXPECT_EQ(Environment.Intensity, 1.0f);
}

TEST(FRendererSceneContractTests, ViewStatisticsDefaultToAnEmptyBoundedSummary)
{
	const Durin::FSceneViewStatistics Statistics;
	EXPECT_EQ(Statistics.Visibility.SubmittedPrimitives, 0u);
	EXPECT_EQ(Statistics.Visibility.VisiblePrimitives, 0u);
	EXPECT_EQ(Statistics.Summary.Triangles, 0u);
	EXPECT_EQ(Statistics.Summary.DrawCalls, 0u);
	EXPECT_FALSE(Statistics.Shadow.bEnabled);
	EXPECT_EQ(Statistics.Shadow.ContactRoute, Durin::EContactShadowExecutionRoute::None);
	EXPECT_EQ(Statistics.VolumetricCloud.Quality, Durin::EVolumetricCloudQuality::High);
	EXPECT_EQ(Statistics.VolumetricCloud.Route, Durin::EVolumetricCloudExecutionRoute::None);
}

TEST(FRendererSceneContractTests, ViewStatisticsPreserveStableMetricSemantics)
{
	Durin::FViewRenderTelemetry Telemetry;
	Telemetry.Visibility.SubmittedPrimitives = 13;
	Telemetry.Visibility.VisiblePrimitives = 8;
	Telemetry.StaticMesh.PreparedStaticMeshPrimitives = 4;
	Telemetry.SplineMesh.PreparedSplineMeshPrimitives = 1;
	Telemetry.StaticMesh.PreparedStaticMeshTriangles = 120;
	Telemetry.SplineMesh.PreparedSplineMeshTriangles = 20;
	Telemetry.DirectionalShadow.ShadowPreparedTriangles = 500;
	Telemetry.StaticMesh.StaticMeshSuccessfulDraws = 5;
	Telemetry.GBuffer.GBufferStaticMeshSuccessfulDraws = 3;
	Telemetry.GBuffer.GBufferSplineMeshSuccessfulDraws = 1;
	Telemetry.DirectionalShadow.ShadowSuccessfulDraws = 7;
	Telemetry.Lighting.SelectedDirectionalLights = 1;
	Telemetry.Lighting.SelectedPointLights = 3;
	Telemetry.Lighting.SelectedSpotLights = 2;
	Telemetry.DirectionalShadow.ShadowValidReceiverViews = 1;
	Telemetry.DirectionalShadow.ShadowCascadeCount = 3;
	Telemetry.ContactShadow.ContactShadowEnabledViews = 1;
	Telemetry.ContactShadow.ContactShadowComputeViews = 1;
	Telemetry.VolumetricCloud.VolumetricCloudQuality = Durin::EVolumetricCloudQuality::Epic;
	Telemetry.VolumetricCloud.VolumetricCloudDebugMode = Durin::EVolumetricCloudDebugMode::Transmittance;
	Telemetry.VolumetricCloud.VolumetricCloudComputeViews = 1;
	Telemetry.VolumetricCloud.VolumetricCloudEnabledViews = 1;
	Telemetry.VolumetricCloud.VolumetricCloudTargetWidth = 960;
	Telemetry.VolumetricCloud.VolumetricCloudTargetHeight = 540;
	Telemetry.VolumetricCloud.VolumetricCloudOutputWidth = 1920;
	Telemetry.VolumetricCloud.VolumetricCloudOutputHeight = 1080;
	Telemetry.VolumetricCloud.VolumetricCloudPrimarySamples = 1000;
	Telemetry.VolumetricCloud.VolumetricCloudLightSamples = 2000;
	Telemetry.VolumetricCloud.VolumetricCloudHistoryAccepted = 1;
	Telemetry.VolumetricCloud.VolumetricCloudTemporalDraws = 1;
	Telemetry.VolumetricCloud.VolumetricCloudRetainedBytes = 4096;
	Telemetry.VolumetricCloud.VolumetricCloudRouteReasons[static_cast<size_t>(Durin::FVolumetricCloudSpatialRenderer::ERouteReason::Compute)] = 1;

	const Durin::FSceneViewStatistics Statistics =
		Durin::BuildSceneViewStatistics(Telemetry);
	EXPECT_EQ(Statistics.Visibility.SubmittedPrimitives, 13u);
	EXPECT_EQ(Statistics.Visibility.VisiblePrimitives, 8u);
	EXPECT_EQ(Statistics.StaticMesh.Primitives, 4u);
	EXPECT_EQ(Statistics.SplineMesh.Primitives, 1u);
	EXPECT_EQ(Statistics.StaticMesh.Triangles, 100u);
	EXPECT_EQ(Statistics.SplineMesh.Triangles, 20u);
	EXPECT_EQ(Statistics.Summary.Triangles, 120u);
	EXPECT_EQ(Statistics.Shadow.Triangles, 500u);
	EXPECT_EQ(Statistics.StaticMesh.DrawCalls, 9u);
	EXPECT_EQ(Statistics.Shadow.DrawCalls, 7u);
	EXPECT_TRUE(Statistics.Shadow.bEnabled);
	EXPECT_EQ(Statistics.Shadow.Cascades, 3u);
	EXPECT_TRUE(Statistics.Shadow.bContactEnabled);
	EXPECT_EQ(Statistics.Shadow.ContactRoute, Durin::EContactShadowExecutionRoute::Compute);
	EXPECT_EQ(Statistics.Lights.Directional, 1u);
	EXPECT_EQ(Statistics.Lights.Point, 3u);
	EXPECT_EQ(Statistics.Lights.Spot, 2u);
	EXPECT_EQ(Statistics.VolumetricCloud.Quality, Durin::EVolumetricCloudQuality::Epic);
	EXPECT_EQ(Statistics.VolumetricCloud.DebugMode, Durin::EVolumetricCloudDebugMode::Transmittance);
	EXPECT_EQ(Statistics.VolumetricCloud.Route, Durin::EVolumetricCloudExecutionRoute::Compute);
	EXPECT_EQ(Statistics.VolumetricCloud.Reason, Durin::EVolumetricCloudRouteReason::Compute);
	EXPECT_EQ(Statistics.VolumetricCloud.TargetWidth, 960u);
	EXPECT_EQ(Statistics.VolumetricCloud.PrimarySamples, 1000u);
	EXPECT_EQ(Statistics.VolumetricCloud.RetainedBytes, 4096u);
	EXPECT_TRUE(Statistics.VolumetricCloud.bHistoryAvailable);
	EXPECT_TRUE(Statistics.VolumetricCloud.bHistoryAccepted);
	EXPECT_FALSE(Statistics.VolumetricCloud.bGPUTimingAvailable);

	Telemetry.ContactShadow.ContactShadowComputeViews = 0;
	Telemetry.ContactShadow.ContactShadowFragmentViews = 1;
	const Durin::FSceneViewStatistics FragmentStatistics =
		Durin::BuildSceneViewStatistics(Telemetry);
	EXPECT_EQ(FragmentStatistics.Shadow.ContactRoute, Durin::EContactShadowExecutionRoute::Fragment);
}

TEST(FRendererSceneContractTests, SceneViewportStatisticsAreCoherentAndIsolated)
{
	FRenderingThreadScope RenderingThread;
	const std::shared_ptr<Durin::FSceneViewport> Main =
		Durin::FSceneViewport::CreateOffscreen(nullptr);
	const std::shared_ptr<Durin::FSceneViewport> Auxiliary =
		Durin::FSceneViewport::CreateOffscreen(nullptr);

	Durin::FSceneViewStatistics MainStatistics;
	MainStatistics.Visibility.VisiblePrimitives = 4;
	MainStatistics.Summary.Triangles = 120;
	MainStatistics.Summary.DrawCalls = 7;
	Durin::FSceneViewStatistics AuxiliaryStatistics;
	AuxiliaryStatistics.Visibility.VisiblePrimitives = 1;
	AuxiliaryStatistics.Summary.Triangles = 12;
	AuxiliaryStatistics.Summary.DrawCalls = 3;

	struct FPublishViewportStatisticsCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "PublishViewportStatistics";
		}
	};
	Durin::EnqueueRenderCommand<FPublishViewportStatisticsCommand>(
		[Main, Auxiliary, MainStatistics, AuxiliaryStatistics](
			Durin::FRHICommandListImmediate&
		) {
			Main->PublishRenderStatistics_RenderThread(MainStatistics, true);
			Auxiliary->PublishRenderStatistics_RenderThread(
				AuxiliaryStatistics, true
			);
		}
	);
	Durin::FlushRenderingCommands();

	Durin::FSceneViewportStatisticsSnapshot MainSnapshot =
		Main->GetRenderStatisticsSnapshot();
	const Durin::FSceneViewportStatisticsSnapshot AuxiliarySnapshot =
		Auxiliary->GetRenderStatisticsSnapshot();
	EXPECT_TRUE(MainSnapshot.bAvailable);
	EXPECT_EQ(MainSnapshot.Revision, 1u);
	EXPECT_EQ(MainSnapshot.Statistics, MainStatistics);
	EXPECT_TRUE(AuxiliarySnapshot.bAvailable);
	EXPECT_EQ(AuxiliarySnapshot.Revision, 1u);
	EXPECT_EQ(AuxiliarySnapshot.Statistics, AuxiliaryStatistics);

	Durin::EnqueueRenderCommand<FPublishViewportStatisticsCommand>(
		[Main](Durin::FRHICommandListImmediate&) {
			Main->PublishRenderStatistics_RenderThread({}, false);
		}
	);
	Durin::FlushRenderingCommands();
	MainSnapshot = Main->GetRenderStatisticsSnapshot();
	EXPECT_FALSE(MainSnapshot.bAvailable);
	EXPECT_EQ(MainSnapshot.Revision, 2u);
	EXPECT_EQ(MainSnapshot.Statistics, Durin::FSceneViewStatistics{});
	EXPECT_EQ(Auxiliary->GetRenderStatisticsSnapshot(), AuxiliarySnapshot);
}

TEST(FRendererSceneContractTests, SceneViewportStatisticsPublishDuringConcurrentReads)
{
	FRenderingThreadScope RenderingThread;
	auto Viewport = Durin::FSceneViewport::CreateOffscreen(nullptr);
	std::atomic<bool> bReaderReady = false;
	std::atomic<bool> bStopReader = false;
	std::atomic<bool> bObservedMixedSnapshot = false;
	std::atomic<uint64> ReadCount = 0;
	std::jthread Reader([&] {
		bReaderReady.store(true, std::memory_order_release);
		while (!bStopReader.load(std::memory_order_acquire))
		{
			const auto Snapshot = Viewport->GetRenderStatisticsSnapshot();
			if (Snapshot.bAvailable
				&& Snapshot.Statistics.Summary.Triangles != Snapshot.Statistics.Summary.DrawCalls)
				bObservedMixedSnapshot.store(true, std::memory_order_relaxed);
			ReadCount.fetch_add(1, std::memory_order_relaxed);
		}
	});
	while (!bReaderReady.load(std::memory_order_acquire))
		std::this_thread::yield();

	struct FPublishConcurrentViewportStatisticsCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "PublishConcurrentViewportStatistics";
		}
	};
	Durin::EnqueueRenderCommand<FPublishConcurrentViewportStatisticsCommand>(
		[Viewport](Durin::FRHICommandListImmediate&) {
			for (uint64 Value = 1; Value <= 1000; ++Value)
			{
				Durin::FSceneViewStatistics Statistics;
				Statistics.Summary.Triangles = Value;
				Statistics.Summary.DrawCalls = Value;
				Viewport->PublishRenderStatistics_RenderThread(Statistics, true);
			}
		}
	);
	Durin::FlushRenderingCommands();
	bStopReader.store(true, std::memory_order_release);
	Reader.join();

	EXPECT_GT(ReadCount.load(std::memory_order_relaxed), 0u);
	EXPECT_FALSE(bObservedMixedSnapshot.load(std::memory_order_relaxed));
	const auto Snapshot = Viewport->GetRenderStatisticsSnapshot();
	EXPECT_EQ(Snapshot.Revision, 1000u);
	EXPECT_EQ(Snapshot.Statistics.Summary.Triangles, 1000u);
	EXPECT_EQ(Snapshot.Statistics.Summary.DrawCalls, 1000u);

	std::weak_ptr<Durin::FSceneViewport> WeakViewport = Viewport;
	Durin::EnqueueRenderCommand<FPublishConcurrentViewportStatisticsCommand>(
		[Viewport](Durin::FRHICommandListImmediate&) {
			Viewport->PublishRenderStatistics_RenderThread({}, false);
		}
	);
	Viewport.reset();
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(WeakViewport.expired());
}

TEST(FRendererSceneContractTests, SceneViewportRenderGraphCaptureIsExplicitAndIsolated)
{
	FRenderingThreadScope RenderingThread;
	const std::shared_ptr<Durin::FSceneViewport> Main =
		Durin::FSceneViewport::CreateOffscreen(nullptr);
	const std::shared_ptr<Durin::FSceneViewport> Auxiliary =
		Durin::FSceneViewport::CreateOffscreen(nullptr);

	EXPECT_FALSE(Main->ConsumeRenderGraphCaptureRequest());
	Main->RequestRenderGraphCapture();
	EXPECT_TRUE(Main->ConsumeRenderGraphCaptureRequest());
	EXPECT_FALSE(Main->ConsumeRenderGraphCaptureRequest());
	EXPECT_FALSE(Auxiliary->ConsumeRenderGraphCaptureRequest());

	auto Capture = std::make_shared<Durin::FRDGCapture>();
	Capture->Dump = "main graph";
	Capture->Statistics.DeclaredPasses = 7;
	struct FPublishViewportRenderGraphCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "PublishViewportRenderGraph";
		}
	};
	Durin::EnqueueRenderCommand<FPublishViewportRenderGraphCommand>(
		[Main, Capture](Durin::FRHICommandListImmediate&) {
			Main->PublishRenderGraphCapture_RenderThread(Capture, true);
		}
	);
	Durin::FlushRenderingCommands();

	const Durin::FSceneViewportRenderGraphSnapshot MainSnapshot =
		Main->GetRenderGraphSnapshot();
	EXPECT_TRUE(MainSnapshot.bAvailable);
	ASSERT_NE(MainSnapshot.Capture, nullptr);
	EXPECT_EQ(MainSnapshot.Revision, 1u);
	EXPECT_EQ(MainSnapshot.Capture->Dump, "main graph");
	EXPECT_EQ(MainSnapshot.Capture->Statistics.DeclaredPasses, 7u);
	EXPECT_FALSE(Auxiliary->GetRenderGraphSnapshot().bAvailable);

	Durin::EnqueueRenderCommand<FPublishViewportRenderGraphCommand>(
		[Main](Durin::FRHICommandListImmediate&) {
			Main->PublishRenderGraphCapture_RenderThread(nullptr, false);
		}
	);
	Durin::FlushRenderingCommands();
	const Durin::FSceneViewportRenderGraphSnapshot Failed =
		Main->GetRenderGraphSnapshot();
	EXPECT_FALSE(Failed.bAvailable);
	EXPECT_EQ(Failed.Revision, 2u);
	EXPECT_EQ(Failed.Capture, nullptr);
	EXPECT_EQ(MainSnapshot.Capture->Dump, "main graph");
}

TEST(FRendererSceneContractTests, DirectionalShadowCandidatesStartFromSceneAndKeepRelevantOffCameraCasters)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.25), Durin::FVector3(0.25)
	);
	auto Add = [&](uint64 Id, const Durin::FVector3& Position) {
		Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveComponentId(Id), std::make_unique<Durin::FStaticMeshSceneProxy>(&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}), Durin::Math::TranslationMatrix(Position));
	};
	Add(1, {3.0, 0.0, 0.0});
	Add(2, {3.0, 10.0, 0.0});
	Add(3, {3.0, 1000.0, 0.0});
	Durin::FlushRenderingCommands();
	Durin::FSceneView View;
	View.ProjectionMatrix = MakePerspectiveProjection();
	View.ViewProjectionMatrix = View.ProjectionMatrix;
	View.Settings.DirectionalShadow.Candidate =
		Durin::EDirectionalShadowCandidate::SingleMap;
	View.ViewportWidth = 1920;
	View.ViewportHeight = 1080;
	Durin::FViewRenderTelemetry CameraTelemetry;
	const Durin::FSceneVisibilityResult Camera =
		Durin::PrepareSceneVisibility(Scene, View, CameraTelemetry);
	ASSERT_EQ(Camera.SceneInfos.size(), 1u);
	EXPECT_EQ(Camera.SceneInfos.front()->GetId().Value, 1u);

	Durin::FDirectionalLightSceneData Light;
	Light.Direction = {0.0, 0.0, -1.0};
	Light.Intensity = 1.0f;
	Durin::FPreparedDirectionalShadowView Shadow;
	View.FeatureLevel = Durin::ERHIFeatureLevel::ES3_1;
	ASSERT_TRUE(Durin::TryPrepareDirectionalShadowView(
		View, Durin::FLightComponentId(8), Light, Shadow
	));
	EXPECT_EQ(Shadow.Cascades[0].CasterView.GetFeatureLevel(), View.GetFeatureLevel());
	const Durin::FDirectionalShadowCasterTable Table =
		Durin::PrepareDirectionalShadowCasterTable(Scene, Shadow);
	const Durin::FDirectionalShadowCasterCandidates& Casters = Table.Cascades[0];
	ASSERT_EQ(Casters.SceneInfos.size(), 2u);
	EXPECT_EQ(Casters.SceneInfos[0]->GetId().Value, 1u);
	EXPECT_EQ(Casters.SceneInfos[1]->GetId().Value, 2u);
	EXPECT_EQ(Casters.Culled, 1u);
	const Durin::FDirectionalShadowCasterTable ComparisonTable =
		Durin::PrepareDirectionalShadowCasterTable(Scene, Shadow, true);
	const Durin::FDirectionalShadowCasterCandidates& Comparison =
		ComparisonTable.Cascades[0];
	EXPECT_EQ(Comparison.SceneInfos.size(), 3u);
	EXPECT_EQ(Table.SceneTraversals, 1u);
	EXPECT_EQ(Table.UniqueSubmitted, 3u);
	EXPECT_EQ(Table.CascadeClassificationTests, 3u);
	EXPECT_EQ(Table.MembershipPopcount, 2u);
}

TEST(FRendererSceneContractTests, DirectionalShadowCasterTableBuildsZeroThroughAllCascadeMasksOnce)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.05), Durin::FVector3(0.05)
	);
	for (const auto [Id, X] : std::array{
			 std::pair<uint64, double>{1u, 0.0},
			 std::pair<uint64, double>{2u, 1.5},
			 std::pair<uint64, double>{3u, 2.5},
			 std::pair<uint64, double>{4u, 10.0}
		 })
	{
		Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveComponentId(Id), std::make_unique<Durin::FStaticMeshSceneProxy>(&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}), Durin::Math::TranslationMatrix(Durin::FVector3(X, 0.0, 0.0)));
	}
	Durin::FlushRenderingCommands();

	Durin::FPreparedDirectionalShadowView Shadow;
	Shadow.bEnabled = true;
	Shadow.CascadeCount = Durin::DirectionalShadowCascadeCount;
	for (uint32 CascadeIndex = 0;
		 CascadeIndex < Shadow.CascadeCount; ++CascadeIndex)
	{
		auto& Cascade = Shadow.Cascades[CascadeIndex];
		Cascade.bEnabled = true;
		Cascade.CasterVolume.Right = {1.0, 0.0, 0.0};
		Cascade.CasterVolume.Up = {0.0, 1.0, 0.0};
		Cascade.CasterVolume.Forward = {0.0, 0.0, 1.0};
		const double Extent = static_cast<double>(CascadeIndex + 1);
		Cascade.CasterVolume.Minimum = {-Extent, -Extent, -Extent};
		Cascade.CasterVolume.Maximum = {Extent, Extent, Extent};
	}
	const Durin::FDirectionalShadowCasterTable Table =
		Durin::PrepareDirectionalShadowCasterTable(Scene, Shadow);
	ASSERT_EQ(Table.Records.size(), 4u);
	std::array<uint8, 4> Masks{};
	for (const Durin::FDirectionalShadowCasterRecord& Record : Table.Records)
	{
		ASSERT_NE(Record.SceneInfo, nullptr);
		Masks[Record.SceneInfo->GetId().Value - 1] = Record.CascadeMask;
	}
	EXPECT_EQ(Masks[0], 0b111u);
	EXPECT_EQ(Masks[1], 0b110u);
	EXPECT_EQ(Masks[2], 0b100u);
	EXPECT_EQ(Masks[3], 0u);
	EXPECT_EQ(Table.SceneTraversals, 1u);
	EXPECT_EQ(Table.CascadeClassificationTests, 12u);
	EXPECT_EQ(Table.MembershipPopcount, 6u);
	EXPECT_EQ(Table.Cascades[0].SceneInfos.size(), 1u);
	EXPECT_EQ(Table.Cascades[1].SceneInfos.size(), 2u);
	EXPECT_EQ(Table.Cascades[2].SceneInfos.size(), 3u);
}

TEST(FRendererSceneContractTests, PrimitiveMembershipOwnsClassificationBoundsAndFifoLifetime)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-1.0, -2.0, -3.0),
		Durin::FVector3(1.0, 2.0, 3.0)
	);
	const Durin::FPrimitiveComponentId Id(41);
	const Durin::FMatrix InitialTransform = Durin::Math::TranslationMatrix(
		Durin::FVector3(10.0, 20.0, 30.0)
	);

	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}), InitialTransform);
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
	ASSERT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetPrimitiveSceneInfos().front();
	EXPECT_EQ(Info->GetId(), Id);
	EXPECT_TRUE(Info->GetLocalBounds().bIsValid);
	EXPECT_EQ(Info->GetWorldBounds().Min, Durin::FVector3(9.0, 18.0, 27.0));
	EXPECT_EQ(Info->GetWorldBounds().Max, Durin::FVector3(11.0, 22.0, 33.0));
	Scene.UpdatePrimitiveVisibility(Id, false);
	Durin::FlushRenderingCommands();
	EXPECT_FALSE(Info->IsVisible());
	Scene.UpdatePrimitiveVisibility(Id, true);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Info->IsVisible());
	Scene.UpdatePrimitiveTransform(
		Id,
		Durin::Math::ScaleMatrix(Durin::FVector3(-2.0, 3.0, 0.5))
	);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Info->GetWorldBounds().Min, Durin::FVector3(-2.0, -6.0, -1.5));
	EXPECT_EQ(Info->GetWorldBounds().Max, Durin::FVector3(2.0, 6.0, 1.5));

	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, nullptr, Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);

	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(Scene, Id);
	Scene.UpdatePrimitiveTransform(Id, Durin::FMatrix(2.0));
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());

	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FStaticMeshSceneProxy>(&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
}

TEST(FRendererSceneContractTests, VisibilityClassifiesOnceAndKeepsFallbacksVisible)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	Durin::FStaticMeshRenderData ValidRenderData;
	ValidRenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.5), Durin::FVector3(0.5)
	);
	Durin::FStaticMeshRenderData InvalidBoundsRenderData;

	auto AddStaticMesh = [&](uint64 Id, const Durin::FVector3& Location,
							 bool bVisible, const Durin::FStaticMeshRenderData* RenderData) {
		Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveComponentId(Id), std::make_unique<Durin::FStaticMeshSceneProxy>(RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}), Durin::Math::TranslationMatrix(Location), bVisible);
	};
	AddStaticMesh(1, {3.0, 0.0, 0.0}, true, &ValidRenderData);
	AddStaticMesh(2, {3.0, 0.0, 0.0}, false, &ValidRenderData);
	AddStaticMesh(3, {3.0, 20.0, 0.0}, true, &ValidRenderData);
	AddStaticMesh(4, {3.0, 0.0, 0.0}, true, &InvalidBoundsRenderData);
	Durin::FlushRenderingCommands();

	Durin::FSceneView View;
	View.ProjectionMatrix = MakePerspectiveProjection();
	View.ViewProjectionMatrix = View.ProjectionMatrix;
	Durin::FViewRenderTelemetry Telemetry;
	Telemetry.Visibility.SubmittedPrimitives = 99;
	Telemetry.Lighting.SelectedPointLights = 7;
	Telemetry.VolumetricCloud.VolumetricCloudQuality =
		Durin::EVolumetricCloudQuality::Epic;
	Durin::FSceneVisibilityResult Visibility;
	Durin::PrepareSceneVisibility(Scene, View, Telemetry, Visibility, true);
	EXPECT_EQ(Visibility.PrimitiveRecords.size(), 4u);
	EXPECT_EQ(Telemetry.Visibility.SubmittedPrimitives, 4u);
	EXPECT_EQ(Telemetry.Visibility.HiddenPrimitives, 1u);
	EXPECT_EQ(Telemetry.Visibility.FrustumCulledPrimitives, 1u);
	EXPECT_EQ(Telemetry.Visibility.VisiblePrimitives, 2u);
	EXPECT_EQ(Telemetry.Visibility.InvalidBoundsFallbacks, 1u);
	EXPECT_EQ(Telemetry.Visibility.InvalidViewFallbacks, 0u);
	EXPECT_EQ(Telemetry.Lighting.SelectedPointLights, 7u);
	EXPECT_EQ(Telemetry.VolumetricCloud.VolumetricCloudQuality, Durin::EVolumetricCloudQuality::Epic);
	EXPECT_EQ(Visibility.SceneInfos.size(), 2u);
	const auto CandidateCapacity = Visibility.SceneInfos.capacity();
	Durin::PrepareSceneVisibility(Scene, View, Telemetry, Visibility);
	EXPECT_TRUE(Visibility.PrimitiveRecords.empty());
	EXPECT_EQ(Visibility.SceneInfos.size(), 2u);
	EXPECT_EQ(Visibility.SceneInfos.capacity(), CandidateCapacity);
	EXPECT_EQ(Telemetry.Visibility.SubmittedPrimitives, 4u);

	View.Settings.Mode.VisibilityMode =
		Durin::EViewVisibilityMode::FrustumCullingDisabled;
	Durin::FViewRenderTelemetry DisabledTelemetry;
	const Durin::FSceneVisibilityResult Disabled =
		Durin::PrepareSceneVisibility(Scene, View, DisabledTelemetry);
	EXPECT_EQ(DisabledTelemetry.Visibility.HiddenPrimitives, 1u);
	EXPECT_EQ(DisabledTelemetry.Visibility.FrustumCulledPrimitives, 0u);
	EXPECT_EQ(DisabledTelemetry.Visibility.VisiblePrimitives, 3u);
	EXPECT_EQ(Disabled.SceneInfos.size(), 3u);
	EXPECT_TRUE(Disabled.PrimitiveRecords.empty());
	EXPECT_EQ(Disabled.PrimitiveRecords.capacity(), 0u);

	View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::Normal;
	View.ProjectionMatrix[0][0] =
		std::numeric_limits<double>::quiet_NaN();
	Durin::FViewRenderTelemetry InvalidViewTelemetry;
	const Durin::FSceneVisibilityResult InvalidView =
		Durin::PrepareSceneVisibility(Scene, View, InvalidViewTelemetry);
	EXPECT_EQ(InvalidViewTelemetry.Visibility.HiddenPrimitives, 1u);
	EXPECT_EQ(InvalidViewTelemetry.Visibility.VisiblePrimitives, 3u);
	EXPECT_EQ(InvalidViewTelemetry.Visibility.InvalidViewFallbacks, 3u);
	EXPECT_EQ(InvalidView.SceneInfos.size(), 3u);
}

TEST(FRendererSceneContractTests, VisibilityPolicyAndSequentialViewsAreIndependent)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.5), Durin::FVector3(0.5)
	);
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Durin::FPrimitiveComponentId(9), std::make_unique<Durin::FStaticMeshSceneProxy>(&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}), Durin::Math::TranslationMatrix(Durin::FVector3(3.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();

	Durin::FSceneView MainView;
	MainView.ProjectionMatrix = MakePerspectiveProjection();
	MainView.ViewProjectionMatrix = MainView.ProjectionMatrix;
	Durin::FSceneView AuxiliaryView = MainView;
	AuxiliaryView.ViewProjectionMatrix = AuxiliaryView.ProjectionMatrix
										 * Durin::Math::TranslationMatrix(Durin::FVector3(0.0, -20.0, 0.0));

	Durin::FViewRenderTelemetry MainTelemetry;
	Durin::FViewRenderTelemetry AuxiliaryTelemetry;
	Durin::FSceneVisibilityResult Scratch;
	Durin::PrepareSceneVisibility(Scene, AuxiliaryView, AuxiliaryTelemetry, Scratch);
	EXPECT_TRUE(Scratch.SceneInfos.empty());
	EXPECT_EQ(Scratch.SceneInfos.capacity(), 0u);
	Durin::PrepareSceneVisibility(Scene, MainView, MainTelemetry, Scratch, true);
	ASSERT_EQ(Scratch.SceneInfos.size(), 1u);
	ASSERT_EQ(Scratch.PrimitiveRecords.size(), 1u);
	const auto* CandidateStorage = Scratch.SceneInfos.data();
	Durin::PrepareSceneVisibility(Scene, AuxiliaryView, AuxiliaryTelemetry, Scratch);
	EXPECT_TRUE(Scratch.SceneInfos.empty());
	EXPECT_TRUE(Scratch.PrimitiveRecords.empty());
	Durin::PrepareSceneVisibility(Scene, MainView, MainTelemetry, Scratch);
	ASSERT_EQ(Scratch.SceneInfos.size(), 1u);
	EXPECT_EQ(Scratch.SceneInfos.data(), CandidateStorage);
	EXPECT_EQ(Scratch.SceneInfos.front()->GetId(), Durin::FPrimitiveComponentId(9));
	EXPECT_EQ(
		Durin::PrepareSceneVisibility(Scene, MainView, MainTelemetry)
			.SceneInfos.size(),
		1u
	);
	EXPECT_TRUE(
		Durin::PrepareSceneVisibility(Scene, AuxiliaryView, AuxiliaryTelemetry)
			.SceneInfos.empty()
	);
	Durin::FViewRenderTelemetry RepeatedMainTelemetry;
	EXPECT_EQ(
		Durin::PrepareSceneVisibility(Scene, MainView, RepeatedMainTelemetry)
			.SceneInfos.size(),
		1u
	);

	Durin::FSceneView SubmittedView = MainView;
	SubmittedView.Settings.Mode.VisibilityMode =
		Durin::EViewVisibilityMode::FrustumCullingDisabled;
	auto ObservedMode = std::make_shared<Durin::EViewVisibilityMode>(
		Durin::EViewVisibilityMode::Normal
	);
	struct FObserveVisibilityPolicyCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveVisibilityPolicy";
		}
	};
	Durin::EnqueueRenderCommand<FObserveVisibilityPolicyCommand>(
		[ViewSnapshot = SubmittedView, ObservedMode](
			Durin::FRHICommandListImmediate&
		) {
			*ObservedMode = ViewSnapshot.Settings.Mode.VisibilityMode;
		}
	);
	SubmittedView.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::Normal;
	Durin::FlushRenderingCommands();
	EXPECT_EQ(
		*ObservedMode,
		Durin::EViewVisibilityMode::FrustumCullingDisabled
	);
}

TEST(FRendererSceneContractTests, DirectionalLightProxyOutlivesPublisherAndUsesFifoRetirement)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	const Durin::FLightComponentId Id(7);
	Durin::FLightSceneProxy* FirstToken = nullptr;
	{
		Durin::FDirectionalLightSceneData Data;
		Data.Intensity = 2.0f;
		FirstToken = PublishLightForTest<Durin::FDirectionalLightSceneProxy>(
			Scene, Id, Data
		);
	}
	Durin::FlushRenderingCommands();
	FObservedLight Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 2.0f);

	Durin::FDirectionalLightSceneData Replacement;
	Replacement.Intensity = 5.0f;
	Durin::FSceneInterfaceTestAccess::TryRemoveLightProxy(Scene, FirstToken);
	auto* ReplacementToken =
		PublishLightForTest<Durin::FDirectionalLightSceneProxy>(
			Scene, Id, Replacement
		);
	Durin::FSceneInterfaceTestAccess::TryRemoveLightProxy(Scene, ReplacementToken);
	Durin::FlushRenderingCommands();
	EXPECT_FALSE(ObserveLight(Scene).bPresent);

	PublishLightForTest<Durin::FDirectionalLightSceneProxy>(
		Scene, Id, Replacement
	);
	Durin::FlushRenderingCommands();
	Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 5.0f);
}

TEST(FRendererSceneContractTests, ComponentPublicationRequiresCommandAdmission)
{
	ASSERT_EQ(Durin::GetRenderCommandAdmissionState(), Durin::ERenderCommandAdmissionState::Stopped);
	EXPECT_DEATH_IF_SUPPORTED({
		auto Scene = Durin::FSceneInterfaceTestAccess::CreateScene();
		auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "RejectedScenePublicationWorld");
		EXPECT_TRUE(World->InitializeSubsystems());
		auto* Level = Durin::NewObject<Durin::DLevel>(World, "RejectedScenePublicationLevel");
		World->SetCurrentLevel(Level);
		World->SetRenderScene(Scene.get());
		Level->SpawnActor<Durin::ADirectionalLightActor>("RejectedScenePublicationLight"); }, "");
}

TEST(FRendererSceneContractTests, LightFamiliesReplaceTypedMembershipAtomically)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	Durin::FPointLightSceneData Point;
	Point.Intensity = 1.0f;
	auto* PointToken = PublishLightForTest<Durin::FPointLightSceneProxy>(
		Scene, Durin::FLightComponentId(77), Point
	);
	Durin::FSpotLightSceneData Spot;
	Spot.Intensity = 1.0f;
	Durin::FSceneInterfaceTestAccess::TryRemoveLightProxy(Scene, PointToken);
	PublishLightForTest<Durin::FSpotLightSceneProxy>(
		Scene, Durin::FLightComponentId(77), Spot
	);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPointLightSceneInfos().empty());
	ASSERT_EQ(Scene.GetSpotLightSceneInfos().size(), 1u);
	EXPECT_EQ(Scene.GetSpotLightSceneInfos().front()->GetId().Value, 77u);
}

TEST(FRendererSceneContractTests, PreparedLightsUseStableIdAndSharedLocalBudget)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	for (uint64 Id : {101u, 100u})
	{
		Durin::FDirectionalLightSceneData Data;
		Data.Intensity = 1.0f;
		PublishLightForTest<Durin::FDirectionalLightSceneProxy>(
			Scene, Durin::FLightComponentId(Id), Data
		);
	}
	for (uint64 Id = 10; Id > 0; --Id)
	{
		if ((Id & 1u) == 0)
		{
			Durin::FPointLightSceneData Data;
			Data.Intensity = 1.0f;
			Data.Range = 5.0f;
			PublishLightForTest<Durin::FPointLightSceneProxy>(
				Scene, Durin::FLightComponentId(Id), Data
			);
		}
		else
		{
			Durin::FSpotLightSceneData Data;
			Data.Intensity = 1.0f;
			Data.Range = 5.0f;
			PublishLightForTest<Durin::FSpotLightSceneProxy>(
				Scene, Durin::FLightComponentId(Id), Data
			);
		}
	}
	Durin::FlushRenderingCommands();
	struct FObservedPreparation
	{
		Durin::FPreparedLightView Lights;
		Durin::FViewRenderTelemetry Telemetry;
	};
	auto Observed = std::make_shared<FObservedPreparation>();
	Durin::FSceneView View;
	View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
	struct FPrepareLightsCommand
	{
		static constexpr auto GetName() -> const char* { return "PrepareLights"; }
	};
	Durin::EnqueueRenderCommand<FPrepareLightsCommand>(
		[&Scene, View, Observed](Durin::FRHICommandListImmediate&) {
			Observed->Lights = Durin::PrepareLightView_RenderThread(
				Scene, View, Observed->Telemetry
			);
		}
	);
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Observed->Lights.Directional.size(), 1u);
	EXPECT_EQ(Observed->Lights.Directional.front().Id.Value, 100u);
	ASSERT_EQ(Observed->Lights.Local.size(), 4u);
	for (size_t Index = 0; Index < Observed->Lights.Local.size(); ++Index)
		EXPECT_EQ(Observed->Lights.Local[Index].Id.Value, Index + 1);
	EXPECT_EQ(Observed->Telemetry.Lighting.OverflowDirectionalLights, 1u);
	EXPECT_EQ(Observed->Telemetry.Lighting.SelectedPointLights, 2u);
	EXPECT_EQ(Observed->Telemetry.Lighting.SelectedSpotLights, 2u);
	EXPECT_EQ(Observed->Telemetry.Lighting.OverflowPointLights, 3u);
	EXPECT_EQ(Observed->Telemetry.Lighting.OverflowSpotLights, 3u);
	EXPECT_EQ(Observed->Telemetry.Lighting.PackedLightBytes, sizeof(Durin::FForwardLightingUniform));
}

TEST(FRendererSceneContractTests, PreparedLightsCullOnlyOutsideLocalInfluenceBounds)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	auto AddPoint = [&](uint64 Id, const Durin::FVector3& Position) {
		Durin::FPointLightSceneData Data;
		Data.Position = Position;
		Data.Intensity = 1.0f;
		Data.Range = 1.0f;
		PublishLightForTest<Durin::FPointLightSceneProxy>(
			Scene, Durin::FLightComponentId(Id), Data
		);
	};
	AddPoint(1, {3.0, 0.0, 0.0});
	AddPoint(2, {3.0, 20.0, 0.0});
	Durin::FlushRenderingCommands();
	auto Observed = std::make_shared<std::pair<
		Durin::FPreparedLightView, Durin::FViewRenderTelemetry>>();
	Durin::FSceneView View;
	View.ProjectionMatrix = MakePerspectiveProjection();
	View.ViewProjectionMatrix = View.ProjectionMatrix;
	struct FCullLightsCommand
	{
		static constexpr auto GetName() -> const char* { return "CullLights"; }
	};
	Durin::EnqueueRenderCommand<FCullLightsCommand>(
		[&Scene, View, Observed](Durin::FRHICommandListImmediate&) {
			Observed->first = Durin::PrepareLightView_RenderThread(
				Scene, View, Observed->second
			);
		}
	);
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Observed->first.Local.size(), 1u);
	EXPECT_EQ(Observed->first.Local.front().Id.Value, 1u);
	EXPECT_EQ(Observed->second.Lighting.FrustumCulledPointLights, 1u);
}

TEST(FRendererSceneContractTests, SplineDeformationAndBoundsUpdateAtomicallyInTypedMembership)
{
	FRenderingThreadScope RenderingThread;
	Durin::FSceneTestOwner SceneOwner;
	Durin::FScene& Scene = *SceneOwner;
	Durin::FStaticMeshRenderData RenderData;
	Durin::FSplineMeshRenderDynamicData First{
		.Params = {},
		.LocalBounds = Durin::FBox({0.0, -1.0, -1.0}, {10.0, 1.0, 1.0})
	};
	const Durin::FPrimitiveComponentId Id(92);
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FSplineMeshSceneProxy>(&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, First), Durin::Math::TranslationMatrix(Durin::FVector3(2.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetPrimitiveSceneInfos().front();
	EXPECT_EQ(Info->GetSplineMeshProxy().GetDynamicData().Params, First.Params);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 2.0);

	auto Second = First;
	Second.Params.EndPosition = {20.0, 10.0, 0.0};
	Second.LocalBounds = Durin::FBox({-2.0, -3.0, -1.0}, {23.0, 13.0, 1.0});
	Scene.UpdateSplineMeshDynamicData(Id, Second);
	Durin::FlushRenderingCommands();
	Info = Scene.GetPrimitiveSceneInfos().front();
	EXPECT_EQ(Info->GetSplineMeshProxy().GetDynamicData().Params, Second.Params);
	EXPECT_EQ(Info->GetSplineMeshProxy().GetAcceptedDynamicUpdateCount(), 1u);
	EXPECT_EQ(Info->GetLocalBounds().Min.x, -2.0);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 0.0);
	auto Invalid = Second;
	Invalid.LocalBounds = {};
	Scene.UpdateSplineMeshDynamicData(Id, Invalid);
	Durin::FlushRenderingCommands();
	Info = Scene.GetPrimitiveSceneInfos().front();
	EXPECT_EQ(Info->GetSplineMeshProxy().GetDynamicData().Params, Second.Params);
	EXPECT_EQ(Info->GetSplineMeshProxy().GetAcceptedDynamicUpdateCount(), 1u);
	EXPECT_EQ(Info->GetLocalBounds().Min.x, -2.0);

	Durin::FViewRenderTelemetry Telemetry;
	Durin::FSceneView View;
	View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
	const Durin::FSceneVisibilityResult Visibility =
		Durin::PrepareSceneVisibility(Scene, View, Telemetry);
	EXPECT_EQ(Visibility.SceneInfos.size(), 1u);
	EXPECT_EQ(Telemetry.SplineMesh.VisibleSplineMeshCandidates, 1u);
	Durin::FSceneInterfaceTestAccess::RemovePrimitiveProxy(Scene, Id);
	Scene.UpdateSplineMeshDynamicData(Id, Second); // No membership; cannot affect the replacement.
	Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene, Id, std::make_unique<Durin::FSplineMeshSceneProxy>(&RenderData,
		std::vector<Durin::FMaterialRenderProxyRef>{}, First), Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().front()->GetSplineMeshProxy().GetDynamicData().Params, First.Params);
	Scene.UpdateSplineMeshDynamicData(Id, Second);
	Scene.UpdateSplineMeshDynamicData(Id, First); // Later submission wins even when its values repeat.
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().front()->GetSplineMeshProxy().GetDynamicData().Params, First.Params);
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().front()->GetSplineMeshProxy().GetAcceptedDynamicUpdateCount(), 2u);

}

TEST(FRendererSceneContractTests, CollectsIndependentGeometrySnapshotsTransactionally)
{
	using namespace Durin;
	class alignas(64) FTestBinding final : public FVertexFactoryBinding
	{
	public:
		explicit FTestBinding(int& InDestructions) : Destructions(InDestructions) {}
		~FTestBinding() override { ++Destructions; }
		auto GetFactoryKey() const -> FXxHash64 override { return {123}; }
		auto GetLayoutKey() const -> FXxHash64 override { return {456}; }
	private:
		int& Destructions;
	};
	int Destructions = 0;
	{
		auto Binding = std::make_shared<const FTestBinding>(Destructions);
		FMeshBatch Batch{
			.PrimitiveId = FPrimitiveComponentId(77),
			.BatchId = 5,
			.LocalToWorld = FMatrix(1.0),
			.WorldBounds = FBox({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}),
			.FactoryKey = Binding->GetFactoryKey(),
			.LayoutKey = Binding->GetLayoutKey(),
			.Binding = Binding};
		FMeshBatchCollector Collector(EMeshCollectionPurpose::Receiver);
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::Empty);
		EXPECT_EQ(Collector.GetOutcomeCount(EGeometrySubmissionOutcome::Empty), 1u);
		FMeshBatchElement Element{
			.ElementId = 8,
			.Draw = {.bIndexed = false, .ElementCount = 3},
			.Vertices = {
				.Buffer = new FRHIBuffer(FRHIBufferCreateDesc::CreateVertex("CollectionContract", 36)),
				.Range = {36, 0, 12, 12}}};
		Batch.Elements.push_back(Element);
		Batch.Elements.push_back(Element);
		Batch.Elements.back().ElementId = 9;
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::Submitted);
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
		Batch.BatchId = 6;
		Batch.Elements.back().Draw.ElementCount = 6;
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
		EXPECT_EQ(Collector.GetBatches().size(), 1u);
		Batch.Elements.back().Draw.ElementCount = 3;
		Batch.Elements.back().Vertices.Buffer = nullptr;
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::ResourceFailure);
		Batch.Elements.back().Vertices.Buffer = new FRHIBuffer(
			FRHIBufferCreateDesc::CreateIndex("WrongVertexUsage", 36, 4));
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
		Batch.Elements.back().Vertices = Element.Vertices;
		Batch.LayoutKey = {999};
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
		Batch.LayoutKey = Binding->GetLayoutKey();
		Batch.bReceiver = false;
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::Excluded);
		Batch.bReceiver = true;
		Batch.LocalToWorld = FMatrix(0.0);
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
		Batch.LocalToWorld = FMatrix(1.0);
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::Submitted);
		Batch.WorldBounds = {};
		Batch.BatchId = 7;
		EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
		Batch = {};
		Binding.reset();
		EXPECT_EQ(Destructions, 0);
		ASSERT_EQ(Collector.GetBatches().size(), 2u);
		EXPECT_EQ(Collector.GetBatches()[0].Elements.size(), 2u);
		EXPECT_EQ(Collector.GetBatches()[0].WorldBounds.Min, FVector3(-1.0));
	}
	EXPECT_EQ(Destructions, 1);
	std::vector<FRHIResource*> Resources;
	FRHIResource::GatherResourcesToDelete(Resources);
	FRHIResource::DeleteResources(Resources);
}

namespace
{
	class FPublicationTestDeclaration final : public Durin::FRHIVertexDeclaration
	{
	public:
		Durin::FVertexDeclarationElementList Elements;
		auto GetElements() const -> const Durin::FVertexDeclarationElementList& override { return Elements; }
	};

	struct FGeometryPublicationFixture
	{
		Durin::FMeshGeometryElement Element;
		FGeometryPublicationFixture()
		{
			using namespace Durin;
			Element.ElementId = 13;
			Element.Draw = {.bIndexed = false, .ElementCount = 3};
			Element.Vertices = {new FRHIBuffer(FRHIBufferCreateDesc::CreateVertex("PublishedGeometry", 36)), {36, 0, 12, 12}};
		}
		auto MakeBinding() const -> std::unique_ptr<Durin::FStaticMeshBatchBinding>
		{
			using namespace Durin;
			auto Binding = std::make_unique<FStaticMeshBatchBinding>();
			auto* Declaration = new FPublicationTestDeclaration;
			Declaration->Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 12);
			Binding->Declaration = Declaration;
			Binding->DeclarationElements = Declaration->Elements;
			Binding->Streams = {{0, Element.Vertices.Buffer, 0, 12}};
			Binding->NumVertices = 3;
			return Binding;
		}
		~FGeometryPublicationFixture()
		{
			Element = {};
			std::vector<Durin::FRHIResource*> Resources;
			Durin::FRHIResource::GatherResourcesToDelete(Resources);
			Durin::FRHIResource::DeleteResources(Resources);
		}
	};
}

TEST(FRendererSceneContractTests, GeometryPublicationRejectsInvalidRangesInputsAndDuplicateIds)
{
	using namespace Durin;
	FGeometryPublicationFixture Fixture;
	auto ExpectFailure = [](const FMeshGeometryRecord::FResult& Result, EGeometrySubmissionOutcome Outcome) {
		EXPECT_FALSE(Result);
		if (!Result) EXPECT_EQ(Result.error(), Outcome);
	};
	ExpectFailure(FMeshGeometryRecord::Publish({}, {Fixture.Element}), EGeometrySubmissionOutcome::ResourceFailure);
	ExpectFailure(FMeshGeometryRecord::Publish(Fixture.MakeBinding(), {}), EGeometrySubmissionOutcome::Empty);
	ExpectFailure(FMeshGeometryRecord::Publish(Fixture.MakeBinding(), {Fixture.Element, Fixture.Element}), EGeometrySubmissionOutcome::InvalidSubmission);
	auto Invalid = Fixture.Element;
	Invalid.Draw.ElementCount = 6;
	ExpectFailure(FMeshGeometryRecord::Publish(Fixture.MakeBinding(), {Invalid}), EGeometrySubmissionOutcome::InvalidSubmission);
	Invalid = Fixture.Element;
	Invalid.Vertices.Buffer = nullptr;
	ExpectFailure(FMeshGeometryRecord::Publish(Fixture.MakeBinding(), {Invalid}), EGeometrySubmissionOutcome::ResourceFailure);
	auto Binding = Fixture.MakeBinding();
	Binding->Streams[0].Offset = 12;
	ExpectFailure(FMeshGeometryRecord::Publish(std::move(Binding), {Fixture.Element}), EGeometrySubmissionOutcome::InvalidSubmission);
	Binding = Fixture.MakeBinding();
	Binding->DeclarationElements[0].Stride = 16;
	ExpectFailure(FMeshGeometryRecord::Publish(std::move(Binding), {Fixture.Element}), EGeometrySubmissionOutcome::InvalidSubmission);
	Invalid = Fixture.Element;
	Invalid.ElementId = 99;
	Invalid.Draw.ElementCount = 0;
	auto Published = FMeshGeometryRecord::Publish(Fixture.MakeBinding(), {Invalid, Fixture.Element});
	ASSERT_TRUE(Published);
	ASSERT_EQ((*Published)->GetElements().size(), 1u);
	EXPECT_EQ((*Published)->GetElements()[0].ElementId, Fixture.Element.ElementId);
}

TEST(FRendererSceneContractTests, PublishedGeometryAdmissionRejectsMixedOrMismatchedSnapshots)
{
	using namespace Durin;
	FGeometryPublicationFixture Fixture;
	auto Published = FMeshGeometryRecord::Publish(Fixture.MakeBinding(), {Fixture.Element});
	ASSERT_TRUE(Published);
	FMeshBatch Batch;
	Batch.PrimitiveId = FPrimitiveComponentId(81);
	Batch.WorldBounds = FBox(FVector3(-1.0), FVector3(1.0));
	Batch.GeometryRecord = *Published;
	Batch.Binding = (*Published)->GetBinding();
	Batch.FactoryKey = Batch.Binding->GetFactoryKey();
	Batch.LayoutKey = Batch.Binding->GetLayoutKey();
	FMeshBatchCollector Collector(EMeshCollectionPurpose::Receiver);
	EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
	Batch.PublishedMaterials.resize(1);
	Batch.Elements.emplace_back();
	EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
	Batch.Elements.clear();
	Batch.Binding = Fixture.MakeBinding();
	EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
	Batch.Binding = (*Published)->GetBinding();
	Batch.LocalToWorld = FMatrix(0.0);
	EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
	Batch.LocalToWorld = FMatrix(1.0);
	EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::Submitted);
	ASSERT_EQ(Collector.GetBatches().size(), 1u);
	EXPECT_TRUE(Collector.GetBatches()[0].Elements.empty());
	const auto View = Collector.GetBatches()[0].GetElement(0);
	EXPECT_EQ(&View.Draw, &(*Published)->GetElements()[0].Draw);
	Fixture.Element.Draw.ElementCount = 300;
	EXPECT_EQ(View.Draw.ElementCount, 3u);
	EXPECT_EQ(Collector.Add(Batch), EGeometrySubmissionOutcome::InvalidSubmission);
}

TEST(FRendererSceneContractTests, GeometryRebindingSharesStorageAndRetainsPriorRevision)
{
	using namespace Durin;
	FGeometryPublicationFixture Fixture;
	auto Published = FMeshGeometryRecord::Publish(Fixture.MakeBinding(), {Fixture.Element});
	ASSERT_TRUE(Published);
	const auto OldId = (*Published)->GetRecordId();
	auto InvalidBinding = Fixture.MakeBinding();
	InvalidBinding->Streams.clear();
	auto Failed = (*Published)->WithBinding(std::move(InvalidBinding));
	ASSERT_FALSE(Failed);
	EXPECT_EQ(Failed.error(), EGeometrySubmissionOutcome::ResourceFailure);
	EXPECT_EQ((*Published)->GetRecordId(), OldId);
	auto Rebound = (*Published)->WithBinding(Fixture.MakeBinding());
	ASSERT_TRUE(Rebound);
	EXPECT_NE((*Rebound)->GetRecordId(), OldId);
	EXPECT_EQ((*Rebound)->GetGeometryId(), (*Published)->GetGeometryId());
	EXPECT_EQ((*Rebound)->GetElements().data(), (*Published)->GetElements().data());
	EXPECT_NE((*Rebound)->GetBinding(), (*Published)->GetBinding());
	std::weak_ptr<const FMeshGeometryRecord> Old = *Published;
	Published->reset();
	EXPECT_TRUE(Old.expired());
	EXPECT_TRUE((*Rebound)->GetElements()[0].Vertices.IsValid());
}

TEST(FRendererSceneContractTests, PrimitiveCollectionHasAnEmptyDefault)
{
	using namespace Durin;
	class FEmptyProxy final : public FPrimitiveSceneProxy
	{
	public:
		auto GetKind() const -> EPrimitiveSceneProxyKind override
		{
			return EPrimitiveSceneProxyKind::StaticMesh;
		}
		auto GetLocalBounds() const -> FBox override { return {}; }
	};
	FEmptyProxy Proxy;
	FMeshBatchCollector Collector(EMeshCollectionPurpose::Receiver);
	Proxy.CollectMeshBatches(FMeshCollectionContext{}, Collector);
	EXPECT_TRUE(Collector.GetBatches().empty());
}
#endif

namespace Durin::Tests
{
	namespace
	{
		constexpr uint64 MiB = 1024ull * 1024ull;
		struct FRDGAllocatorTestCommand final
		{
			static constexpr auto GetName() -> const char* { return "RDGAllocatorTest"; }
		};

		class FAllocatorRenderingThreadScope final
		{
		public:
			FAllocatorRenderingThreadScope()
			{
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				if (!IsTaskSchedulerRunning()) require(InitializeTaskScheduler(2));
				InitRenderingThread();
			}
			~FAllocatorRenderingThreadScope() { ShutdownRenderingThread(); }
		};

		// Account native lifetime separately from pool lifetime without consuming VRAM.
		template<typename Resource>
		class TAccountedRDGResource final : public Resource
		{
		public:
			template<typename Desc>
			TAccountedRDGResource(const Desc& InDesc, uint64 InBytes, uint64& InLiveBytes)
				: Resource(InDesc), Bytes(InBytes), LiveBytes(InLiveBytes)
			{
				LiveBytes += Bytes;
			}
			~TAccountedRDGResource() override { LiveBytes -= Bytes; }
		private:
			uint64 Bytes;
			uint64& LiveBytes;
		};

		// A scoped CPU backend exercises the production allocator and graph transaction.
		class FRDGAllocationTestRHI final : public FDynamicRHI
		{
		public:
			FRDGAllocationTestRHI() : Previous(GDynamicRHI) { GDynamicRHI = this; }
			~FRDGAllocationTestRHI() override
			{
				RHIFlushDeferredResources();
				EXPECT_EQ(LiveBytes, 0u);
				GDynamicRHI = Previous;
			}
			uint64 LiveBytes = 0, PeakBytes = 0;
			uint32 Creates = 0, Collections = 0;
			uint32 FailOnCreate = 0;
			bool bSupportAliasing = false;
			uint32 AliasCreates = 0, FailOnAlias = 0;
			auto RHISupportsTransientAliasing() const -> bool override { return bSupportAliasing; }
			template<typename Resource> class TAlias final : public Resource
			{
			public:
				template<typename Desc> TAlias(const Desc& Description, Resource* InSource)
					: Resource(Description), Source(InSource) {}
				TRefCountPtr<Resource> Source;
			};
			auto RHICreateAliasedTexture(const FRHITextureCreateDesc& Desc, FRHITexture* Source) -> FTextureRHIRef override
			{ if (++AliasCreates == FailOnAlias) return {}; return MakeRefCount<TAlias<FRHITexture>>(Desc, Source); }
			auto RHICreateAliasedBuffer(const FRHIBufferCreateDesc& Desc, FRHIBuffer* Source) -> FBufferRHIRef override
			{ if (++AliasCreates == FailOnAlias) return {}; return MakeRefCount<TAlias<FRHIBuffer>>(Desc, Source); }
			auto Init(const FRHIInitializationContext&) -> void override {}
			auto Shutdown() -> void override {}
			auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override {}
			auto RHIEndFrame() -> void override {}
			auto RHICreateViewport(const FRHIViewportCreateInfo&) -> FViewportRHIRef override { return {}; }
			auto RHIResizeViewport(FRHIViewport*, uint32, uint32, bool) -> void override {}
			auto RHICreateGraphicsPipelineState(FName, const FGraphicsPipelineStateInitializer&)
				-> TRefCountPtr<FRHIGraphicsPipelineState> override { return {}; }
			auto RHIGetDefaultContext() -> IRHICommandContext* override { return nullptr; }
			auto RHIGetViewportBackBuffer(FRHIViewport*) -> FTextureRHIRef override { return {}; }
			auto RHICreateVertexDeclaration(const FVertexDeclarationElementList&)
				-> TRefCountPtr<FRHIVertexDeclaration> override { return {}; }
			auto RHIIsTextureSupported(const FRHITextureCreateDesc&) const -> bool override { return true; }
			auto RHICreateSampler(const FRHISamplerDesc&) -> TRefCountPtr<FRHISampler> override { return {}; }
			auto RHICreateShader(const FRHIShaderCreateDesc&) -> FShaderRHIRef override { return {}; }
			auto RHICreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc& Desc) -> FTextureRHIRef override
			{
				if (!CheckCreation()) return {};
				auto Resource = MakeRefCount<TAccountedRDGResource<FRHITexture>>(
					Desc, uint64(Desc.Extent.x) * Desc.Extent.y * 4, LiveBytes);
				PeakBytes = std::max(PeakBytes, LiveBytes);
				return Resource;
			}
			auto RHICreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc& Desc) -> FBufferRHIRef override
			{
				if (!CheckCreation()) return {};
				auto Resource = MakeRefCount<TAccountedRDGResource<FRHIBuffer>>(
					Desc, Desc.Size, LiveBytes);
				PeakBytes = std::max(PeakBytes, LiveBytes);
				return Resource;
			}
			auto RHICollectCompletedResources() -> void override
			{
				++Collections;
				RHIFlushDeferredResources();
			}
		private:
			auto CheckCreation() -> bool
			{ return ++Creates != FailOnCreate; }
			FDynamicRHI* Previous;
		};

		class FUnpublishedAllocator final : public FRDGAllocator
		{
		public:
			explicit FUnpublishedAllocator(FRendererRDGAllocator& InPool) : Pool(InPool) {}
			auto Allocate(std::span<const FRDGAllocationRequest> Requests,
				FRDGAllocatedResources& Resources) -> FRDGAllocationResult override
			{
				std::vector<FRDGAllocationRequest> Copies(Requests.begin(), Requests.end());
				for (auto& Copy : Copies) Copy.Retirement = std::make_shared<FRDGAllocationRetirement>();
				return Pool.Allocate(Copies, Resources);
			}
		private:
			FRendererRDGAllocator& Pool;
		};

		class FInjectedRetirementAllocator final : public FRDGAllocator
		{
		public:
			FInjectedRetirementAllocator(FRendererRDGAllocator& InPool,
				std::shared_ptr<const FRDGAllocationRetirement> InRetirement)
				: Pool(InPool), Retirement(std::move(InRetirement)) {}
			auto Allocate(std::span<const FRDGAllocationRequest> Requests,
				FRDGAllocatedResources& Resources) -> FRDGAllocationResult override
			{
				std::vector<FRDGAllocationRequest> Copies(Requests.begin(), Requests.end());
				for (auto& Copy : Copies) Copy.Retirement = Retirement;
				return Pool.Allocate(Copies, Resources);
			}
		private:
			FRendererRDGAllocator& Pool;
			std::shared_ptr<const FRDGAllocationRetirement> Retirement;
		};

		class FExplicitPolicyAllocator final : public FRDGAllocator
		{
		public:
			explicit FExplicitPolicyAllocator(FRendererRDGAllocator& InPool) : Pool(InPool) {}
			std::optional<uint32> Failure;
			auto Allocate(std::span<const FRDGAllocationRequest> Requests, FRDGAllocatedResources& Output)
				-> FRDGAllocationResult override { return Pool.Allocate(Requests, Output, Failure); }
		private:
			FRendererRDGAllocator& Pool;
		};

		struct FRDGTestRequest final
		{
			bool bTexture;
			uint32 Megabytes;
			bool bExtracted = false;
		};

		auto ExecuteAllocationBatch(FRDGAllocator& Allocator,
			std::span<const FRDGTestRequest> Requests) -> FRDGCapture
		{
			FRHICommandListExecutor Executor;
			FRDGBuilder Builder;
			std::vector<FTextureRHIRef> ExportedTextures(Requests.size());
			std::vector<FBufferRHIRef> ExportedBuffers(Requests.size());
			uint32 Index = 0;
			for (const auto& Request : Requests)
			{
				const auto Name = "Resource" + std::to_string(Index++);
				if (Request.bTexture)
				{
					const auto Texture = Builder.CreateTexture({.Texture =
						FRHITextureCreateDesc::Create2D("BudgetTest", 8192,
							Request.Megabytes * 32, EPixelFormat::RGBA8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable)}, Name);
					const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + Name, ERDGPassType::Graphics);
					FRDGBuilderTestAccessor::UseColorAttachment(Builder, Pass, Texture,
						{ERHITextureAspect::Color, 0, 1, 0, 1},
						ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);
					if (Request.bExtracted)
						Builder.QueueTextureExtraction(Texture, &ExportedTextures[Index - 1],
							ERHIAccess::ColorAttachmentReadWrite);
				}
				else
				{
					const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(
						Request.Megabytes * uint32(MiB), 4, EBufferUsageFlags::UnorderedAccess)}, Name);
					const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write" + Name, ERDGPassType::Compute);
					FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, Request.Megabytes * MiB,
						ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
					if (Request.bExtracted)
						Builder.QueueBufferExtraction(Buffer, &ExportedBuffers[Index - 1],
							ERHIAccess::ComputeShaderReadWrite);
				}
			}

			const auto Result = Builder.Execute(Executor.GetImmediateCommandList(), &Allocator);
			EXPECT_NE(Durin::GetRDGExecutionStatus(Result), ERDGExecutionStatus::CompileFailed) << (Result ? "success" : ToString(Result.error()));
			return Builder.Capture();
		}

		auto ExecuteAllocationBatch(FRDGAllocator& Allocator,
			std::initializer_list<FRDGTestRequest> Requests) -> FRDGCapture
		{
			return ExecuteAllocationBatch(Allocator,
				std::span<const FRDGTestRequest>(Requests.begin(), Requests.size()));
		}
	}

#if defined(DURIN_RENDERER_RDG_ALLOCATOR_TESTS)
	TEST(FRendererRDGAllocatorTests, AllocationFailurePolicyIsExplicitAndDoesNotLeakBetweenInvocations)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			FExplicitPolicyAllocator Explicit(Allocator);
			auto Execute = [&](FRDGAllocator& Selected) {
				FRHICommandListExecutor Executor;
				FRDGBuilder Graph;
				const auto Buffer = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(64, 4,
					EBufferUsageFlags::UnorderedAccess), .ObservationTag = 17}, "PolicyBuffer");
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Graph, "Write", ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseBuffer(Graph, Pass, Buffer, 0, 64, ERDGUse::Write,
					ERHIAccess::ComputeShaderReadWrite, true);
				return Graph.Execute(Executor.GetImmediateCommandList(), &Selected);
			};
			{
				FScopedRendererQualificationPolicy Global({.FailedAllocationObservation = 17});
				EXPECT_TRUE(Execute(Allocator));
			}
			Explicit.Failure = 17;
			EXPECT_FALSE(Execute(Explicit));
			EXPECT_TRUE(Execute(Allocator));
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, GraphAliasingSharesOnePoolEntryAndRecoversFromAliasCreationFailure)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			RHI.bSupportAliasing = true;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			uint32 Callbacks = 0;
			auto Execute = [&]() {
				FRHICommandListExecutor Executor;
				FRDGBuilder Graph;
				Graph.SetTransientAliasingEnabled(true);
				for (uint32 Index = 0; Index != 3; ++Index)
				{
					const auto Buffer = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess)}, std::format("Buffer{}", Index));
					const auto Pass = FRDGBuilderTestAccessor::AddPass(Graph, std::format("Write{}", Index), ERDGPassType::Compute,
						[&](FRHICommandListImmediate&, const FRDGPassResources&) { ++Callbacks; });
					FRDGBuilderTestAccessor::UseBuffer(Graph, Pass, Buffer, 0, 64, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
				}
				const auto Result = Graph.Execute(Executor.GetImmediateCommandList(), &Allocator);
				if (Result)
				{
					const auto Capture = Graph.Capture();
					EXPECT_EQ(Capture.AllocationStatistics.ActiveBytes, 64u);
					EXPECT_EQ(Capture.AllocationStatistics.RetainedResources, 1u);
					EXPECT_EQ(Capture.Resources[0].PhysicalAllocationId, Capture.Resources[2].PhysicalAllocationId);
				}
				return Result;
			};
			ASSERT_TRUE(Execute());
			ASSERT_TRUE(Execute());
			EXPECT_EQ(RHI.Creates, 1u);
			EXPECT_EQ(RHI.AliasCreates, 4u);
			EXPECT_EQ(Callbacks, 6u);
			RHI.FailOnAlias = 6;
			EXPECT_FALSE(Execute());
			EXPECT_EQ(Callbacks, 6u);
			RHI.FailOnAlias = 0;
			ASSERT_TRUE(Execute());
			EXPECT_EQ(Callbacks, 9u);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, CreationFailureAbortsGraphAndNextExecutionCanSucceed)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			RHI.FailOnCreate = 1;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			auto Execute = [&] {
				FRHICommandListExecutor Executor;
				FRDGBuilder Builder;
				const auto Buffer = Builder.CreateBuffer({.Buffer = FRHIBufferDesc(64, 4,
					EBufferUsageFlags::UnorderedAccess)}, "RejectedBuffer");
				const auto Pass = FRDGBuilderTestAccessor::AddPass(Builder, "Write", ERDGPassType::Compute);
				FRDGBuilderTestAccessor::UseBuffer(Builder, Pass, Buffer, 0, 64, ERDGUse::Write,
					ERHIAccess::ComputeShaderReadWrite, true);

				return Builder.Execute(Executor.GetImmediateCommandList(), &Allocator);
			};
			const auto First = Execute();
			const auto Retry = Execute();
			EXPECT_TRUE(HasRDGTestReason(First.error(), ERDGAllocationError::PhysicalAllocationFailed));
			EXPECT_TRUE(Retry);
			EXPECT_EQ(RHI.Creates, 2u);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, UnpublishedRetirementCannotEvictOrExceedThePoolBudget)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			FUnpublishedAllocator Unpublished(Allocator);
			const auto Held = ExecuteAllocationBatch(Unpublished, {{true, 160}, {false, 320}});
			EXPECT_EQ(Held.AllocationStatistics.RetainedBytes, 480 * MiB);
			const auto Full = ExecuteAllocationBatch(Allocator, {{false, 160}});
			EXPECT_EQ(Full.AllocationStatistics.RetainedBytes, 640 * MiB);
			const auto Rejected = ExecuteAllocationBatch(Allocator, {{false, 161}});
			EXPECT_EQ(Rejected.AllocationStatistics.ActiveResources, 0u);
			EXPECT_EQ(Rejected.AllocationStatistics.Failures, 1u);
			EXPECT_EQ(Rejected.AllocationStatistics.Evictions, 1u);
			EXPECT_EQ(Rejected.AllocationStatistics.RetainedBytes, 480 * MiB);
			EXPECT_EQ(Rejected.AllocationStatistics.RetainedResources, 2u);
			EXPECT_EQ(RHI.Creates, 3u);
			EXPECT_LE(RHI.PeakBytes, 640 * MiB);
			Allocator.Release_RenderThread();
			const auto Recovered = ExecuteAllocationBatch(Allocator, {{false, 161}});
			EXPECT_EQ(Recovered.AllocationStatistics.ActiveResources, 1u);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, FailedSubmissionRemainsQuarantinedUnderPoolPressure)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			const auto Generation = AllocateRHIDeviceGeneration();

			auto FailedRetirement = std::make_shared<FRDGAllocationRetirement>();
			FRHIGPUQueueTimeline FailedQueue(Generation, {0});
			const auto FailedPoint = FailedQueue.Reserve();
			const auto FailedSignal = FRHIGPUSyncPoint::Create();
			ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(FailedSignal, FailedPoint));
			ASSERT_TRUE(FailedQueue.MarkSubmitted(FailedPoint));
			FailedQueue.Fail();
			ASSERT_EQ(FailedSignal.GetState(), ERHIGPUSubmissionState::Failed);
			FRDGBuilderTestAccessor::SetAllocationRetirementCompletion(
				*FailedRetirement, FailedSignal);
			FInjectedRetirementAllocator Failed(Allocator, FailedRetirement);
			const auto Quarantined = ExecuteAllocationBatch(Failed, {{false, 320}});
			ASSERT_EQ(Quarantined.Resources.size(), 1u);
			const uint64 QuarantinedId = Quarantined.Resources[0].PhysicalAllocationId;

			auto CompleteRetirement = std::make_shared<FRDGAllocationRetirement>();
			FRHIGPUQueueTimeline CompleteQueue(Generation, {1});
			const auto CompletePoint = CompleteQueue.Reserve();
			const auto CompleteSignal = FRHIGPUSyncPoint::Create();
			ASSERT_TRUE(FRHIGPUSyncPointBackend::Attach(
				CompleteSignal, CompletePoint));
			ASSERT_TRUE(CompleteQueue.MarkSubmitted(CompletePoint));
			ASSERT_TRUE(CompleteQueue.ObserveCompleted(CompletePoint));
			FRDGBuilderTestAccessor::SetAllocationRetirementCompletion(
				*CompleteRetirement, CompleteSignal);
			FInjectedRetirementAllocator Complete(Allocator, CompleteRetirement);
			const auto Unrelated = ExecuteAllocationBatch(Complete, {{false, 320}});
			ASSERT_EQ(Unrelated.Resources.size(), 1u);
			EXPECT_NE(Unrelated.Resources[0].PhysicalAllocationId, QuarantinedId);
			const auto Reused = ExecuteAllocationBatch(Complete, {{false, 320}});
			ASSERT_EQ(Reused.Resources.size(), 1u);
			EXPECT_EQ(Reused.Resources[0].PhysicalAllocationId,
				Unrelated.Resources[0].PhysicalAllocationId);

			const auto Pressured = ExecuteAllocationBatch(Complete, {{false, 321}});
			EXPECT_EQ(Pressured.AllocationStatistics.ActiveResources, 0u);
			EXPECT_EQ(Pressured.AllocationStatistics.Failures, 1u);
			EXPECT_EQ(Pressured.AllocationStatistics.RetainedResources, 1u);
			EXPECT_EQ(Pressured.AllocationStatistics.RetainedBytes, 320 * MiB);
			Allocator.Release_RenderThread();
			const auto Recovered = ExecuteAllocationBatch(Complete, {{false, 321}});
			EXPECT_EQ(Recovered.AllocationStatistics.ActiveResources, 1u);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, BucketsReserveDuplicateDescriptorsInCreationOrder)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			std::vector<FRDGTestRequest> Requests;
			for (uint32 Index = 0; Index < 64; ++Index)
			{
				Requests.push_back({true, 1});
				Requests.push_back({false, 1});
			}
			const auto First = ExecuteAllocationBatch(Allocator, Requests);
			ASSERT_EQ(First.AllocationStatistics.ActiveResources, 128u);
			for (auto& Request : Requests) Request.bTexture = !Request.bTexture;
			for (uint32 Batch = 0; Batch < 3; ++Batch)
			{
				const auto Reused = ExecuteAllocationBatch(Allocator, Requests);
				ASSERT_EQ(Reused.AllocationStatistics.ActiveResources, 128u);
				for (size_t Index = 0; Index < Requests.size(); ++Index)
					EXPECT_EQ(Reused.Resources[Index].PhysicalAllocationId,
						First.Resources[Index ^ 1].PhysicalAllocationId);
			}
			EXPECT_EQ(RHI.Creates, 128u);
			const auto Replaced = ExecuteAllocationBatch(Allocator, {{false, 640}});
			EXPECT_EQ(Replaced.AllocationStatistics.Evictions, 128u);
			EXPECT_EQ(Replaced.AllocationStatistics.RetainedResources, 1u);
			const auto Restored = ExecuteAllocationBatch(Allocator, Requests);
			EXPECT_EQ(Restored.AllocationStatistics.ActiveResources, 128u);
			EXPECT_EQ(Restored.AllocationStatistics.Evictions, 129u);
			EXPECT_EQ(RHI.Creates, 257u);
			EXPECT_LE(RHI.PeakBytes, 640 * MiB);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, BatchEvictionMergesKindsAndReindexesSurvivors)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			const auto First = ExecuteAllocationBatch(Allocator,
				{{true, 100}, {false, 100}, {true, 100}, {false, 100},
					{true, 100}, {false, 100}});
			const auto Evicted = ExecuteAllocationBatch(Allocator,
				{{false, 240}, {true, 100}});
			ASSERT_EQ(Evicted.AllocationStatistics.ActiveResources, 2u);
			EXPECT_EQ(Evicted.AllocationStatistics.Evictions, 2u);
			EXPECT_EQ(Evicted.AllocationStatistics.RetainedBytes, 640 * MiB);
			EXPECT_EQ(RHI.Collections, 1u);
			const auto Reused = ExecuteAllocationBatch(Allocator,
				{{true, 100}, {true, 100}, {false, 100}, {false, 100}, {false, 240}});
			ASSERT_EQ(Reused.AllocationStatistics.ActiveResources, 5u);
			EXPECT_EQ(Reused.Resources[0].PhysicalAllocationId, First.Resources[0].PhysicalAllocationId);
			EXPECT_EQ(Reused.Resources[1].PhysicalAllocationId, First.Resources[4].PhysicalAllocationId);
			EXPECT_EQ(Reused.Resources[2].PhysicalAllocationId, First.Resources[3].PhysicalAllocationId);
			EXPECT_EQ(Reused.Resources[3].PhysicalAllocationId, First.Resources[5].PhysicalAllocationId);
			EXPECT_EQ(Reused.Resources[4].PhysicalAllocationId, Evicted.Resources[0].PhysicalAllocationId);
			EXPECT_EQ(RHI.Creates, 7u);
			EXPECT_LE(RHI.PeakBytes, 640 * MiB);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, ExportCompactionRemovesIdsAndPreservesBucketOrder)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			const auto First = ExecuteAllocationBatch(Allocator,
				{{true, 1, true}, {true, 1}, {false, 1, true}, {false, 1}});
			ASSERT_EQ(First.AllocationStatistics.ActiveResources, 4u);
			EXPECT_EQ(First.AllocationStatistics.RetainedResources, 2u);
			const auto Second = ExecuteAllocationBatch(Allocator,
				{{true, 1}, {true, 1}, {false, 1}, {false, 1}});
			ASSERT_EQ(Second.AllocationStatistics.ActiveResources, 4u);
			EXPECT_EQ(Second.Resources[0].PhysicalAllocationId, First.Resources[1].PhysicalAllocationId);
			EXPECT_EQ(Second.Resources[2].PhysicalAllocationId, First.Resources[3].PhysicalAllocationId);
			EXPECT_GT(Second.Resources[1].PhysicalAllocationId, First.Resources[3].PhysicalAllocationId);
			EXPECT_GT(Second.Resources[3].PhysicalAllocationId, First.Resources[3].PhysicalAllocationId);
			EXPECT_EQ(RHI.Creates, 6u);
			Allocator.Release_RenderThread();
			const auto Released = ExecuteAllocationBatch(Allocator, {{true, 1}, {false, 1}});
			EXPECT_EQ(Released.AllocationStatistics.ActiveResources, 2u);
			EXPECT_EQ(RHI.Creates, 8u);
			Coordinator.Apply_RenderThread(ERendererResourceInvalidationCause::Device, {});
			const auto Invalidated = ExecuteAllocationBatch(Allocator, {{true, 1}, {false, 1}});
			EXPECT_EQ(Invalidated.AllocationStatistics.ActiveResources, 2u);
			EXPECT_EQ(RHI.Creates, 10u);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, FailedBatchReindexesPoolAndAllowsReuse)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			RHI.FailOnCreate = 1;
			ExecuteAllocationBatch(Allocator, {{true, 1}});
			RHI.FailOnCreate = 4;
			const auto Failed = ExecuteAllocationBatch(Allocator,
				{{true, 1}, {true, 1}, {false, 2}});
			EXPECT_EQ(Failed.AllocationStatistics.RetainedResources, 0u);
			ExecuteAllocationBatch(Allocator, {{false, 2}});
			EXPECT_EQ(RHI.Creates, 5u);
			const auto Recovered = ExecuteAllocationBatch(Allocator, {{true, 1}, {true, 1}});
			ASSERT_EQ(Recovered.AllocationStatistics.ActiveResources, 2u);
			EXPECT_GT(Recovered.Resources[0].PhysicalAllocationId, 1u);
			EXPECT_GT(Recovered.Resources[1].PhysicalAllocationId, 1u);
			const auto Reused = ExecuteAllocationBatch(Allocator, {{true, 1}, {true, 1}});
			ASSERT_EQ(Reused.AllocationStatistics.ActiveResources, 2u);
			EXPECT_EQ(Reused.Resources[0].PhysicalAllocationId, Recovered.Resources[0].PhysicalAllocationId);
			EXPECT_EQ(Reused.Resources[1].PhysicalAllocationId, Recovered.Resources[1].PhysicalAllocationId);
			EXPECT_EQ(RHI.Creates, 7u);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, ReservesWholeBatchAndCollectsBeforeCreating)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			const auto First = ExecuteAllocationBatch(Allocator,
				{{true, 200}, {true, 200}, {false, 200}});
			ASSERT_EQ(First.AllocationStatistics.ActiveBytes, 600 * MiB);
			const auto Second = ExecuteAllocationBatch(Allocator,
				{{false, 240}, {true, 200}, {true, 200}});
			EXPECT_EQ(Second.AllocationStatistics.ActiveBytes, 640 * MiB);
			EXPECT_EQ(RHI.Creates, 4u);
			EXPECT_EQ(RHI.Collections, 1u);
			EXPECT_LE(RHI.PeakBytes, 640 * MiB);
			EXPECT_EQ(Second.AllocationStatistics.Evictions, 1u);
			EXPECT_EQ(First.Resources[0].PhysicalAllocationId, Second.Resources[1].PhysicalAllocationId);
			EXPECT_EQ(First.Resources[1].PhysicalAllocationId, Second.Resources[2].PhysicalAllocationId);
			EXPECT_NE(Second.Resources[1].PhysicalAllocationId, Second.Resources[2].PhysicalAllocationId);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, FailedBatchRollsBackAndNextExecutionAllocatesImmediately)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			RHI.FailOnCreate = 2;
			const auto Failed = ExecuteAllocationBatch(Allocator, {{true, 1}, {false, 1}});
			EXPECT_EQ(Failed.AllocationStatistics.ActiveBytes, 0u);
			EXPECT_EQ(Failed.AllocationStatistics.RetainedBytes, 0u);
			EXPECT_EQ(RHI.Creates, 2u);
			const auto Recovered = ExecuteAllocationBatch(Allocator, {{true, 1}, {false, 1}});
			EXPECT_EQ(Recovered.AllocationStatistics.ActiveBytes, 2 * MiB);
			EXPECT_EQ(RHI.Creates, 4u);
			EXPECT_EQ(RHI.Collections, 1u);
			EXPECT_EQ(RHI.LiveBytes, 2 * MiB);
		});
		FlushRenderingCommands();
	}

	TEST(FRendererRDGAllocatorTests, FailedBatchPreservesExistingReusableResources)
	{
		FAllocatorRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FRDGAllocationTestRHI RHI;
			FRendererResourceCoordinator Coordinator;
			FRendererRDGAllocator Allocator(Coordinator);
			ExecuteAllocationBatch(Allocator, {{true, 1}, {false, 1}});
			RHI.FailOnCreate = 3;
			const auto Failed = ExecuteAllocationBatch(Allocator, {{true, 1}, {true, 2}});
			EXPECT_EQ(Failed.AllocationStatistics.RetainedBytes, MiB);
			EXPECT_EQ(Failed.AllocationStatistics.ActiveBytes, 0u);
			const auto Reused = ExecuteAllocationBatch(Allocator, {{true, 1}});
			EXPECT_EQ(Reused.AllocationStatistics.ActiveBytes, MiB);
			EXPECT_EQ(RHI.Creates, 3u);
			RHI.FailOnCreate = 5;
			const auto RetryFailed = ExecuteAllocationBatch(Allocator, {{true, 2}, {false, 2}});
			EXPECT_EQ(RHI.Creates, 5u);
			EXPECT_EQ(RetryFailed.AllocationStatistics.RetainedBytes, 0u);
			EXPECT_EQ(RetryFailed.AllocationStatistics.ActiveBytes, 0u);
			const auto Recovered = ExecuteAllocationBatch(Allocator, {{true, 2}, {false, 2}});
			EXPECT_EQ(Recovered.AllocationStatistics.ActiveBytes, 4 * MiB);
			EXPECT_EQ(RHI.LiveBytes, 4 * MiB);
		});
		FlushRenderingCommands();
	}
#endif

}

#if !defined(DURIN_RENDERER_RDG_ALLOCATOR_TESTS)
TEST(FRendererSceneContractTests, CompiledSurfaceLayoutsRejectUnsupportedContracts)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	FShaderReflectionData Reflection;
	Reflection.ResourceBindings = {
		{.SetIndex = 0, .BindingIndex = 0, .Type = ERHIBindingType::UniformBuffer},
		{.SetIndex = 1, .BindingIndex = 2, .Type = ERHIBindingType::UniformBuffer},
		{.SetIndex = 1, .BindingIndex = 27, .Type = ERHIBindingType::UniformBuffer}};
	const FCompiledSurfaceBindingLayout Layout(Reflection);
	ASSERT_TRUE(Layout.IsValid());
	ASSERT_EQ(Layout.GetEntries().size(), 3u);
	EXPECT_EQ(Layout.GetEntries()[0].Source, FCompiledSurfaceBindingLayout::ESource::View);
	EXPECT_EQ(Layout.GetEntries()[1].Source, FCompiledSurfaceBindingLayout::ESource::Material);
	EXPECT_EQ(Layout.GetEntries()[2].Source, FCompiledSurfaceBindingLayout::ESource::HitProxy);
	for (uint32 Mutation = 0; Mutation < 5; ++Mutation)
	{
		auto Invalid = Reflection;
		switch (Mutation)
		{
		case 0: Invalid.ResourceBindings[0].SetIndex = 2; break;
		case 1: Invalid.ResourceBindings[0].Type = ERHIBindingType::Texture; break;
		case 2: Invalid.ResourceBindings[0].ArraySize = 2; break;
		case 3: Invalid.ResourceBindings.push_back(Invalid.ResourceBindings[0]); break;
		case 4: Invalid.ResourceBindings[0].BindingIndex = 18; break;
		}
		EXPECT_FALSE(FCompiledSurfaceBindingLayout(Invalid).IsValid()) << Mutation;
	}
}

TEST(FRendererSceneContractTests, CommandDescriptionsInvalidatePolicyAndRetainUnseenEntries)
{
	using namespace Durin;
	FMeshDrawDescriptionCache Cache;
	Cache.BeginSubmission();
	auto Command = std::make_shared<FMeshDrawDescription>();
	Command->Material = GetErrorMaterialRenderData();
	const FMeshDrawDescriptionCache::FKey Key{11, 2, Command->Material.Representation.GetRecordId(), 0};
	Cache.Store(Key, Command);
	Cache.EndSubmission();
	Cache.BeginSubmission();
	EXPECT_EQ(Cache.Find(Key, Command->Material), Command);
	auto ChangedMaterial = Command->Material;
	ChangedMaterial.PlanningPassIdentity.bTwoSided = !ChangedMaterial.PlanningPassIdentity.bTwoSided;
	EXPECT_FALSE(Cache.Find(Key, ChangedMaterial));
	for (uint32 Index = 0; Index < Key.size(); ++Index)
	{
		auto ChangedKey = Key;
		++ChangedKey[Index];
		EXPECT_FALSE(Cache.Find(ChangedKey, Command->Material));
	}
	Cache.EndSubmission();
	EXPECT_EQ(Cache.Num(), 1u);
	std::weak_ptr<const FMeshDrawDescription> Weak = Command;
	Cache.BeginSubmission();
	Cache.EndSubmission();
	EXPECT_EQ(Cache.Num(), 1u);
	Cache.BeginSubmission();
	EXPECT_EQ(Cache.Find(Key, Command->Material), Command);
	Cache.EndSubmission();
	Cache.Reset();
	EXPECT_FALSE(Weak.expired());
	Command.reset();
	EXPECT_TRUE(Weak.expired());
}

TEST(FRendererSceneContractTests, CompactOrderingMatchesFullKeysAndRetainsTemplateStorage)
{
	using namespace Durin;
	FMeshDrawSortKey Original;
	Original.VertexFactory = {3, 7, 1};
	Original.PrimitiveId = 9;
	Original.BatchId = 2;
	Original.SelectedLODIndex = 1;
	Original.SectionIndex = 4;
	auto MakeVisible = [](const FMeshDrawSortKey& Full) {
		auto Owner = std::make_shared<FMeshDrawDescription>();
		Owner->SortKey = Full;
		FVisibleMeshDrawSortKey Visible;
		Visible.State = {Owner, &Owner->SortKey};
		Visible.PrimitiveId = Full.PrimitiveId;
		Visible.BatchId = Full.BatchId;
		Visible.SelectedLODIndex = Full.SelectedLODIndex;
		return Visible;
	};
	const auto Visible = MakeVisible(Original);
	EXPECT_EQ(Visible.GetState().VertexFactory, Original.VertexFactory);
	for (uint32 Field = 0; Field < 7; ++Field)
	{
		auto Changed = Original;
		switch (Field)
		{
		case 0: Changed.Pipeline[0]++; break;
		case 1: Changed.VertexFactory[0]++; break;
		case 2: Changed.Geometry[0]++; break;
		case 3: Changed.PrimitiveId++; Changed.SectionIndex = 0; break;
		case 4: Changed.BatchId++; Changed.SectionIndex = 0; break;
		case 5: Changed.SelectedLODIndex++; Changed.SectionIndex = 0; break;
		case 6: Changed.SectionIndex++; break;
		}
		const auto Other = MakeVisible(Changed);
		EXPECT_EQ(Visible <=> Other, Original <=> Changed) << Field;
	}
	auto Shared = Visible;
	Shared.PrimitiveId++;
	EXPECT_LT(Visible, Shared);
	EXPECT_EQ(Shared.GetState().VertexFactory.data(), Visible.GetState().VertexFactory.data());
}

TEST(FRendererSceneContractTests, AdjacentBindingGroupsRebindOnAnyChangeFailureOrBoundary)
{
	using namespace Durin;
	FMeshDrawBindingGroup Group;
	std::array<int, 6> Owners{};
	uint32 BindCalls = 0;
	auto Bind = [&](int Pipeline, int Vertex, int Fragment, bool bSuccess = true) {
		return Group.Apply(&Owners[Pipeline], &Owners[Vertex], Fragment < 0 ? nullptr : &Owners[Fragment], [&] {
			++BindCalls;
			return bSuccess;
		});
	};
	ASSERT_TRUE(Bind(0, 1, 2));
	ASSERT_TRUE(Bind(0, 1, 2));
	EXPECT_EQ(BindCalls, 1u);
	ASSERT_TRUE(Bind(0, 3, 2)); // vertex change must also rebind the complete fragment state
	ASSERT_TRUE(Bind(0, 3, 4));
	ASSERT_TRUE(Bind(5, 3, 4));
	EXPECT_EQ(BindCalls, 4u);
	EXPECT_FALSE(Bind(0, 1, 2, false));
	ASSERT_TRUE(Bind(5, 3, 4)); // failed partial binding invalidates even the preceding group
	EXPECT_EQ(BindCalls, 6u);
	Group = {}; // new pass or intervening non-mesh work
	ASSERT_TRUE(Bind(5, 3, 4));
	EXPECT_EQ(BindCalls, 7u);
	ASSERT_TRUE(Bind(0, 1, -1));
	ASSERT_TRUE(Bind(0, 1, -1)); // opaque shadow has no fragment resources
	EXPECT_EQ(BindCalls, 8u);
}

TEST(FRendererSceneContractTests, PreparedSurfaceBindingsClearPreviousPayloadOnFailure)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	FShaderReflectionData Reflection;
	Reflection.ResourceBindings = {{.SetIndex = 1, .BindingIndex = 2,
		.Type = ERHIBindingType::UniformBuffer}};
	const FCompiledSurfaceBindingLayout Layout(Reflection);
	FResolvedSurfaceMaterial Material;
	Material.bCompiledLayout = true;
	FPreparedSurfaceMaterialBindings Bindings;
	const auto Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Fragment, FXxHash128{}));
	const auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create(
		"PreparedSurface", 64, 16, EBufferUsageFlags::UniformBuffer));
	const FRHIUniformBufferRange Range{Buffer.GetReference(), 16, 32};
	ASSERT_TRUE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		Range, {}, {}, {}, {}, Bindings));
	ASSERT_EQ(Bindings.GetResources().size(), 1u);
	EXPECT_EQ(Bindings.GetResources()[0].Offset, 16u);
	EXPECT_EQ(Bindings.GetResources()[0].Resource->GetResourceType(), ERHIResourceType::BufferView);
	EXPECT_FALSE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		{Buffer.GetReference(), 48, 32}, {}, {}, {}, {}, Bindings));
	EXPECT_EQ(Bindings.GetShader(), nullptr);
	EXPECT_TRUE(Bindings.GetResources().empty());
	FRHICommandList Commands;
	const auto Uniform = Commands.CreateUniformBuffer({64}, ERHIBufferLifetimeUsage::MultiFrame, FByteBuffer(64));
	ASSERT_TRUE(Uniform);
	ASSERT_TRUE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		{Uniform.GetReference(), 16, 32}, {}, {}, {}, {}, Bindings));
	ASSERT_EQ(Bindings.GetResources().size(), 1u);
	EXPECT_EQ(Bindings.GetResources()[0].Resource, Uniform.GetReference());
	EXPECT_EQ(Bindings.GetResources()[0].Offset, 16u);
	EXPECT_EQ(Bindings.GetResources()[0].Size, 32u);
	EXPECT_FALSE(PrepareCompiledSurfaceMaterial(Shader.GetReference(), Layout, Material,
		{Uniform.GetReference(), 48, 32}, {}, {}, {}, {}, Bindings));
	EXPECT_EQ(Bindings.GetShader(), nullptr);
	EXPECT_TRUE(Bindings.GetResources().empty());
}

TEST(FRendererSceneContractTests, VertexFactoryPrecacheDescriptionsAreCpuOnlyAndOwnExplicitDefaultLayouts)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	const auto Local = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
	const auto Spline = FindMeshVertexFactory(FSplineMeshBatchBinding{}.GetFactoryKey());
	ASSERT_TRUE(Local);
	ASSERT_TRUE(Spline);
	EXPECT_TRUE(Local->GetType().SupportsPSOPrecaching());
	EXPECT_TRUE(Spline->GetType().SupportsPSOPrecaching());
	EXPECT_FALSE(EnumHasAnyFlags(Local->GetType().GetFlags(), EVertexFactoryFlags::SupportsManualVertexFetch));
	for (const auto Stream : {EVertexInputStreamType::Default, EVertexInputStreamType::PositionOnly, EVertexInputStreamType::PositionAndNormalOnly})
	{
		const auto Elements = FPSOPrecacheVertexFactoryData(&Local->GetType()).GetVertexElements(Stream);
		const auto SplineElements = FPSOPrecacheVertexFactoryData(&Spline->GetType()).GetVertexElements(Stream);
		ASSERT_TRUE(Elements);
		ASSERT_TRUE(SplineElements);
		EXPECT_EQ(*Elements, *SplineElements);
		EXPECT_TRUE(ValidateVertexDeclarationElements(*Elements));
		EXPECT_EQ(std::ranges::count_if(*Elements, [](const auto& Element) { return Element.Type != EVertexElementType::None; }),
			Stream == EVertexInputStreamType::Default ? 8 : Stream == EVertexInputStreamType::PositionOnly ? 1 : 2);
	}
	const auto Fixed = FPSOPrecacheVertexFactoryData(&Local->GetType()).GetVertexElements();
	ASSERT_TRUE(Fixed);
	auto Custom = *Fixed;
	Custom[0].Stride += 4;
	const FPSOPrecacheVertexFactoryData Snapshot(&Local->GetType(), Custom);
	EXPECT_TRUE(Snapshot.MatchesVertexElements(Custom));
	EXPECT_FALSE(Snapshot.MatchesVertexElements(*Fixed));
	Custom[0].Stride += 4;
	EXPECT_FALSE(Snapshot.MatchesVertexElements(Custom));
	const auto Position = Snapshot.GetVertexElements(EVertexInputStreamType::PositionOnly);
	ASSERT_TRUE(Position);
	EXPECT_EQ((*Position)[0], (*Fixed)[0]); // Default overrides do not change reduced streams.
	EXPECT_FALSE(Snapshot.GetVertexElements(EVertexInputStreamType::Count));
	FVertexDeclarationElementList Reused = *Fixed;
	Local->GetType().GetPSOPrecacheVertexFetchElements(EVertexInputStreamType::Count, Reused);
	EXPECT_TRUE(std::ranges::all_of(Reused, [](const auto& Element) { return Element.Type == EVertexElementType::None; }));
}

TEST(FRendererSceneContractTests, VertexFactoryPrecacheRejectsUnsupportedMissingAndMalformedLayouts)
{
	using namespace Durin;
	static const FVertexFactoryType Legacy("PrecacheLegacyFixture");
	static const FVertexFactoryType ExplicitOnly("PrecacheExplicitFixture", EVertexFactoryFlags::SupportsPSOPrecaching);
	static const FVertexFactoryType Manual("PrecacheManualFixture",
		EVertexFactoryFlags::SupportsPSOPrecaching | EVertexFactoryFlags::SupportsManualVertexFetch,
		+[](EVertexInputStreamType, FVertexDeclarationElementList& Elements) { Elements = {}; });
	EXPECT_EQ(FPSOPrecacheVertexFactoryData{}.GetVertexElements().error(), EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&Legacy).GetVertexElements().error(), EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&ExplicitOnly).GetVertexElements().error(), EPSOPrecacheVertexFactoryError::MissingVertexElements);
	FVertexDeclarationElementList Elements{};
	Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 16);
	EXPECT_TRUE(FPSOPrecacheVertexFactoryData(&ExplicitOnly, Elements).GetVertexElements());
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&Legacy, Elements).GetVertexElements().error(), EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
	EXPECT_EQ(FPSOPrecacheVertexFactoryData(&ExplicitOnly, Elements).GetVertexElements(EVertexInputStreamType::PositionOnly).error(), EPSOPrecacheVertexFactoryError::UnsupportedStream);
	EXPECT_TRUE(FPSOPrecacheVertexFactoryData(&Manual).GetVertexElements());
	EXPECT_FALSE(FPSOPrecacheVertexFactoryData(&Manual).GetVertexElements(EVertexInputStreamType::PositionAndNormalOnly));
	EXPECT_FALSE(FPSOPrecacheVertexFactoryData(&ExplicitOnly, {}).GetVertexElements());
	for (uint32 Mutation = 0; Mutation < 5; ++Mutation)
	{
		SCOPED_TRACE(Mutation);
		auto Invalid = Elements;
		switch (Mutation)
		{
		case 0: Invalid[0].Stride = 4; break;
		case 1: Invalid[1] = FVertexElement(1, 0, EVertexElementType::Float1, 0, 4); break;
		case 2: Invalid[1] = FVertexElement(0, 0, EVertexElementType::Float1, 1, 16); break;
		case 3: Invalid[1] = FVertexElement(0, 12, EVertexElementType::Float1, 1, 20); break;
		case 4: Invalid[2] = FVertexElement(1, 0, EVertexElementType::Float1, 1, 4); break;
		}
		const auto Result = FPSOPrecacheVertexFactoryData(&ExplicitOnly, Invalid).GetVertexElements();
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error(), EPSOPrecacheVertexFactoryError::InvalidVertexElements);
	}
}

namespace
{
	class FMaterialPSOTestPipelines
	{
	public:
		FMaterialPSOTestPipelines() : Released(Gate.get_future().share())
		{
			using namespace Durin;
			OwnsScheduler = !IsTaskSchedulerRunning();
			if (OwnsScheduler) require(InitializeTaskScheduler(1));
			FRHICapabilities Caps;
			Caps.MaxComputeWorkGroupCount = {65535, 65535, 65535};
			FRHIPipelineCompileBackend Backend{
				.FindGraphics = [](const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
				.FindCompute = [](const auto&) -> FComputePipelineStateRHIRef { return {}; },
				.CreateGraphics = [](const auto&, const auto&) -> FGraphicsPipelineStateRHIRef { return {}; },
				.CreateCompute = [this](const FRHIComputePipelineCreationInputs& Input, const auto&) -> FComputePipelineStateRHIRef {
					if (Input.DebugName == "gated") Released.wait();
					if (Input.DebugName == "failed") return {};
					return MakeRefCount<FRHIComputePipelineState>();
				},
				.PublishTerminalFailure = [](std::exception_ptr) { ADD_FAILURE() << "Unexpected material test pipeline failure."; }};
			Cache = std::make_unique<FRHIPipelineStateCache>(Caps, std::move(Backend));
			Shader = MakeRefCount<FRHIShader>(FRHIShaderDesc(EShaderFrequency::Compute, {}));
		}
		~FMaterialPSOTestPipelines()
		{
			Release(); Cache.reset(); Shader = nullptr;
			if (OwnsScheduler) Durin::ShutdownTaskScheduler();
			Durin::RHIFlushDeferredResources();
		}
		auto Release() -> void { if (!GateReleased) { GateReleased = true; Gate.set_value(); } }
		auto TryRequestSlot(uint32 Slot) -> std::expected<std::shared_ptr<Durin::FComputePipelineState>, Durin::ERHIPipelineRequestRejection>
		{
			using namespace Durin;
			FComputePipelineStateInitializer Input;
			Input.ComputeShader = Shader;
			Input.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, Slot, ERHIBindingType::UniformBuffer);
			auto Result = Cache->GetCompute(Input, "slot");
			if (Result) Descriptions.insert_or_assign(Result->get(), FPSOPrecacheData(Input));
			return Result;
		}
		auto Request(bool Gated, bool Failed = false) -> std::shared_ptr<Durin::FPipelineState>
		{
			using namespace Durin;
			FComputePipelineStateInitializer Input;
			Input.ComputeShader = Shader;
			Input.PipelineLayout.PushConstantRanges.push_back({EShaderStageFlags::Compute, 0, Failed ? 12u : Gated ? 8u : 4u});
			const auto Result = Cache->GetCompute(Input, Failed ? "failed" : Gated ? "gated" : "ready");
			require(Result.has_value());
			Descriptions.insert_or_assign(Result->get(), FPSOPrecacheData(Input));
			return *Result;
		}
		auto Describe(const std::shared_ptr<Durin::FPipelineState>& Pipeline, bool Required = true)
			-> Durin::FPSOPrecacheData
		{
			auto Data = Descriptions.at(Pipeline.get()); Data.bRequired = Required; return Data;
		}
		auto DescribeSlot(uint32 Slot) -> Durin::FPSOPrecacheData
		{
			using namespace Durin;
			FComputePipelineStateInitializer Input;
			Input.ComputeShader = Shader;
			Input.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
				EShaderStageFlags::Compute, Slot, ERHIBindingType::UniformBuffer);
			return FPSOPrecacheData(Input);
		}
		auto Submit(const Durin::FPSOPrecacheData& Data)
			-> std::expected<std::shared_ptr<Durin::FPipelineState>, Durin::ERHIPipelineRequestRejection>
		{
			using namespace Durin;
			const auto& Input = Data.ComputeInitializer;
			const uint32 Size = Input.PipelineLayout.PushConstantRanges.empty() ? 0 : Input.PipelineLayout.PushConstantRanges.front().Size;
			auto Result = Cache->GetCompute(Input, Size == 12 ? "failed" : Size == 8 ? "gated" : "ready");
			if (!Result) return std::unexpected(Result.error());
			return *Result;
		}

	private:
		std::unordered_map<const Durin::FPipelineState*, Durin::FPSOPrecacheData> Descriptions;
		std::promise<void> Gate;
		std::shared_future<void> Released;
		bool GateReleased = false, OwnsScheduler = false;
		std::unique_ptr<Durin::FRHIPipelineStateCache> Cache;
		Durin::FShaderRHIRef Shader;
	};

	auto MaterialPSOTestParams() -> Durin::FMaterialPSOPrecacheParams
	{
		using namespace Durin;
		const auto Factory = RendererPrivate::FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
		require(Factory != nullptr);
		FMaterialPSOPrecacheParams Params;
		Params.VertexFactoryData = FPSOPrecacheVertexFactoryData(&Factory->GetType());
		return Params;
	}
}

TEST(FRendererSceneContractTests, PSOPrecacheDataValidatesSelectedInitializerAndPriority)
{
    using namespace Durin;
    FMaterialPSOTestPipelines Pipelines;
    auto Native = Pipelines.Request(false);
    auto Data = Pipelines.Describe(Native, false);
    EXPECT_EQ(Data.Type, FPSOPrecacheData::EType::Compute);
    EXPECT_FALSE(Data.bRequired);
    Data.Priority = EPSOPrecachePriority::Highest;
    EXPECT_TRUE(Data.IsValid());
    Data.Type = FPSOPrecacheData::EType::Graphics;
    EXPECT_FALSE(Data.IsValid());
    Data.Type = static_cast<FPSOPrecacheData::EType>(255);
    EXPECT_FALSE(Data.IsValid());
    Data.Type = FPSOPrecacheData::EType::Compute;
    Data.Priority = EPSOPrecachePriority::NotSet;
    EXPECT_FALSE(Data.IsValid());
}

TEST(FRendererSceneContractTests, MaterialPSOInitializerValidationAndInvalidationPrecedePublication)
{
    using namespace Durin;
    uint32 Submissions = 0;
    FMaterialPSORequestManager Invalid([&](const FPSOPrecacheData&)
        -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
        ++Submissions; return std::unexpected(ERHIPipelineRequestRejection::InvalidDescription);
    });
    auto Request = Invalid.PrecachePSOs(MaterialPSOTestParams());
    Invalid.Pump([](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.emplace_back(); return true;
    });
    EXPECT_EQ(Submissions, 0u);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);

    FMaterialPSOTestPipelines Pipelines;
    auto Ready = Pipelines.Request(false);
    ASSERT_TRUE(Ready->Wait());
    FMaterialPSORequestManager* Current = nullptr;
    FMaterialPSORequestManager Retiring([&](const FPSOPrecacheData& Data) {
        const auto Copy = Data; Current->Invalidate(); return Pipelines.Submit(Copy);
    });
    Current = &Retiring;
    Request = Retiring.PrecachePSOs(MaterialPSOTestParams());
    Retiring.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready)); return true;
    });
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Stale);
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
}

TEST(FRendererSceneContractTests, MaterialPSONotificationsAreOwnerDirectedAndReentrant)
{
    using namespace Durin;
    FMaterialPSORequestManager Manager;
    auto Shared = Manager.PrecachePSOs(MaterialPSOTestParams());
    FMaterialPSOPrecacheOwner FirstOwner, SecondOwner;
    auto First = FirstOwner.Observe(Shared), Second = SecondOwner.Observe(Shared);
    uint32 FirstChanges = 0, SecondChanges = 0;
    auto A = First.Subscribe([&] { ++FirstChanges; (void)Manager.GetPendingRequestCount(); });
    auto B = Second.Subscribe([&] { ++SecondChanges; (void)Manager.GetPendingRequestCount(); });
    EXPECT_EQ(FirstChanges, 1u); EXPECT_EQ(SecondChanges, 1u);
    FirstOwner.Invalidate();
    EXPECT_EQ(FirstChanges, 2u); EXPECT_EQ(SecondChanges, 1u);
    EXPECT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Stale);
    EXPECT_EQ(Second.GetProgress().State, EMaterialPSOPrecacheState::Pending);
    First.Cancel();
    EXPECT_EQ(SecondChanges, 1u);
    Manager.Invalidate();
    EXPECT_EQ(SecondChanges, 2u);
    EXPECT_EQ(Second.GetProgress().State, EMaterialPSOPrecacheState::Stale);
}

TEST(FRendererSceneContractTests, FailedOptionalPSODiagnosticsDoNotAuthorizeAnInvalidatedOwner)
{
    using namespace Durin;
    FMaterialPSOTestPipelines Pipelines;
    auto Ready = Pipelines.Request(false), Failed = Pipelines.Request(false, true);
    ASSERT_TRUE(Ready->Wait());
    EXPECT_FALSE(Failed->Wait());
    FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
    auto Shared = Manager.PrecachePSOs(MaterialPSOTestParams());
    FMaterialPSOPrecacheOwner Owner;
    auto Request = Owner.Observe(Shared);
    Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers = {Pipelines.Describe(Ready, true), Pipelines.Describe(Failed, false)};
        return true;
    });
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Request.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < Deadline)
    {
        Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
        std::this_thread::yield();
    }
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    EXPECT_TRUE(Request.IsCurrent());
    Owner.Invalidate();
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Request.IsCurrent());
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
    EXPECT_TRUE(Shared.IsCurrent());
    EXPECT_TRUE(Shared.GetProgress().bRequiredReady);
    Manager.Invalidate();
    EXPECT_EQ(Shared.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Shared.IsCurrent());
    EXPECT_FALSE(Shared.GetProgress().bRequiredReady);
}

TEST(FRendererSceneContractTests, ComponentPSONotificationsIgnoreReplacedAndDestroyedJobSets)
{
    using namespace Durin;
    FMaterialPSORequestManager Manager;
    auto First = Manager.PrecachePSOs(MaterialPSOTestParams());
    auto Params = MaterialPSOTestParams(); Params.PrecachePSOParams.bWireframe = true;
    auto Second = Manager.PrecachePSOs(Params);
    FPSOPrecacheComponentData Component;
    uint32 OldChanges = 0, NewChanges = 0, DestroyedChanges = 0;
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({First}, {}, [&] { ++OldChanges; }));
    FPSOPrecacheComponentData::DispatchOwnerNotifications();
    const auto OldBefore = OldChanges;
    std::thread CancelOld([&] { First.Cancel(); }); CancelOld.join();
    Component.Reset();
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Second}, {}, [&] { ++NewChanges; }));
    FPSOPrecacheComponentData::DispatchOwnerNotifications();
    EXPECT_EQ(OldChanges, OldBefore);
    EXPECT_EQ(NewChanges, 1u);
    {
        FPSOPrecacheComponentData Destroyed;
        ASSERT_TRUE(Destroyed.SetMaterialPSOPrecacheData({Second}, {}, [&] { ++DestroyedChanges; }));
        FPSOPrecacheComponentData::DispatchOwnerNotifications();
        std::thread CancelCurrent([&] { Second.Cancel(); }); CancelCurrent.join();
    }
    const auto BeforeDestroyDispatch = DestroyedChanges;
    FPSOPrecacheComponentData::DispatchOwnerNotifications();
    EXPECT_EQ(DestroyedChanges, BeforeDestroyDispatch);
    EXPECT_EQ(NewChanges, 2u);
}

TEST(FRendererSceneContractTests, MaterialPSORequestsShareWorkWithoutSharingCancellationOrOwnerStaleness)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false);
	ASSERT_TRUE(Ready->Wait());
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto First = Manager.PrecachePSOs(MaterialPSOTestParams());
	auto Params = MaterialPSOTestParams();
	Params.VertexFactoryData.CustomDefaultVertexElements = *Params.VertexFactoryData.GetVertexElements();
	auto Peer = Manager.PrecachePSOs(Params);
	ASSERT_NE(First.GetValue(), 0u);
	EXPECT_EQ(First.GetValue(), Peer.GetValue());
	FMaterialPSOPrecacheOwner OwnerScope;
	auto Owner = OwnerScope.Observe(Peer);
	First.Cancel();
	EXPECT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Canceled);
	uint32 Collections = 0;
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		++Collections; Out.PSOInitializers.push_back(Pipelines.Describe(Ready, true)); return true;
	});
	EXPECT_EQ(Collections, 1u);
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	OwnerScope.Invalidate();
	EXPECT_EQ(Owner.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_FALSE(Owner.GetProgress().bRequiredReady);
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	Manager.Invalidate();
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_NE(Manager.PrecachePSOs(Params).GetValue(), Peer.GetValue());
	Manager.Close();
	EXPECT_EQ(Manager.PrecachePSOs(Params).GetValue(), 0u);
}

TEST(FRendererSceneContractTests, ComponentPriorityBoostReachesSharedMaterialCollectionAndNativeWork)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto Ordinary = Manager.PrecachePSOs(MaterialPSOTestParams());
	auto Params = MaterialPSOTestParams(); Params.PrecachePSOParams.bWireframe = true;
	auto Urgent = Manager.PrecachePSOs(Params);
	auto Peer = Manager.PrecachePSOs(Params);
	FPSOPrecacheComponentData Component;
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Urgent}));
	EXPECT_TRUE(Component.CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority::Highest));
	Peer.BoostPriority(EPSOPrecachePriority::Low);
	EXPECT_EQ(Peer.GetPriority(), EPSOPrecachePriority::Highest);
	std::vector<bool> Order;
	auto Native = Pipelines.Request(true);
	Manager.Pump([&](const auto& Input, auto& Collection) -> std::expected<bool, std::string> {
		Order.push_back(Input.PrecachePSOParams.bWireframe);
		Collection.PSOInitializers.push_back(Pipelines.Describe(Native, true)); return true;
	});
	EXPECT_EQ(Order, (std::vector<bool>{true, false}));
	EXPECT_EQ(Native->GetPriority(), EPSOPrecachePriority::Highest);
	EXPECT_FALSE(Component.IsReadyForProxyCreation());
	Pipelines.Release(); EXPECT_TRUE(Native->Wait());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	EXPECT_TRUE(Component.IsReadyForProxyCreation());
	EXPECT_FALSE(Component.CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority::Highest));
}

TEST(FRendererSceneContractTests, ComponentPSOSetsSeparateRequiredReadinessFromAllWorkAndIgnoreLateBatches)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false);
	ASSERT_TRUE(Ready->Wait());
	auto Gated = Pipelines.Request(true);
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto First = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		Out.PSOInitializers = {Pipelines.Describe(Ready, true), Pipelines.Describe(Gated, false)}; return true;
	});
	ASSERT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Pending);
	EXPECT_TRUE(First.GetProgress().bRequiredReady);
	FPSOPrecacheComponentData Component, Peer;
	uint32 OldCallbacks = 0, NewCallbacks = 0;
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({First}, [&] { ++OldCallbacks; }));
	ASSERT_TRUE(Peer.SetMaterialPSOPrecacheData({First}));
	const auto OldBatch = Component.GetLatestJobSet();
	EXPECT_TRUE(Component.IsPSOPrecaching());
	EXPECT_TRUE(Component.IsReadyForProxyCreation());
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Pending);
	auto Params = MaterialPSOTestParams(); Params.PrecachePSOParams.bReversedZ = true;
	auto New = Manager.PrecachePSOs(Params);
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> { Out.PSOInitializers = {Pipelines.Describe(Ready, true)}; return true; });
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({New}, [&] { ++NewCallbacks; }));
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Ready);
	EXPECT_EQ(NewCallbacks, 1u);
	Component.Poll(); EXPECT_EQ(NewCallbacks, 1u);
	Pipelines.Release(); ASSERT_TRUE(Gated->Wait());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	EXPECT_FALSE(Component.SetLatestJobSetCompleted(OldBatch));
	EXPECT_EQ(OldCallbacks, 0u);
	EXPECT_EQ(Peer.Poll(), EMaterialPSOPrecacheState::Ready);
	Component.Reset();
	EXPECT_FALSE(Component.IsReadyForProxyCreation());
	EXPECT_EQ(Peer.Poll(), EMaterialPSOPrecacheState::Ready);
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({New}, [&] { Component.Reset(); }));
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Canceled);
	auto Ephemeral = std::make_unique<FPSOPrecacheComponentData>();
	ASSERT_TRUE(Ephemeral->SetMaterialPSOPrecacheData({New}, [&] { Ephemeral.reset(); }));
	auto* Completing = Ephemeral.get();
	EXPECT_EQ(Completing->Poll(), EMaterialPSOPrecacheState::Canceled);
	Manager.Close();
}

TEST(FRendererSceneContractTests, MaterialPSOFailuresRemainObservableAndRequestsHaveNoAggregateCountLimit)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false); ASSERT_TRUE(Ready->Wait());
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto Failure = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { return std::unexpected(std::string("fixture collection failure")); });
	EXPECT_EQ(Failure.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	EXPECT_FALSE(Failure.GetProgress().bRequiredReady);
	EXPECT_EQ(Failure.GetDiagnostic(), "fixture collection failure");
	FPSOPrecacheComponentData Component;
	ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Failure}));
	EXPECT_EQ(Component.Poll(), EMaterialPSOPrecacheState::Failed);
	EXPECT_FALSE(Component.IsReadyForProxyCreation());
	Manager.Invalidate();
	auto NativeFailure = Pipelines.Request(false, true);
	(void)NativeFailure->Wait();
	ASSERT_EQ(NativeFailure->GetState(), ERHIPipelineRequestState::Failed);
	Failure = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> { Out.PSOInitializers = {Pipelines.Describe(NativeFailure, true)}; return true; });
	const auto NativeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Failure.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < NativeDeadline)
	{
		Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
		std::this_thread::yield();
	}
	EXPECT_EQ(Failure.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	EXPECT_FALSE(Failure.GetProgress().bRequiredReady);
	Manager.Invalidate();
	std::vector<FMaterialPSOPrecacheRequestID> Requests;
	for (size_t Index = 0; Index < 512; ++Index)
	{
		auto Params = MaterialPSOTestParams();
		FVertexDeclarationElementList Elements{};
		Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, uint16(16 + Index));
		Params.VertexFactoryData.CustomDefaultVertexElements = Elements;
		Requests.push_back(Manager.PrecachePSOs(Params));
		ASSERT_NE(Requests.back().GetValue(), 0u);
	}
	EXPECT_NE(Manager.PrecachePSOs(MaterialPSOTestParams()).GetValue(), 0u);
	uint32 Collections = 0;
	for (size_t Pump = 0; Pump < 64; ++Pump)
		Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
			++Collections; Out.PSOInitializers = {Pipelines.Describe(Ready, true)}; return true;
		});
	EXPECT_EQ(Collections, 512u);
	EXPECT_EQ(Manager.GetPendingRequestCount(), 0u);
	for (const auto& Request : Requests) EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	// Retaining all completed observations neither occupies active capacity nor
	// causes their collectors to run on later frames.
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	Requests.clear();
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
	EXPECT_NE(Manager.PrecachePSOs(MaterialPSOTestParams()).GetValue(), 0u);
	Manager.Close();
}

TEST(FRendererSceneContractTests, MaterialPSOCompletionReleasesPipelinesAndResourcesButKeepsSharedStatus)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Ready = Pipelines.Request(false); ASSERT_TRUE(Ready->Wait());
	std::weak_ptr<FPipelineState> WeakPipeline = Ready;
	auto Resources = std::make_shared<int>(42);
	std::weak_ptr<int> WeakResources = Resources;
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		Out.PSOInitializers.push_back(Pipelines.Describe(Ready, true)); Out.Resources = std::move(Resources); return true;
	});
	Ready.reset();
	EXPECT_TRUE(WeakPipeline.expired());
	EXPECT_TRUE(WeakResources.expired());
	EXPECT_EQ(Request.GetProgress().Pipelines, 1u);
	EXPECT_TRUE(Request.GetProgress().bRequiredReady);
	EXPECT_EQ(Manager.GetPendingRequestCount(), 0u);
	const auto Peer = Manager.PrecachePSOs(MaterialPSOTestParams());
	EXPECT_EQ(Peer.GetValue(), Request.GetValue());
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	Manager.Invalidate();
	EXPECT_EQ(Peer.GetProgress().State, EMaterialPSOPrecacheState::Stale);
}

TEST(FRendererSceneContractTests, MaterialPSOCollectionConcurrencyIsBoundedAcrossPumps)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Gated = Pipelines.Request(true);
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
	std::vector<FMaterialPSOPrecacheRequestID> Requests;
	for (uint32 Index = 0; Index < 24; ++Index)
	{
		auto Params = MaterialPSOTestParams(); Params.Material.ShaderMap.OpacityMaskThreshold = float(Index);
		Requests.push_back(Manager.PrecachePSOs(Params));
	}
	uint32 Collections = 0;
	auto Collect = [&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		++Collections; Out.PSOInitializers = {Pipelines.Describe(Gated, true)}; return true;
	};
	for (uint32 Frame = 0; Frame < 5; ++Frame) Manager.Pump(Collect);
	EXPECT_EQ(Collections, FMaterialPSORequestManager::MaximumActiveCollections);
	Pipelines.Release(); ASSERT_TRUE(Gated->Wait());
	for (uint32 Frame = 0; Frame < 4; ++Frame) Manager.Pump(Collect);
	EXPECT_EQ(Collections, 24u);
	EXPECT_EQ(Manager.GetPendingRequestCount(), 0u);
}

TEST(FRendererSceneContractTests, MaterialPSOResumesAfterNativeCapacityWithoutResubmittingAcceptedPrefix)
{
	using namespace Durin;
	FMaterialPSOTestPipelines Pipelines;
	auto Gated = Pipelines.Request(true);
	std::vector<std::shared_ptr<FComputePipelineState>> Occupied;
	for (uint32 Slot = 0; Slot < 254; ++Slot)
	{
		auto Result = Pipelines.TryRequestSlot(Slot); ASSERT_TRUE(Result);
		Occupied.push_back(*Result);
	}
	uint32 Accepted = 0, Rejected = 0;
	std::weak_ptr<FPipelineState> Prefix;
	FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) {
		auto Result = Pipelines.Submit(Data);
		if (Result) { ++Accepted; if (Accepted == 1) Prefix = *Result; }
		else { EXPECT_EQ(Result.error(), ERHIPipelineRequestRejection::CapacityExceeded); ++Rejected; }
		return Result;
	});
	auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	uint32 Collections = 0;
	auto Collect = [&](const auto&, auto& Out) -> std::expected<bool, std::string> {
		++Collections;
		Out.PSOInitializers = {Pipelines.DescribeSlot(254), Pipelines.DescribeSlot(255)};
		return true;
	};
	Manager.Pump(Collect); Manager.Pump(Collect);
	EXPECT_EQ(Accepted, 1u); EXPECT_EQ(Rejected, 2u);
	EXPECT_EQ(Collections, 1u);
	EXPECT_EQ(Request.GetProgress().Pipelines, 2u);
	EXPECT_GE(Request.GetProgress().Pending, 1u);
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Pending);
	Pipelines.Release(); ASSERT_TRUE(Gated->Wait());
	for (auto& Pipeline : Occupied) ASSERT_TRUE(Pipeline->Wait());

	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Request.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < Deadline)
	{
		Manager.Pump(Collect); std::this_thread::yield();
	}
	EXPECT_EQ(Collections, 1u);
	EXPECT_EQ(Accepted, 2u);
	EXPECT_TRUE(Prefix.expired());
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);

}

TEST(FRendererSceneContractTests, MaterialPSOFailureSurvivesObserverCancellationAndOwnerInvalidation)
{
    using namespace Durin;
    FMaterialPSOPrecacheOwner OwnerScope;
    const auto Request = OwnerScope.Observe(FMaterialPSOPrecacheRequestID::Rejected("failed collection"));
    Request.Cancel();
    OwnerScope.Invalidate();
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_EQ(Request.GetDiagnostic(), "failed collection");
}

TEST(FRendererSceneContractTests, MaterialPSOCollectionCannotPublishEmptyOrInvalidatedSuccess)
{
	using namespace Durin;
	FMaterialPSORequestManager Manager;
	auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { return true; });
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	Manager.Invalidate();
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	Request = Manager.PrecachePSOs(MaterialPSOTestParams());
	Manager.Pump([&](const auto&, auto&) -> std::expected<bool, std::string> { Manager.Invalidate(); return true; });
	EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_FALSE(Request.GetProgress().bRequiredReady);
}

TEST(FRendererSceneContractTests, ResolvesRegisteredMeshFactoriesWithoutFamilyDispatch)
{
	using namespace Durin;
	using namespace Durin::RendererPrivate;
	const auto Local = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
	const auto Spline = FindMeshVertexFactory(FSplineMeshBatchBinding{}.GetFactoryKey());
	ASSERT_TRUE(Local);
	ASSERT_TRUE(Spline);
	EXPECT_NE(Local->GetType().GetStableKey(), Spline->GetType().GetStableKey());
	EXPECT_NE(Local->GetLayoutKey(), Spline->GetLayoutKey());
	for (uint32 Pass = 0; Pass < 3; ++Pass)
	{
		ASSERT_NE(Local->GetShaderType(Pass), nullptr);
		ASSERT_NE(Spline->GetShaderType(Pass), nullptr);
		EXPECT_NE(Local->GetShaderType(Pass), Spline->GetShaderType(Pass));
	}
	EXPECT_EQ(Local->GetShaderType(99), nullptr);
	EXPECT_EQ(Spline->GetShaderType(99), nullptr);
	EXPECT_FALSE(FindMeshVertexFactory({0}));
	EXPECT_FALSE(RegisterMeshVertexFactory(nullptr));
	EXPECT_FALSE(RegisterMeshVertexFactory(Local));
	EXPECT_EQ(FindMeshVertexFactory(Local->GetType().GetStableKey()), Local);
	FShaderCompileOptions Options;
	const auto* SplineShader = Spline->GetShaderType(MaterialMeshPassForward);
	SplineShader->ModifyCompilationEnvironment({SplineShader, SplineShader->GetVirtualShaderPath(), SplineShader->GetEntryPoint(), SplineShader->GetFrequency()}, Options);
	EXPECT_TRUE(std::ranges::any_of(Options.Macros, [](const auto& Macro) {
		return Macro.Name == "DURIN_SPLINE_MESH" && Macro.Value == "1";
	}));
}

namespace Durin
{
	struct FSceneRendererTestAccess
	{
		static auto Context(FSceneRenderer& Renderer) -> FSceneFrameContext&
		{ return Renderer.Context; }
	};

	class FRejectAllocation final : public FRDGAllocator
	{
	public:
		auto Allocate(std::span<const FRDGAllocationRequest>, FRDGAllocatedResources&)
			-> FRDGAllocationResult override
		{ return std::unexpected(FRDGAllocationError{FRDGAllocationFailure{
			.Reason = ERDGAllocationError::PhysicalAllocationFailed}}); }
	};

	TEST(FRendererSceneContractTests, ResourcePreparationReadsSamplingSequenceWithoutBeginningHistory)
	{
		FRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<Tests::FRDGAllocatorTestCommand>([](FRHICommandListImmediate&) {
			FSceneViewState State;
			EXPECT_EQ(State.GetVolumetricCloudHistory().GetSuccessfulSequence(), 0u);
			EXPECT_FALSE(State.GetVolumetricCloudHistory().IsSubmissionActive());
			FSceneViewTemporalMetadata Metadata;
			auto First = State.GetVolumetricCloudHistory().Begin(Metadata, 1, false);
			EXPECT_EQ(First.SuccessfulSequence, State.GetVolumetricCloudHistory().GetSuccessfulSequence());
			State.GetVolumetricCloudHistory().SetPendingClear(0, 0);
			State.GetVolumetricCloudHistory().Commit();
			const auto PreparedSequence = State.GetVolumetricCloudHistory().GetSuccessfulSequence();
			EXPECT_EQ(PreparedSequence, 1u);
			EXPECT_FALSE(State.GetVolumetricCloudHistory().IsSubmissionActive());
			auto Next = State.GetVolumetricCloudHistory().Begin(Metadata, 2, false);
			EXPECT_EQ(Next.SuccessfulSequence, PreparedSequence);
			State.GetVolumetricCloudHistory().Abort();
			EXPECT_EQ(State.GetVolumetricCloudHistory().GetSuccessfulSequence(), PreparedSequence);
			EXPECT_FALSE(State.GetVolumetricCloudHistory().IsSubmissionActive());
		});
		FlushRenderingCommands();
	}

	TEST(FRendererSceneContractTests, SceneRenderAuthorsIntoCallerGraphAndAllocationFailureDoesNotPublish)
	{
		FRenderingThreadScope RenderingThread;
		EnqueueRenderCommand<Tests::FRDGAllocatorTestCommand>([](FRHICommandListImmediate& Commands) {
			Tests::FRDGAllocationTestRHI RHI;
			FSceneRenderingService Service;
			FSceneRenderer Submission(Service);
			auto& Context = FSceneRendererTestAccess::Context(Submission);
			auto Output = MakeRefCount<FRHITexture>(FRHITextureCreateDesc::Create2D(
				"CallerOutput", 32, 32, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource));
			Context.Logical.Width = Context.Logical.Height = 32;
			Context.Logical.OutputTarget = Output.GetReference();
			Context.Logical.PreparedView.emplace();
			Context.Logical.CallerView = &Context.Logical.PreparedView->View;
			FRDGBuilder Graph;
			Submission.Render(Graph);
			EXPECT_FALSE(Graph.HasCompiledPlan());
			EXPECT_EQ(RHI.Creates, 0u);
			EXPECT_TRUE(Submission.GetCompletion().IsValid());
			const auto Consumer = FRDGBuilderTestAccessor::AddPass(Graph, "Caller.ReadScene", ERDGPassType::Graphics);
			FRDGBuilderTestAccessor::UseTexture(Graph, Consumer, Submission.GetOutputTexture(),
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERDGUse::Read, ERHIAccess::GraphicsShaderRead);
			Graph.MarkPassRoot(Consumer);
			FRejectAllocation Allocator;
			const auto Execution = Graph.Execute(Commands, &Allocator);
			ASSERT_FALSE(Execution.has_value());
			EXPECT_EQ(GetRDGExecutionStatus(Execution), ERDGExecutionStatus::PreparationFailed);
			ASSERT_TRUE(Graph.HasCompiledPlan());
			EXPECT_EQ(Graph.GetPasses().back().Name, "Caller.ReadScene");
			const auto Capture = Graph.Capture();
			EXPECT_FALSE(std::ranges::any_of(Capture.Resources, [](const auto& Resource) {
				return Resource.Name == "Scene.ProductionDeferredParameters";
			}));
			const auto Base = std::ranges::find_if(Graph.GetPasses(), [](const auto& Pass) { return Pass.Name == BaseScenePassName; });
			ASSERT_NE(Base, Graph.GetPasses().end());
			for (const auto& Parameter : Capture.Parameters)
				if (Parameter.PassDeclarationIndex == Base->DeclarationIndex)
					EXPECT_FALSE(Parameter.bPassManagedTransition) << Parameter.FieldPath;
			EXPECT_EQ(Graph.GetState(), ERDGBuilderState::Failed);
		});
		FlushRenderingCommands();
	}
}
#endif

#if !defined(DURIN_RENDERER_RDG_ALLOCATOR_TESTS)
TEST(FRendererSceneContractTests, SkyLightingDependencyPollingDoesNotDelayFirstOrFinalCapture)
{
    using namespace Durin;
    FSkyLightingCaptureSchedule Schedule;
    FSkyLightingRequestKey Request{1, 1, 1, 1};
    // Shader and PSO become ready on successive frames. A 60-second automatic
    // interval must not turn those two Pending results into minute-long waits.
    EXPECT_TRUE(Schedule.CanAttempt(Request, 0.0, true, 60.0));
    EXPECT_TRUE(Schedule.CanAttempt(Request, 0.016, true, 60.0));
    EXPECT_TRUE(Schedule.CanAttempt(Request, 0.032, true, 60.0));
    Schedule.Submit(0.032);
    EXPECT_FALSE(Schedule.CanAttempt(Request, 0.048, false, 60.0));
    // Registration completion or a manual recapture can request a final capture
    // immediately after an early one; ordinary sky animation remains throttled.
    ++Request.Request;
    EXPECT_TRUE(Schedule.CanAttempt(Request, 0.048, true, 60.0));
    Schedule.Submit(0.048);
    EXPECT_FALSE(Schedule.CanAttempt(Request, 60.032, false, 60.0));
    EXPECT_TRUE(Schedule.CanAttempt(Request, 60.048, false, 60.0));
}

TEST(FRendererSceneContractTests, SkyLightingFailureBackoffDoesNotBlockNewCaptureRequests)
{
    using namespace Durin;
    FSkyLightingCaptureSchedule Schedule;
    const FSkyLightingRequestKey Failed{1, 1, 1, 1};
    Schedule.Fail(Failed, 10.0, 1.0);
    EXPECT_FALSE(Schedule.CanAttempt(Failed, 10.016, true, 1.0));
    EXPECT_TRUE(Schedule.CanAttempt(Failed, 11.0, true, 1.0));
    for (const FSkyLightingRequestKey NewRequest : {
        FSkyLightingRequestKey{2, 1, 1, 1}, FSkyLightingRequestKey{1, 2, 1, 1},
        FSkyLightingRequestKey{1, 1, 2, 1}, FSkyLightingRequestKey{1, 1, 1, 2}})
        EXPECT_TRUE(Schedule.CanAttempt(NewRequest, 10.016, true, 1.0));
    Schedule.Submit(11.0);
    // A subsequent Pending dependency poll carries no stale failure deadline.
    EXPECT_TRUE(Schedule.CanAttempt(Failed, 11.016, true, 1.0));
    EXPECT_FALSE(Schedule.CanAttempt(Failed, 11.016, false, 1.0));
}

TEST(FRendererSceneContractTests, SkyLightingPublishesOnlyCompletedJobs)
{
    using namespace Durin;
    const auto Device = AllocateRHIDeviceGeneration();
    FRHIGPUQueueTimeline Timeline(Device, {0});
    FSkyLightingSceneState Scene;
    auto Previous = std::make_shared<FSkyLightingGeneration>();
    Previous->Revision = 7;
    Scene.Published = Previous;
    auto Candidate = std::make_shared<FSkyLightingGeneration>();
    Candidate->Resources.Device = Device;
    Candidate->Revision = 8;
    auto Completion = Timeline.Reserve();
    Scene.Job = FSkyLightingJob{Candidate, Completion, {}};
    auto Status = std::make_shared<FSkyLightUpdateStatus>();
    EXPECT_FALSE(Scene.PollJob(true, Status, 1));
    EXPECT_EQ(Scene.Published, Previous);
    ASSERT_TRUE(Timeline.MarkSubmitted(Completion));
    EXPECT_FALSE(Scene.PollJob(true, Status, 1));
    EXPECT_EQ(Scene.Published, Previous);
    ASSERT_TRUE(Timeline.ObserveCompleted(Completion));
    auto Finished = Scene.PollJob(true, Status, 2.5);
    ASSERT_TRUE(Finished);
    EXPECT_FALSE(Scene.Job);
    EXPECT_TRUE(Finished->Completion.IsRetirementEligible());
    EXPECT_EQ(Scene.Published, Candidate);
    EXPECT_EQ(Status->PublishedRevision.load(), 8u);
    EXPECT_EQ(Status->CompletedUpdates.load(), 1u);
    EXPECT_EQ(Status->UpdateMilliseconds.load(), 2.5);
    EXPECT_EQ(Status->State.load(), ESkyLightUpdateState::Ready);
    EXPECT_FALSE(Scene.PollJob(true, Status, 0));
    EXPECT_EQ(Status->CompletedUpdates.load(), 1u);
}

TEST(FRendererSceneContractTests, SkyLightingFailuresReleaseSchedulingButPreserveResultAndRetirementLease)
{
    using namespace Durin;
    for (const auto Failure : {ERHIGPUSubmissionState::Canceled, ERHIGPUSubmissionState::Failed, ERHIGPUSubmissionState::DeviceLost})
    {
        const auto Device = AllocateRHIDeviceGeneration();
        FRHIGPUQueueTimeline Timeline(Device, {0});
        FSkyLightingSceneState Scene;
        auto Previous = std::make_shared<FSkyLightingGeneration>();
        Previous->Revision = 7;
        Scene.Published = Previous;
        auto Candidate = std::make_shared<FSkyLightingGeneration>();
        Candidate->Resources.Device = Device;
        const std::weak_ptr<const FSkyLightingGeneration> Lease = Candidate;
        auto Completion = Timeline.Reserve();
        Scene.Job = FSkyLightingJob{std::move(Candidate), Completion, {}};
        auto Status = std::make_shared<FSkyLightUpdateStatus>();
        Status->PublishedRevision = 7;
        if (Failure == ERHIGPUSubmissionState::Canceled) ASSERT_TRUE(Timeline.Cancel(Completion));
        else Timeline.Fail(Failure == ERHIGPUSubmissionState::DeviceLost);
        auto Finished = Scene.PollJob(true, Status, 0);
        ASSERT_TRUE(Finished);
        EXPECT_FALSE(Scene.Job);
        EXPECT_EQ(Scene.Published, Previous);
        EXPECT_EQ(Status->PublishedRevision.load(), 7u);
        EXPECT_EQ(Status->State.load(), ESkyLightUpdateState::Failed);
        EXPECT_EQ(Status->CompletedUpdates.load(), 0u);
        EXPECT_FALSE(Lease.expired());
        EXPECT_EQ(Finished->Completion.IsRetirementEligible(), Failure == ERHIGPUSubmissionState::Canceled);
        Finished.reset();
        EXPECT_TRUE(Lease.expired());
    }
}

TEST(FRendererSceneContractTests, SkyLightingChangedAuthorityNeverPublishesCompletedOldJob)
{
    using namespace Durin;
    FRHIGPUQueueTimeline Timeline(AllocateRHIDeviceGeneration(), {0});
    FSkyLightingSceneState Scene;
    const auto Completion = Timeline.Reserve();
    Scene.Job = FSkyLightingJob{std::make_shared<FSkyLightingGeneration>(), Completion, {}};
    ASSERT_TRUE(Timeline.MarkSubmitted(Completion));
    ASSERT_TRUE(Timeline.ObserveCompleted(Completion));
    auto Status = std::make_shared<FSkyLightUpdateStatus>();
    Status->State = ESkyLightUpdateState::Pending;
    EXPECT_TRUE(Scene.PollJob(false, Status, 0));
    EXPECT_FALSE(Scene.Job);
    EXPECT_FALSE(Scene.Published);
    EXPECT_EQ(Status->CompletedUpdates.load(), 0u);
    EXPECT_EQ(Status->State.load(), ESkyLightUpdateState::Pending);
}

TEST(FRendererSceneContractTests, RetirementPreservesPhysicalLeasesAcrossLogicalInvalidation)
{
    using namespace Durin;
    const auto Device = AllocateRHIDeviceGeneration();
    FRHIGPUQueueTimeline Timeline(Device, {0});
    FRendererResourceRetirementQueue Retirement;
    FRHIQueueCapabilities Physical;
    Physical.DeviceGeneration = Device;
    auto Resource = std::make_shared<int>(7);
    const std::weak_ptr<int> Lease = Resource;
    auto Pending = Timeline.Reserve();
    Retirement.Retire(std::move(Resource), Pending, Device);
    FRenderResourceGeneration Logical;
    Logical.Advance(ERenderResourceGenerationDependency::Device);
    Retirement.Collect(Physical);
    EXPECT_FALSE(Lease.expired());
    Timeline.Fail(true);
    Retirement.Collect(Physical);
    EXPECT_FALSE(Lease.expired());
    // Only replacement of the actual RHI device permits abandoning failed native work.
    Physical.DeviceGeneration = AllocateRHIDeviceGeneration();
    Retirement.Collect(Physical);
    EXPECT_TRUE(Lease.expired());
}

TEST(FRendererSceneContractTests, HitProxyCacheRetainsTextureIdentityUntilInvalidation)
{
    using namespace Durin;
    FRenderingThreadScope RenderingThread;
    EnqueueRenderCommand("RetainHitProxyTextureIdentity", [](FRHICommandListImmediate&) {
        class FTrackedTexture final : public FRHITexture
        {
        public:
            explicit FTrackedTexture(bool& InDestroyed)
                : FRHITexture(FRHITextureCreateDesc::Create2D("CachedIdentity", 1, 1, EPixelFormat::RGBA8_UNORM)), Destroyed(InDestroyed) {}
            ~FTrackedTexture() override { Destroyed = true; }
        private:
            bool& Destroyed;
        };
        bool bDestroyed = false;
        FHitProxyMapCache Cache;
        FTextureRHIRef Texture = MakeRefCount<FTrackedTexture>(bDestroyed);
        auto* Identity = Texture.GetReference();
        Cache.Publish_RenderThread({}, FHitProxyMap::Create(1, 1, FByteBuffer(sizeof(FHitProxyPixel))), false, {Texture});
        Texture = nullptr;
        EXPECT_EQ(Identity->GetRefCount(), 1u);
        EXPECT_FALSE(bDestroyed);
        Cache.Invalidate_RenderThread();
        RHIFlushDeferredResources();
        EXPECT_TRUE(bDestroyed);
    });
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, ViewportPreparationNeverBlocksInteractiveRendering)
{
    using namespace Durin;
    FRenderingThreadScope RenderingThread;
    auto Viewport = FSceneViewport::CreateOffscreen(nullptr);
    Viewport->RequestResourcePreparation();
    EnqueueRenderCommand("RecheckViewportPreparation", [&](FRHICommandListImmediate& Commands) {
        FTestViewStateRenderer Renderer;
        FSceneViewRenderRequest Request;
        Request.View.ViewportWidth = 64;
        EXPECT_EQ(Viewport->RenderView_RenderThread(Renderer, Commands, Request), ERenderViewResult::Success);
        Request.View.ViewportWidth = 128;
        EXPECT_EQ(Viewport->RenderView_RenderThread(Renderer, Commands, Request), ERenderViewResult::Success);
        EXPECT_EQ(Viewport->GetResourcePreparationProgress_RenderThread().State, ERenderResourcePreparationState::Ready);
        EXPECT_EQ(Renderer.PrepareCount, 1u);
        EXPECT_EQ(Renderer.RenderCount, 2u);
        Request.View.ViewportWidth = 64;
        EXPECT_EQ(Viewport->RenderView_RenderThread(Renderer, Commands, Request), ERenderViewResult::Success);
        EXPECT_EQ(Renderer.RenderCount, 3u);
        Viewport->RequestResourcePreparation();
        Request.View.ViewportWidth = 128;
        EXPECT_EQ(Viewport->RenderView_RenderThread(Renderer, Commands, Request), ERenderViewResult::Success);
        EXPECT_EQ(Renderer.PrepareCount, 2u);
        EXPECT_EQ(Viewport->GetResourcePreparationProgress_RenderThread().State, ERenderResourcePreparationState::Pending);
    });
    FlushRenderingCommands();
}

TEST(FRendererSceneContractTests, RetirementShutdownDrainsDeviceBeforeReleasingFailedLeases)
{
    using namespace Durin;
    const auto Device = AllocateRHIDeviceGeneration();
    FRHIGPUQueueTimeline Timeline(Device, {0});
    FRendererResourceRetirementQueue Retirement;
    const auto Completion = Timeline.Reserve();
    auto Resource = std::make_shared<int>(7);
    const std::weak_ptr<int> Lease = Resource;
    Retirement.Retire(std::move(Resource), Completion, Device);
    Timeline.Fail(true);
    bool bDrained = false;
    Retirement.Drain([&] { EXPECT_FALSE(Lease.expired()); bDrained = true; });
    EXPECT_TRUE(bDrained);
    EXPECT_TRUE(Lease.expired());
    Retirement.Drain([] { ADD_FAILURE() << "Empty retirement queue must not wait for the GPU."; });
}

TEST(FRendererSceneContractTests, RetirementNeedsNeitherSceneNorNewWorkAndHonorsCanceledPrefix)
{
    using namespace Durin;
    const auto Device = AllocateRHIDeviceGeneration();
    FRHIGPUQueueTimeline Timeline(Device, {0});
    FRHIQueueCapabilities Physical;
    Physical.DeviceGeneration = Device;
    FRendererResourceRetirementQueue Retirement;
    const auto Prior = Timeline.Reserve();
    const auto Canceled = Timeline.Reserve();
    ASSERT_TRUE(Timeline.MarkSubmitted(Prior));
    ASSERT_TRUE(Timeline.Cancel(Canceled));
    std::weak_ptr<int> Lease;
    {
        auto SceneOwned = std::make_shared<int>(42);
        Lease = SceneOwned;
        Retirement.Retire(SceneOwned, Canceled, Device);
    }
    Retirement.Collect(Physical);
    EXPECT_FALSE(Lease.expired());
    ASSERT_TRUE(Timeline.ObserveCompleted(Prior));
    Retirement.Collect(Physical);
    EXPECT_TRUE(Lease.expired());
}

TEST(FRendererSceneContractTests, CollectionSnapshotsDoNotMergeAcrossConsumersOrRemoval)
{
    using namespace Durin;
    FRenderingThreadScope RenderingThread;
    FSceneTestOwner SceneOwner;
    auto& Scene = *SceneOwner;
    const FGuid Id(5, 6, 7, 8);
    auto First = std::make_shared<FMaterialParameterCollectionSnapshot>();
    First->Layout.CollectionId = Id; First->Version = 1;
    auto Second = std::make_shared<FMaterialParameterCollectionSnapshot>(*First);
    Second->Version = 2;
    std::promise<void> Started, Release;
    auto StartedEvent = Started.get_future();
    auto ReleaseEvent = Release.get_future().share();
    EnqueueRenderCommand("BlockCollectionPublication", [&](FRHICommandListImmediate&) {
        Started.set_value(); ReleaseEvent.wait();
    });
    StartedEvent.wait();
    Scene.UpdateMaterialParameterCollection(First);
    std::shared_ptr<const FMaterialParameterCollectionSnapshot> Between, Removed, Final;
    EnqueueRenderCommand("ReadFirstCollection", [&](FRHICommandListImmediate&) {
        Between = Scene.GetMaterialParameterCollection_RenderThread(Id);
    });
    Scene.UpdateMaterialParameterCollection(Second);
    Scene.RemoveMaterialParameterCollection(Id);
    EnqueueRenderCommand("ReadRemovedCollection", [&](FRHICommandListImmediate&) {
        Removed = Scene.GetMaterialParameterCollection_RenderThread(Id);
    });
    Scene.UpdateMaterialParameterCollection(First);
    EnqueueRenderCommand("ReadRepublishedCollection", [&](FRHICommandListImmediate&) {
        Final = Scene.GetMaterialParameterCollection_RenderThread(Id);
    });
    Release.set_value();
    FlushRenderingCommands();
    EXPECT_EQ(Between, First);
    EXPECT_FALSE(Removed);
    EXPECT_EQ(Final, First);
}

TEST(FRendererSceneContractTests, UnreadyComponentIsExcludedAcrossBatchesAndShadowLODs)
{
    using namespace Durin;
    FPreparedStaticMeshView Receiver;
    Receiver.Primitives.resize(3);
    Receiver.Primitives[0].PrimitiveId = FPrimitiveComponentId(11);
    Receiver.Primitives[1].PrimitiveId = FPrimitiveComponentId(22);
    Receiver.Primitives[2].PrimitiveId = FPrimitiveComponentId(11);
    Receiver.Opaque.push_back({.ResolvedIndex = 0, .PrimitiveIndex = 0});
    Receiver.Opaque.push_back({.ResolvedIndex = 1, .PrimitiveIndex = 1});
    Receiver.Masked.push_back({.ResolvedIndex = 2, .PrimitiveIndex = 2});
    Receiver.Translucent.push_back({.ResolvedIndex = 3, .PrimitiveIndex = 0});
    const auto Original = Receiver;
    FResolvedStaticMeshView Resolved;
    Resolved.Draws.resize(4);
    for (auto& Draw : Resolved.Draws) Draw.bReady = true;
    Resolved.Draws[2].bReady = false;
    std::array<FPreparedStaticMeshView, 2> Shadows;
    for (auto& Shadow : Shadows)
    {
        Shadow.Primitives.resize(2);
        // Different view/LOD ordering must still use the component identity.
        Shadow.Primitives[0].PrimitiveId = FPrimitiveComponentId(22);
        Shadow.Primitives[1].PrimitiveId = FPrimitiveComponentId(11);
        Shadow.Opaque.push_back({.ResolvedIndex = 7, .PrimitiveIndex = 1});
        Shadow.Masked.push_back({.ResolvedIndex = 9, .PrimitiveIndex = 0});
    }
    RemoveUnreadyStaticMeshPrimitives(Receiver, Resolved, Shadows);
    ASSERT_EQ(Receiver.Opaque.size(), 1u);
    EXPECT_EQ(Receiver.Opaque.front().ResolvedIndex, 1u);
    EXPECT_TRUE(Receiver.Masked.empty());
    EXPECT_TRUE(Receiver.Translucent.empty());
    for (const auto& Shadow : Shadows)
    {
        EXPECT_TRUE(Shadow.Opaque.empty());
        ASSERT_EQ(Shadow.Masked.size(), 1u);
        EXPECT_EQ(Shadow.Masked.front().ResolvedIndex, 9u);
    }
    // No permanent visibility state: the next submission can admit the component.
    Receiver = Original;
    Resolved.Draws[2].bReady = true;
    RemoveUnreadyStaticMeshPrimitives(Receiver, Resolved);
    EXPECT_EQ(Receiver.GetNumSections(), 4u);
    Receiver.ResourceFailurePrimitives.push_back(FPrimitiveComponentId(22));
    RemoveUnreadyStaticMeshPrimitives(Receiver, Resolved);
    EXPECT_EQ(Receiver.GetNumSections(), 3u);
    EXPECT_EQ(Receiver.Opaque.front().ResolvedIndex, 0u);
}

TEST(FRendererSceneContractTests, RequiredPSOCollectionDoesNotWaitForOptionalShaders)
{
    using namespace Durin;
    FMaterialPSOTestPipelines Pipelines;
    auto Ready = Pipelines.Request(false);
    ASSERT_TRUE(Ready->Wait());
    FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data) { return Pipelines.Submit(Data); });
    auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
    uint32 Pumps = 0;
    auto Collect = [&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        if (++Pumps == 1)
        {
            Out.PSOInitializers.push_back(Pipelines.Describe(Ready));
            Out.bRequiredPSOsCollected = true;
            return false;
        }
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready, false));
        return true;
    };
    Manager.Pump(Collect);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Pending);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    FPSOPrecacheComponentData Component;
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Request}));
    EXPECT_TRUE(Component.IsReadyForProxyCreation());
    EXPECT_TRUE(Component.IsPSOPrecaching());
    Manager.Pump(Collect);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);

    auto Params = MaterialPSOTestParams();
    Params.PrecachePSOParams.Participation = {false, false};
    auto Optional = Manager.PrecachePSOs(Params);
    EXPECT_FALSE(Optional.GetProgress().bRequiredReady);
    Manager.Pump([](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.bRequiredPSOsCollected = true;
        return false;
    });
    EXPECT_TRUE(Optional.GetProgress().bRequiredReady);
    EXPECT_EQ(Optional.GetProgress().State, EMaterialPSOPrecacheState::Pending);

    Manager.Invalidate();
    Request = Manager.PrecachePSOs(MaterialPSOTestParams());
    Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready));
        Out.bRequiredPSOsCollected = true;
        return std::unexpected("Optional shader collection failed.");
    });
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Request}));
    EXPECT_TRUE(Component.IsReadyForProxyCreation());

    Manager.Invalidate();
    EXPECT_FALSE(Component.IsReadyForProxyCreation());
    Request = Manager.PrecachePSOs(MaterialPSOTestParams()); Pumps = 0;
    Manager.Pump(Collect);
    Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
        Out.PSOInitializers.push_back(Pipelines.Describe(Ready)); return true;
    });
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
}

TEST(FRendererSceneContractTests, AdmissionBudgetIsSharedByViewsAndDefersResourceSlotsWithoutFailure)
{
    using namespace Durin;
    FRenderResourceAdmissionBudget Budget(1, 1, std::chrono::seconds(30));
    TRenderResourceCreationSlot<int> Slot;
    FRenderResourceGeneration Generation;
    auto Create = []() {
        if (!FRenderResourcePreparationScope::TryBeforeShader())
        {
            FRenderPipelineRequestScope::MarkResourcePending();
            return TRenderResourceCreateResult<int>::Pending();
        }
        return TRenderResourceCreateResult<int>::Success(42);
    };
    auto Report = [](const auto&) { ADD_FAILURE() << "Budget deferral is not a resource failure."; };
    {
        FRenderResourceAdmissionScope FirstView(Budget, 7);
        EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforeShader());
        EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforePipeline());
    }
    {
        Budget.BeginFrame(7); // Re-entering the same frame must preserve its allowance.
        FRenderResourceAdmissionScope SecondView(Budget, 7);
        EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforePipeline());
        EXPECT_EQ(Slot.Resolve(Generation, Create, Report), nullptr);
        EXPECT_EQ(Budget.GetShaderAdmissions(), 1u);
        EXPECT_EQ(Budget.GetPipelineAdmissions(), 1u);
        EXPECT_EQ(Budget.GetDeferredAdmissions(), 2u);
    }
    {
        FRenderResourceAdmissionScope NextFrame(Budget, 8);
        const auto* Payload = Slot.Resolve(Generation, Create, Report);
        ASSERT_NE(Payload, nullptr);
        EXPECT_EQ(*Payload, 42);
    }
    {
        FRenderResourceAdmissionBudget PipelineLimited(1, 0, std::chrono::seconds(30));
        FRenderResourceAdmissionScope View(PipelineLimited, 9);
        EXPECT_FALSE(FRenderResourcePreparationScope::TryBeforePipeline());
        const auto DeferredBefore = FRenderResourceAdmissionScope::GetDeferredAdmissions();
        EXPECT_TRUE(FRenderResourcePreparationScope::TryBeforeShader());
        // Pipeline backpressure must not turn an admitted Shader failure into Pending.
        EXPECT_EQ(FRenderResourceAdmissionScope::GetDeferredAdmissions(), DeferredBefore);
    }
}

TEST(FRendererSceneContractTests, ShadowOnlyPendingComponentIsExcludedFromReceiverAndAllCascades)
{
    using namespace Durin;
    FPreparedStaticMeshView Receiver;
    Receiver.Primitives.resize(2);
    Receiver.Primitives[0].PrimitiveId = FPrimitiveComponentId(1);
    Receiver.Primitives[1].PrimitiveId = FPrimitiveComponentId(2);
    Receiver.Opaque = {{.ResolvedIndex = 0, .PrimitiveIndex = 0}, {.ResolvedIndex = 1, .PrimitiveIndex = 1}};
    FResolvedStaticMeshView Ready;
    Ready.Draws.resize(2); for (auto& Draw : Ready.Draws) Draw.bReady = true;
    std::array Shadows{Receiver, Receiver};
    std::array Resources{Ready, Ready};
    Resources[1].Draws[0].bReady = false;
    RemoveUnreadyStaticMeshPrimitives(Receiver, Ready, Shadows, Resources);
    ASSERT_EQ(Receiver.Opaque.size(), 1u);
    EXPECT_EQ(Receiver.Opaque.front().ResolvedIndex, 1u);
    for (const auto& Shadow : Shadows)
    {
        ASSERT_EQ(Shadow.Opaque.size(), 1u);
        EXPECT_EQ(Shadow.Opaque.front().ResolvedIndex, 1u);
    }
}

#endif

TEST(FRendererSceneContractTests, MaterialPSOPassSelectionUsesParticipationPathsAndMaterialEligibility)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    FMaterialPSOPrecacheParams Params;
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Opaque;
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Lit;
    auto Count = [](const auto& Plan, EMeshPipelinePass Pass) {
        return std::ranges::count_if(Plan, [Pass](const auto& Entry) { return Entry.Pass == Pass; });
    };
    auto Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Plan.size(), 22u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::HybridRetainedForward), 0);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::SortedTranslucency), 0);
    EXPECT_EQ(std::ranges::count_if(Plan, [](const auto& Entry) { return Entry.bRequired; }), 6);
    for (const auto& Entry : Plan)
        if (Entry.bRequired) {
            EXPECT_FALSE(Entry.Raster.bWireframe);
            EXPECT_FALSE(Entry.Raster.bReversedZ);
            EXPECT_NE(Entry.Pass, EMeshPipelinePass::HitProxy);
        }
    Params.PrecachePSOParams.Participation.bCastShadow = false;
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Plan.size(), 20u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 0);
    Params.PrecachePSOParams.Participation = {true, false};
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    ASSERT_EQ(Plan.size(), 2u);
    for (const auto& Entry : Plan) EXPECT_EQ(Entry.Pass, EMeshPipelinePass::ShadowDepth);
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Translucent;
    EXPECT_TRUE(SelectMaterialPSOPrecachePasses(Params, true).empty());
    Params.PrecachePSOParams.Participation = {false, false};
    EXPECT_TRUE(SelectMaterialPSOPrecachePasses(Params, true).empty());
    Params.PrecachePSOParams.Participation = {};
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Plan.size(), 20u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::SortedTranslucency), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 0);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 0);
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Masked;
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Unlit;
    Plan = SelectMaterialPSOPrecachePasses(Params, true);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::HybridRetainedForward), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 2);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 0);
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Lit;
    Plan = SelectMaterialPSOPrecachePasses(Params, false);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::HybridRetainedForward), 8);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 0);
    FSceneRenderPreparationConfig Config;
    Config.Settings.bHybrid = Config.Settings.bHitProxy = Config.bSupportsWireframe = false;
    Params.PrecachePSOParams.bReversedZ = true;
    Plan = SelectMaterialPSOPrecachePasses(Params, true, Config);
    EXPECT_EQ(Plan.size(), 6u);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::Forward), 4);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::ShadowDepth), 2);
    for (const auto& Entry : Plan) {
        EXPECT_FALSE(Entry.Raster.bWireframe);
        if (Entry.bRequired) EXPECT_EQ(Entry.Raster.bReversedZ, Entry.Pass != EMeshPipelinePass::ShadowDepth);
    }
    Config = {};
    Config.Settings.bForward = false;
    Plan = SelectMaterialPSOPrecachePasses(Params, true, Config);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::Forward), 0);
    EXPECT_EQ(Count(Plan, EMeshPipelinePass::GBuffer), 8);
}

TEST(FRendererSceneContractTests, EmptyApplicableMaterialPSOSetPublishesReadinessWithoutNativeWork)
{
    using namespace Durin;
    uint32 Submissions = 0;
    FMaterialPSORequestManager Manager([&](const FPSOPrecacheData&)
        -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
        ++Submissions;
        return std::unexpected(ERHIPipelineRequestRejection::CapacityExceeded);
    });
    auto Params = MaterialPSOTestParams();
    Params.PrecachePSOParams.Participation = {true, false};
    Params.Material.ShaderMap.BlendMode = EMaterialBlendMode::Translucent;
    auto Request = Manager.PrecachePSOs(Params);
    ASSERT_TRUE(Request.IsValid());
    EXPECT_FALSE(Request.GetProgress().bRequiredReady);
    Manager.Pump([](const auto& In, auto& Out) -> std::expected<bool, std::string> {
        EXPECT_TRUE(RendererPrivate::SelectMaterialPSOPrecachePasses(In, true).empty());
        Out.bRequiredPSOsCollected = true;
        return true;
    });
    EXPECT_EQ(Submissions, 0u);
    EXPECT_TRUE(Request.GetProgress().bRequiredReady);
    EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Ready);
}

TEST(FRendererSceneContractTests, ShadowPSORequirementsUseTheNormalizedDrawingState)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    auto Params = MaterialPSOTestParams();
    Params.PrecachePSOParams.Participation = {true, false};
    Params.PrecachePSOParams.bWireframe = true;
    Params.PrecachePSOParams.bReversedZ = true;
    FSceneRenderPreparationConfig Config;
    Config.bSupportsWireframe = false;
    const auto Plan = SelectMaterialPSOPrecachePasses(Params, true, Config);
    ASSERT_EQ(Plan.size(), 2u);
    for (const auto& Entry : Plan) {
        EXPECT_EQ(Entry.Pass, EMeshPipelinePass::ShadowDepth);
        EXPECT_TRUE(Entry.bRequired);
        EXPECT_FALSE(Entry.Raster.bWireframe);
        EXPECT_FALSE(Entry.Raster.bReversedZ);
        const auto State = MakeMeshPipelineState(Params.Material, Entry.Raster);
        const auto Draw = DescribeMeshPipeline(EMeshPipelinePass::ShadowDepth,
            MakeMeshPipelineState(Params.Material, {true, Entry.Raster.bReverseCulling, false, true}),
            Params.PrecachePSOParams.Topology);
        const auto Precache = DescribeMeshPipeline(Entry.Pass, State, Params.PrecachePSOParams.Topology);
        EXPECT_EQ(Draw.Rasterizer, Precache.Rasterizer);
        EXPECT_EQ(Draw.Depth, Precache.Depth);
        EXPECT_EQ(Draw.RenderTargets, Precache.RenderTargets);
    }
}

TEST(FRendererSceneContractTests, OptionalPSOAdmissionFailuresDoNotWithholdRequiredPipelines)
{
    using namespace Durin;
    for (uint32 Failure = 0; Failure < 4; ++Failure)
    {
        SCOPED_TRACE(Failure);
        FMaterialPSOTestPipelines Pipelines;
        auto Ready = Pipelines.Request(false);
        ASSERT_TRUE(Ready->Wait());
        FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data)
            -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
            if (Data.bRequired) return Pipelines.Submit(Data);
            if (Failure == 1) return std::shared_ptr<FPipelineState>{};
            if (Failure == 2) throw std::runtime_error("Optional backend rejection");
            return std::unexpected(ERHIPipelineRequestRejection::Unsupported);
        });
        auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
        Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
            auto Optional = Pipelines.Describe(Ready, false);
            if (Failure == 3) Optional.Type = static_cast<FPSOPrecacheData::EType>(255);
            Out.PSOInitializers = {Pipelines.Describe(Ready), Optional};
            Out.bRequiredPSOsCollected = true;
            return true;
        });
        EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
        EXPECT_TRUE(Request.GetProgress().bRequiredReady);
        EXPECT_EQ(Request.GetProgress().RequiredPending, 0u);
        EXPECT_EQ(Request.GetProgress().Failed, 1u);
        EXPECT_FALSE(Request.GetDiagnostic().empty());
        FPSOPrecacheComponentData Component;
        ASSERT_TRUE(Component.SetMaterialPSOPrecacheData({Request}));
        EXPECT_TRUE(Component.IsReadyForProxyCreation());
        Manager.Invalidate();
        EXPECT_FALSE(Component.IsReadyForProxyCreation());
    }
}

TEST(FRendererSceneContractTests, OptionalAdmissionFailureStillObservesPendingAndFailedRequiredWork)
{
    using namespace Durin;
    for (bool bRequiredFails : {false, true})
    {
        SCOPED_TRACE(bRequiredFails);
        FMaterialPSOTestPipelines Pipelines;
        auto Required = Pipelines.Request(!bRequiredFails, bRequiredFails);
        if (bRequiredFails) EXPECT_FALSE(Required->Wait());
        FMaterialPSORequestManager Manager([&](const FPSOPrecacheData& Data)
            -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
            if (!Data.bRequired) return std::unexpected(ERHIPipelineRequestRejection::Unsupported);
            return Pipelines.Submit(Data);
        });
        auto Request = Manager.PrecachePSOs(MaterialPSOTestParams());
        Manager.Pump([&](const auto&, auto& Out) -> std::expected<bool, std::string> {
            Out.PSOInitializers = {Pipelines.Describe(Required), Pipelines.Describe(Required, false)};
            Out.bRequiredPSOsCollected = true;
            return true;
        });
        EXPECT_FALSE(Request.GetProgress().bRequiredReady);
        if (!bRequiredFails) {
            EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Pending);
            EXPECT_EQ(Request.GetProgress().RequiredPending, 1u);
            Pipelines.Release();
            ASSERT_TRUE(Required->Wait());
            Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
        }
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (Request.GetProgress().State == EMaterialPSOPrecacheState::Pending && std::chrono::steady_clock::now() < Deadline) {
            Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { ADD_FAILURE(); return false; });
            std::this_thread::yield();
        }
        EXPECT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Failed);
        EXPECT_EQ(Request.GetProgress().bRequiredReady, !bRequiredFails);
        EXPECT_EQ(Request.GetProgress().Failed, bRequiredFails ? 2u : 1u);
    }
}

TEST(FRendererSceneContractTests, PlannedPSOsCoverDrawingStatesAndRequiredNormalizedRoutes)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    auto SameDescription = [](const auto& A, const auto& B) {
        return A.Topology == B.Topology && A.RenderTargets == B.RenderTargets
            && A.Rasterizer == B.Rasterizer && A.Depth == B.Depth && A.ColorBlends == B.ColorBlends;
    };
    for (const auto Blend : {EMaterialBlendMode::Opaque, EMaterialBlendMode::Masked, EMaterialBlendMode::Translucent})
    for (const auto Shading : {EMaterialShadingModel::Lit, EMaterialShadingModel::Unlit})
    for (const auto Topology : {EGeometryTopology::TriangleList, EGeometryTopology::LineList})
    for (bool bSupportsGBuffer : {false, true})
    for (bool bOrientationChanging : {false, true})
    for (bool bTwoSided : {false, true})
    {
        SCOPED_TRACE(std::format("blend={} shading={} topology={} gbuffer={} orientation={} two-sided={}",
            uint8(Blend), uint8(Shading), uint8(Topology), bSupportsGBuffer, bOrientationChanging, bTwoSided));
        FMaterialPSOPrecacheParams Params;
        Params.Material.ShaderMap.BlendMode = Blend;
        Params.Material.ShaderMap.ShadingModel = Shading;
        Params.Material.bTwoSided = bTwoSided;
        Params.PrecachePSOParams.bOrientationChanging = bOrientationChanging;
        Params.PrecachePSOParams.Topology = Topology;
        const auto Plan = SelectMaterialPSOPrecachePasses(Params, bSupportsGBuffer);
        std::vector<EMeshPipelinePass> DrawRoutes{EMeshPipelinePass::Forward, EMeshPipelinePass::HitProxy};
        if (Blend == EMaterialBlendMode::Translucent) DrawRoutes.push_back(EMeshPipelinePass::SortedTranslucency);
        else {
            DrawRoutes.push_back(EMeshPipelinePass::ShadowDepth);
            DrawRoutes.push_back(Shading == EMaterialShadingModel::Lit && bSupportsGBuffer
                ? EMeshPipelinePass::GBuffer : EMeshPipelinePass::HybridRetainedForward);
        }
        for (const auto Route : DrawRoutes)
        for (uint8 Variant = 0; Variant < 8; ++Variant)
        {
            const FMeshPipelineRasterOptions DrawRaster{(Variant & 4) != 0, (Variant & 1) != 0,
                bOrientationChanging, (Variant & 2) != 0};
            const auto DrawDescription = DescribeMeshPipeline(Route, MakeMeshPipelineState(Params.Material, DrawRaster),
                Topology, Blend, DrawRaster.bReversedZ);
            bool bCovered = false, bRequiredCovered = false;
            for (const auto& Entry : Plan)
            {
                if (Entry.Pass != Route) continue;
                const auto PrecacheDescription = DescribeMeshPipeline(Entry.Pass, MakeMeshPipelineState(Params.Material, Entry.Raster),
                    Topology, Blend, Entry.Raster.bReversedZ);
                if (!SameDescription(DrawDescription, PrecacheDescription)) continue;
                bCovered = true;
                bRequiredCovered |= Entry.bRequired;
            }
            EXPECT_TRUE(bCovered);
            if (Route == EMeshPipelinePass::ShadowDepth
                || (Route != EMeshPipelinePass::HitProxy && !DrawRaster.bWireframe && !DrawRaster.bReversedZ))
                EXPECT_TRUE(bRequiredCovered);
        }
    }
}

TEST(FRendererSceneContractTests, PrecacheExcludesUnavailableVertexFactoryRoutesBeforeRequiredClassification)
{
    using namespace Durin;
    using namespace Durin::RendererPrivate;
    const auto Local = FindMeshVertexFactory(FStaticMeshBatchBinding{}.GetFactoryKey());
    ASSERT_TRUE(Local);
    class FLimitedFactory final : public FMeshVertexFactoryImplementation
    {
        const FMeshVertexFactoryImplementation& Base;
        uint32 Unavailable;
    public:
        FLimitedFactory(const FMeshVertexFactoryImplementation& InBase, uint32 InUnavailable)
            : Base(InBase), Unavailable(InUnavailable) {}
        auto GetType() const -> const FVertexFactoryType& override { return Base.GetType(); }
        auto GetLayoutKey() const -> FXxHash64 override { return Base.GetLayoutKey(); }
        auto GetShaderType(uint32 Pass) const -> FShaderType* override
        { return Pass == Unavailable ? nullptr : Base.GetShaderType(Pass); }
        auto Resolve(const FMeshMaterialShaderMap&, uint32) const -> std::shared_ptr<const FMeshVertexShaderBinding> override { return {}; }
    };
    auto Params = MaterialPSOTestParams();
    Params.Material.ShaderMap.ShadingModel = EMaterialShadingModel::Lit;
    const FLimitedFactory NoShadow(*Local, MaterialMeshPassShadow);
    EXPECT_FALSE(NoShadow.Supports(MaterialMeshPassShadow, {}));
    auto Plan = SelectMaterialPSOPrecachePasses(Params, NoShadow);
    ASSERT_FALSE(Plan.empty());
    for (const auto& Entry : Plan) EXPECT_NE(Entry.Pass, EMeshPipelinePass::ShadowDepth);
    const FLimitedFactory NoReceiver(*Local, MaterialMeshPassForward);
    EXPECT_FALSE(NoReceiver.Supports(MaterialMeshPassForward, {}));
    EXPECT_TRUE(NoReceiver.Supports(MaterialMeshPassGBuffer, {}));
    Plan = SelectMaterialPSOPrecachePasses(Params, NoReceiver);
    ASSERT_EQ(Plan.size(), 2u);
    for (const auto& Entry : Plan) {
        EXPECT_EQ(Entry.Pass, EMeshPipelinePass::ShadowDepth);
        EXPECT_TRUE(Entry.bRequired);
    }
    const FLimitedFactory NoGBuffer(*Local, MaterialMeshPassGBuffer);
    EXPECT_FALSE(NoGBuffer.Supports(MaterialMeshPassGBuffer, {}));
    Plan = SelectMaterialPSOPrecachePasses(Params, NoGBuffer);
    EXPECT_EQ(std::ranges::count_if(Plan, [](const auto& Entry) { return Entry.Pass == EMeshPipelinePass::HybridRetainedForward; }), 8);
    EXPECT_EQ(std::ranges::count_if(Plan, [](const auto& Entry) { return Entry.Pass == EMeshPipelinePass::GBuffer; }), 0);
}

TEST(FRendererSceneContractTests, MaterialPSORequestsDoNotShareDifferentSceneFeatureLevels)
{
    using namespace Durin;
    FMaterialPSORequestManager Manager;
    auto High = MaterialPSOTestParams();
    High.FeatureLevel = ERHIFeatureLevel::SM5;
    auto Low = High;
    Low.FeatureLevel = ERHIFeatureLevel::ES3_1;
    auto HighRequest = Manager.PrecachePSOs(High);
    auto LowRequest = Manager.PrecachePSOs(Low);
    ASSERT_TRUE(HighRequest.IsValid());
    ASSERT_TRUE(LowRequest.IsValid());
    EXPECT_NE(HighRequest.GetValue(), LowRequest.GetValue());
    EXPECT_EQ(Manager.PrecachePSOs(High).GetValue(), HighRequest.GetValue());
    EXPECT_EQ(Manager.PrecachePSOs(Low).GetValue(), LowRequest.GetValue());
    auto Invalid = High;
    Invalid.FeatureLevel = static_cast<ERHIFeatureLevel>(255);
    EXPECT_FALSE(Manager.PrecachePSOs(Invalid).IsValid());
}

TEST(FRendererSceneContractTests, PreparationConfigVersionInvalidatesReadyCoverageBeforeResourceRetirement)
{
	using namespace Durin;
	FMaterialPSORequestManager Manager;
	Manager.SetPreparationConfigVersion(1);
	auto Params = MaterialPSOTestParams();
	Params.PreparationConfig.FeatureLevel = Params.FeatureLevel;
	Params.PreparationConfig.Version = 1;
	auto Old = Manager.PrecachePSOs(Params);
	Manager.Pump([](const auto&, auto& Collection) -> std::expected<bool, std::string> {
		Collection.bRequiredPSOsCollected = true;
		return true;
	});
	ASSERT_TRUE(Old.GetProgress().bRequiredReady);
	EXPECT_EQ(Old.GetProgress().State, EMaterialPSOPrecacheState::Ready);
	Manager.SetPreparationConfigVersion(2);
	EXPECT_FALSE(Old.IsCurrent());
	EXPECT_FALSE(Old.GetProgress().bRequiredReady);
	EXPECT_EQ(Old.GetProgress().State, EMaterialPSOPrecacheState::Stale);
	EXPECT_FALSE(Manager.PrecachePSOs(Params).IsValid());
	Params.PreparationConfig.Version = 2;
	Params.PreparationConfig.Settings.bHybrid = false;
	auto Current = Manager.PrecachePSOs(Params);
	ASSERT_TRUE(Current.IsValid());
	EXPECT_NE(Current.GetValue(), Old.GetValue());
	Manager.InvalidateOutdatedPreparationConfigs();
	EXPECT_TRUE(Current.IsCurrent());
	Manager.Pump([](const auto& Snapshot, auto& Collection) -> std::expected<bool, std::string> {
		EXPECT_EQ(Snapshot.PreparationConfig.Version, 2u);
		EXPECT_FALSE(Snapshot.PreparationConfig.Settings.bHybrid);
		Collection.bRequiredPSOsCollected = true;
		return true;
	});
	EXPECT_TRUE(Current.GetProgress().bRequiredReady);
}

TEST(FRendererSceneContractTests, PreparationConfigSnapshotParticipatesInSharingAndPreservesFailureDiagnostics)
{
	using namespace Durin;
	FMaterialPSORequestManager Manager;
	Manager.SetPreparationConfigVersion(1);
	auto Params = MaterialPSOTestParams();
	Params.PreparationConfig.Version = 1;
	Params.PreparationConfig.FeatureLevel = Params.FeatureLevel;
	auto First = Manager.PrecachePSOs(Params);
	Params.PreparationConfig.bSupportsWireframe = false;
	auto Second = Manager.PrecachePSOs(Params);
	EXPECT_NE(First.GetValue(), Second.GetValue());
	Manager.Pump([](const auto&, auto&) -> std::expected<bool, std::string> { return std::unexpected("Fixture collection failure"); });
	ASSERT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	Manager.SetPreparationConfigVersion(2);
	Manager.InvalidateOutdatedPreparationConfigs();
	EXPECT_FALSE(First.IsCurrent());
	EXPECT_FALSE(First.GetProgress().bRequiredReady);
	EXPECT_EQ(First.GetProgress().State, EMaterialPSOPrecacheState::Failed);
	EXPECT_EQ(First.GetDiagnostic(), "Fixture collection failure");
}
