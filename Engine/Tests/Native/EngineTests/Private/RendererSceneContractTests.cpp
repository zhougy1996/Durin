#include "Rendering/SkeletalMeshSceneProxy.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/LightSceneProxy.h"
#include "CoreGlobals.h"
#include "Client/SceneViewport.h"
#include "HAL/PlatformLTS.h"
#include "NativeTestSupport.h"
#include "Modules/ModuleTestSupport.h"
#include "Math/Operations.h"
#include "RenderingThread.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/MeshRendererShared.h"
#include "Renderers/SceneFrameGraphExecutor.h"
#include "Renderers/SceneFrameGraphContributors.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/SceneViewState.h"
#include "Renderers/SurfaceMaterial.h"
#include "Renderers/DirectionalShadowView.h"
#include "RendererModule.h"
#include "Scene.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "StaticMesh/StaticMeshResources.h"
#include "ViewRenderStatistics.h"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
	struct FInspectionOutputParameters final
	{
		Durin::FRDGTokenParameter Output;

		static auto GetRDGParametersMetadata()
			-> const Durin::FRDGParametersMetadata*
		{
			static const std::array Members{
				Durin::MakeRDGResourceParameterMemberMetadata<
					FInspectionOutputParameters, decltype(Output),
					Durin::FRDGTokenParameter>("Output",
						offsetof(FInspectionOutputParameters, Output),
						Durin::ERDGParameterMemberKind::Token,
						Durin::ERDGResourceKind::Token,
						Durin::ERDGParameterRangeKind::None,
						Durin::ERDGUse::Write,
						Durin::ERHIAccess::None, true),
			};
			static const auto Metadata =
				Durin::MakeInlineRDGParametersMetadata<
					FInspectionOutputParameters>(
						"FInspectionOutputParameters", Members);
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
				if (bEscaped) bEscaped = false;
				else if (Character == '\\') bEscaped = true;
				else if (Character == Quote) Quote = '\0';
				continue;
			}
			if (Character == '"' || Character == '\'')
			{
				Quote = Character;
				continue;
			}
			if (Character == '(') ++Depth;
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

	auto FindManualRenderGraphAuthoring(std::string_view Source,
		std::string_view DisplayPath) -> std::vector<std::string>
	{
		std::vector<std::string> Violations;
		constexpr std::array ManualUses{
			"UseTexture", "UseBuffer", "UseToken", "UseValue",
			"UseColorAttachment", "UseDepthStencilAttachment",
			"UseManagedColorAttachment", "UseManagedDepthStencilAttachment",
			"UseManagedTexture",
		};
		for (const std::string_view Method : ManualUses)
		{
			for (const std::string_view Access : {".", "->"})
			{
				const std::string MemberCall = std::string(Access)
					+ std::string(Method) + "(";
				if (Source.find(MemberCall) != std::string_view::npos)
					Violations.push_back(std::string(DisplayPath) + ": manual "
						+ std::string(Method));
			}
		}

		for (const std::string_view AddPass : {".AddPass(", "->AddPass("})
		{
			size_t Position = Source.find(AddPass);
			while (Position != std::string_view::npos)
			{
				const size_t OpenParenthesis = Position + AddPass.size() - 1;
				if (!HasMovedParameterArgument(Source, OpenParenthesis))
					Violations.push_back(std::string(DisplayPath)
						+ ": AddPass without a moved parameter object");
				Position = Source.find(AddPass, OpenParenthesis + 1);
			}
		}
		return Violations;
	}
}

template <typename T>
concept CHasResolvedReceiver = requires(T Value) { Value.ResolvedReceiver; };

TEST(FRendererSceneContractTests,
	ProductionRendererRequiresParameterizedRenderGraphAuthoring)
{
	EXPECT_TRUE(FindManualRenderGraphAuthoring(
		"Graph.AddPass(\"Parameterized\", Type, std::move(Parameters), Callback);",
		"seeded-valid").empty());
	const auto SeededViolations = FindManualRenderGraphAuthoring(
		"Graph.AddPass(\"Manual\", Type); Graph.UseTexture(Pass, Texture);",
		"seeded-invalid");
	ASSERT_EQ(SeededViolations.size(), 2u);

	const std::filesystem::path SourceRoot(DURIN_RENDERER_SOURCE_DIR);
	ASSERT_TRUE(std::filesystem::is_directory(SourceRoot));
	std::vector<std::filesystem::path> Sources;
	for (const auto& Entry : std::filesystem::recursive_directory_iterator(
		SourceRoot))
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
		Violations.insert(Violations.end(), FileViolations.begin(),
			FileViolations.end());
	}
	EXPECT_TRUE(Violations.empty()) << ::testing::PrintToString(Violations);
}

template <typename T>
concept CHasResolvedDirectionalShadow = requires(T Value) {
	Value.ResolvedDirectionalShadow;
};

template <typename T>
concept CHasTelemetry = requires(T Value) { Value.Telemetry; };

template <typename T>
concept CHasResolvedTargets = requires(T Value) { Value.Targets; };

template <typename T>
concept CHasDeferredParameters = requires(T Value) { Value.DeferredParameters; };

template <typename T>
concept CHasReadyDrawHash = requires(T Value) { Value.ReadyDraws; };

template <typename T>
concept CHasMaterialBindingHash = requires(T Value) { Value.MaterialBindings; };

template <typename T>
concept CHasResolvedDrawRecords = requires(T Value) { Value.Draws; };

template <typename T>
concept CHasRenderObservations = requires(T Value) { Value.Observations; };

template <typename T>
concept CHasDirectExecutionCounters = requires(T Value) {
	Value.AttemptedDraws;
	Value.SuccessfulDraws;
	Value.RejectedDraws;
};

TEST(FRendererSceneContractTests, MaterialProgramIdentityParticipatesInRendererKeys)
{
	Durin::FEffectiveMeshPipelineKey FirstPipeline;
	FirstPipeline.Material.ShaderMap.RenderLayout =
		Durin::MakeDefaultMaterialRenderLayout().Identity;
	FirstPipeline.Material.ShaderMap.ProgramIdentity.Digest.HashLow =
		0x0123456789abcdefull;
	FirstPipeline.Material.ShaderMap.ProgramIdentity.Digest.HashHigh =
		0xfedcba9876543210ull;
	auto SecondPipeline = FirstPipeline;
	SecondPipeline.Material.ShaderMap.ProgramIdentity.Digest.HashLow =
		0x1123456789abcdefull;
	const Durin::FMaterialRenderRepresentation Representation =
		Durin::MakeCanonicalMaterialRenderRepresentation();
	const Durin::FVertexDeclarationElementList Elements{};
	const std::array<uint32, 6> Geometry{};
	const auto First = Durin::RendererPrivate::MakeMeshDrawSortKey(
		Durin::EMeshBasePass::Opaque, FirstPipeline, Representation,
		0, Elements, Geometry, 1, 0, 0);
	const auto Second = Durin::RendererPrivate::MakeMeshDrawSortKey(
		Durin::EMeshBasePass::Opaque, SecondPipeline, Representation,
		0, Elements, Geometry, 1, 0, 0);
	EXPECT_NE(First.Pipeline, Second.Pipeline);
	EXPECT_NE(Durin::RendererPrivate::GetIdentityText(
		FirstPipeline.Material.ShaderMap),
		Durin::RendererPrivate::GetIdentityText(
			SecondPipeline.Material.ShaderMap));
}

template <typename T>
concept CHasUploadRange = requires(T Value) { Value.Range; };

template <typename T>
concept CHasPublicQualificationSwitches = requires(T Value) {
	Value.bEnableGBufferQualification;
	Value.bEnableDeferredDirectionalQualification;
	Value.bEnableGroundTruthAmbientOcclusionQualification;
	Value.bForceFragmentContactVisibility;
};

template <typename T>
concept CHasPreparedView = requires(T Value) { Value.PreparedView; };

template <typename TContributor, typename TInputs>
concept CAcceptsContributorInputs = requires(
	const TInputs& Inputs) {
	TContributor::AddPasses(Inputs);
};

template <typename TContributor, typename TInputs, typename TOutput>
concept CContributorReturns = requires(const TInputs& Inputs) {
	{ TContributor::AddPasses(Inputs) } -> std::same_as<TOutput>;
};

static_assert(!std::is_copy_constructible_v<Durin::FSceneViewStateOwner>);
static_assert(!std::is_copy_assignable_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_move_constructible_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_move_assignable_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_final_v<Durin::FSceneFrameGraphExecutor>);
static_assert(!std::is_empty_v<Durin::FSceneFrameGraphExecutor>);
static_assert(!std::is_copy_constructible_v<Durin::FSceneFrameGraphExecutor>);
static_assert(!std::is_copy_assignable_v<Durin::FSceneFrameGraphExecutor>);
static_assert(!std::is_move_constructible_v<Durin::FSceneFrameGraphExecutor>);
static_assert(!std::is_move_assignable_v<Durin::FSceneFrameGraphExecutor>);
static_assert(!CHasPublicQualificationSwitches<
	Durin::FSceneViewRenderOptions>);
static_assert(!std::is_copy_constructible_v<
	Durin::FScopedRendererQualificationPolicy>);
static_assert(!CHasResolvedReceiver<Durin::FSceneRenderPlan>);
static_assert(!CHasResolvedDirectionalShadow<Durin::FSceneRenderPlan>);
static_assert(!CHasTelemetry<Durin::FSceneRenderPlan>);
static_assert(!CHasResolvedTargets<Durin::FSceneRenderPlan>);
static_assert(!CHasResolvedTargets<Durin::FResolvedSceneFrame>);
static_assert(!CHasTelemetry<Durin::FSceneFrameOutcome>);
static_assert(!CHasDeferredParameters<Durin::FSceneFrameOutcome>);
static_assert(std::is_default_constructible_v<Durin::FSceneFrameOutcome>);
static_assert(!CHasUploadRange<Durin::FPreparedSkeletalPaletteTable::FEntry>);
static_assert(CHasUploadRange<Durin::FResolvedSkeletalPaletteTable::FEntry>);
static_assert(std::is_copy_constructible_v<Durin::FSceneFrameTopology>);
static_assert(std::is_same_v<
	Durin::FDirectionalShadowGraphContributor::Result,
	Durin::FDirectionalShadowPassResult>);
static_assert(CAcceptsContributorInputs<
	Durin::FDirectionalShadowGraphContributor,
	Durin::FDirectionalShadowGraphInputs>);
static_assert(CAcceptsContributorInputs<
	Durin::FGBufferGraphContributor, Durin::FGBufferGraphInputs>);
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
static_assert(CAcceptsContributorInputs<
	Durin::FVolumetricCloudShadowGraphContributor,
	Durin::FCloudShadowGraphInputs>);
static_assert(CAcceptsContributorInputs<
	Durin::FBaseSceneGraphContributor,
	Durin::FBaseSceneGraphInputs>);
static_assert(CContributorReturns<Durin::FDirectionalShadowGraphContributor,
	Durin::FDirectionalShadowGraphInputs,
	Durin::FDirectionalShadowGraphOutput>);
static_assert(CContributorReturns<Durin::FGBufferGraphContributor,
	Durin::FGBufferGraphInputs, Durin::FGBufferGraphOutput>);
static_assert(CContributorReturns<Durin::FAmbientOcclusionGraphContributor,
	Durin::FAmbientOcclusionGraphInputs,
	Durin::FAmbientOcclusionGraphOutput>);
static_assert(CContributorReturns<
	Durin::FContactShadowVisibilityGraphContributor,
	Durin::FContactShadowGraphInputs, Durin::FContactShadowGraphOutput>);
static_assert(CContributorReturns<
	Durin::FVolumetricCloudShadowGraphContributor,
	Durin::FCloudShadowGraphInputs, Durin::FCloudShadowGraphOutput>);
static_assert(CContributorReturns<
	Durin::FDeferredDirectionalLightingGraphContributor,
	Durin::FDeferredLightingGraphInputs,
	Durin::FDeferredLightingGraphOutput>);
static_assert(CContributorReturns<Durin::FBaseSceneGraphContributor,
	Durin::FBaseSceneGraphInputs, Durin::FBaseSceneGraphOutput>);
static_assert(CContributorReturns<
	Durin::FVolumetricCloudSpatialGraphContributor,
	Durin::FCloudSpatialGraphInputs, Durin::FCloudSpatialGraphOutput>);
static_assert(CContributorReturns<
	Durin::FVolumetricCloudCompositeGraphContributor,
	Durin::FCloudCompositeGraphInputs, Durin::FCloudCompositeGraphOutput>);
static_assert(CContributorReturns<Durin::FSceneColorGraphContributor,
	Durin::FSceneColorGraphInputs, Durin::FSceneColorGraphOutput>);
static_assert(CContributorReturns<Durin::FPostProcessGraphContributor,
	Durin::FPostProcessGraphInputs, Durin::FPostProcessGraphOutput>);
static_assert(CContributorReturns<Durin::FEditorAssistanceGraphContributor,
	Durin::FEditorAssistanceGraphInputs,
	Durin::FEditorAssistanceGraphOutput>);
static_assert(!CHasPreparedView<Durin::FDirectionalShadowGraphInputs>);
static_assert(!CHasPreparedView<Durin::FPostProcessGraphInputs>);
static_assert(!CAcceptsContributorInputs<
	Durin::FDirectionalShadowGraphContributor, Durin::FSceneRenderPlan>);
static_assert(CHasResolvedDrawRecords<Durin::FResolvedStaticMeshView>);
static_assert(CHasResolvedDrawRecords<Durin::FResolvedSkeletalMeshView>);
static_assert(CHasResolvedDrawRecords<Durin::FResolvedTerrainView>);
static_assert(!CHasReadyDrawHash<Durin::FResolvedStaticMeshView>);
static_assert(!CHasReadyDrawHash<Durin::FResolvedSkeletalMeshView>);
static_assert(!CHasReadyDrawHash<Durin::FResolvedTerrainView>);
static_assert(!CHasMaterialBindingHash<Durin::FResolvedStaticMeshView>);
static_assert(!CHasMaterialBindingHash<Durin::FResolvedSkeletalMeshView>);
static_assert(!CHasMaterialBindingHash<Durin::FResolvedTerrainView>);
static_assert(CHasRenderObservations<Durin::FResolvedStaticMeshView>);
static_assert(CHasRenderObservations<Durin::FResolvedSkeletalMeshView>);
static_assert(CHasRenderObservations<Durin::FResolvedTerrainView>);
static_assert(!CHasDirectExecutionCounters<Durin::FResolvedStaticMeshView>);
static_assert(!CHasDirectExecutionCounters<Durin::FResolvedSkeletalMeshView>);
static_assert(!CHasDirectExecutionCounters<Durin::FResolvedTerrainView>);
static_assert(static_cast<uint8>(
	Durin::ERDGAllocationObservation::Count) == 12);

TEST(FRendererSceneContractTests, RDGAllocationPolicyKeepsStructuralBoundary)
{
	using FPolicy = Durin::FRendererRDGAllocationPolicy;
	EXPECT_TRUE(FPolicy::IsBatchWithinStructuralBudget(
		FPolicy::MaximumRetainedBytes));
	EXPECT_FALSE(FPolicy::IsBatchWithinStructuralBudget(
		FPolicy::MaximumRetainedBytes + 1));
}

TEST(FRendererSceneContractTests, QualificationPolicyIsLexicallyScoped)
{
	EXPECT_FALSE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
	{
		Durin::FScopedRendererQualificationPolicy Outer({
			.bEnableGBuffer = true});
		EXPECT_TRUE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
		{
			Durin::FScopedRendererQualificationPolicy Inner({
				.bForceFragmentVolumetricCloud = true});
			const auto InnerPolicy =
				Durin::GetRendererQualificationPolicy();
			EXPECT_FALSE(InnerPolicy.bEnableGBuffer);
			EXPECT_TRUE(InnerPolicy.bForceFragmentVolumetricCloud);
		}
		EXPECT_TRUE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
	}
	EXPECT_FALSE(Durin::GetRendererQualificationPolicy().bEnableGBuffer);
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

TEST(FRendererSceneContractTests,
	ContactShadowPilotsComposeExactGraphicsAndComputeShaderAuthority)
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
		EXPECT_EQ(Graphics->Members[Index].ShaderBindingType,
			Durin::ERHIBindingType::Texture);
		EXPECT_EQ(Compute->Members[Index].ShaderBindingType,
			Durin::ERHIBindingType::Texture);
		EXPECT_EQ(Graphics->Members[Index].Access,
			Durin::ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Compute->Members[Index].Access,
			Durin::ERHIAccess::ComputeShaderRead);
	}
	EXPECT_FALSE(Graphics->Members.back().bShaderBinding);
	EXPECT_EQ(Graphics->Members.back().Kind,
		Durin::ERDGParameterMemberKind::ColorAttachment);
	EXPECT_TRUE(Compute->Members.back().bShaderBinding);
	EXPECT_STREQ(Compute->Members.back().ShaderBindingName,
		"ContactVisibilityOutput");
	EXPECT_EQ(Compute->Members.back().ShaderBindingType,
		Durin::ERHIBindingType::StorageImage);
	EXPECT_EQ(Compute->Members.back().Use, Durin::ERDGUse::Write);
}

TEST(FRendererSceneContractTests, SceneFrameTopologyUsesExclusiveRoutes)
{
	Durin::FSceneFrameTopology Topology;
	EXPECT_FALSE(Topology.UsesContactShadowVisibilityFragment());
	EXPECT_FALSE(Topology.UsesContactShadowVisibilityCompute());
	Topology.ContactShadowVisibility = Durin::ESceneFrameRoute::Fragment;
	EXPECT_TRUE(Topology.UsesContactShadowVisibilityFragment());
	EXPECT_FALSE(Topology.UsesContactShadowVisibilityCompute());
	Topology.ContactShadowVisibility = Durin::ESceneFrameRoute::Compute;
	EXPECT_FALSE(Topology.UsesContactShadowVisibilityFragment());
	EXPECT_TRUE(Topology.UsesContactShadowVisibilityCompute());

	Topology.VolumetricCloudShadow = Durin::ESceneFrameRoute::Fragment;
	EXPECT_TRUE(Topology.UsesCloudShadowFragment());
	EXPECT_FALSE(Topology.UsesCloudShadowCompute());
	Topology.VolumetricCloud = Durin::ESceneFrameRoute::Compute;
	EXPECT_FALSE(Topology.UsesCloudFragment());
	EXPECT_TRUE(Topology.UsesCloudCompute());
}

TEST(FRendererSceneContractTests, FeatureContributorOrderIsStableAndUnique)
{
	const std::array<std::string_view, 12> Names{
		Durin::FDirectionalShadowGraphContributor::Name,
		Durin::FGBufferGraphContributor::Name,
		Durin::FAmbientOcclusionGraphContributor::Name,
		Durin::FContactShadowVisibilityGraphContributor::Name,
		Durin::FVolumetricCloudShadowGraphContributor::Name,
		Durin::FDeferredDirectionalLightingGraphContributor::Name,
		Durin::FBaseSceneGraphContributor::Name,
		Durin::FVolumetricCloudSpatialGraphContributor::Name,
		Durin::FVolumetricCloudCompositeGraphContributor::Name,
		Durin::FSceneColorGraphContributor::Name,
		Durin::FPostProcessGraphContributor::Name,
		Durin::FEditorAssistanceGraphContributor::Name};
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
	ASSERT_EQ(Metadata->Members.size(), 3u);

	const auto& Completion = Metadata->Members[0];
	EXPECT_STREQ(Completion.Name, "Completion");
	EXPECT_EQ(Completion.Kind,
		Durin::ERDGParameterMemberKind::ValueWrite);
	EXPECT_EQ(Completion.ArraySize, 1u);
	EXPECT_FALSE(Completion.bOptional);
	EXPECT_EQ(Completion.Use, Durin::ERDGUse::Write);
	EXPECT_TRUE(Completion.bDiscard);

	const auto& Colors = Metadata->Members[1];
	EXPECT_STREQ(Colors.Name, "Colors");
	EXPECT_EQ(Colors.Kind,
		Durin::ERDGParameterMemberKind::ManagedColorAttachment);
	EXPECT_EQ(Colors.ArraySize, 4u);
	EXPECT_TRUE(Colors.bOptional);
	EXPECT_EQ(Colors.Use, Durin::ERDGUse::ReadWrite);
	EXPECT_EQ(Colors.Access, Durin::ERHIAccess::ColorAttachmentReadWrite);
	EXPECT_TRUE(Colors.bDiscard);
	EXPECT_EQ(Colors.LoadAction,
		Durin::ERHIRenderTargetLoadAction::Clear);
	EXPECT_EQ(Colors.StoreAction,
		Durin::ERHIRenderTargetStoreAction::Store);
	EXPECT_TRUE(Colors.bPassManagedTransition);
	EXPECT_EQ(Colors.ResultAccess, Durin::ERHIAccess::GraphicsShaderRead);

	const auto& Depth = Metadata->Members[2];
	EXPECT_STREQ(Depth.Name, "Depth");
	EXPECT_EQ(Depth.Kind,
		Durin::ERDGParameterMemberKind::ManagedDepthStencilAttachment);
	EXPECT_EQ(Depth.ArraySize, 1u);
	EXPECT_TRUE(Depth.bOptional);
	EXPECT_EQ(Depth.Use, Durin::ERDGUse::ReadWrite);
	EXPECT_EQ(Depth.Access, Durin::ERHIAccess::DepthStencilReadWrite);
	EXPECT_TRUE(Depth.bDiscard);
	EXPECT_EQ(Depth.LoadAction,
		Durin::ERHIRenderTargetLoadAction::Clear);
	EXPECT_EQ(Depth.StoreAction,
		Durin::ERHIRenderTargetStoreAction::Store);
	EXPECT_TRUE(Depth.bPassManagedTransition);
	EXPECT_EQ(Depth.ResultAccess, Durin::ERHIAccess::GraphicsShaderRead);
}

TEST(FRendererSceneContractTests, SurfaceMaterialUniformPreservesCanonicalBytes)
{
	Durin::FMaterialRenderBinding Binding;
	Binding.BaseColor = {0.1f, 0.2f, 0.3f, 0.4f};
	Binding.Emissive = {0.5f, 0.6f, 0.7f};
	Binding.Metallic = 0.8f;
	Binding.Normal = {0.9f, 1.0f, 1.1f};
	Binding.Roughness = 1.2f;
	Binding.AmbientOcclusion = 0.35f;
	Binding.OpacityMask = 0.65f;
	for (size_t Role = 0; Role < Binding.Textures.size(); ++Role)
	{
		Binding.UVScales[Role] = {
			static_cast<float>(Role + 1), static_cast<float>(Role + 2)};
		Binding.UVOffsets[Role] = {
			static_cast<float>(Role + 3), static_cast<float>(Role + 4)};
		Binding.UVChannels[Role] = static_cast<float>(Role);
		Binding.UVRotations[Role] = static_cast<float>(Role) * 0.125f;
	}

	Durin::RendererPrivate::FSurfaceMaterialUniform Expected;
	Expected.BaseColor = Binding.BaseColor;
	Expected.EmissiveMetallic = Durin::FVector4f(Binding.Emissive, Binding.Metallic);
	Expected.NormalRoughness = Durin::FVector4f(Binding.Normal, Binding.Roughness);
	Expected.SurfaceParams = Durin::FVector4f(
		Binding.AmbientOcclusion, Binding.OpacityMask, 1.0f, 1.0f);
	for (size_t Role = 0; Role < Binding.Textures.size(); ++Role)
	{
		Expected.UVTransforms[Role] = Durin::FVector4f(
			Binding.UVScales[Role].x, Binding.UVScales[Role].y,
			Binding.UVOffsets[Role].x, Binding.UVOffsets[Role].y);
	}
	Expected.UVChannels0 = Durin::FVector4f(
		Binding.UVChannels[0], Binding.UVChannels[1],
		Binding.UVChannels[2], Binding.UVChannels[3]);
	Expected.UVChannels1 = Durin::FVector4f(
		Binding.UVChannels[4], Binding.UVChannels[5],
		Binding.UVChannels[6], Binding.UVChannels[7]);
	Expected.UVRotations0 = Durin::FVector4f(
		Binding.UVRotations[0], Binding.UVRotations[1],
		Binding.UVRotations[2], Binding.UVRotations[3]);
	Expected.UVRotations1 = Durin::FVector4f(
		Binding.UVRotations[4], Binding.UVRotations[5],
		Binding.UVRotations[6], Binding.UVRotations[7]);

	const auto Lit = Durin::RendererPrivate::MakeSurfaceMaterialUniform(
		Binding, true, true);
	EXPECT_EQ(std::memcmp(&Lit, &Expected, sizeof(Expected)), 0);
	const auto LitWithoutSpecularAA =
		Durin::RendererPrivate::MakeSurfaceMaterialUniform(
			Binding, true, false);
	Expected.SurfaceParams.w = 0.0f;
	EXPECT_EQ(
		std::memcmp(&LitWithoutSpecularAA, &Expected, sizeof(Expected)), 0);
	const auto Unlit = Durin::RendererPrivate::MakeSurfaceMaterialUniform(
		Binding, false, true);
	Expected.SurfaceParams.z = 0.0f;
	EXPECT_EQ(std::memcmp(&Unlit, &Expected, sizeof(Expected)), 0);
}

TEST(FRendererSceneContractTests, SurfaceMaterialFallbacksAndPassRolesAreCanonical)
{
	using namespace Durin::RendererPrivate;
	const std::array ExpectedFallbacks{
		Durin::EDefaultTexture::White,
		Durin::EDefaultTexture::FlatNormal,
		Durin::EDefaultTexture::White,
		Durin::EDefaultTexture::White,
		Durin::EDefaultTexture::White,
		Durin::EDefaultTexture::Black,
		Durin::EDefaultTexture::White,
		Durin::EDefaultTexture::White};
	EXPECT_EQ(SurfaceTextureFallbacks, ExpectedFallbacks);
	EXPECT_EQ(GetSurfaceMaterialRequiredRoleMask(
		ESurfaceMaterialPass::OpaqueShadow), 0u);
	EXPECT_EQ(GetSurfaceMaterialRequiredRoleMask(
		ESurfaceMaterialPass::MaskedShadow), 0x80u);
	EXPECT_EQ(GetSurfaceMaterialRequiredRoleMask(
		ESurfaceMaterialPass::Forward), 0xffu);
	EXPECT_EQ(GetSurfaceMaterialRequiredRoleMask(
		ESurfaceMaterialPass::GBuffer), 0xffu);
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
				ObserveReleasedViewState);
		}
		auto InvalidateViewState(Durin::FSceneViewStateId) -> void override {}
		auto InvalidateAllViewStates() -> void override {}
		auto RenderView(
			Durin::FRHICommandListImmediate&,
			Durin::IScene*,
			const Durin::FSceneView&,
			Durin::FRHITexture*,
			bool,
			const Durin::FSceneViewRenderOptions&,
			Durin::FSceneViewStatistics*,
			Durin::FRDGCapture*) -> Durin::ERenderViewResult override
		{
			return Durin::ERenderViewResult::RendererResourcesUnavailable;
		}

		uint64 NextId = 1000;
		uint32 CreateCount = 0;
	};

	std::vector<Durin::FViewRenderTelemetry>* GObservedViewTelemetrySnapshots = nullptr;
	std::vector<Durin::FRDGCapture>* GObservedRenderGraphCaptures = nullptr;

	auto ObserveViewTelemetrySnapshot(
		const Durin::FViewRenderTelemetry& Telemetry) -> void
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
			});
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
			std::shared_ptr<std::atomic<bool>> InDestroyedOnRenderingThread)
			: FStaticMeshSceneProxy(RenderData, {}, 0)
			, Destroyed(std::move(InDestroyed))
			, DestroyedOnRenderingThread(
				std::move(InDestroyedOnRenderingThread))
		{
		}

		~FTrackedStaticMeshSceneProxy() override
		{
			DestroyedOnRenderingThread->store(
				Durin::IsInRenderingThread(), std::memory_order_release);
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
}

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
		});
	{
		std::unique_lock Lock(BlockedRenderCommand->Mutex);
		BlockedRenderCommand->CV.wait(Lock, [&] {
			return BlockedRenderCommand->bStarted;
		});
	}

	Durin::FRendererModule RendererModule;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-1.0), Durin::FVector3(1.0));
	auto Destroyed = std::make_shared<std::atomic<bool>>(false);
	auto DestroyedOnRenderingThread =
		std::make_shared<std::atomic<bool>>(false);
	auto ObservedQueuedMutation = std::make_shared<std::atomic<bool>>(false);

	Durin::FScenePtr Scene = RendererModule.CreateScene();
	auto* ConcreteScene = static_cast<Durin::FScene*>(Scene.get());
	ConcreteScene->AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(1),
		std::make_unique<FTrackedStaticMeshSceneProxy>(
			&RenderData, Destroyed, DestroyedOnRenderingThread),
		Durin::FMatrix(1.0));
	struct FObserveQueuedSceneMutationCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveQueuedSceneMutation";
		}
	};
	Durin::EnqueueRenderCommand<FObserveQueuedSceneMutationCommand>(
		[ConcreteScene, ObservedQueuedMutation](
			Durin::FRHICommandListImmediate&) {
			ObservedQueuedMutation->store(
				ConcreteScene->GetPrimitiveSceneInfos().size() == 1,
				std::memory_order_release);
		});

	Scene.reset();
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
		DestroyedOnRenderingThread->load(std::memory_order_acquire));
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
			FirstId, ObserveReleasedViewState);
	Durin::FSceneViewStateOwner Second =
		Durin::FSceneViewStateOwnerTestAccess::Make(
			SecondId, ObserveReleasedViewState);
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

TEST(FRendererSceneContractTests,
	ViewStateIdsSurviveRendererRestartWithoutReuseAndLeakedOwnersBecomeInert)
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

TEST(FRendererSceneContractTests,
	ViewStateTransactionsPreserveLastSuccessfulFittedMetadata)
{
	FRenderingThreadScope RenderingThread;
	auto Result = std::make_shared<std::vector<Durin::FSceneViewTemporalContext>>();
	auto ProbeRevisions = std::make_shared<std::vector<uint64>>();
	struct FExerciseViewStateTransactionsCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ExerciseViewStateTransactions";
		}
	};
	Durin::EnqueueRenderCommand<FExerciseViewStateTransactionsCommand>(
		[Result, ProbeRevisions](Durin::FRHICommandListImmediate&) {
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
					View, nullptr, 640, 360);
			Result->push_back(State->Begin(First, 1, false));
			State->GetHistoryProbe().PendingRevision = 11;
			State->Commit();
			ProbeRevisions->push_back(
				State->GetHistoryProbe().CommittedRevision);

			Durin::FSceneViewTemporalMetadata Moved = First;
			Moved.ViewLocation = {5.0, 6.0, 7.0};
			Result->push_back(State->Begin(Moved, 2, false));
			State->GetHistoryProbe().PendingRevision = 22;
			State->Abort();
			ProbeRevisions->push_back(
				State->GetHistoryProbe().CommittedRevision);

			Result->push_back(State->Begin(Moved, 3, false));
			State->GetHistoryProbe().PendingRevision = 33;
			State->Commit();
			ProbeRevisions->push_back(
				State->GetHistoryProbe().CommittedRevision);

			(void)State->Begin(Moved, 4, false);
			State->Commit();
			ProbeRevisions->push_back(
				State->GetHistoryProbe().CommittedRevision);

			Durin::FSceneViewTemporalMetadata ProjectionChanged = Moved;
			ProjectionChanged.ProjectionMatrix[0][0] = 2.0;
			Result->push_back(State->Begin(ProjectionChanged, 5, false));
			State->Abort();

			State->Invalidate(
				Durin::ESceneViewDiscontinuity::ManualInvalidation);
			Result->push_back(State->Begin(Moved, 6, false));
			State->Abort();

			Result->push_back(State->Begin(Moved, 200, true));
			State->Abort();

			EXPECT_TRUE(Registry.Remove(SecondId));
			EXPECT_FALSE(Registry.Remove(SecondId));
			EXPECT_EQ(Registry.ReleaseAll(), 1u);
		});
	Durin::FlushRenderingCommands();

	ASSERT_EQ(Result->size(), 6u);
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[0].Discontinuities,
		Durin::ESceneViewDiscontinuity::FirstUse));
	EXPECT_FALSE((*Result)[0].bHistoryValid);
	EXPECT_TRUE((*Result)[1].bHistoryValid);
	EXPECT_EQ((*Result)[1].SuccessfulSequence, 1u);
	EXPECT_EQ((*Result)[1].PreviousSubmissionSerial, 1u);
	EXPECT_EQ((*Result)[1].Previous.ViewLocation, Durin::FVector3(0.0));
	EXPECT_TRUE((*Result)[2].bHistoryValid);
	EXPECT_EQ((*Result)[2].PreviousSubmissionSerial, 1u);
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[3].Discontinuities,
		Durin::ESceneViewDiscontinuity::ProjectionChange));
	EXPECT_FALSE((*Result)[3].bHistoryValid);
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[4].Discontinuities,
		Durin::ESceneViewDiscontinuity::ManualInvalidation));
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[5].Discontinuities,
		Durin::ESceneViewDiscontinuity::ExplicitCameraCut));
	EXPECT_TRUE(Durin::HasAnyViewDiscontinuity(
		(*Result)[5].Discontinuities,
		Durin::ESceneViewDiscontinuity::InactiveGapExpiry));
	ASSERT_EQ(ProbeRevisions->size(), 4u);
	EXPECT_EQ((*ProbeRevisions)[0], 11u);
	EXPECT_EQ((*ProbeRevisions)[1], 11u);
	EXPECT_EQ((*ProbeRevisions)[2], 33u);
	EXPECT_EQ((*ProbeRevisions)[3], 33u);
}

TEST(FRendererSceneContractTests,
	VolumetricCloudHistoryPolicyFollowsOuterViewTransactions)
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
				View, nullptr, 640, 360);
			(void)State.Begin(Metadata, 1, false);
			State.GetVolumetricCloudHistory().SetPendingClear(11, 101);
			State.Commit();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);

			(void)State.Begin(Metadata, 2, false);
			State.GetVolumetricCloudHistory().SetPendingClear(22, 202);
			State.Abort();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			(void)State.Begin(Metadata, 3, false);
			State.GetVolumetricCloudHistory().SetPendingClear(33, 303);
			State.Commit();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			Durin::FSceneViewTemporalMetadata Resized = Metadata;
			Resized.OutputWidth = 1280;
			const auto ResizedContext = State.Begin(Resized, 4, false);
			EXPECT_FALSE(ResizedContext.bHistoryValid);
			State.Abort();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			(void)State.Begin(Resized, 5, false);
			State.Commit();
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedCloudKey);

			State.Invalidate(Durin::ESceneViewDiscontinuity::ManualInvalidation);
			Observed->push_back(State.GetVolumetricCloudHistory().CommittedPolicyKey);
			EXPECT_EQ(State.GetVolumetricCloudHistory().GetRetainedBytes(), 0u);
		});
	Durin::FlushRenderingCommands();

	EXPECT_EQ(*Observed, (std::vector<uint64>{
		11, 11, 101, 33, 303, 33, 303, 0, 0, 0}));
}

TEST(FRendererSceneContractTests,
	SceneViewportsOwnIsolatedOptInStateAndConsumeCutsOnce)
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
					View, nullptr, 320, 180);
			(void)State.Begin(Baseline, 1, false);
			State.Commit();

			auto ObserveAbort = [&](Durin::FSceneViewTemporalMetadata Candidate,
				uint64 Serial) {
				Contexts->push_back(State.Begin(Candidate, Serial, false));
				State.Abort();
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
				Durin::ESceneViewDiscontinuity::DeviceInvalidation);
			ObserveAbort(Baseline, 6);
			// Abort must not consume a hard invalidation.
			ObserveAbort(Baseline, 7);
			// Non-monotonic serial input is treated as an expired gap, never wrapped.
			ObserveAbort(Baseline, 0);
		});
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
			(*Contexts)[Index].Discontinuities, Causes[Index]));
		EXPECT_FALSE((*Contexts)[Index].bHistoryValid);
	}
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
	GObservedRenderGraphCaptures = &Captures;
	Durin::SetSceneRenderGraphCaptureSink(ObserveRenderGraphCapture);
	{
		Durin::FRDGBuilder Builder;
		Builder.EnablePassCulling();
		const auto Output = Builder.CreateToken("Scene.Output");
		auto Parameters = Builder.AllocParameters<FInspectionOutputParameters>();
		Parameters->Output = {Output};
		const auto Final = Builder.AddPass(
			"Scene.FinalOutput", Durin::ERDGPassType::Graphics,
			std::move(Parameters));
		Builder.MarkPassRoot(Final, "offscreen-output");
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		Durin::PublishSceneRenderGraphCapture(*Result.Graph);
	}
	Durin::SetSceneRenderGraphCaptureSink(nullptr);
	GObservedRenderGraphCaptures = nullptr;
	ASSERT_EQ(Captures.size(), 1u);
	ASSERT_EQ(Captures[0].Passes.size(), 1u);
	EXPECT_EQ(Captures[0].Passes[0].Name, "Scene.FinalOutput");
	EXPECT_EQ(Captures[0].Passes[0].ParameterStructName,
		"FInspectionOutputParameters");
	ASSERT_EQ(Captures[0].Parameters.size(), 1u);
	EXPECT_EQ(Captures[0].Parameters[0].FieldPath,
		"FInspectionOutputParameters.Output");
	EXPECT_EQ(Captures[0].CullingDecisions[0].Reason, "offscreen-output");
	EXPECT_NE(Captures[0].Dump.find("Scene.FinalOutput"), std::string::npos);
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
	EXPECT_EQ(Settings.DirectionalShadow.FilterQuality,
		Durin::EDirectionalShadowFilterQuality::Medium);
	EXPECT_EQ(Settings.DirectionalShadow.ContactRoutePreference,
		Durin::EContactShadowRoutePreference::Auto);
	EXPECT_TRUE(Settings.AmbientOcclusion.bEnabled);
	EXPECT_EQ(Settings.AmbientOcclusion.Quality,
		Durin::EGroundTruthAmbientOcclusionQuality::HalfResolution);
	EXPECT_EQ(Settings.VolumetricCloud.Quality,
		Durin::EVolumetricCloudQuality::High);
	EXPECT_EQ(Settings.VolumetricCloud.DebugMode,
		Durin::EVolumetricCloudDebugMode::Lit);
}

TEST(FRendererSceneContractTests, ViewRenderOptionsDefaultToNoEnvironmentOverride)
{
	const Durin::FSceneViewRenderOptions Options;
	EXPECT_FALSE(Options.Environment.has_value());

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
	EXPECT_EQ(Statistics.Shadow.ContactRoute,
		Durin::EContactShadowExecutionRoute::None);
	EXPECT_EQ(Statistics.VolumetricCloud.Quality,
		Durin::EVolumetricCloudQuality::High);
	EXPECT_EQ(Statistics.VolumetricCloud.Route,
		Durin::EVolumetricCloudExecutionRoute::None);
}

TEST(FRendererSceneContractTests, ViewStatisticsPreserveStableMetricSemantics)
{
	Durin::FViewRenderTelemetry Telemetry;
	Telemetry.Visibility.SubmittedPrimitives = 13;
	Telemetry.Visibility.VisiblePrimitives = 8;
	Telemetry.StaticMesh.PreparedStaticMeshPrimitives = 4;
	Telemetry.SplineMesh.PreparedSplineMeshPrimitives = 1;
	Telemetry.SkeletalMesh.PreparedSkeletalMeshPrimitives = 2;
	Telemetry.Terrain.VisibleTerrainPatches = 3;
	Telemetry.StaticMesh.PreparedStaticMeshTriangles = 120;
	Telemetry.SplineMesh.PreparedSplineMeshTriangles = 20;
	Telemetry.SkeletalMesh.PreparedSkeletalMeshTriangles = 40;
	Telemetry.Terrain.PreparedTerrainTriangles = 60;
	Telemetry.DirectionalShadow.ShadowPreparedTriangles = 500;
	Telemetry.StaticMesh.StaticMeshSuccessfulDraws = 5;
	Telemetry.SkeletalMesh.SkeletalMeshSuccessfulDraws = 2;
	Telemetry.Terrain.TerrainSuccessfulDraws = 1;
	Telemetry.GBuffer.GBufferStaticMeshSuccessfulDraws = 3;
	Telemetry.GBuffer.GBufferSplineMeshSuccessfulDraws = 1;
	Telemetry.GBuffer.GBufferSkeletalMeshSuccessfulDraws = 2;
	Telemetry.GBuffer.GBufferTerrainSuccessfulDraws = 4;
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
	Telemetry.VolumetricCloud.VolumetricCloudRouteReasons[
		static_cast<size_t>(Durin::FVolumetricCloudSpatialRenderer::ERouteReason::Compute)] = 1;

	const Durin::FSceneViewStatistics Statistics =
		Durin::BuildSceneViewStatistics(Telemetry);
	EXPECT_EQ(Statistics.Visibility.SubmittedPrimitives, 13u);
	EXPECT_EQ(Statistics.Visibility.VisiblePrimitives, 8u);
	EXPECT_EQ(Statistics.StaticMesh.Primitives, 4u);
	EXPECT_EQ(Statistics.SplineMesh.Primitives, 1u);
	EXPECT_EQ(Statistics.SkeletalMesh.Primitives, 2u);
	EXPECT_EQ(Statistics.Terrain.VisiblePatches, 3u);
	EXPECT_EQ(Statistics.StaticMesh.Triangles, 100u);
	EXPECT_EQ(Statistics.SplineMesh.Triangles, 20u);
	EXPECT_EQ(Statistics.SkeletalMesh.Triangles, 40u);
	EXPECT_EQ(Statistics.Terrain.Triangles, 60u);
	EXPECT_EQ(Statistics.Summary.Triangles, 220u);
	EXPECT_EQ(Statistics.Shadow.Triangles, 500u);
	EXPECT_EQ(Statistics.StaticMesh.DrawCalls, 9u);
	EXPECT_EQ(Statistics.SkeletalMesh.DrawCalls, 4u);
	EXPECT_EQ(Statistics.Terrain.DrawCalls, 5u);
	EXPECT_EQ(Statistics.Shadow.DrawCalls, 7u);
	EXPECT_TRUE(Statistics.Shadow.bEnabled);
	EXPECT_EQ(Statistics.Shadow.Cascades, 3u);
	EXPECT_TRUE(Statistics.Shadow.bContactEnabled);
	EXPECT_EQ(Statistics.Shadow.ContactRoute,
		Durin::EContactShadowExecutionRoute::Compute);
	EXPECT_EQ(Statistics.Lights.Directional, 1u);
	EXPECT_EQ(Statistics.Lights.Point, 3u);
	EXPECT_EQ(Statistics.Lights.Spot, 2u);
	EXPECT_EQ(Statistics.VolumetricCloud.Quality,
		Durin::EVolumetricCloudQuality::Epic);
	EXPECT_EQ(Statistics.VolumetricCloud.DebugMode,
		Durin::EVolumetricCloudDebugMode::Transmittance);
	EXPECT_EQ(Statistics.VolumetricCloud.Route,
		Durin::EVolumetricCloudExecutionRoute::Compute);
	EXPECT_EQ(Statistics.VolumetricCloud.Reason,
		Durin::EVolumetricCloudRouteReason::Compute);
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
	EXPECT_EQ(FragmentStatistics.Shadow.ContactRoute,
		Durin::EContactShadowExecutionRoute::Fragment);
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
			Durin::FRHICommandListImmediate&) {
			Main->PublishRenderStatistics_RenderThread(MainStatistics, true);
			Auxiliary->PublishRenderStatistics_RenderThread(
				AuxiliaryStatistics, true);
		});
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
		});
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
		});
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
		});
	Viewport.reset();
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(WeakViewport.expired());
}

TEST(FRendererSceneContractTests,
	SceneViewportRenderGraphCaptureIsExplicitAndIsolated)
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
		});
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
		});
	Durin::FlushRenderingCommands();
	const Durin::FSceneViewportRenderGraphSnapshot Failed =
		Main->GetRenderGraphSnapshot();
	EXPECT_FALSE(Failed.bAvailable);
	EXPECT_EQ(Failed.Revision, 2u);
	EXPECT_EQ(Failed.Capture, nullptr);
	EXPECT_EQ(MainSnapshot.Capture->Dump, "main graph");
}

TEST(FRendererSceneContractTests,
	DirectionalShadowCandidatesStartFromSceneAndKeepRelevantOffCameraCasters)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.25), Durin::FVector3(0.25));
	auto Add = [&](uint64 Id, const Durin::FVector3& Position) {
		Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(Id),
			std::make_unique<Durin::FStaticMeshSceneProxy>(
				&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
			Durin::Math::TranslationMatrix(Position));
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
	ASSERT_EQ(Camera.StaticMeshSceneInfos.size(), 1u);
	EXPECT_EQ(Camera.StaticMeshSceneInfos.front()->GetId().Value, 1u);

	Durin::FDirectionalLightSceneData Light;
	Light.Direction = {0.0, 0.0, -1.0};
	Light.Intensity = 1.0f;
	Durin::FPreparedDirectionalShadowView Shadow;
	ASSERT_TRUE(Durin::TryPrepareDirectionalShadowView(
		View, Durin::FLightSceneId(8), Light, Shadow));
	const Durin::FDirectionalShadowCasterTable Table =
		Durin::PrepareDirectionalShadowCasterTable(Scene, Shadow);
	const Durin::FDirectionalShadowCasterCandidates& Casters = Table.Cascades[0];
	ASSERT_EQ(Casters.StaticMeshes.size(), 2u);
	EXPECT_EQ(Casters.StaticMeshes[0]->GetId().Value, 1u);
	EXPECT_EQ(Casters.StaticMeshes[1]->GetId().Value, 2u);
	EXPECT_EQ(Casters.Culled, 1u);
	const Durin::FDirectionalShadowCasterTable ComparisonTable =
		Durin::PrepareDirectionalShadowCasterTable(Scene, Shadow, true);
	const Durin::FDirectionalShadowCasterCandidates& Comparison =
		ComparisonTable.Cascades[0];
	EXPECT_EQ(Comparison.StaticMeshes.size(), 3u);
	EXPECT_EQ(Table.SceneTraversals, 1u);
	EXPECT_EQ(Table.UniqueSubmitted, 3u);
	EXPECT_EQ(Table.CascadeClassificationTests, 3u);
	EXPECT_EQ(Table.MembershipPopcount, 2u);
}

TEST(FRendererSceneContractTests,
	DirectionalShadowCasterTableBuildsZeroThroughAllCascadeMasksOnce)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.05), Durin::FVector3(0.05));
	for (const auto [Id, X] : std::array{
		std::pair<uint64, double>{1u, 0.0},
		std::pair<uint64, double>{2u, 1.5},
		std::pair<uint64, double>{3u, 2.5},
		std::pair<uint64, double>{4u, 10.0}})
	{
		Scene.AddOrReplacePrimitive(Durin::FPrimitiveSceneId(Id),
			std::make_unique<Durin::FStaticMeshSceneProxy>(
				&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
			Durin::Math::TranslationMatrix(Durin::FVector3(X, 0.0, 0.0)));
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
	EXPECT_EQ(Table.Cascades[0].StaticMeshes.size(), 1u);
	EXPECT_EQ(Table.Cascades[1].StaticMeshes.size(), 2u);
	EXPECT_EQ(Table.Cascades[2].StaticMeshes.size(), 3u);
}

TEST(FRendererSceneContractTests, PrimitiveMembershipOwnsClassificationBoundsAndFifoLifetime)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-1.0, -2.0, -3.0),
		Durin::FVector3(1.0, 2.0, 3.0));
	const Durin::FPrimitiveSceneId Id(41);
	const Durin::FMatrix InitialTransform = Durin::Math::TranslationMatrix(
		Durin::FVector3(10.0, 20.0, 30.0));

	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
		InitialTransform);
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);
	ASSERT_EQ(Scene.GetStaticMeshSceneInfos().size(), 1u);
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetStaticMeshSceneInfos().front();
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
		Durin::Math::ScaleMatrix(Durin::FVector3(-2.0, 3.0, 0.5)));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Info->GetWorldBounds().Min, Durin::FVector3(-2.0, -6.0, -1.5));
	EXPECT_EQ(Info->GetWorldBounds().Max, Durin::FVector3(2.0, 6.0, 1.5));

	Scene.AddOrReplacePrimitive(Id, nullptr, Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetPrimitiveSceneInfos().size(), 1u);

	Scene.RemovePrimitive(Id);
	Scene.UpdatePrimitiveTransform(Id, Durin::FMatrix(2.0));
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPrimitiveSceneInfos().empty());

	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
		Durin::FMatrix(1.0));
	Durin::FlushRenderingCommands();
	EXPECT_EQ(Scene.GetStaticMeshSceneInfos().size(), 1u);

}

TEST(FRendererSceneContractTests, VisibilityClassifiesOnceAndKeepsFallbacksVisible)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData ValidRenderData;
	ValidRenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.5), Durin::FVector3(0.5));
	Durin::FStaticMeshRenderData InvalidBoundsRenderData;

	auto AddStaticMesh = [&](uint64 Id, const Durin::FVector3& Location,
		bool bVisible, const Durin::FStaticMeshRenderData* RenderData) {
		Scene.AddOrReplacePrimitive(
			Durin::FPrimitiveSceneId(Id),
			std::make_unique<Durin::FStaticMeshSceneProxy>(
				RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
			Durin::Math::TranslationMatrix(Location),
			bVisible);
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
	const Durin::FSceneVisibilityResult Visibility =
		Durin::PrepareSceneVisibility(Scene, View, Telemetry);
	EXPECT_EQ(Visibility.PrimitiveRecords.size(), 4u);
	EXPECT_EQ(Telemetry.Visibility.SubmittedPrimitives, 4u);
	EXPECT_EQ(Telemetry.Visibility.HiddenPrimitives, 1u);
	EXPECT_EQ(Telemetry.Visibility.FrustumCulledPrimitives, 1u);
	EXPECT_EQ(Telemetry.Visibility.VisiblePrimitives, 2u);
	EXPECT_EQ(Telemetry.Visibility.InvalidBoundsFallbacks, 1u);
	EXPECT_EQ(Telemetry.Visibility.InvalidViewFallbacks, 0u);
	EXPECT_EQ(Telemetry.Lighting.SelectedPointLights, 7u);
	EXPECT_EQ(Telemetry.VolumetricCloud.VolumetricCloudQuality,
		Durin::EVolumetricCloudQuality::Epic);
	EXPECT_EQ(Visibility.StaticMeshSceneInfos.size(), 2u);

	View.Settings.Mode.VisibilityMode =
		Durin::EViewVisibilityMode::FrustumCullingDisabled;
	Durin::FViewRenderTelemetry DisabledTelemetry;
	const Durin::FSceneVisibilityResult Disabled =
		Durin::PrepareSceneVisibility(Scene, View, DisabledTelemetry);
	EXPECT_EQ(DisabledTelemetry.Visibility.HiddenPrimitives, 1u);
	EXPECT_EQ(DisabledTelemetry.Visibility.FrustumCulledPrimitives, 0u);
	EXPECT_EQ(DisabledTelemetry.Visibility.VisiblePrimitives, 3u);
	EXPECT_EQ(Disabled.StaticMeshSceneInfos.size(), 3u);

	View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::Normal;
	View.ProjectionMatrix[0][0] =
		std::numeric_limits<double>::quiet_NaN();
	Durin::FViewRenderTelemetry InvalidViewTelemetry;
	const Durin::FSceneVisibilityResult InvalidView =
		Durin::PrepareSceneVisibility(Scene, View, InvalidViewTelemetry);
	EXPECT_EQ(InvalidViewTelemetry.Visibility.HiddenPrimitives, 1u);
	EXPECT_EQ(InvalidViewTelemetry.Visibility.VisiblePrimitives, 3u);
	EXPECT_EQ(InvalidViewTelemetry.Visibility.InvalidViewFallbacks, 3u);
	EXPECT_EQ(InvalidView.StaticMeshSceneInfos.size(), 3u);

}

TEST(FRendererSceneContractTests, VisibilityPolicyAndSequentialViewsAreIndependent)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	RenderData.LocalBounds = Durin::FBox(
		Durin::FVector3(-0.5), Durin::FVector3(0.5));
	Scene.AddOrReplacePrimitive(
		Durin::FPrimitiveSceneId(9),
		std::make_unique<Durin::FStaticMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 0),
		Durin::Math::TranslationMatrix(Durin::FVector3(3.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();

	Durin::FSceneView MainView;
	MainView.ProjectionMatrix = MakePerspectiveProjection();
	MainView.ViewProjectionMatrix = MainView.ProjectionMatrix;
	Durin::FSceneView AuxiliaryView = MainView;
	AuxiliaryView.ViewProjectionMatrix = AuxiliaryView.ProjectionMatrix
		* Durin::Math::TranslationMatrix(Durin::FVector3(0.0, -20.0, 0.0));

	Durin::FViewRenderTelemetry MainTelemetry;
	Durin::FViewRenderTelemetry AuxiliaryTelemetry;
	EXPECT_EQ(
		Durin::PrepareSceneVisibility(Scene, MainView, MainTelemetry)
			.StaticMeshSceneInfos.size(),
		1u);
	EXPECT_TRUE(
		Durin::PrepareSceneVisibility(Scene, AuxiliaryView, AuxiliaryTelemetry)
			.StaticMeshSceneInfos.empty());
	Durin::FViewRenderTelemetry RepeatedMainTelemetry;
	EXPECT_EQ(
		Durin::PrepareSceneVisibility(Scene, MainView, RepeatedMainTelemetry)
			.StaticMeshSceneInfos.size(),
		1u);

	Durin::FSceneView SubmittedView = MainView;
	SubmittedView.Settings.Mode.VisibilityMode =
		Durin::EViewVisibilityMode::FrustumCullingDisabled;
	auto ObservedMode = std::make_shared<Durin::EViewVisibilityMode>(
		Durin::EViewVisibilityMode::Normal);
	struct FObserveVisibilityPolicyCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "ObserveVisibilityPolicy";
		}
	};
	Durin::EnqueueRenderCommand<FObserveVisibilityPolicyCommand>(
		[ViewSnapshot = SubmittedView, ObservedMode](
			Durin::FRHICommandListImmediate&) {
			*ObservedMode = ViewSnapshot.Settings.Mode.VisibilityMode;
		});
	SubmittedView.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::Normal;
	Durin::FlushRenderingCommands();
	EXPECT_EQ(
		*ObservedMode,
		Durin::EViewVisibilityMode::FrustumCullingDisabled);

}

TEST(FRendererSceneContractTests, DirectionalLightProxyOutlivesPublisherAndUsesFifoReplacement)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	const Durin::FLightSceneId Id(7);
	{
		Durin::FDirectionalLightSceneData Data;
		Data.Intensity = 2.0f;
		Scene.AddOrReplaceLight(Id,
			std::make_unique<Durin::FDirectionalLightSceneProxy>(Data));
	}
	Durin::FlushRenderingCommands();
	FObservedLight Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 2.0f);

	Durin::FDirectionalLightSceneData Replacement;
	Replacement.Intensity = 5.0f;
	Scene.AddOrReplaceLight(Id,
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Replacement));
	Scene.RemoveLight(Id);
	Durin::FlushRenderingCommands();
	EXPECT_FALSE(ObserveLight(Scene).bPresent);

	Scene.AddOrReplaceLight(Id,
		std::make_unique<Durin::FDirectionalLightSceneProxy>(Replacement));
	Durin::FlushRenderingCommands();
	Observed = ObserveLight(Scene);
	ASSERT_TRUE(Observed.bPresent);
	EXPECT_EQ(Observed.Data.Intensity, 5.0f);
}

TEST(FRendererSceneContractTests, LightFamiliesReplaceTypedMembershipAtomically)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FPointLightSceneData Point;
	Point.Intensity = 1.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(77),
		std::make_unique<Durin::FPointLightSceneProxy>(Point));
	Durin::FSpotLightSceneData Spot;
	Spot.Intensity = 1.0f;
	Scene.AddOrReplaceLight(Durin::FLightSceneId(77),
		std::make_unique<Durin::FSpotLightSceneProxy>(Spot));
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Scene.GetPointLightSceneInfos().empty());
	ASSERT_EQ(Scene.GetSpotLightSceneInfos().size(), 1u);
	EXPECT_EQ(Scene.GetSpotLightSceneInfos().front()->GetId().Value, 77u);
}

TEST(FRendererSceneContractTests, PreparedLightsUseStableIdAndSharedLocalBudget)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	for (uint64 Id : {101u, 100u})
	{
		Durin::FDirectionalLightSceneData Data;
		Data.Intensity = 1.0f;
		Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
			std::make_unique<Durin::FDirectionalLightSceneProxy>(Data));
	}
	for (uint64 Id = 10; Id > 0; --Id)
	{
		if ((Id & 1u) == 0)
		{
			Durin::FPointLightSceneData Data;
			Data.Intensity = 1.0f;
			Data.Range = 5.0f;
			Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
				std::make_unique<Durin::FPointLightSceneProxy>(Data));
		}
		else
		{
			Durin::FSpotLightSceneData Data;
			Data.Intensity = 1.0f;
			Data.Range = 5.0f;
			Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
				std::make_unique<Durin::FSpotLightSceneProxy>(Data));
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
				Scene, View, Observed->Telemetry);
		});
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
	EXPECT_EQ(Observed->Telemetry.Lighting.PackedLightBytes,
		sizeof(Durin::FForwardLightingUniform));
}

TEST(FRendererSceneContractTests, PreparedLightsCullOnlyOutsideLocalInfluenceBounds)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	auto AddPoint = [&](uint64 Id, const Durin::FVector3& Position) {
		Durin::FPointLightSceneData Data;
		Data.Position = Position;
		Data.Intensity = 1.0f;
		Data.Range = 1.0f;
		Scene.AddOrReplaceLight(Durin::FLightSceneId(Id),
			std::make_unique<Durin::FPointLightSceneProxy>(Data));
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
				Scene, View, Observed->second);
		});
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Observed->first.Local.size(), 1u);
	EXPECT_EQ(Observed->first.Local.front().Id.Value, 1u);
	EXPECT_EQ(Observed->second.Lighting.FrustumCulledPointLights, 1u);
}

TEST(FRendererSceneContractTests, SkeletalPoseAndBoundsUpdateAtomicallyInTypedMembership)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FSkeletalMeshRenderData RenderData;
	auto FirstPose = std::make_shared<Durin::FSkeletalPosePalette>();
	FirstPose->Revision = 1;
	FirstPose->Matrices = {Durin::FMatrix4f(1.0f)};
	FirstPose->LocalBounds = Durin::FBox({0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
	const Durin::FPrimitiveSceneId Id(91);
	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FSkeletalMeshSceneProxy>(
			&RenderData, std::vector<Durin::FMaterialRenderProxyRef>{}, 1, FirstPose),
		Durin::Math::TranslationMatrix(Durin::FVector3(2.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetSkeletalMeshSceneInfos().size(), 1u);
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetSkeletalMeshSceneInfos().front();
	EXPECT_EQ(Info->GetSkeletalMeshProxy().GetPose()->Revision, 1u);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 2.0);

	auto SecondPose = std::make_shared<Durin::FSkeletalPosePalette>(*FirstPose);
	SecondPose->Revision = 2;
	SecondPose->LocalBounds = Durin::FBox({-2.0, -1.0, -1.0}, {3.0, 1.0, 1.0});
	Scene.UpdateSkeletalMeshDynamicData(Id, SecondPose);
	Durin::FlushRenderingCommands();
	Info = Scene.GetSkeletalMeshSceneInfos().front();
	EXPECT_EQ(Info->GetSkeletalMeshProxy().GetPose()->Revision, 2u);
	EXPECT_EQ(Info->GetLocalBounds().Min.x, -2.0);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 0.0);
	Durin::FViewRenderTelemetry Telemetry;
	Durin::FSceneView View;
	View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
	const Durin::FSceneVisibilityResult Visibility =
		Durin::PrepareSceneVisibility(Scene, View, Telemetry);
	EXPECT_EQ(Visibility.SkeletalMeshSceneInfos.size(), 1u);
	EXPECT_EQ(Telemetry.SkeletalMesh.VisibleSkeletalMeshCandidates, 1u);
}

TEST(FRendererSceneContractTests, SplineDeformationAndBoundsUpdateAtomicallyInTypedMembership)
{
	FRenderingThreadScope RenderingThread;
	Durin::FScene Scene;
	Durin::FStaticMeshRenderData RenderData;
	Durin::FSplineMeshRenderDynamicData First{
		.Params = {},
		.LocalBounds = Durin::FBox({0.0, -1.0, -1.0}, {10.0, 1.0, 1.0}),
		.Revision = 1};
	const Durin::FPrimitiveSceneId Id(92);
	Scene.AddOrReplacePrimitive(Id,
		std::make_unique<Durin::FSplineMeshSceneProxy>(&RenderData,
			std::vector<Durin::FMaterialRenderProxyRef>{}, 1, First),
		Durin::Math::TranslationMatrix(Durin::FVector3(2.0, 0.0, 0.0)));
	Durin::FlushRenderingCommands();
	ASSERT_EQ(Scene.GetSplineMeshSceneInfos().size(), 1u);
	const Durin::FPrimitiveSceneInfo* Info = Scene.GetSplineMeshSceneInfos().front();
	EXPECT_EQ(Info->GetSplineMeshProxy().GetDynamicData().Revision, 1u);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 2.0);

	auto Second = First;
	Second.Revision = 2;
	Second.Params.EndPosition = {20.0, 10.0, 0.0};
	Second.LocalBounds = Durin::FBox({-2.0, -3.0, -1.0}, {23.0, 13.0, 1.0});
	Scene.UpdateSplineMeshDynamicData(Id, Second);
	Durin::FlushRenderingCommands();
	Info = Scene.GetSplineMeshSceneInfos().front();
	EXPECT_EQ(Info->GetSplineMeshProxy().GetDynamicData().Revision, 2u);
	EXPECT_EQ(Info->GetSplineMeshProxy().GetAcceptedDynamicUpdateCount(), 1u);
	EXPECT_EQ(Info->GetLocalBounds().Min.x, -2.0);
	EXPECT_EQ(Info->GetWorldBounds().Min.x, 0.0);
	auto Stale = Second;
	Stale.Revision = 1;
	Stale.LocalBounds = Durin::FBox(Durin::FVector3(-100.0), Durin::FVector3(100.0));
	Scene.UpdateSplineMeshDynamicData(Id, Stale);
	Durin::FlushRenderingCommands();
	Info = Scene.GetSplineMeshSceneInfos().front();
	EXPECT_EQ(Info->GetSplineMeshProxy().GetDynamicData().Revision, 2u);
	EXPECT_EQ(Info->GetSplineMeshProxy().GetAcceptedDynamicUpdateCount(), 1u);
	EXPECT_EQ(Info->GetLocalBounds().Min.x, -2.0);

	Durin::FViewRenderTelemetry Telemetry;
	Durin::FSceneView View;
	View.Settings.Mode.VisibilityMode = Durin::EViewVisibilityMode::FrustumCullingDisabled;
	const Durin::FSceneVisibilityResult Visibility =
		Durin::PrepareSceneVisibility(Scene, View, Telemetry);
	EXPECT_EQ(Visibility.SplineMeshSceneInfos.size(), 1u);
	EXPECT_EQ(Telemetry.SplineMesh.VisibleSplineMeshCandidates, 1u);
}
