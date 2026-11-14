#include "MaterialRenderProxyUpdateTestSupport.h"
#include "WaterRenderingTestSupport.h"
#include "CoreMinimal.h"
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
#include "RendererModule.h"
#include "RenderResourcePreparation.h"
#include "Renderers/SceneRendererProfiling.h"
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
		auto& NormalRoot = Input.IR.SurfaceRoot.Inputs[size_t(EMaterialSurfaceOutput::Normal)];
		NormalRoot.bExpression = true; NormalRoot.ExpressionIndex = SurfaceNormal;
		Input.IR.SurfaceRoot.Inputs[size_t(EMaterialSurfaceOutput::BaseColor)].Literal = {.02f, .2f, .3f};
		Input.IR.SurfaceRoot.Inputs[size_t(EMaterialSurfaceOutput::Roughness)].Literal = {.25f};
		Input.IR.WorldPositionOffset.bExpression = Displace;
		Input.IR.WorldPositionOffset.ExpressionIndex = Offset;
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
