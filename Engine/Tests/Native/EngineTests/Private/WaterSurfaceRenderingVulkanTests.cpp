#include "MaterialRenderProxyUpdateTestSupport.h"
#include "WaterRenderingTestSupport.h"
#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "CoreGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialShaderMap.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/TextureCube.h"
#include "RendererModule.h"
#include "RenderResourcePreparation.h"
#include "Renderers/SceneRendererProfiling.h"
#include "RDG/RDGDiagnostics.h"
#include "SceneViewProjection.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "LightSceneTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/ShaderData.h"
#include <gtest/gtest.h>
#include <glm/gtc/packing.hpp>

using namespace Durin::Tests::Water;

namespace
{
	// Stage 0 water fixture: the UV stream retains the undisplaced XY position.
	// Keep the proof independent of plugin code and renderer extensions.
#if DURIN_WITH_EDITOR
	auto MakeWaterPathInput(bool Displace) -> Durin::MIR::FCompilerInput
	{
		using namespace Durin;
		using Op = EMaterialProgramOpcode;
		using Type = EMaterialProgramValueType;
		MIR::FCompilerInput Input;
		MIR::FGraphBuilder Empty(std::span<DMaterialExpression* const>{});
		Input.IR = Empty.FinishSurface({}).IR;
		Input.StaticProperties.bTwoSided = true;
		auto Add = [&](MIR::FNode Node) {
			std::vector<FMaterialValueSemantics> Arguments;
			for (uint32 Index : Node.Inputs) Arguments.push_back(Input.IR.Nodes[Index].GetSemantics());
			const auto Semantics = ResolveMaterialProgramNodeSemantics(Node.Opcode, Node.ResultType, Arguments);
			check(Semantics.has_value());
			Node.LegalStages = Semantics->Stages;
			Node.SpatialKind = Semantics->Kind;
			Node.CoordinateSpace = Semantics->Space;
			Input.IR.Nodes.push_back(std::move(Node));
			return uint32(Input.IR.Nodes.size() - 1);
		};
		auto Constant = [&](float Value) { return Add({.Opcode = Op::Constant, .Payload = FMaterialProgramLiteral{Value}}); };
		auto Binary = [&](Op Code, uint32 A, uint32 B) { return Add({.Opcode = Code, .Inputs = {A, B}}); };
		const auto Zero = Constant(0), One = Constant(1);
		const auto UV = Add({.Opcode = Op::UVChannel, .ResultType = Type::Float2, .Inputs = {Zero}});
		const auto X = Add({.Opcode = Op::Swizzle, .Inputs = {UV}, .Payload = MIR::FSwizzle{1, {0}}});
		const auto Time = Add({.Opcode = Op::Time});
		const auto Phase = Binary(Op::Subtract, Binary(Op::Multiply, X, Constant(6)), Binary(Op::Multiply, Time, Constant(2)));
		const auto Sine = Add({.Opcode = Op::Sine, .Inputs = {Phase}});
		const auto Cosine = Add({.Opcode = Op::Cosine, .Inputs = {Phase}});
		const auto Offset = Add({.Opcode = Op::MakeFloat3, .ResultType = Type::Float3,
			.Inputs = {Binary(Op::Multiply, Cosine, Constant(.04f)), Zero, Binary(Op::Multiply, Sine, Constant(.08f))}});
		const auto Normal = Add({.Opcode = Op::MakeFloat3, .ResultType = Type::Float3,
			.Inputs = {Binary(Op::Multiply, Cosine, Constant(-.48f)), Zero,
				Binary(Op::Subtract, One, Binary(Op::Multiply, Sine, Constant(.24f)))}});
		const auto UnitNormal = Add({.Opcode = Op::Normalize, .ResultType = Type::Float3, .Inputs = {Normal}});
		const auto NormalXY = Add({.Opcode = Op::Swizzle, .ResultType = Type::Float2,
			.Inputs = {UnitNormal}, .Payload = MIR::FSwizzle{2, {0, 1}}});
		const auto Half = Add({.Opcode = Op::Constant, .ResultType = Type::Float2, .Payload = FMaterialProgramLiteral{.5f, .5f}});
		const auto ScaledNormal = Add({.Opcode = Op::Multiply, .ResultType = Type::Float2, .Inputs = {NormalXY, Half}});
		const auto EncodedNormal = Add({.Opcode = Op::Add, .ResultType = Type::Float2, .Inputs = {ScaledNormal, Half}});
		// DecodeNormalRG gives the analytic upward normal the material terminal's
		// required Tangent/Normal semantics without adding a new expression type.
		const auto SurfaceNormal = Add({.Opcode = Op::DecodeNormalRG, .ResultType = Type::Float3, .Inputs = {EncodedNormal}});
		auto& NormalRoot = Input.IR.GetAttribute(EMaterialProperty::MP_Normal);
		NormalRoot.bExpression = true; NormalRoot.ExpressionIndex = SurfaceNormal;
		Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor).Literal = {.02f, .2f, .3f};
		Input.IR.GetAttribute(EMaterialProperty::MP_Roughness).Literal = {.25f};
		Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression = Displace;
		Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex = Offset;
		return Input;
	}
#endif

	auto MakeWaterPathGrid(std::optional<double> Time = {}) -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		using namespace Durin;
		constexpr uint32 Cells = 32, Side = Cells + 1, Count = Side * Side;
		auto Data = std::make_unique<FStaticMeshRenderData>();
		Data->MaterialSlots.resize(1); Data->LODResources.resize(1); Data->LODVertexFactories.resize(1);
		auto& LOD = Data->LODResources[0];
		std::vector<FVector3f> Positions;
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> UVs;
		for (uint32 Y = 0; Y <= Cells; ++Y) for (uint32 X = 0; X <= Cells; ++X)
		{
			const float PX = -.8f + 1.6f * X / Cells, PY = -.7f + 1.4f * Y / Cells;
			FVector3f Position(PX, PY, .5f);
			if (Time)
			{
				const double Phase = 6 * double(PX) - 2 * *Time;
				Position.x += float(.04 * std::cos(Phase));
				Position.z += float(.08 * std::sin(Phase));
			}
			Positions.push_back(Position);
			UVs[0].emplace_back(PX, PY);
		}
		std::vector<uint32> Indices;
		for (uint32 Y = 0; Y < Cells; ++Y) for (uint32 X = 0; X < Cells; ++X)
		{
			const uint32 A = Y * Side + X, B = A + Side;
			Indices.insert(Indices.end(), {A, A + 1, B + 1, A, B + 1, B});
		}
		LOD.VertexBuffers.PositionVertexBuffer.Init(std::move(Positions));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<FVector3f>(Count, {0, 0, 1}), std::vector<FVector4f>(Count, {1, 0, 0, 1}));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), Count, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(std::vector<FVector4f>(Count, FVector4f(1)), Count);
		LOD.IndexBuffer.Init(std::move(Indices));
		// Include the full horizontal and vertical Gerstner envelope in BOTH section
		// and primitive bounds. RecalculateBounds would discard this authored padding.
		const FBox Bounds({-.84, -.7, .42}, {.84, .7, .58});
		LOD.Sections.push_back({.Name = "Water", .FirstIndex = 0, .IndexCount = Cells * Cells * 6,
			.MinVertexIndex = 0, .MaxVertexIndex = Count - 1, .MaterialSlotIndex = 0, .LocalBounds = Bounds});
		LOD.NumTexCoords = 1; LOD.bHasColorVertexData = true; LOD.LocalBounds = Bounds;
		Data->LocalBounds = Bounds;
		return Data;
	}
}


TEST(FWaterRenderingPathVulkanTests, FiniteGerstnerGridMatchesCPUAndRemainsSelectable)
{
	using namespace Durin;
	using namespace Durin::Tests;
	const char* ProofOverride = std::getenv("DURIN_WATER_PROOF_DIR");
	const std::filesystem::path ProofDirectory = ProofOverride ? std::filesystem::path(ProofOverride)
		: std::filesystem::path(FPaths::RootDir()) / "Build/WaterSurfaceValidation/Proof";
#if !DURIN_WITH_EDITOR
	if (!GetConfiguredShaderTarget()) ASSERT_TRUE(InitializeShaderData(FShaderDataConfiguration::Cooked(ProofDirectory)));
#endif
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("WaterRenderingPath");
	Lifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(Lifecycle);
	{
#if DURIN_WITH_EDITOR
		auto AnimatedInput = MakeWaterPathInput(true), ReferenceInput = MakeWaterPathInput(false);
		ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(AnimatedInput.Environment));
		AnimatedInput.Environment.FeatureLevel = FSceneInterface::GetDefaultFeatureLevel();
		AnimatedInput.Environment.Quality = GetMaterialQualityLevel();
		ReferenceInput.Environment = AnimatedInput.Environment;
		const auto AnimatedResult = MIR::Compile(AnimatedInput);
		const auto ReferenceResult = MIR::Compile(ReferenceInput);
		ASSERT_TRUE(AnimatedResult) << FormatMaterialProgramDiagnostics(AnimatedResult.Diagnostics);
		ASSERT_TRUE(ReferenceResult) << FormatMaterialProgramDiagnostics(ReferenceResult.Diagnostics);
		const auto Animated = std::make_shared<const FMaterialProgram>(AnimatedResult.Resource);
		const auto Reference = std::make_shared<const FMaterialProgram>(ReferenceResult.Resource);
		const auto AnimatedProperties = AnimatedInput.StaticProperties, ReferenceProperties = ReferenceInput.StaticProperties;
		std::filesystem::create_directories(ProofDirectory / "Shaders");
		// Prepare only this proof's resources for the separate runtime-only test.
		// The Game executable neither authors graphs nor calls a source compiler.
		for (auto* Input : {&AnimatedInput, &ReferenceInput})
		{
			Input->Environment.bIncludeEditorPasses = false;
			const auto GameProgram = MIR::Compile(*Input);
			ASSERT_TRUE(GameProgram) << (GameProgram.Diagnostics.empty() ? "no diagnostic" : GameProgram.Diagnostics.front().Message);
			FByteBuffer Bytes;
			const std::array<const FMaterialProgram*, 1> Programs{&GameProgram.Resource};
			ASSERT_TRUE(EncodeMaterialCookedProgramFamily(Programs, Input->StaticProperties,
				ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes));
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, ProofDirectory / (Input == &AnimatedInput ? "Animated.bin" : "Reference.bin")));
		}
		FByteBuffer ShaderLibrary;
		ASSERT_TRUE(BuildCookedShaderLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, ShaderLibrary));
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(ShaderLibrary, ProofDirectory / ShaderCookedLibraryRelativePath));
#else
		std::shared_ptr<const FMaterialProgram> Animated, Reference;
		FMaterialStaticProperties AnimatedProperties, ReferenceProperties;
		const auto AnimatedBytes = FFileHelper::LoadFileToArray(ProofDirectory / "Animated.bin");
		const auto ReferenceBytes = FFileHelper::LoadFileToArray(ProofDirectory / "Reference.bin");
		ASSERT_TRUE(AnimatedBytes) << "Run the Editor water proof to precompile its resources first.";
		ASSERT_TRUE(ReferenceBytes);
		const auto AnimatedDecode = DecodeMaterialCookedProgramFamily(*AnimatedBytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game,
			GetMaterialQualityLevel(), FSceneInterface::GetDefaultFeatureLevel(), {}, AnimatedProperties, Animated);
		ASSERT_TRUE(AnimatedDecode) << AnimatedDecode.error();
		const auto ReferenceDecode = DecodeMaterialCookedProgramFamily(*ReferenceBytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game,
			GetMaterialQualityLevel(), FSceneInterface::GetDefaultFeatureLevel(), {}, ReferenceProperties, Reference);
		ASSERT_TRUE(ReferenceDecode) << ReferenceDecode.error();
#endif
		ASSERT_TRUE(Animated->IsValid());
		ASSERT_TRUE(Reference->IsValid());
		auto AnimatedProxy = MakeRefCount<FMaterialRenderProxy>(), ReferenceProxy = MakeRefCount<FMaterialRenderProxy>();
		Durin::Testing::QueueMaterialTestState(*AnimatedProxy, {.StaticProperties = AnimatedProperties, .CompiledProgram = Animated});
		Durin::Testing::QueueMaterialTestState(*ReferenceProxy, {.StaticProperties = ReferenceProperties, .CompiledProgram = Reference});
		const std::array<double, 3> Times{0., .7, 0.};
		auto Grid = MakeWaterPathGrid();
		std::array<std::unique_ptr<FStaticMeshRenderData>, 3> CPUGrids;
		for (size_t I = 0; I < Times.size(); ++I) CPUGrids[I] = MakeWaterPathGrid(Times[I]);
		const std::shared_ptr<void> ReleaseGrids(nullptr, [&](void*) {
			EnqueueRenderCommand("ReleaseWaterGrids", [&](FRHICommandListImmediate&) {
				Grid->ReleaseResources();
				for (auto& CPUGrid : CPUGrids) CPUGrid->ReleaseResources();
			});
			FlushRenderingCommands();
		});
		EnqueueRenderCommand("InitializeWaterGrid", [&](FRHICommandListImmediate& Commands) {
			ASSERT_TRUE(Grid->InitResources(Commands));
			for (auto& CPUGrid : CPUGrids) ASSERT_TRUE(CPUGrid->InitResources(Commands));
		});
		FlushRenderingCommands();
		FSceneTestOwner Water;
		FSceneInterfaceTestAccess::AddPrimitiveProxy(*Water, FPrimitiveComponentId(740),
			std::make_unique<FStaticMeshSceneProxy>(Grid.get(), std::vector{AnimatedProxy}), FMatrix(1));
		FDirectionalLightSceneData Light; Light.Intensity = 3.f;
		ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(*Water, FLightComponentId(740), Light), nullptr);
		std::array<FByteBuffer, 3> Pixels;
		for (size_t I = 0; I < Times.size(); ++I)
		{
			FSceneTestOwner CPU;
			FSceneInterfaceTestAccess::AddPrimitiveProxy(*CPU, FPrimitiveComponentId(741),
				std::make_unique<FStaticMeshSceneProxy>(CPUGrids[I].get(), std::vector{ReferenceProxy}), FMatrix(1));
			ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(*CPU, FLightComponentId(741), Light), nullptr);
			FSceneView View;
			View.ViewportWidth = 192; View.ViewportHeight = 108;
			View.MaterialTimeSeconds = Times[I];
			View.Settings.Mode.RenderMode = ERenderMode::Lit;
			View.Settings.Mode.VisibilityMode = EViewVisibilityMode::Normal;
			FByteBuffer Expected;
			const auto Target = FRHITextureCreateDesc::Create2D("WaterPath", 192, 108, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
			ASSERT_EQ(RenderPreparedGeometryView(Renderer, *Water, View, Target, {},
				[&](FRHICommandListImmediate& C, FRHITexture* T) { ReadWaterPixels(C, T, Pixels[I]); }), ERenderViewResult::Success);
			ASSERT_EQ(RenderPreparedGeometryView(Renderer, *CPU, View, Target, {},
				[&](FRHICommandListImmediate& C, FRHITexture* T) { ReadWaterPixels(C, T, Expected); }), ERenderViewResult::Success);
			ASSERT_EQ(Pixels[I].size(), 192u * 108u * 4u);
			ASSERT_EQ(Expected.size(), Pixels[I].size());
			size_t Different = 0, Visible = 0;
			for (size_t P = 0; P < Expected.size(); P += 4)
			{
				bool Mismatch = false;
				for (size_t C = 0; C < 3; ++C)
					Mismatch |= std::abs(std::to_integer<int>(Expected[P + C]) - std::to_integer<int>(Pixels[I][P + C])) > 2;
				Different += Mismatch;
				Visible += std::to_integer<int>(Pixels[I][P + 2]) > 16;
			}
			EXPECT_LT(Different, 192u * 108u / 100u) << "CPU/GPU displaced grid differs at time " << Times[I];
			EXPECT_GT(Visible, 4000u);
#if DURIN_WITH_EDITOR
			FHitProxyRenderRequest Selection;
			Selection.View = View;
			Selection.Primitives = {{740, {740}}};
			std::shared_ptr<const FHitProxyMap> Map;
			const auto SelectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			do
			{
				EnqueueRenderCommand("SelectWaterGrid", [&](FRHICommandListImmediate& Commands) {
					FGeometryQualificationFrame Frame(Commands);
					Map = Renderer.RenderHitProxies(Commands, Water.Get(), Selection);
				});
				FlushRenderingCommands();
				if (Map) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < SelectionDeadline);
			ASSERT_TRUE(Map);
			FHitProxyPixel Center;
			ASSERT_TRUE(Map->ReadPixel(96, 54, Center));
			EXPECT_EQ(Center.Id.Value, 740u);
#endif
		}
		EXPECT_NE(Pixels[0], Pixels[1]);
		EXPECT_EQ(Pixels[0], Pixels[2]);
		Water.Reset();
		FlushRenderingCommands();
	}
}

#if DURIN_WITH_EDITOR
TEST(FWaterRenderingPathVulkanTests, SingleLayerWaterSnapshotsAndKeepsNearestDepth)
{
	using namespace Durin;
	using namespace Durin::Tests;
	using P = EMaterialProperty;
	EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("Water"));
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("SingleLayerWater");
	Lifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(Lifecycle);

	FMaterialCompilerEnvironment Environment;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment, VulkanShaderTarget));
	Environment.FeatureLevel = FSceneInterface::GetDefaultFeatureLevel();
	Environment.Quality = GetMaterialQualityLevel();
	const auto Material = [&](FMaterialStaticProperties Properties, FMaterialProgramLiteral Emissive, bool bMaskHole = false) {
		MIR::FCompilerInput Input{.StaticProperties = Properties, .Environment = Environment};
		Input.IR.GetAttribute(P::MP_EmissiveColor).Literal = Emissive;
		if (Properties.ShadingModel == EMaterialShadingModel::SingleLayerWater)
			Input.IR.GetAttribute(P::MP_Opacity).Literal = {0};
		if (bMaskHole)
		{
			using Op = EMaterialProgramOpcode;
			using Type = EMaterialProgramValueType;
			Input.IR.Nodes = {
				{.Opcode = Op::Constant, .ResultType = Type::Float, .Payload = FMaterialProgramLiteral{}},
				{.Opcode = Op::UVChannel, .ResultType = Type::Float2, .Inputs = {0}},
				{.Opcode = Op::Swizzle, .ResultType = Type::Float, .Inputs = {1}, .Payload = MIR::FSwizzle{1, {0}}},
				{.Opcode = Op::Step, .ResultType = Type::Float, .Inputs = {0, 2}},
			};
			for (auto& Node : Input.IR.Nodes)
			{
				std::vector<FMaterialValueSemantics> Operands;
				for (uint32 Link : Node.Inputs) Operands.push_back(Input.IR.Nodes[Link].GetSemantics());
				const auto Semantics = ResolveMaterialProgramNodeSemantics(Node.Opcode, Node.ResultType, Operands);
				check(Semantics.has_value());
				Node.LegalStages = Semantics->Stages;
			}
			auto& Mask = Input.IR.GetAttribute(P::MP_OpacityMask);
			Mask.bExpression = true;
			Mask.ExpressionIndex = 3;
		}
		const auto Compiled = MIR::Compile(Input);
		EXPECT_TRUE(Compiled) << FormatMaterialProgramDiagnostics(Compiled.Diagnostics);
		auto Proxy = MakeRefCount<FMaterialRenderProxy>();
		if (Compiled) Durin::Testing::QueueMaterialTestState(*Proxy, {
			.StaticProperties = Properties, .CompiledProgram = std::make_shared<const FMaterialProgram>(Compiled.Resource)});
		return Proxy;
	};
	const auto Bed = Material({.bTwoSided = true}, {1, 1, 1});
	const auto Post = Material({.bTwoSided = true}, {0, 1, 0});
	const auto Overlay = Material({.BlendMode = EMaterialBlendMode::Translucent,
		.ShadingModel = EMaterialShadingModel::Unlit, .bTwoSided = true}, {1, 0, 0});
	const auto Water = Material({.ShadingModel = EMaterialShadingModel::SingleLayerWater, .bTwoSided = true}, {});
	const auto Hole = Material({.BlendMode = EMaterialBlendMode::Masked,
		.ShadingModel = EMaterialShadingModel::SingleLayerWater, .bTwoSided = true}, {}, true);
	auto AnimatedInput = MakeWaterPathInput(true);
	AnimatedInput.Environment = Environment;
	AnimatedInput.StaticProperties.ShadingModel = EMaterialShadingModel::SingleLayerWater;
	AnimatedInput.IR.GetAttribute(P::MP_Opacity).Literal = {0};
	const auto AnimatedProgram = MIR::Compile(AnimatedInput);
	ASSERT_TRUE(AnimatedProgram) << FormatMaterialProgramDiagnostics(AnimatedProgram.Diagnostics);
	auto Animated = MakeRefCount<FMaterialRenderProxy>();
	Durin::Testing::QueueMaterialTestState(*Animated, {.StaticProperties = AnimatedInput.StaticProperties,
		.CompiledProgram = std::make_shared<const FMaterialProgram>(AnimatedProgram.Resource)});
	ASSERT_FALSE(HasFailure());
	auto Grid = MakeWaterPathGrid();
	auto DisplacedGrid = MakeWaterPathGrid(.7);
	const std::shared_ptr<void> Release(nullptr, [&](void*) {
		EnqueueRenderCommand("ReleaseWaterFixture", [&](FRHICommandListImmediate&) {
			Grid->ReleaseResources();
			DisplacedGrid->ReleaseResources();
		});
		FlushRenderingCommands();
	});
	EnqueueRenderCommand("InitializeWaterFixture", [&](FRHICommandListImmediate& Commands) {
		ASSERT_TRUE(Grid->InitResources(Commands));
		ASSERT_TRUE(DisplacedGrid->InitResources(Commands));
	});
	FlushRenderingCommands();
	ASSERT_FALSE(HasFailure());

	struct FCapture { FRDGCapture Graph; FByteBuffer Pixels; };
	struct FVariant
	{
		bool bOverlay = false;
		bool bFront = false;
		bool bBack = false;
		bool bHole = false;
		bool bSwap = false;
		bool bPost = false;
		bool bMixedDepth = false;
	};
	static FRDGCapture* Sink = nullptr;
	const auto Render = [&](bool bReversed, FVariant Variant, FCapture& Out,
		const FMaterialRenderProxyRef& FrontWater = {}, FStaticMeshRenderData* FrontGrid = nullptr) {
		SCOPED_TRACE(std::format("overlay={} front={} back={} hole={} swap={} post={}", Variant.bOverlay,
			Variant.bFront, Variant.bBack, Variant.bHole, Variant.bSwap, Variant.bPost));
		FSceneTestOwner Scene;
		const auto Add = [&](uint32 Id, double Z, const FMaterialRenderProxyRef& Proxy,
			double ScaleX = 1.0, FStaticMeshRenderData* Mesh = nullptr) {
			FMatrix Transform(1);
			Transform[0][0] = ScaleX;
			Transform[3][2] = Z - .5;
			FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(Id),
				std::make_unique<FStaticMeshSceneProxy>(Mesh ? Mesh : Grid.get(), std::vector{Proxy}), Transform);
		};
		Add(700, .8, Bed);
		if (Variant.bOverlay) Add(701, Variant.bMixedDepth ? .36 : Variant.bPost ? .25 : .5, Overlay);
		if (Variant.bFront)
			Add(Variant.bSwap ? 703 : 702, .35, FrontWater ? FrontWater : (Variant.bHole ? Hole : Water), 1.0, FrontGrid);
		if (Variant.bBack) Add(Variant.bSwap ? 702 : 703, .65, Water);
		if (Variant.bPost) Add(704, Variant.bMixedDepth ? .33 : .15, Post, .18);
		FSceneView View;
		View.ViewportWidth = 86;
		View.ViewportHeight = 64;
		View.AspectRatioConstraint = 2.0;
		View.MaterialTimeSeconds = .7;
		View.Settings.Mode.RenderMode = ERenderMode::Lit;
		View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
		View.Settings.AmbientOcclusion.bEnabled = false;
		if (bReversed)
		{
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ProjectionMatrix[2][2] = -1;
			View.ProjectionMatrix[3][2] = 1;
			View.ViewProjectionMatrix = View.ProjectionMatrix;
		}
		const auto Target = FRHITextureCreateDesc::Create2D("SingleLayerWaterFixture", 86, 64, EPixelFormat::SRGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
		const auto Result = RenderPreparedGeometryView(Renderer, *Scene, View, Target, [&] {
			Sink = &Out.Graph;
			SetSceneRenderGraphCaptureSink(+[](const FRDGCapture& Graph) { *Sink = Graph; });
		}, [&](FRHICommandListImmediate& Commands, FRHITexture* Texture) { ReadWaterPixels(Commands, Texture, Out.Pixels); });
		SetSceneRenderGraphCaptureSink(nullptr);
		Sink = nullptr;
		EXPECT_EQ(Result, ERenderViewResult::Success);
	};
	for (bool bReversed : {false, true})
	{
		SCOPED_TRACE(bReversed);
		FCapture BedOnly, NoWater, Front, Back, HoleAtFront, Swapped, PostOnly, PostAndWater;
		Render(bReversed, {}, BedOnly);
		Render(bReversed, {.bOverlay = true}, NoWater);
		Render(bReversed, {.bOverlay = true, .bFront = true, .bBack = true}, Front);
		Render(bReversed, {.bOverlay = true, .bBack = true}, Back);
		Render(bReversed, {.bOverlay = true, .bFront = true, .bBack = true, .bHole = true}, HoleAtFront);
		Render(bReversed, {.bOverlay = true, .bFront = true, .bBack = true, .bSwap = true}, Swapped);
		Render(bReversed, {.bOverlay = true, .bPost = true}, PostOnly);
		Render(bReversed, {.bOverlay = true, .bFront = true, .bPost = true}, PostAndWater);
		ASSERT_FALSE(HasFailure());
		EXPECT_EQ(Front.Pixels, BedOnly.Pixels);
		EXPECT_EQ(Swapped.Pixels, Front.Pixels);
		EXPECT_EQ(Back.Pixels, NoWater.Pixels);
		const auto Pixel = [](const FCapture& Image, uint32 X) { return std::span(Image.Pixels).subspan((32 * 86 + X) * 4, 4); };
		EXPECT_TRUE(std::ranges::equal(Pixel(HoleAtFront, 24), Pixel(NoWater, 24)));
		EXPECT_TRUE(std::ranges::equal(Pixel(HoleAtFront, 62), Pixel(BedOnly, 62)));
		EXPECT_EQ(PostAndWater.Pixels, PostOnly.Pixels);
		EXPECT_FALSE(std::ranges::equal(Pixel(PostAndWater, 43), Pixel(PostAndWater, 24)));
		EXPECT_NE(Front.Pixels, NoWater.Pixels);
		EXPECT_FALSE(std::ranges::any_of(NoWater.Graph.Resources, [](const auto& Resource) { return Resource.Name.starts_with("Water."); }));
		EXPECT_FALSE(std::ranges::any_of(NoWater.Graph.Passes, [](const auto& Pass) { return Pass.Name.starts_with("Water."); }));
		const auto Pass = [&](std::string_view Name) { return std::ranges::find(Front.Graph.Passes, Name, &FRDGPassCapture::Name); };
		ASSERT_NE(Pass("Water.Snapshot"), Front.Graph.Passes.end());
		ASSERT_NE(Pass("Water.NearestDepth"), Front.Graph.Passes.end());
		ASSERT_NE(Pass("Water.Shading"), Front.Graph.Passes.end());
		ASSERT_NE(Pass("Scene.Color"), Front.Graph.Passes.end());
		EXPECT_LT(Pass("Water.Snapshot"), Pass("Water.NearestDepth"));
		EXPECT_LT(Pass("Water.NearestDepth"), Pass("Water.Shading"));
		EXPECT_LT(Pass("Water.Shading"), Pass("Scene.Color"));
		for (const auto Name : {"Water.BackgroundColor", "Water.BackgroundDeviceZ", "Water.Coverage"})
		{
			const auto Texture = std::ranges::find(Front.Graph.Resources, Name, &FRDGResourceCapture::Name);
			ASSERT_NE(Texture, Front.Graph.Resources.end());
			const auto Shade = Pass("Water.Shading")->DeclarationIndex;
			EXPECT_EQ(std::ranges::count_if(Front.Graph.Uses, [&](const auto& Use) {
				return Use.PassDeclarationIndex == Shade && Use.ResourceId == Texture->ResourceId
					&& Use.Use == ERDGUse::Read && Use.Access == ERHIAccess::GraphicsShaderRead;
			}), 1);
			EXPECT_EQ(std::ranges::count_if(Front.Graph.Uses, [&](const auto& Use) {
				return Use.PassDeclarationIndex == Shade && Use.ResourceId == Texture->ResourceId && Use.Use != ERDGUse::Read;
			}), 0);
		}
		const auto Evidence = std::filesystem::path(FPaths::RootDir()) / "Build/SingleLayerWater/Stage2";
		std::filesystem::create_directories(Evidence);
		const auto Name = bReversed ? "ReversedZ" : "ForwardZ";
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Front.Pixels, Evidence / (std::string(Name) + ".rgba8")));
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Front.Graph.Dump)), Evidence / (std::string(Name) + "-rdg.txt")));

		const FVariant Mixed{.bOverlay = true, .bFront = true, .bPost = true, .bMixedDepth = true};
		FCapture Flat, GPUDisplaced, CPUDisplaced;
		Render(bReversed, Mixed, Flat);
		Render(bReversed, Mixed, GPUDisplaced, Animated);
		Render(bReversed, Mixed, CPUDisplaced, Water, DisplacedGrid.get());
		ASSERT_FALSE(HasFailure());
		ASSERT_EQ(GPUDisplaced.Pixels.size(), CPUDisplaced.Pixels.size());
		uint32 Different = 0;
		for (size_t Offset = 0; Offset < GPUDisplaced.Pixels.size(); Offset += 4)
			Different += !std::ranges::equal(std::span(GPUDisplaced.Pixels).subspan(Offset, 4),
				std::span(CPUDisplaced.Pixels).subspan(Offset, 4));
		EXPECT_LT(Different, 86u * 64u / 100u);
		EXPECT_NE(Flat.Pixels, GPUDisplaced.Pixels);
		const auto Integration = Evidence.parent_path() / "Stage5";
		std::filesystem::create_directories(Integration);
		EXPECT_TRUE(FFileHelper::SaveArrayToFile(GPUDisplaced.Pixels, Integration / (std::string(Name) + "-displaced.rgba8")));
		EXPECT_TRUE(FFileHelper::SaveArrayToFile(CPUDisplaced.Pixels, Integration / (std::string(Name) + "-cpu-displaced.rgba8")));
		EXPECT_TRUE(FFileHelper::SaveArrayToFile(Flat.Pixels, Integration / (std::string(Name) + "-flat.rgba8")));
	}
}

TEST(FWaterRenderingPathVulkanTests, WaterVolumeMatchesThreeLinearHDRBedProbes)
{
	using namespace Durin;
	using namespace Durin::Tests;
	using P = EMaterialProperty;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("WaterVolume");
	Lifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(Lifecycle);

	FMaterialCompilerEnvironment Environment;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment, VulkanShaderTarget));
	Environment.FeatureLevel = FSceneInterface::GetDefaultFeatureLevel();
	Environment.Quality = GetMaterialQualityLevel();
	MIR::FCompilerInput Input{.StaticProperties = {.ShadingModel = EMaterialShadingModel::Unlit, .bTwoSided = true},
		.Environment = Environment};
	Input.IR.GetAttribute(P::MP_EmissiveColor).Literal = {1, 1, 1};
	const auto BedProgram = MIR::Compile(Input);
	ASSERT_TRUE(BedProgram) << FormatMaterialProgramDiagnostics(BedProgram.Diagnostics);
	auto Bed = MakeRefCount<FMaterialRenderProxy>();
	Durin::Testing::QueueMaterialTestState(*Bed, {.StaticProperties = Input.StaticProperties,
		.CompiledProgram = std::make_shared<const FMaterialProgram>(BedProgram.Resource)});

	Input.StaticProperties.ShadingModel = EMaterialShadingModel::SingleLayerWater;
	Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::IndexOfRefractionFromF0;
	Input.IR.GetAttribute(P::MP_EmissiveColor).Literal = {};
	Input.IR.GetAttribute(P::MP_Specular).Literal = {.25f};
	Input.IR.GetAttribute(P::MP_Roughness).Literal = {.1f};
	Input.IR.GetAttribute(P::MP_Opacity).Literal = {};
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {.01f, .01f, .01f};
	auto Water = MakeRefCount<FMaterialRenderProxy>();
	const auto PublishWater = [&] {
		const auto Program = MIR::Compile(Input);
		ASSERT_TRUE(Program) << FormatMaterialProgramDiagnostics(Program.Diagnostics);
		Durin::Testing::QueueMaterialTestState(*Water, {.StaticProperties = Input.StaticProperties,
			.CompiledProgram = std::make_shared<const FMaterialProgram>(Program.Resource)});
	};
	PublishWater();
	ASSERT_FALSE(HasFailure());

	auto Grid = MakeWaterPathGrid();
	const std::shared_ptr<void> Release(nullptr, [&](void*) {
		EnqueueRenderCommand("ReleaseWaterVolumeFixture", [&](FRHICommandListImmediate&) { Grid->ReleaseResources(); });
		FlushRenderingCommands();
	});
	EnqueueRenderCommand("InitializeWaterVolumeFixture", [&](FRHICommandListImmediate& Commands) {
		ASSERT_TRUE(Grid->InitResources(Commands));
	});
	FlushRenderingCommands();
	ASSERT_FALSE(HasFailure());
	FSceneTestOwner Scene;
	const auto Add = [&](uint32 Id, double CenterX, double Height, double Width, double Length,
		const FMaterialRenderProxyRef& Proxy, double CenterY = 0.0) {
		FMatrix Transform(1.0);
		Transform[0][0] = Width / 1.6;
		Transform[1][1] = Length / 1.4;
		Transform[3] = FVector4(CenterX, CenterY, Height - .5, 1.0);
		FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(Id),
			std::make_unique<FStaticMeshSceneProxy>(Grid.get(), std::vector{Proxy}), Transform);
	};
	Add(710, -1.0, -.25, 1.0, 6.0, Bed);
	Add(711, 0.0, -1.0, 1.0, 6.0, Bed);
	Add(712, 1.0, -2.0, 1.0, 6.0, Bed);
	Add(713, 0.0, 0.0, 6.0, 6.0, Water);
	Add(714, 0.0, -.1, 1.0, 1.0, Water);
	FSceneView View;
	View.ViewportWidth = 320; View.ViewportHeight = 240;
	View.ViewLocation = {0, 0, 3};
	View.ViewMatrix = FMatrix(0.0);
	View.ViewMatrix[2][0] = -1.0;
	View.ViewMatrix[1][1] = 1.0;
	View.ViewMatrix[0][2] = 1.0;
	View.ViewMatrix[3] = FVector4(3, 0, 0, 1);
	View.Settings.Mode.RenderMode = ERenderMode::Lit;
	View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
	View.Settings.Mode.bEnableSpecularAA = false;
	View.Settings.AmbientOcclusion.bEnabled = false;
	View.Settings.PostProcess.bEnableFXAA = false;
	constexpr double Aspect = 320.0 / 240.0;
	const double VerticalFov = 2.0 * std::atan(std::tan(Math::DegreesToRadians(30.0)) / Aspect)
		* (180.0 / std::numbers::pi);
	const auto Evidence = std::filesystem::path(FPaths::RootDir()) / "Build/SingleLayerWater/Stage3";
	std::filesystem::create_directories(Evidence);
	static FByteBuffer* Sink = nullptr;
	const auto CaptureHDR = [&](FByteBuffer& HDR, EWaterVolumeReadback Readback = EWaterVolumeReadback::None) {
		const auto Target = FRHITextureCreateDesc::Create2D("WaterVolumeHDR", View.ViewportWidth, View.ViewportHeight, EPixelFormat::SRGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
		const auto Result = RenderPreparedGeometryView(Renderer, *Scene, View, Target, [&] {
			Sink = &HDR;
			SetHDRSceneColorCaptureSink(+[](FRHICommandListImmediate& Commands, FRHITexture* Color, FRHITexture*) {
				ReadWaterPixels(Commands, Color, *Sink);
			});
		}, {}, {}, {.WaterVolumeReadback = Readback});
		SetHDRSceneColorCaptureSink(nullptr);
		Sink = nullptr;
		ASSERT_EQ(Result, ERenderViewResult::Success);
		ASSERT_EQ(HDR.size(), View.ViewportWidth * View.ViewportHeight * 8u);
	};
	const auto ReadChannel = [&](const FByteBuffer& HDR, uint32 X, uint32 Y, uint32 Channel) -> float {
		uint16 Half = 0;
		std::memcpy(&Half, HDR.data() + (Y * View.ViewportWidth + X) * 8 + Channel * 2, sizeof(Half));
		return glm::unpackHalf2x16(Half).x;
	};
	const auto CheckColor = [&](const FByteBuffer& HDR, uint32 X, uint32 Y, FVector3f Expected) {
		for (uint32 Channel = 0; Channel < 3; ++Channel)
			EXPECT_NEAR(ReadChannel(HDR, X, Y, Channel), Expected[Channel], std::max(.002f, std::abs(Expected[Channel]) * .01f))
				<< "pixel " << X << ", " << Y << ", channel " << Channel;
	};
	std::string Measurements = "depth_convention,x,y,expected,red,green,blue,view_transmission,bed_transmission\n";
	for (auto Depth : {ESceneDepthConvention::ForwardZ, ESceneDepthConvention::ReversedZ})
	{
		View.DepthConvention = Depth;
		ASSERT_TRUE(SceneViewProjection::BuildPerspectiveProjection(VerticalFov, Aspect, .1, 100.0, Depth, View.ProjectionMatrix));
		View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
		FByteBuffer HDR, ViewTransmission, BedTransmission;
		CaptureHDR(HDR);
		CaptureHDR(ViewTransmission, EWaterVolumeReadback::ViewTransmission);
		CaptureHDR(BedTransmission, EWaterVolumeReadback::BedTransmission);
		ASSERT_FALSE(HasFailure());
		const auto Name = Depth == ESceneDepthConvention::ForwardZ ? "ForwardZ" : "ReversedZ";
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(HDR, Evidence / (std::string(Name) + ".rgba16f")));
		for (const auto& [Y, DepthCm] : std::array{std::pair{195u, 25.0f}, std::pair{120u, 100.0f}, std::pair{45u, 200.0f}})
		{
			const float Transmission = std::exp(-.01f * DepthCm);
			const float Expected = Transmission * Transmission;
			CheckColor(HDR, 160, Y, FVector3f(Expected));
			CheckColor(ViewTransmission, 160, Y, FVector3f(Transmission));
			CheckColor(BedTransmission, 160, Y, FVector3f(Transmission));
			Measurements += std::format("{},160,{},{},{},{},{},{},{}\n", Name, Y, Expected,
				ReadChannel(HDR, 160, Y, 0), ReadChannel(HDR, 160, Y, 1), ReadChannel(HDR, 160, Y, 2),
				ReadChannel(ViewTransmission, 160, Y, 0), ReadChannel(BedTransmission, 160, Y, 0));
		}
	}
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Measurements)), Evidence / "absorption.csv"));

	// Keep the same bed geometry and vary one material input at a time.
	Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::None;
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {};
	PublishWater();
	FByteBuffer HDR;
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 195, FVector3f(1.0f));
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {.01f, .005f, 0.0f};
	View.ClearColor = {.2f, .4f, .6f, 1.0f};
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 195, {std::exp(-.5f), std::exp(-.25f), 1.0f});
	CheckColor(HDR, 160, 5, {0.0f, 0.0f, .6f});
	Input.IR.GetAttribute(P::MP_Opacity).Literal = {1.0f};
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 195, FVector3f(0.0f));
	Input.IR.GetAttribute(P::MP_Opacity).Literal = {};
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {};
	Input.IR.GetCustomOutput(SingleLayerWater::ColorScale).Literal = {.5f, .5f, .5f};
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 195, FVector3f(.75f));
	Input.IR.GetCustomOutput(SingleLayerWater::ColorScale).Literal = {1.0f, 1.0f, 1.0f};
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {.01f, .01f, .01f};
	Add(715, 0.0, -.00005, .12, 1.0, Bed);
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 120, FVector3f(1.0f));
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(715));

	Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::IndexOfRefractionFromF0;
	Input.IR.GetAttribute(P::MP_Normal).Literal = {0, .5f, .8660254f};
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {};
	PublishWater();
	FByteBuffer Refraction;
	CaptureHDR(Refraction, EWaterVolumeReadback::RefractionOffset);
	ASSERT_FALSE(HasFailure());
	const float Offset = ReadChannel(Refraction, 160, 120, 0);
	// Snell transmission through the tilted normal, projected onto the 1 m and 25 cm beds.
	EXPECT_NEAR(Offset, -9.659f, .1f);
	EXPECT_NEAR(ReadChannel(Refraction, 160, 195, 0), -2.593f, .1f);
	Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::IndexOfRefraction;
	Input.IR.GetAttribute(P::MP_Refraction).Literal = {1.0f};
	PublishWater();
	CaptureHDR(Refraction, EWaterVolumeReadback::RefractionOffset);
	ASSERT_FALSE(HasFailure());
	EXPECT_FLOAT_EQ(ReadChannel(Refraction, 160, 120, 0), 0.0f);

	Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::IndexOfRefractionFromF0;
	MIR::FCompilerInput ForegroundInput{.StaticProperties = {.ShadingModel = EMaterialShadingModel::Unlit, .bTwoSided = true},
		.Environment = Environment};
	ForegroundInput.IR.GetAttribute(P::MP_EmissiveColor).Literal = {0, 1, 0};
	const auto ForegroundProgram = MIR::Compile(ForegroundInput);
	ASSERT_TRUE(ForegroundProgram) << FormatMaterialProgramDiagnostics(ForegroundProgram.Diagnostics);
	auto Foreground = MakeRefCount<FMaterialRenderProxy>();
	Durin::Testing::QueueMaterialTestState(*Foreground, {.StaticProperties = ForegroundInput.StaticProperties,
		.CompiledProgram = std::make_shared<const FMaterialProgram>(ForegroundProgram.Resource)});
	const double PostY = double(Offset) * (2.0 / 320.0) * 2.8 / std::sqrt(3.0);
	Add(716, 0.0, .2, .16, .15, Foreground, PostY);
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 120, FVector3f(1.0f));
	CheckColor(HDR, static_cast<uint32>(std::round(160.0f + Offset)), 120, {0.0f, 1.0f, 0.0f});
	CheckColor(HDR, Offset > 0 ? 319u : 0u, 120, FVector3f(1.0f));
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(716));

	// The shallow stripe overlaps the projected ray after it has already passed
	// the stripe's depth. Its screen-space edge cannot intercept the ray.
	const double ShallowStripeY = -5.0 * (2.0 / 320.0) * 3.25 / std::sqrt(3.0);
	Add(717, 0.0, -.25, .16, .04, Foreground, ShallowStripeY);
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 120, FVector3f(1.0f));
	CaptureHDR(Refraction, EWaterVolumeReadback::RefractionOffset);
	ASSERT_FALSE(HasFailure());
	EXPECT_NEAR(ReadChannel(Refraction, 160, 120, 0), Offset, .1f);
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(717));

	// Track an actual underwater landmark in the shaded HDR image with the camera locked.
	Add(718, 0.0, -.999, 1.0, .18, Foreground);
	for (uint32 Scale : {1u, 2u})
	{
		View.ViewportWidth = 320 * Scale;
		View.ViewportHeight = 240 * Scale;
		std::array<float, 2> StripeCenter{};
		for (uint32 Enabled : {0u, 1u})
		{
			Input.StaticProperties.RefractionMethod = Enabled ? EMaterialRefractionMethod::IndexOfRefractionFromF0
				: EMaterialRefractionMethod::None;
			PublishWater();
			CaptureHDR(HDR);
			ASSERT_FALSE(HasFailure());
			const auto File = std::format("refraction-{}x{}-{}.rgba16f", View.ViewportWidth, View.ViewportHeight, Enabled);
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(HDR, Evidence / File));
			uint32 Count = 0;
			for (uint32 X = 0; X < View.ViewportWidth; ++X)
			{
				const auto Y = View.ViewportHeight / 2;
				if (ReadChannel(HDR, X, Y, 1) - ReadChannel(HDR, X, Y, 0) > .5f)
				{
					StripeCenter[Enabled] += float(X) + .5f;
					++Count;
				}
			}
			ASSERT_GT(Count, 0u);
			StripeCenter[Enabled] /= float(Count);
		}
		EXPECT_NEAR((StripeCenter[0] - StripeCenter[1]) / float(Scale), Offset, 1.0f);
	}
	View.ViewportWidth = 320;
	View.ViewportHeight = 240;
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(718));

	// Distinguish the exposed pier (green), submerged pier (blue), and bed (white).
	// A single bed at the foot of the pier leaves its entire face visible.
	for (uint32 Id : {710u, 711u, 712u})
		FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(Id));
	Add(721, 0.0, -2.0, 8.0, 8.0, Bed);
	const auto TopDownView = View;
	const auto AddPierFace = [&](uint32 Id, double CenterZ, const FMaterialRenderProxyRef& Material) {
		FMatrix Transform(1.0);
		Transform[0] = FVector4(0, 0, 1.25, 0);
		Transform[1] = FVector4(0, .5, 0, 0);
		Transform[2] = FVector4(-1, 0, 0, 0);
		Transform[3] = FVector4(.5, 0, CenterZ, 1);
		FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(Id),
			std::make_unique<FStaticMeshSceneProxy>(Grid.get(), std::vector{Material}), Transform);
	};
	AddPierFace(719, 1.0, Foreground);
	ForegroundInput.IR.GetAttribute(P::MP_EmissiveColor).Literal = {0, 0, 1};
	const auto WetProgram = MIR::Compile(ForegroundInput);
	ASSERT_TRUE(WetProgram) << FormatMaterialProgramDiagnostics(WetProgram.Diagnostics);
	auto WetPier = MakeRefCount<FMaterialRenderProxy>();
	Durin::Testing::QueueMaterialTestState(*WetPier, {.StaticProperties = ForegroundInput.StaticProperties,
		.CompiledProgram = std::make_shared<const FMaterialProgram>(WetProgram.Resource)});
	AddPierFace(720, -1.0, WetPier);
	View.ViewLocation = {-6, 0, 3};
	const double Axis = 1.0 / std::sqrt(5.0);
	View.ViewMatrix[0] = FVector4(2 * Axis, 0, Axis, 0);
	View.ViewMatrix[1] = FVector4(0, 1, 0, 0);
	View.ViewMatrix[2] = FVector4(-Axis, 0, 2 * Axis, 0);
	View.ViewMatrix[3] = FVector4(15 * Axis, 0, 0, 1);
	Input.IR.GetAttribute(P::MP_Normal).Literal = {0, 0, 1};
	Input.IR.GetAttribute(P::MP_Specular).Literal = {.5f};
	PublishWater();
	for (auto Depth : {ESceneDepthConvention::ForwardZ, ESceneDepthConvention::ReversedZ})
	{
		SCOPED_TRACE(Depth == ESceneDepthConvention::ForwardZ ? "ForwardZ oblique pier" : "ReversedZ oblique pier");
		View.DepthConvention = Depth;
		ASSERT_TRUE(SceneViewProjection::BuildPerspectiveProjection(VerticalFov, Aspect, .1, 100.0, Depth, View.ProjectionMatrix));
		View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
		CaptureHDR(HDR);
		ASSERT_FALSE(HasFatalFailure());
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(HDR, Evidence / "oblique-pier.rgba16f"));
		CheckColor(HDR, 160, 100, {0, 1, 0});
		// At (146,138) the transmitted ray intersects the pier at y=-.338 m,
		// inside its -.35 m edge. At (160,152) it crosses the bed before the pier.
		EXPECT_GT(ReadChannel(HDR, 146, 138, 2) - ReadChannel(HDR, 146, 138, 0), .5f);
		CheckColor(HDR, 160, 152, FVector3f(1.0f));
		// The submerged face must be continuous from the waterline down to its apparent foot.
		uint32 End = 120;
		while (End < View.ViewportHeight && ReadChannel(HDR, 160, End, 2) - ReadChannel(HDR, 160, End, 0) > .5f)
			++End;
		EXPECT_GT(End, 130u);
		ASSERT_LT(End, View.ViewportHeight);
		for (uint32 Y = End; Y < View.ViewportHeight; ++Y)
			EXPECT_LE(ReadChannel(HDR, 160, Y, 2) - ReadChannel(HDR, 160, Y, 0), .5f) << Y;
		Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::None;
		PublishWater();
		CaptureHDR(HDR);
		ASSERT_FALSE(HasFatalFailure());
		EXPECT_GT(ReadChannel(HDR, 160, End, 2) - ReadChannel(HDR, 160, End, 0), .5f)
			<< "Above-water refraction should raise the apparent foot of the pier";
		Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::IndexOfRefractionFromF0;
		PublishWater();
	}

	// Extend water just over the reprojected footprint at the finite pool edge.
	// A green foreground quad at receiver-like depth then occludes one corner.
	const FVector3 ScreenForward(2.0 * Axis, 0, -Axis);
	const FVector3 ScreenRight(0, 1, 0);
	const FVector3 ScreenUp(Axis, 0, 2.0 * Axis);
	const double FocalPixels = 160.0 / std::tan(Math::DegreesToRadians(30.0));
	const auto AddScreenQuad = [&](uint32 Id, FVector2 Center, double Size, double Depth,
		const FMaterialRenderProxyRef& Proxy) {
		const auto ToCenter = ScreenForward + ScreenRight * ((Center.x - 160.0) / FocalPixels)
			+ ScreenUp * ((120.0 - Center.y) / FocalPixels);
		const auto Position = View.ViewLocation + ToCenter * Depth;
		const double Width = Size * Depth / FocalPixels;
		FMatrix Transform(1.0);
		Transform[0] = FVector4(ScreenRight * (Width / 1.6), 0);
		Transform[1] = FVector4(ScreenUp * (Width / 1.4), 0);
		Transform[2] = FVector4(ScreenForward, 0);
		Transform[3] = FVector4(Position - ScreenForward * .5, 1);
		FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(Id),
			std::make_unique<FStaticMeshSceneProxy>(Grid.get(), std::vector{Proxy}), Transform);
	};
	const double BedDepthAtLowerCorner = -5.0
		/ (ScreenForward.z + ScreenUp.z * ((120.0 - 214.5) / FocalPixels));
	const double CornerWaterDepth = BedDepthAtLowerCorner - .004;
	Input.IR.GetAttribute(P::MP_EmissiveColor).Literal = {.25f, 0, 0};
	PublishWater();
	AddScreenQuad(722, {165.0, 214.0}, 2.0, CornerWaterDepth, Water);
	AddScreenQuad(723, {164.5, 214.5}, .8, CornerWaterDepth - .004, Foreground);
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 164, 214, {0, 1, 0});
	for (const auto [X, Y] : {std::pair{164u, 213u}, std::pair{165u, 213u}, std::pair{165u, 214u}})
		EXPECT_GT(ReadChannel(HDR, X, Y, 0), 1.0f) << "Covered tap (" << X << ", " << Y << ")";
	CheckColor(HDR, 165, 177, {1.25f, 1, 1});
	CaptureHDR(Refraction, EWaterVolumeReadback::RefractionOffset);
	ASSERT_FALSE(HasFailure());
	EXPECT_GT(ReadChannel(Refraction, 165, 177, 1), 25.0f);
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(723));
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(722));
	Input.IR.GetAttribute(P::MP_EmissiveColor).Literal = {};
	View = TopDownView;
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(719));
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(720));
	FSceneInterfaceTestAccess::RemovePrimitiveProxy(*Scene, FPrimitiveComponentId(721));
	Add(710, -1.0, -.25, 1.0, 6.0, Bed);
	Add(711, 0.0, -1.0, 1.0, 6.0, Bed);
	Add(712, 1.0, -2.0, 1.0, 6.0, Bed);

	Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::None;
	Input.IR.GetAttribute(P::MP_Normal).Literal = {0, 0, 1};
	Input.IR.GetAttribute(P::MP_Specular).Literal = {};
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {.005f, .005f, .005f};
	Input.IR.GetCustomOutput(SingleLayerWater::Scattering).Literal = {.005f, .005f, .005f};
	FDirectionalLightSceneData Sun;
	Sun.Direction = {std::sqrt(.75), 0.0, -.5};
	Sun.Intensity = 1.0f;
	Sun.bCastShadows = false;
	ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(*Scene, FLightComponentId(717), Sun), nullptr);
	for (const auto& [Phase, Expected] : std::array{std::pair{-.5f, .095317f}, std::pair{0.0f, .079577f}, std::pair{.5f, .021784f}})
	{
		Input.IR.GetCustomOutput(SingleLayerWater::PhaseG).Literal = {Phase};
		PublishWater();
		CaptureHDR(HDR);
		ASSERT_FALSE(HasFailure());
		CheckColor(HDR, 160, 120, FVector3f(.135335f + .316060f * Expected));
	}
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {};
	Input.IR.GetCustomOutput(SingleLayerWater::Scattering).Literal = {.01f, .01f, .01f};
	Input.IR.GetCustomOutput(SingleLayerWater::PhaseG).Literal = {};
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	CheckColor(HDR, 160, 120, FVector3f(.135335f + .632121f * .079577f));

	// Transparent water retains the dielectric highlight without adding diffuse base color.
	Input.IR.GetCustomOutput(SingleLayerWater::Scattering).Literal = {};
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {1.f, 1.f, 1.f};
	Input.IR.GetAttribute(P::MP_Specular).Literal = {.5f};
	Input.IR.GetAttribute(P::MP_Roughness).Literal = {.5f};
	Input.IR.GetAttribute(P::MP_BaseColor).Literal = {};
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	const float Highlight = ReadChannel(HDR, 160, 120, 0);
	EXPECT_GT(Highlight, 0.0f);
	Input.IR.GetAttribute(P::MP_BaseColor).Literal = {1, 0, 0};
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	for (uint32 Channel = 0; Channel < 3; ++Channel)
		EXPECT_FLOAT_EQ(ReadChannel(HDR, 160, 120, Channel), Highlight);
	Input.IR.GetAttribute(P::MP_Opacity).Literal = {1};
	PublishWater();
	CaptureHDR(HDR);
	ASSERT_FALSE(HasFailure());
	EXPECT_GT(ReadChannel(HDR, 160, 120, 0), Highlight + .1f);
	EXPECT_FLOAT_EQ(ReadChannel(HDR, 160, 120, 1), Highlight);

	Input.IR.GetAttribute(P::MP_Opacity).Literal = {};
	Input.IR.GetAttribute(P::MP_BaseColor).Literal = {};
	Input.IR.GetAttribute(P::MP_Specular).Literal = {};
	Input.IR.GetCustomOutput(SingleLayerWater::Scattering).Literal = {.2f, .2f, .2f};
	Input.IR.GetCustomOutput(SingleLayerWater::Absorption).Literal = {};
	for (float Phase : {-1.f, -.97f, .97f, 1.f})
	{
		Input.IR.GetCustomOutput(SingleLayerWater::PhaseG).Literal = {Phase};
		PublishWater();
		CaptureHDR(HDR);
		ASSERT_FALSE(HasFailure());
		const float Scattered = ReadChannel(HDR, 160, 120, 0);
		EXPECT_GT(Scattered, 0.0f) << Phase;
		EXPECT_TRUE(std::isfinite(Scattered)) << Phase;
	}
}

TEST(FWaterRenderingPathVulkanTests, WaterSkyReflectionCompositesOnlyTheNearestCoveredSurface)
{
	using namespace Durin;
	using namespace Durin::Tests;
	using P = EMaterialProperty;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("WaterReflection");
	Lifecycle.Start(Renderer);
	FGeometryRendererCleanup Cleanup(Lifecycle);
	TStrongObjectPtr<DTextureCube> Cube(NewObject<DTextureCube>(nullptr, NAME_None));
	auto CubeData = std::make_unique<FTextureCubePlatformData>();
	CubeData->PixelFormat = EPixelFormat::RGBA32_FLOAT;
	for (uint32 Side = 0; Side < CubeData->Faces.size(); ++Side)
	{
		auto& Face = CubeData->Faces[Side];
		Face.PixelFormat = CubeData->PixelFormat;
		const std::array<float, 4> Color{.25f + .1f * Side, .5f, .75f, 1.f};
		FByteBuffer Bytes(sizeof(Color));
		std::memcpy(Bytes.data(), Color.data(), sizeof(Color));
		Face.Mips.push_back({FSharedByteBuffer::Take(std::move(Bytes)), 1, 1, sizeof(Color)});
	}
	Cube->SetPlatformData(std::move(CubeData));
	Cube->UpdateResource();
	FlushRenderingCommands();
	const std::shared_ptr<void> ReleaseCube(nullptr, [&](void*) {
		auto* Texture = Cube.Get();
		Cube.Reset();
		MarkAsGarbage(Texture);
		CollectGarbage(EObjectFlags::Standalone);
		FlushRenderingCommands();
	});
	auto Sky = std::make_shared<FSkyLightSceneProxy>();
	Sky->PersistentId = FGuid::NewGuid();
	Sky->SelectionKey = "WaterReflectionSky";
	Sky->InstanceId = 1;
	Sky->SourceEpoch = 1;
	Sky->RequestSerial = 1;
	Sky->bEligible = true;
	Sky->bAutomaticRefresh = false;
	Sky->Texture = Cube->GetTextureReferenceRHI();
	Sky->UpdateStatus = std::make_shared<FSkyLightUpdateStatus>();
	ASSERT_TRUE(Sky->Texture);
	FMaterialCompilerEnvironment Environment;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment, VulkanShaderTarget));
	Environment.FeatureLevel = FSceneInterface::GetDefaultFeatureLevel();
	Environment.Quality = GetMaterialQualityLevel();
	auto Water = MakeRefCount<FMaterialRenderProxy>();
	const auto PublishWater = [&](float Roughness, bool bMasked = false) {
		MIR::FCompilerInput Input{.StaticProperties = {
			.BlendMode = bMasked ? EMaterialBlendMode::Masked : EMaterialBlendMode::Opaque,
			.ShadingModel = EMaterialShadingModel::SingleLayerWater, .bTwoSided = true}, .Environment = Environment};
		Input.IR.GetAttribute(P::MP_Roughness).Literal = {Roughness};
		Input.IR.GetAttribute(P::MP_Specular).Literal = {.25f};
		if (bMasked) Input.IR.GetAttribute(P::MP_OpacityMask).Literal = {};
		const auto Program = MIR::Compile(Input);
		ASSERT_TRUE(Program) << FormatMaterialProgramDiagnostics(Program.Diagnostics);
		Durin::Testing::QueueMaterialTestState(*Water, {.StaticProperties = Input.StaticProperties,
			.CompiledProgram = std::make_shared<const FMaterialProgram>(Program.Resource)});
	};
	PublishWater(.1f);
	ASSERT_FALSE(HasFailure());
	auto Grid = MakeWaterPathGrid();
	const std::shared_ptr<void> Release(nullptr, [&](void*) {
		EnqueueRenderCommand("ReleaseWaterReflectionFixture", [&](FRHICommandListImmediate&) { Grid->ReleaseResources(); });
		FlushRenderingCommands();
	});
	EnqueueRenderCommand("InitializeWaterReflectionFixture", [&](FRHICommandListImmediate& Commands) {
		ASSERT_TRUE(Grid->InitResources(Commands));
	});
	FlushRenderingCommands();
	ASSERT_FALSE(HasFailure());
	FSceneTestOwner Scene;
	FSceneInterfaceTestAccess::AddSkyLightProxy(*Scene, Sky);
	const auto Surface = [&](uint32 Id, double Height) {
		FMatrix Transform(1.0);
		Transform[0][0] = 3.0;
		Transform[1][1] = 3.0;
		Transform[3][2] = Height - .5;
		FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(Id),
			std::make_unique<FStaticMeshSceneProxy>(Grid.get(), std::vector{Water}), Transform);
	};
	Surface(720, 0.0);
	FSceneView View;
	View.ViewportWidth = 96; View.ViewportHeight = 96;
	View.Settings.Mode.RenderMode = ERenderMode::Lit;
	View.Settings.Mode.VisibilityMode = EViewVisibilityMode::FrustumCullingDisabled;
	View.Settings.Mode.bEnableSpecularAA = false;
	View.Settings.AmbientOcclusion.bEnabled = false;
	View.ClearColor = {.05f, .05f, .05f, .7f};
	ASSERT_TRUE(SceneViewProjection::BuildPerspectiveProjection(60.0, 1.0, .1, 100.0,
		View.DepthConvention, View.ProjectionMatrix));
	const auto Aim = [&](FVector3 Position) {
		const FVector3 Forward = Math::Normalize(-Position);
		const FVector3 Right = Math::Normalize(Math::Cross(FVector3(1, 0, 0), Forward));
		const FVector3 Up = Math::Cross(Forward, Right);
		View.ViewLocation = Position;
		for (uint32 Axis = 0; Axis < 3; ++Axis)
			View.ViewMatrix[Axis] = FVector4(Forward[Axis], Right[Axis], Up[Axis], 0.0);
		View.ViewMatrix[3] = FVector4(-Math::Dot(Forward, Position), -Math::Dot(Right, Position), -Math::Dot(Up, Position), 1.0);
		View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
	};
	Aim({0, 0, 3});
	const auto Target = FRHITextureCreateDesc::Create2D("WaterReflectionHDR", 96, 96, EPixelFormat::SRGBA8_UNORM)
		.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
	struct FCapture { FByteBuffer HDR; FRDGCapture Graph; };
	static FCapture* Sink = nullptr;
	const auto Capture = [&](bool bDisable, FCapture& Out) {
		const auto Result = RenderPreparedGeometryView(Renderer, *Scene, View, Target, [&] {
			Sink = &Out;
			SetHDRSceneColorCaptureSink(+[](FRHICommandListImmediate& Commands, FRHITexture* Color, FRHITexture*) {
				ReadWaterPixels(Commands, Color, Sink->HDR);
			});
			SetSceneRenderGraphCaptureSink(+[](const FRDGCapture& Graph) { Sink->Graph = Graph; });
		}, {}, {}, {.bDisableWaterReflections = bDisable});
		SetHDRSceneColorCaptureSink(nullptr);
		SetSceneRenderGraphCaptureSink(nullptr);
		Sink = nullptr;
		ASSERT_EQ(Result, ERenderViewResult::Success);
		ASSERT_EQ(Out.HDR.size(), 96u * 96u * 8u);
	};
	const auto ReflectionPasses = [](const FCapture& Capture) {
		return std::ranges::count(Capture.Graph.Passes, "Water.SkyReflection", &FRDGPassCapture::Name);
	};
	FCapture Normal;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	do
	{
		Capture(false, Normal);
		ASSERT_FALSE(HasFailure());
		if (ReflectionPasses(Normal) != 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now() < Deadline);
	ASSERT_EQ(ReflectionPasses(Normal), 1);
	FCapture Disabled, Rear, Rough, Grazing, Masked, MaskedDisabled;
	Capture(true, Disabled);
	Surface(721, -.1);
	Capture(false, Rear);
	PublishWater(.7f);
	Capture(false, Rough);
	PublishWater(.1f);
	Aim({0, 3.7, .6});
	Capture(false, Grazing);
	PublishWater(.1f, true);
	Capture(false, Masked);
	Capture(true, MaskedDisabled);
	PublishWater(.1f);
	Aim({0, 0, 3});
	auto ForegroundMaterial = MakeRefCount<FMaterialRenderProxy>();
	MIR::FCompilerInput ForegroundInput{.StaticProperties = {
		.ShadingModel = EMaterialShadingModel::Unlit, .bTwoSided = true}, .Environment = Environment};
	ForegroundInput.IR.GetAttribute(P::MP_EmissiveColor).Literal = {.3f, .2f, .1f};
	const auto ForegroundProgram = MIR::Compile(ForegroundInput);
	ASSERT_TRUE(ForegroundProgram) << FormatMaterialProgramDiagnostics(ForegroundProgram.Diagnostics);
	Durin::Testing::QueueMaterialTestState(*ForegroundMaterial, {.StaticProperties = ForegroundInput.StaticProperties,
		.CompiledProgram = std::make_shared<const FMaterialProgram>(ForegroundProgram.Resource)});
	FMatrix ForegroundTransform(1.0);
	ForegroundTransform[3][2] = .2;
	FSceneInterfaceTestAccess::AddPrimitiveProxy(*Scene, FPrimitiveComponentId(722),
		std::make_unique<FStaticMeshSceneProxy>(Grid.get(), std::vector{ForegroundMaterial}), ForegroundTransform);
	FCapture Foreground, ForegroundDisabled;
	Capture(false, Foreground);
	Capture(true, ForegroundDisabled);
	ASSERT_FALSE(HasFailure());
	EXPECT_EQ(ReflectionPasses(Disabled), 0);
	EXPECT_EQ(Rear.HDR, Normal.HDR);
	EXPECT_EQ(Masked.HDR, MaskedDisabled.HDR);
	const auto Center = [](const FCapture& Capture, uint32 Channel) {
		uint16 Half = 0;
		std::memcpy(&Half, Capture.HDR.data() + (48 * 96 + 48) * 8 + Channel * 2, sizeof(Half));
		return glm::unpackHalf2x16(Half).x;
	};
	EXPECT_GT(Center(Normal, 0), Center(Disabled, 0));
	EXPECT_NE(Center(Normal, 0), Center(Rough, 0));
	EXPECT_NE(Center(Normal, 0), Center(Grazing, 0));
	EXPECT_EQ(Center(Normal, 3), Center(Disabled, 3));
	for (uint32 Channel = 0; Channel < 4; ++Channel)
		EXPECT_EQ(Center(Foreground, Channel), Center(ForegroundDisabled, Channel));
	EXPECT_NE(Foreground.HDR, ForegroundDisabled.HDR);
	const auto Evidence = std::filesystem::path(FPaths::RootDir()) / "Build/SingleLayerWater/Stage4";
	std::filesystem::create_directories(Evidence);
	for (const auto& [Name, Capture] : std::array{std::pair{"normal", &Normal}, std::pair{"rough", &Rough},
		std::pair{"grazing", &Grazing}, std::pair{"disabled", &Disabled}})
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Capture->HDR, Evidence / (std::string(Name) + ".rgba16f")));
}
#endif
