#include "ThumbnailManagerTestSupport.h"
#include "Engine/StreamableManager.h"
#include "Thumbnail/ThumbnailManager.h"
#include "ComponentPSOPrecacheTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "NativeAssetBuildTestSupport.h"
#include "RenderResource.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Actor.h"
#include "PSOPrecacheMaterial.h"
#include "VertexFactory.h"

#include "StaticMeshTestAccess.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "Materials/ExplicitMaterialProgramTestFixture.h"
#include "Threading/TaskRuntime.h"
#include "NativeAssetTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "Materials/MaterialTestSupport.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Texture/TextureFactoryTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "MonaUIBackend.h"
#include "MonaTestFixtures.h"
#include "NativeTestSupport.h"
#include "Preview/PreviewMeshResources.h"
#include "RHICommandList.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/ThumbnailCapture.h"
#include "Thumbnail/AssetThumbnailTestFixtures.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Thumbnail/TextureThumbnailRenderer.h"
#include "Preview/TexturePreview.h"
#include "Shader/ShaderMapPreparation.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/Shader.h"
#include "RenderResourceCreation.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Texture/TextureCubeRenderResource.h"
#include "AssetForge/Builtins/Texture2DImport.h"

#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"

namespace
{
	class FReadbackProbeInput final : public Durin::IAssetThumbnailGenerationInput {};

	class FReadbackProbeSession final : public Durin::IThumbnailRendererSession
	{
	public:
		explicit FReadbackProbeSession(uint32& InLoads) : Loads(InLoads) {}

		auto Update() -> Durin::FThumbnailRendererSessionUpdate override
		{
			if (!bStarted)
			{
				bStarted = true;
				++Loads;
				return {.State = Durin::EThumbnailRendererSessionState::WaitingForResources};
			}

			return {.State = Durin::EThumbnailRendererSessionState::WaitingForResources};
		}
		auto PreparePreview(Durin::IThumbnailRenderContext&) -> std::expected<void, std::string> override
		{
			return std::unexpected("The readback probe must never render.");
		}
		auto ValidatePreparedInput() const -> std::expected<void, std::string> override
		{
			return std::unexpected("The readback probe must never render.");
		}
		auto ResetPreview() -> void override {}
	private:
		bool bStarted = false;
		uint32& Loads;
	};

	class FReadbackProbeRenderer final : public Durin::DThumbnailRenderer
	{
		DURIN_TEST_THUMBNAIL_RENDERER(FReadbackProbeRenderer)
	public:
		auto Configure(uint32& InLoads) -> void { Loads = &InLoads; }
		auto GetRegistration() const -> Durin::FThumbnailRenderingInfo override
		{
			return {.AssetClassName = "DReadbackProbe", .RendererName = "ReadbackProbe", .GeneratorSchemaVersion = 1};
		}
		auto CaptureGenerationInput(const Durin::FAssetThumbnailPackageFingerprint& Asset) -> std::expected<Durin::FThumbnailGenerationData, std::string> override
		{
			Durin::FThumbnailGenerationData Result;
			Result.Input = std::make_shared<FReadbackProbeInput>();
			return Result;
		}
		auto CreateGenerationSession(const Durin::IAssetThumbnailGenerationInput&)
			-> std::unique_ptr<Durin::IThumbnailRendererSession> override
		{
			return std::make_unique<FReadbackProbeSession>(*Loads);
		}
	private:
		uint32* Loads = nullptr;
	};

	// Render completion precedes the task-scheduled GameThread publication receipt.
	// Pump until the consumer's state changes, including any queued successor upload.
	template<typename FPredicate>
	auto WaitForResourcePublication(FPredicate&& IsComplete) -> testing::AssertionResult
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (;;)
		{
			Durin::Tests::BeginThumbnailTestFrame();
			Durin::FlushRenderingCommands();
			Durin::PumpGameThreadDeferredWork();
			if (IsComplete()) return testing::AssertionSuccess();
			if (std::chrono::steady_clock::now() >= Deadline)
				return testing::AssertionFailure() << "Timed out waiting for render resource publication.";
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	class FThumbnailVulkanTests : public testing::Test
	{
	public:
		static auto SetUpTestSuite() -> void
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(Durin::InitializeTaskScheduler());
			ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
		}

		static auto TearDownTestSuite() -> void
		{
			// Compiler shutdown is terminal; keep its scheduler alive across cases.
			Durin::ShutdownAssetCompilingManager();
			Durin::ShutdownTaskSystem();
		}

	protected:
		auto SetUp() -> void override
		{
			Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
			Durin::FModuleManager::Get().LoadModuleChecked("TextureCompressor");
			InitializeDObjectSystem();
			Durin::FMountPaths::InitDefaultMountPoints();
			const auto Root = Durin::Testing::CreateTestFixtureDirectory("ThumbnailVulkan");
			std::vector<Durin::FMountPoint> Mounts(
				Durin::FMountPaths::GetRegisteredMountPoints().begin(),
				Durin::FMountPaths::GetRegisteredMountPoints().end()
			);
			Mounts.push_back({.VirtualRoot = "/ThumbnailVulkan/", .Owner = Durin::EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true});
			MountRegistry = std::make_unique<Durin::Testing::FScopedMountRegistryFixture>(Mounts);
			ASSERT_TRUE(MountRegistry->IsValid()) << MountRegistry->GetError();
			ASSERT_TRUE(Durin::RefreshAssetRegistry());
			ASSERT_EQ(Durin::GDynamicRHI, nullptr);
			Durin::InitRenderingThread();
			Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
			Durin::FModuleManager::Get().LoadModule("RenderCore");
			Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
			ASSERT_NE(Durin::GDynamicRHI, nullptr);
			Durin::InitRenderingThread();
			bStarted = true;
			RendererLifecycle.Start(Renderer);
			Engine.SetTestRendererModule(&Renderer);
			Durin::GEngine = &Engine;
			struct FBeginThumbnailFrame
			{
				static constexpr auto GetName() -> const char* { return "BeginThumbnailFrame"; }
			};
			Durin::EnqueueRenderCommand<FBeginThumbnailFrame>([](Durin::FRHICommandListImmediate& List) {
				List.SwitchPipeline(Durin::ERHIPipeline::Graphics);
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(List);
			});
			Contract.Output.Width = 64;
			Contract.Output.Height = 64;
		}

		auto TearDown() -> void override
		{
			if (!bStarted) return;
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			for (const auto& Path : Packages)
			{
				EXPECT_TRUE(Durin::Testing::EnsurePackageNotResident(Path, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
				EXPECT_TRUE(Durin::Testing::RemoveAssetPackageForTests(Path));
			}
			for (const auto Name : {Durin::FThumbnailVisualContract::SphereAssetPath, Durin::FPreviewMeshResources::BoxAssetPath})
			{
				Durin::FSoftObjectPath Path;
				if (Durin::FSoftObjectPath::TryCreate(Name, Path))
					(void)Durin::UnloadPackage(Path.GetPackagePath(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			}
			Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
			Durin::FPackagePath Studio;
			ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Engine/Renderer/ThumbnailStudioCube", Studio));
			(void)Durin::UnloadPackage(Studio, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
			struct FEndThumbnailFrame
			{
				static constexpr auto GetName() -> const char* { return "EndThumbnailFrame"; }
			};
			Durin::EnqueueRenderCommand<FEndThumbnailFrame>([](Durin::FRHICommandListImmediate& List) {
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(List);
			});
			Durin::FlushRenderingCommands();
			Durin::GEngine = nullptr;
			Durin::FTexturePreview::ReleaseSharedResources();
			RendererLifecycle.Shutdown();
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
			Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
			Durin::RHIExit();
		}

		auto Package(const char* Name) -> Durin::FPackagePath
		{
			Durin::FPackagePath Path;
			EXPECT_TRUE(Durin::FPackagePath::TryCreate(std::string("/ThumbnailVulkan/") + Name, Path));
			Packages.push_back(Path);
			return Path;
		}
		auto ImportTexture() -> Durin::DTexture2D*
		{
			const auto Source = Durin::Testing::GetTestWorkDirectory() / "Preview.png";
			WriteMaterialTextureFixture(Source);
			const auto Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
				Source.generic_string(), Package("T_Preview").ToString()
			);
			EXPECT_TRUE(Result) << Result.Message;
			return Result.Asset;
		}
		auto ImportCube() -> Durin::DTextureCube*
		{
			const auto Result = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
				Durin::Tests::GetThumbnailDirectionalCubeFaces(), Package("TC_Preview").ToString()
			);
			EXPECT_TRUE(Result) << Result.Message;
			if (Result.Asset) Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Result.Asset);
			return Result.Asset;
		}
		Durin::Testing::FScopedMountRegistryFixture SavedMountRegistry;
		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> MountRegistry;
		std::vector<Durin::FPackagePath> Packages;
		FMaterialTestEngine Engine;
		Durin::FRendererModule Renderer;
		Durin::FModuleTestHarness RendererLifecycle{"ThumbnailVulkanRenderer"};
		Durin::FThumbnailVisualContract Contract;
		std::string Error;
		bool bStarted = false;
	};
} // namespace

TEST_F(FThumbnailVulkanTests, PreparedShadersAreSharedAndCaptureNeverCompiles)
{
	using namespace Durin;
	ClearShaderMapPreparations();
	std::vector<const FShaderType*> Types;
	ASSERT_TRUE(GetShaderRuntimeRequestBuildTypes(
		{.Owner = "DurinEd", .Name = "DurinEd.TexturePreview"}, Types));
	const auto Before = GetShaderBuildStats();
	TRenderResourceCreationSlot<std::shared_ptr<FShaderMapBase>> Slot;
	const FRenderResourceGeneration Generation;
	uint32 Diagnostics = 0;
	auto Resolve = [&] {
		return Slot.Resolve(Generation, [&] {
			using FResult = TRenderResourceCreateResult<std::shared_ptr<FShaderMapBase>>;
			auto Map = std::make_shared<FShaderMapBase>();
			const auto Completion = InitializeShaderMapFromTypes(*Map, Types, {});
			if (Completion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
			if (Completion) return FResult::Success(std::move(Map));
			return FResult::Failure({.Category = ERenderResourceCreateErrorCategory::ShaderCompile,
				.Cause = Completion.Error});
		}, [&](const FRenderResourceCreateDiagnostic&) { ++Diagnostics; });
	};
	{
		FPreparedShaderMapScope Scope;
		EXPECT_EQ(Resolve(), nullptr);
		EXPECT_EQ(Slot.GetAvailability(), ERenderResourceAvailability::Pending);
		EXPECT_EQ(Diagnostics, 0u);
	}
	EXPECT_EQ(GetShaderBuildStats().Compilations, Before.Compilations);
	auto First = FShaderMapPreparation::Request("DurinEd", {"DurinEd.TexturePreview"});
	auto Second = FShaderMapPreparation::Request("DurinEd", {"DurinEd.TexturePreview"});
	ASSERT_TRUE(WaitForResourcePublication([&] {
		auto Ready = First.Poll();
		return !Ready || *Ready;
	}));
	ASSERT_TRUE(First.Poll().value_or(false));
	ASSERT_TRUE(Second.Poll().value_or(false));
	const auto Prepared = GetShaderBuildStats();
	EXPECT_LE(Prepared.DependencyResolutions - Before.DependencyResolutions, 1u);
	std::ranges::reverse(Types);
	{
		FPreparedShaderMapScope Scope;
		const auto* Map = Resolve();
		ASSERT_NE(Map, nullptr);
		for (const auto* Type : Types) EXPECT_NE((*Map)->GetShader(Type), nullptr);
		EXPECT_EQ(Diagnostics, 0u);
	}
	EXPECT_EQ(GetShaderBuildStats().DependencyResolutions, Prepared.DependencyResolutions);
	EXPECT_EQ(GetShaderBuildStats().Compilations, Prepared.Compilations);
	InvalidateShaderMapPreparations(true);
	ASSERT_TRUE(WaitForResourcePublication([&] { return First.Poll().value_or(false); }));
	EXPECT_GT(GetShaderBuildStats().Compilations, Prepared.Compilations);
	ClearShaderMapPreparations();
	auto Invalid = FShaderMapPreparation::Request("DurinEd", {"Missing.Program"});
	EXPECT_FALSE(Invalid.Poll().has_value());
}

TEST_F(FThumbnailVulkanTests, StaticMeshSessionWaitsForMaterialPublication)
{
	using namespace Durin;
	DStaticMesh* Mesh = nullptr;
	const auto MeshPath = Package("SM_MaterialPublication");
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MeshPath, Mesh));
	FMeshDescription Geometry;
	Geometry.PolygonGroups.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
	auto& Section = Geometry.Sections.emplace_back();
	Section.Name = "Tetrahedron";
	Section.Positions = {FVector3f(-0.6f, -0.5f, -0.4f), FVector3f(0.7f, -0.4f, -0.3f),
		FVector3f(0.0f, 0.8f, -0.2f), FVector3f(0.1f, 0.0f, 0.9f)};
	Section.Indices = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3};
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, std::move(Geometry)));
	Mesh->RequestRenderDataAndResources();
	auto Shaders = Renderer.PrepareSceneThumbnailShaders();
	ASSERT_TRUE(WaitForResourcePublication([&] {
		return Mesh->GetRenderResourceStatus().Readiness == EStaticMeshRenderResourceReadiness::Ready
			&& Shaders.Poll().value_or(false);
	}));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Package("M_MaterialPublication"), Material));
	ASSERT_TRUE(RequestMaterialRecompile(*Material, true));
	Mesh->SetMaterialSlotDefaultMaterial(0, Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(SavePackage(Mesh->GetPackage()));
	ASSERT_FALSE(Material->GetAcceptedCompiledProgram());

	const auto AssetPath = Testing::MakePackageLeafTopLevelAssetPathForTests(MeshPath);
	auto ExtensionOwner = Durin::MakeThumbnailRenderer<StaticMeshEditor::DStaticMeshThumbnailRenderer>();
	auto& Extension = *ExtensionOwner;
	StaticMeshEditor::FStaticMeshThumbnailRendererGenerationInput Input(AssetPath, Contract);
	auto Session = Extension.CreateGenerationSession(Input);
	ASSERT_TRUE(Session);
	EXPECT_EQ(Session->Update().State, EThumbnailRendererSessionState::WaitingForResources);
	auto Load = GetStreamableManager().RequestAsyncLoad(AssetPath, {}, DStaticMesh::StaticClass());
	// Deliberately withhold material completion publication, while mesh, global
	// shaders and asynchronous loading finish. This needs no sleeps or worker gate.
	ASSERT_TRUE(WaitForResourcePublication([&] {
		ProcessAsyncLoading();
		return Load->IsComplete();
	}));
	ASSERT_FALSE(Material->GetAcceptedCompiledProgram());
	const auto Pending = Session->Update();
	ASSERT_EQ(Pending.State, EThumbnailRendererSessionState::WaitingForResources) << Pending.Diagnostic;
	EXPECT_TRUE(Pending.Diagnostic.empty());

	FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
	ASSERT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());
	ASSERT_TRUE(WaitForResourcePublication([&] {
		return Session->Update().State == EThumbnailRendererSessionState::ReadyToRender;
	}));
	FThumbnailCapture Preview(Contract.Output);
	ASSERT_TRUE(Preview.IsAvailable()) << Preview.GetDiagnostic();
	ASSERT_TRUE(Session->PreparePreview(Preview));
	ASSERT_TRUE(Session->ValidatePreparedInput());
	ASSERT_TRUE(Preview.BeginCapture());
	std::expected<std::optional<FByteBuffer>, std::string> Capture;
	ASSERT_TRUE(WaitForResourcePublication([&] {
		Capture = Preview.PollCapture();
		return !Capture || Capture->has_value();
	}));
	ASSERT_TRUE(Capture) << Capture.error();
	ASSERT_TRUE(*Capture);
	EXPECT_EQ((**Capture).size(), Contract.Output.Width * Contract.Output.Height * 4u);
	// Cold view preparation may resolve a fixed Vertex Factory shader after the
	// material/global session is ready. Measure consumption after that boundary.
	const auto Before = GetShaderBuildStats();
	ASSERT_TRUE(Preview.BeginCapture());
	ASSERT_TRUE(WaitForResourcePublication([&] {
		Capture = Preview.PollCapture();
		return !Capture || Capture->has_value();
	}));
	ASSERT_TRUE(Capture) << Capture.error();
	ASSERT_TRUE(*Capture);
	EXPECT_EQ(GetShaderBuildStats().Compilations, Before.Compilations);
	EXPECT_EQ(GetShaderBuildStats().DependencyResolutions, Before.DependencyResolutions);
	auto Properties = Material->GetStaticProperties();
	Properties.bTwoSided = !Properties.bTwoSided;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	EXPECT_FALSE(Session->ValidatePreparedInput());
	// Cancellation of a recompile can retain a current accepted program. Use
	// a cold material to exercise a terminal failure with no renderable fallback.
	DMaterial* Uncompiled = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Package("M_CanceledThumbnail"), Uncompiled));
	ASSERT_TRUE(SavePackage(Uncompiled->GetPackage()));
	Mesh->SetMaterialSlotDefaultMaterial(0, Uncompiled);
	ASSERT_TRUE(RequestMaterialRecompile(*Uncompiled, true));
	ASSERT_FALSE(Uncompiled->GetMaterialCompileStatus().IsCurrent());
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Uncompiled);
	ASSERT_FALSE(Uncompiled->GetAcceptedCompiledProgram());
	const auto Canceled = Session->Update();
	EXPECT_EQ(Canceled.State, EThumbnailRendererSessionState::Failed);
	EXPECT_FALSE(Canceled.Diagnostic.empty());
	Session->ResetPreview();
	auto* PreviewActor = Extension.GetPreviewScene().GetPreviewActor();
	ASSERT_NE(PreviewActor, nullptr);
	auto* PreviewComponent = PreviewActor->FindComponentByExactClass<DStaticMeshComponent>();
	ASSERT_NE(PreviewComponent, nullptr);
	EXPECT_EQ(PreviewComponent->GetStaticMesh(), nullptr);
}

TEST_F(FThumbnailVulkanTests, ColdGenerationReadsBackOnceAndWarmCacheSkipsRendering)
{
	uint32 ProbeLoads = 0;
	auto ProbeRegistration = Durin::FThumbnailManagerTestAccess::RegisterScoped(Durin::GetDefaultThumbnailManager(),
		Durin::MakeThumbnailRenderer<FReadbackProbeRenderer>(ProbeLoads));
	ASSERT_TRUE(ProbeRegistration);
	Durin::FPackagePath ProbePackage;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/ThumbnailVulkan/ReadbackProbe", ProbePackage));
	const Durin::FAssetThumbnailPackageFingerprint Probe{
		.AssetPath = Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(ProbePackage),
		.PackagePath = ProbePackage, .AssetClassName = "DReadbackProbe"};
	auto Registration = Durin::GetDefaultThumbnailManager().RegisterCustomRenderer(Durin::DStaticMesh::StaticClass(), Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer::StaticClass());
	ASSERT_TRUE(Registration) << Registration.error();
	const auto StaticMeshFixturePath = Package("SM_Preview");
	const auto StaticMeshMaterialPath = Package("M_Preview");
	Durin::DStaticMesh* StaticMeshFixture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(
		StaticMeshFixturePath, StaticMeshFixture
	)) << Error;
	ASSERT_NE(StaticMeshFixture, nullptr);
	Durin::FMeshDescription ImportedMesh;
	ImportedMesh.PolygonGroups.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
	Durin::FMeshDescriptionSection& ImportedSection =
		ImportedMesh.Sections.emplace_back();
	ImportedSection.Name = "ThumbnailTetrahedron";
	ImportedSection.Positions = {
		Durin::FVector3f(-0.6f, -0.5f, -0.4f),
		Durin::FVector3f(0.7f, -0.4f, -0.3f),
		Durin::FVector3f(0.0f, 0.8f, -0.2f),
		Durin::FVector3f(0.1f, 0.0f, 0.9f)
	};
	ImportedSection.Indices = {
		0, 2, 1,
		0, 1, 3,
		1, 2, 3,
		2, 0, 3
	};
	ImportedSection.SourceMaterialIndex = 0;
	const auto SynchronousBuild1 = Durin::FStaticMeshTestAccess::Build(StaticMeshFixture, std::move(ImportedMesh));
	ASSERT_TRUE(SynchronousBuild1) << SynchronousBuild1.error();
	Durin::DMaterial* StaticMeshAssetMaterial = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(
		StaticMeshMaterialPath,
		StaticMeshAssetMaterial
	));
	ASSERT_NE(StaticMeshAssetMaterial, nullptr);
	auto Recipe = Durin::Testing::MakePBRMaterialExpressionsForTest();
	// Exercise environment inputs through shader compilation, vertex linkage and thumbnail rendering.
	Durin::TStrongObjectPtr<Durin::DMaterialExpressionWorldPosition> Position(Durin::NewObject<Durin::DMaterialExpressionWorldPosition>(nullptr, Durin::NAME_None));
	Durin::TStrongObjectPtr<Durin::DMaterialExpressionSwizzle> PositionRgb(Durin::NewObject<Durin::DMaterialExpressionSwizzle>(nullptr, Durin::NAME_None));
	Durin::TStrongObjectPtr<Durin::DMaterialExpressionTime> Time(Durin::NewObject<Durin::DMaterialExpressionTime>(nullptr, Durin::NAME_None));
	Position->Id = Durin::FGuid::NewGuid(); PositionRgb->Id = Durin::FGuid::NewGuid(); Time->Id = Durin::FGuid::NewGuid();
	PositionRgb->Input.Connection = {Position->Id};
	// A non-identity swizzle intentionally converts the spatial Position into
	// ordinary color data before it reaches the Emissive terminal.
	PositionRgb->Components = {0, 1, 0};
	Recipe.Expressions.emplace_back(Position.Get()); Recipe.Expressions.emplace_back(PositionRgb.Get()); Recipe.Expressions.emplace_back(Time.Get());
	Recipe.Outputs.Emissive = {PositionRgb->Id}; Recipe.Outputs.Roughness = {Time->Id};
	ASSERT_TRUE(Recipe.Apply(*StaticMeshAssetMaterial));
	ASSERT_TRUE(StaticMeshAssetMaterial->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.85, 0.12, 0.18)
	));
	ASSERT_TRUE(Durin::SavePackage(StaticMeshAssetMaterial->GetPackage()));
	StaticMeshFixture->SetMaterialSlotDefaultMaterial(0, StaticMeshAssetMaterial);
	ASSERT_TRUE(Durin::SavePackage(StaticMeshFixture->GetPackage()));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation
	));
	const Durin::FPackageCatalogEntry StaticMeshAssetData =
		Durin::FindPackageExact(StaticMeshFixturePath);
	ASSERT_NE(StaticMeshAssetData, nullptr);
	const auto StaticMeshExport = Durin::FindAssetExact(
		Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(StaticMeshFixturePath));
	ASSERT_TRUE(StaticMeshExport);
	const Durin::FAssetThumbnailPackageFingerprint StaticMeshFingerprint = {
		.AssetPath = Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(
			StaticMeshAssetData->PackagePath
		),
		.PackagePath = StaticMeshAssetData->PackagePath,
		.AssetClassName = StaticMeshExport.Asset->AssetClassName,
		.PackageFormatVersion = StaticMeshAssetData->FormatVersion,
		.FileSize = static_cast<uint64>(StaticMeshAssetData->FileSize),
		.LastWriteTimeTicks = StaticMeshAssetData->LastWriteTimeTicks
	};
	const std::filesystem::path ThumbnailCacheRoot =
		Durin::Testing::GetTestWorkDirectory() / "StaticMeshRenderedCacheVulkan";
	Durin::Testing::RemoveTestWorkDirectory(ThumbnailCacheRoot);
	ASSERT_EQ(Durin::Mona::GetActiveUIBackend(), nullptr);
	Durin::Tests::FThumbnailTestUIBackend ThumbnailUIBackend;
	Durin::Tests::FScopedActiveUIBackend ThumbnailBackendScope(ThumbnailUIBackend);
	auto PumpCacheToReady = [&](Durin::FAssetThumbnailPool& Cache) {
		Durin::FAssetThumbnailView View;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Durin::PumpGameThreadDeferredWork();
			Durin::FAssetCompilingManager::Get().ProcessAsyncTasks(true);
			Durin::ProcessAsyncLoading();
			Durin::Tests::BeginThumbnailTestFrame();
			Cache.BeginFrame();
			Cache.Request(
				StaticMeshFingerprint,
				Durin::EAssetThumbnailPriority::Visible
			);
			View = Cache.Find(StaticMeshFingerprint.AssetPath);
			Cache.EndFrame();
			if (ProbeLoads == 0 && Cache.GetStats().Generation.Renders == 1)
			{
				// Do not poll capture completion: a real render/readback owns the
				// slot throughout this interval. Other rendered assets must still load.
				Cache.AddReferencer(Probe.AssetPath);
				Cache.Request(Probe, Durin::EAssetThumbnailPriority::Visible);
				const auto ProbeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (ProbeLoads == 0 && std::chrono::steady_clock::now() < ProbeDeadline)
				{
					Cache.EndFrame();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				EXPECT_EQ(ProbeLoads, 1u);
				EXPECT_EQ(Cache.GetStats().Generation.Readbacks, 0u);
				Cache.RemoveReferencer(Probe.AssetPath);
			}
			Durin::FlushRenderingCommands();
			std::this_thread::yield();
			if (View.State == Durin::EAssetThumbnailState::Ready
				&& View.Texture != nullptr)
				break;
		}
		return View;
	};
	{
		// This fixture pumps faster than real frames; its wall-clock deadline owns timeout.
		Durin::FAssetThumbnailPool Cache({.MaximumResourceWaitFrames = 0}, {.CacheRoot = ThumbnailCacheRoot, .ObjectExtension = ".png"});
		const Durin::FAssetThumbnailView Ready = PumpCacheToReady(Cache);
		ASSERT_EQ(Ready.State, Durin::EAssetThumbnailState::Ready)
			<< Ready.Diagnostic;
		ASSERT_NE(Ready.Texture, nullptr);

		const Durin::FAssetThumbnailPoolStats Stats = Cache.GetStats();
		EXPECT_EQ(Stats.Generation.Loads, 2u);
		EXPECT_EQ(Stats.Generation.Renders, 1u);
		EXPECT_EQ(Stats.Generation.Readbacks, 1u);
		EXPECT_EQ(Stats.Generation.DiskHits, 0u);
		EXPECT_EQ(Stats.CaptureOutputCreations, 1u);
		EXPECT_EQ(Stats.CapturesStarted, 1u);
		EXPECT_EQ(Stats.UploadsCompleted, 1u);
		EXPECT_EQ(Stats.LiveGpuTextures, 1u);
		EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 1u);
		// Display readiness no longer implies persistent publication. Let the
		// optional save finish before clearing requests and testing a warm reopen.
		const auto SaveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (Cache.GetStats().Generation.CacheWrites == 0
			   && std::chrono::steady_clock::now() < SaveDeadline)
		{
			Durin::ProcessAsyncLoading();
			Durin::Tests::BeginThumbnailTestFrame();
			Cache.BeginFrame();
			Cache.EndFrame();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ASSERT_EQ(Cache.GetStats().Generation.CacheWrites, 1u);
		Cache.Clear();
		EXPECT_EQ(Cache.GetStats().LiveGpuTextures, 0u);
		EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 0u);
	}
	{
		Durin::FAssetThumbnailPool WarmCache({.MaximumResourceWaitFrames = 0}, {.CacheRoot = ThumbnailCacheRoot, .ObjectExtension = ".png"});
		const Durin::FAssetThumbnailView Ready = PumpCacheToReady(WarmCache);
		ASSERT_EQ(Ready.State, Durin::EAssetThumbnailState::Ready)
			<< Ready.Diagnostic;
		const Durin::FAssetThumbnailPoolStats Stats = WarmCache.GetStats();
		EXPECT_EQ(Stats.Generation.DiskHits, 1u);
		EXPECT_EQ(Stats.Generation.Loads, 0u);
		EXPECT_EQ(Stats.Generation.Renders, 0u);
		EXPECT_EQ(Stats.Generation.Readbacks, 0u);
		EXPECT_EQ(Stats.CaptureOutputCreations, 0u);
		EXPECT_EQ(Stats.CapturesStarted, 0u);
		EXPECT_EQ(Stats.UploadsCompleted, 1u);

		WarmCache.CancelPendingRequests();
		const Durin::FAssetThumbnailView Retained =
			WarmCache.Find(StaticMeshFingerprint.AssetPath);
		EXPECT_EQ(Retained.State, Durin::EAssetThumbnailState::Ready);
		EXPECT_EQ(Retained.Texture, Ready.Texture);
		WarmCache.BeginFrame();
		WarmCache.Request(
			StaticMeshFingerprint,
			Durin::EAssetThumbnailPriority::Visible
		);
		const Durin::FAssetThumbnailView Revisited =
			WarmCache.Find(StaticMeshFingerprint.AssetPath);
		WarmCache.EndFrame();
		EXPECT_EQ(Revisited.State, Durin::EAssetThumbnailState::Ready);
		EXPECT_EQ(Revisited.Texture, Ready.Texture);
		const Durin::FAssetThumbnailPoolStats RevisitedStats =
			WarmCache.GetStats();
		EXPECT_EQ(RevisitedStats.Generation.DiskHits, Stats.Generation.DiskHits);
		EXPECT_EQ(RevisitedStats.UploadsQueued, Stats.UploadsQueued);
		WarmCache.Clear();
	}
}

TEST_F(FThumbnailVulkanTests, EnvironmentValidationAndCancellationReleaseReferences)
{
	auto* Texture = ImportTexture();
	ASSERT_NE(Texture, nullptr);
	auto* Cube = ImportCube();
	ASSERT_NE(Cube, nullptr);
	auto CaptureCubeReference = Cube->GetTextureReferenceRHI();
	Durin::Tests::FAssetThumbnailTestPool Pool(Contract);
	ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
	Durin::FRHITextureReferenceRef Texture2DReference =
		Texture->GetTextureReferenceRHI();
	const Durin::FViewEnvironmentOverride CubeEnvironment{
		.TextureReference = CaptureCubeReference
	};
	const Durin::FViewEnvironmentOverride Texture2DEnvironment{
		.TextureReference = Texture2DReference
	};
	const uint32 CubeReferenceBaseline =
		CaptureCubeReference->GetRefCount();
	const uint32 Texture2DReferenceBaseline =
		Texture2DReference->GetRefCount();
	Pool.SetViewEnvironment(CubeEnvironment);
	Pool.SetView();
	EXPECT_EQ(
		CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u
	);
	Pool.SetViewEnvironment(Texture2DEnvironment);
	EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
	EXPECT_EQ(
		Texture2DReference->GetRefCount(), Texture2DReferenceBaseline + 1u
	);
	Pool.Reset();
	EXPECT_EQ(
		Texture2DReference->GetRefCount(), Texture2DReferenceBaseline
	);
	Pool.Reset();
	EXPECT_EQ(
		Texture2DReference->GetRefCount(), Texture2DReferenceBaseline
	);
	Pool.SetView();
	Pool.SetViewEnvironment(CubeEnvironment);
	EXPECT_EQ(
		CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u
	);
	Pool.Reset();
	EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);

	Durin::FTextureRHIRef OriginalCubeTarget;

	struct FRestoreThumbnailEnvironment
	{
		static constexpr auto GetName() -> const char*
		{
			return "RestoreThumbnailEnvironment";
		}
	};
	auto RestoreCubeTarget = [&] {
		Durin::EnqueueRenderCommand<FRestoreThumbnailEnvironment>(
			[Reference = CaptureCubeReference, OriginalCubeTarget](
				Durin::FRHICommandListImmediate&
			) {
				Durin::GDynamicRHI->RHIUpdateTextureReference(
					Reference.GetReference(), OriginalCubeTarget.GetReference()
				);
			}
		);
		Durin::FlushRenderingCommands();
	};

	Pool.SetViewEnvironment(CubeEnvironment);
	struct FClearThumbnailEnvironment
	{
		static constexpr auto GetName() -> const char*
		{
			return "ClearThumbnailEnvironment";
		}
	};
	Durin::EnqueueRenderCommand<FClearThumbnailEnvironment>(
		[Reference = CaptureCubeReference, &OriginalCubeTarget](
			Durin::FRHICommandListImmediate&
		) {
			OriginalCubeTarget = Reference->GetReferencedTexture_RenderThread();
			Durin::GDynamicRHI->RHIUpdateTextureReference(
				Reference.GetReference(), nullptr
			);
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Pool.BeginCapture());
	Durin::FlushRenderingCommands();
	Durin::FByteBuffer UnavailableEnvironmentPixels;
	EXPECT_EQ(
		Pool.FinishCapture(UnavailableEnvironmentPixels, Error),
		Durin::EThumbnailCaptureState::Failed
	);
	EXPECT_TRUE(UnavailableEnvironmentPixels.empty());
	EXPECT_NE(Error.find("view environment"), std::string::npos);
	Pool.Reset();
	RestoreCubeTarget();

	Pool.SetViewEnvironment(CubeEnvironment);
	ASSERT_TRUE(Pool.BeginCapture());
	Durin::FlushRenderingCommands();
	Durin::FByteBuffer RecoveredReadback;
	ASSERT_EQ(
		Pool.FinishCapture(RecoveredReadback, Error),
		Durin::EThumbnailCaptureState::Ready
	) << Error;
	EXPECT_EQ(RecoveredReadback.size(), 64u * 64u * 4u);
	Pool.Reset();

	Pool.SetViewEnvironment(Texture2DEnvironment);
	ASSERT_TRUE(Pool.BeginCapture());
	Durin::FlushRenderingCommands();
	Durin::FByteBuffer FailedEnvironmentPixels;
	EXPECT_EQ(
		Pool.FinishCapture(FailedEnvironmentPixels, Error),
		Durin::EThumbnailCaptureState::Failed
	);
	EXPECT_TRUE(FailedEnvironmentPixels.empty());
	EXPECT_NE(Error.find("view environment"), std::string::npos);
	Pool.Reset();

	Pool.SetViewEnvironment(CubeEnvironment);
	struct FThumbnailCancellationGate
	{
		std::mutex Mutex;
		std::condition_variable Condition;
		bool bEntered = false;
		bool bReleased = false;
	};
	struct FBlockThumbnailCaptureForCancellation
	{
		static constexpr auto GetName() -> const char*
		{
			return "BlockThumbnailCaptureForCancellation";
		}
	};
	const auto Gate = std::make_shared<FThumbnailCancellationGate>();
	Durin::EnqueueRenderCommand<FBlockThumbnailCaptureForCancellation>(
		[Gate](Durin::FRHICommandListImmediate&) {
			std::unique_lock Lock(Gate->Mutex);
			Gate->bEntered = true;
			Gate->Condition.notify_all();
			Gate->Condition.wait(Lock, [&Gate] { return Gate->bReleased; });
		}
	);
	{
		std::unique_lock Lock(Gate->Mutex);
		Gate->Condition.wait(Lock, [&Gate] { return Gate->bEntered; });
	}
	const bool bCancelledCaptureStarted = Pool.BeginCapture().has_value();
	Pool.Reset();
	const uint32 QueuedReferenceCount =
		CaptureCubeReference->GetRefCount();
	{
		std::lock_guard Lock(Gate->Mutex);
		Gate->bReleased = true;
	}
	Gate->Condition.notify_all();
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(bCancelledCaptureStarted) << Error;
	EXPECT_GT(QueuedReferenceCount, CubeReferenceBaseline);
	EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
	const auto Cancelled = Pool.PollCapture();
	ASSERT_TRUE(Cancelled);
	EXPECT_FALSE(Cancelled->has_value());
}

TEST_F(FThumbnailVulkanTests, Texture2DThumbnailUsesBuiltNormalAndRejectsReplacedAllocation)
{
	using namespace Durin;
	using namespace Durin;
	using namespace Durin::TextureEditor;
	auto* Asset = ImportTexture();
	ASSERT_NE(Asset, nullptr);
	FAssetCompilingManager::Get().FinishCompilationForObject(*Asset);
	Asset->SetSource({});
	Asset->SetBuildSettings(ETextureUsage::Normal, false, 0, ETextureCompressionQuality::Normal,
		ETextureAlphaMipMode::Average, 0.5f);
	// No source art is present: the built BC5 allocation is the only preview input.
	auto Platform = std::make_unique<FTexturePlatformData>();
	Platform->PixelFormat = EPixelFormat::BC5_UNORM;
	FTexture2DMipData Mip;
	Mip.Width = Mip.Height = 4;
	Mip.RowPitch = 16;
	FByteBuffer MipPixels(16, std::byte{0});
	MipPixels[0] = MipPixels[1] = MipPixels[8] = MipPixels[9] = std::byte{128};
	Mip.Pixels = FSharedByteBuffer::Take(std::move(MipPixels));
	Platform->Mips.push_back(std::move(Mip));
	Asset->SetPlatformData(std::move(Platform));
	Asset->UpdateResource();
	ASSERT_TRUE(WaitForResourcePublication([&] { return !Asset->HasPendingRenderResourceInitialization(); }));
	ASSERT_NE(Asset->GetResource(), nullptr);
	ASSERT_TRUE(Asset->GetResource()->GetTextureRHI_GameThread());
	ASSERT_FALSE(Asset->GetSource().IsValid());
	const auto Data = FindPackageExact(Packages.back());
	ASSERT_TRUE(Data);
	const auto AssetData = FindAssetExact(Testing::MakePackageLeafTopLevelAssetPathForTests(Packages.back()));
	ASSERT_TRUE(AssetData);
	FAssetThumbnailRequest Request;
	Request.Asset = {
		.AssetPath = Testing::MakePackageLeafTopLevelAssetPathForTests(Packages.back()),
		.PackagePath = Packages.back(), .AssetClassName = AssetData.Asset->AssetClassName,
		.PackageFormatVersion = Data->FormatVersion, .FileSize = static_cast<uint64>(Data->FileSize),
		.LastWriteTimeTicks = Data->LastWriteTimeTicks};
	auto RendererOwner = Durin::MakeThumbnailRenderer<DTextureThumbnailRenderer>();
	auto& Renderer = *RendererOwner;
	FThumbnailGenerationData Captured;
	auto CapturedResult = Renderer.CaptureGenerationInput(Request.Asset);
	ASSERT_TRUE(CapturedResult) << CapturedResult.error();
	Captured = std::move(*CapturedResult);
	ASSERT_NE(Captured.Input, nullptr);
	auto Session = Renderer.CreateGenerationSession(*Captured.Input);
	ASSERT_TRUE(Session);
	ASSERT_NE(Session, nullptr);
	const auto Loaded = Session->Update();
	ASSERT_EQ(Loaded.State, EThumbnailRendererSessionState::WaitingForResources);
	FThumbnailRendererSessionUpdate Ready;
	ASSERT_TRUE(WaitForResourcePublication([&] {
		Durin::ProcessAsyncLoading();
		Ready = Session->Update();
		return Ready.State != EThumbnailRendererSessionState::WaitingForResources;
	}));
	ASSERT_EQ(Ready.State, EThumbnailRendererSessionState::ReadyToRender) << Ready.Diagnostic;
	FThumbnailCapture Pool(Contract.Output);
	ASSERT_TRUE(Session->PreparePreview(Pool)) << Error;
	ASSERT_TRUE(Session->ValidatePreparedInput()) << Error;
	ASSERT_TRUE(Pool.BeginCapture());
	FlushRenderingCommands();
	FByteBuffer Pixels;
	EThumbnailCaptureState CaptureState{};
	ASSERT_TRUE(WaitForResourcePublication([&] {
		auto Capture = Pool.PollCapture();
		CaptureState = !Capture ? EThumbnailCaptureState::Failed
			: Capture->has_value() ? EThumbnailCaptureState::Ready : EThumbnailCaptureState::ReadbackPending;
		if (!Capture) Error = Capture.error();
		else if (Capture->has_value()) Pixels = std::move(**Capture);
		return CaptureState == EThumbnailCaptureState::Ready || CaptureState == EThumbnailCaptureState::Failed;
	}));
	ASSERT_EQ(CaptureState, EThumbnailCaptureState::Ready) << Error;
	ASSERT_EQ(Pixels.size(), 64u * 64u * 4u);
	const auto Consumed = Pool.PollCapture();
	ASSERT_TRUE(Consumed);
	EXPECT_FALSE(Consumed->has_value());
	const size_t Center = (32 * 64 + 32) * 4;
	EXPECT_NEAR(std::to_integer<int>(Pixels[Center]), 128, 1);
	EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 1]), 128, 1);
	EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 2]), 255, 1);
	EXPECT_EQ(Pixels[Center + 3], std::byte{255});
	Pool.Reset();
	// Exercise the complete cold/warm scheduler path, not only the renderer session.
	auto Registration = GetDefaultThumbnailManager().RegisterCustomRenderer(DTexture2D::StaticClass(), DTextureThumbnailRenderer::StaticClass());
	ASSERT_TRUE(Registration) << Registration.error();
	Tests::FThumbnailTestUIBackend Backend;
	Tests::FScopedActiveUIBackend BackendScope(Backend);
	const auto CacheRoot = Testing::GetTestWorkDirectory() / "BuiltTextureThumbnailCache";
	for (const bool Warm : {false, true})
	{
		FAssetThumbnailPool Cache({}, {.CacheRoot = CacheRoot, .ObjectExtension = ".png"});
		FAssetThumbnailView View;
		ASSERT_TRUE(WaitForResourcePublication([&] {
			Durin::ProcessAsyncLoading();
			Durin::Tests::BeginThumbnailTestFrame();
			Cache.BeginFrame();
			Cache.Request(Request.Asset, EAssetThumbnailPriority::Visible);
			View = Cache.Find(Request.Asset.AssetPath);
			Cache.EndFrame();
			return View.State == EAssetThumbnailState::Ready || View.State == EAssetThumbnailState::Failed;
		}));
		ASSERT_EQ(View.State, EAssetThumbnailState::Ready) << View.Diagnostic;
		ASSERT_NE(View.Texture, nullptr);
		const auto Stats = Cache.GetStats().Generation;
		EXPECT_EQ(Stats.Renders, Warm ? 0u : 1u);
		EXPECT_EQ(Stats.Readbacks, Warm ? 0u : 1u);
		EXPECT_EQ(Stats.Loads, Warm ? 0u : 1u);
		EXPECT_EQ(Stats.DiskHits, Warm ? 1u : 0u);
		if (!Warm)
			ASSERT_TRUE(WaitForResourcePublication([&] {
				Durin::ProcessAsyncLoading();
				Durin::Tests::BeginThumbnailTestFrame();
			Cache.BeginFrame(); Cache.EndFrame();
				return Cache.GetStats().Generation.CacheWrites == 1;
			}));
	}

	Asset->UpdateResource();
	EXPECT_FALSE(Session->ValidatePreparedInput());
	ASSERT_TRUE(WaitForResourcePublication([&] { return !Asset->HasPendingRenderResourceInitialization(); }));
	EXPECT_FALSE(Session->ValidatePreparedInput());
	Session->ResetPreview();
	EXPECT_FALSE(Session->ValidatePreparedInput());
}

TEST_F(FThumbnailVulkanTests, TexturePreviewPreservesRawChannelsSrgbAndAspectRatio)
{
	using namespace Durin;
	using namespace Durin;
	using namespace Durin::TextureEditor;
	FTextureRHIRef Input;
	ENQUEUE_RENDER_COMMAND(CreatePreviewTestInput)([&Input](FRHICommandListImmediate& Commands) {
		auto Desc = FRHITextureCreateDesc::Create2D("PreviewTestInput", 2, 1, EPixelFormat::SRGBA8_UNORM);
		Desc.AddFlags(ETextureCreateFlags::ShaderResource);
		Input = GDynamicRHI->RHICreateTexture(Commands, Desc);
		const FByteBuffer Pixels{std::byte{128}, std::byte{64}, std::byte{32}, std::byte{128},
			std::byte{128}, std::byte{64}, std::byte{32}, std::byte{128}};
		if (Input)
		{
			Commands.UpdateTexture2D(Input, 0, 0,
				FUpdateTextureRegion2D(0, 0, 0, 0, 2, 1), 8, Pixels);
			Commands.TransitionTextures(std::array{Durin::FRHITextureTransition{
				Input, {Durin::ERHITextureAspect::Color, 0, 1, 0, 1},
				Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}});
		}
	});
	FlushRenderingCommands();
	ASSERT_NE(Input, nullptr);
	FThumbnailCapture Pool(Contract.Output);
	for (const auto Channel : {ETexturePreviewChannel::RGBA, ETexturePreviewChannel::Blue, ETexturePreviewChannel::Alpha})
	{
		Pool.SetImageRenderer([Input, Channel](FRHICommandListImmediate& Commands, uint32 W, uint32 H) {
			const FTexturePreviewOptions Options{
				.Usage = ETextureUsage::Normal, .Interpretation = Channel == ETexturePreviewChannel::RGBA
					? ETexturePreviewInterpretation::Raw : ETexturePreviewInterpretation::Auto, .Channel = Channel};
			auto Capture = RenderTexturePreview(Commands, Input, W, H, Options, true);
			if (!Capture) return Capture; // Resource preparation may still be pending.
			EXPECT_TRUE(EnumHasAnyFlags(Capture->GetFlags(), ETextureCreateFlags::CPUReadback));
			auto Interactive = RenderTexturePreview(Commands, Input, W, H, Options);
			EXPECT_NE(Interactive, nullptr);
			if (Interactive) EXPECT_FALSE(EnumHasAnyFlags(Interactive->GetFlags(), ETextureCreateFlags::CPUReadback));
			return Capture;
		});
		ASSERT_TRUE(Pool.BeginCapture());
		FlushRenderingCommands();
		FByteBuffer Pixels;
		EThumbnailCaptureState CaptureState{};
		ASSERT_TRUE(WaitForResourcePublication([&] {
			auto Capture = Pool.PollCapture();
			CaptureState = !Capture ? EThumbnailCaptureState::Failed
				: Capture->has_value() ? EThumbnailCaptureState::Ready : EThumbnailCaptureState::ReadbackPending;
			if (!Capture) Error = Capture.error();
			else if (Capture->has_value()) Pixels = std::move(**Capture);
			return CaptureState == EThumbnailCaptureState::Ready || CaptureState == EThumbnailCaptureState::Failed;
		}));
		ASSERT_EQ(CaptureState, EThumbnailCaptureState::Ready) << Error;
		ASSERT_EQ(Pixels.size(), 64u * 64u * 4u);
		EXPECT_EQ(Pixels[3], std::byte{0}); // Aspect-fit margin stays transparent.
		const size_t Center = (32 * 64 + 32) * 4;
		const int Expected = Channel == ETexturePreviewChannel::Blue ? 32 : 128;
		EXPECT_NEAR(std::to_integer<int>(Pixels[Center]), Expected, 1);
		EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 1]), Channel == ETexturePreviewChannel::RGBA ? 64 : Expected, 1);
		EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 2]), Channel == ETexturePreviewChannel::RGBA ? 32 : Expected, 1);
		EXPECT_EQ(Pixels[Center + 3], Channel == ETexturePreviewChannel::RGBA ? std::byte{128} : std::byte{255});
		Pool.Reset();
	}
}

TEST_F(FThumbnailVulkanTests, CaptureWaitsForOptionalResourcesInsteadOfPublishingInteractiveFallback)
{
    using namespace Durin;
    FThumbnailPreviewScene Scene(Contract);
    FThumbnailCapture Pool(Contract.Output);
    ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
    ASSERT_TRUE(Pool.SetScene(Scene));
    ASSERT_TRUE(Pool.BeginCapture());
    FByteBuffer WarmPixels;
    std::string Error;
    ASSERT_EQ(Tests::FinishThumbnailCapture(Pool, WarmPixels, Error), EThumbnailCaptureState::Ready) << Error;
    ASSERT_FALSE(WarmPixels.empty());

    auto Policy = std::make_shared<std::unique_ptr<FScopedRendererQualificationPolicy>>();
    EnqueueRenderCommand("HoldThumbnailOptionalResource", [Policy](FRHICommandListImmediate&) {
        *Policy = std::make_unique<FScopedRendererQualificationPolicy>(FRendererQualificationPolicy{
            .bEnableGroundTruthAmbientOcclusion = true,
            .PendingFeature = EScenePreparationPending::AmbientOcclusion});
    });
    FlushRenderingCommands();
    ASSERT_TRUE(Pool.SetScene(Scene));
    EXPECT_TRUE(Pool.BeginCapture());
    for (uint32 Pump = 0; Pump < 8; ++Pump)
    {
        FlushRenderingCommands();
        auto Pending = Pool.PollCapture();
        EXPECT_TRUE(Pending.has_value());
        if (Pending) EXPECT_FALSE(Pending->has_value());
    }
    // Restore on the render thread even when a preceding expectation fails.
    EnqueueRenderCommand("ReleaseThumbnailOptionalResource", [Policy](FRHICommandListImmediate&) { Policy->reset(); });
    FlushRenderingCommands();
    FByteBuffer CompletePixels;
    EXPECT_EQ(Tests::FinishThumbnailCapture(Pool, CompletePixels, Error), EThumbnailCaptureState::Ready) << Error;
    EXPECT_FALSE(CompletePixels.empty());
}

TEST_F(FThumbnailVulkanTests, CaptureSucceedsWithComponentPSOPrecachingDisabled)
{
	using namespace Durin;
	Tests::FScopedComponentPSOPrecacheMode PrecacheMode(EComponentPSOPrecacheMode::Disabled);
	FThumbnailPreviewScene Scene(Contract);
	FThumbnailCapture Pool(Contract.Output);
	ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
	auto* Actor = Scene.GetPreviewActor();
	ASSERT_NE(Actor, nullptr);
	auto* Component = Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
		DStaticMeshComponent::StaticClass(), "DisabledPSOPrecacheComponent"));
	ASSERT_NE(Component, nullptr);
	ASSERT_TRUE(Component->IsVisible());
	ASSERT_FALSE(Component->GetPSOPrecacheComponentData().IsReadyForProxyCreation());
	ASSERT_TRUE(Pool.SetScene(Scene));
	ASSERT_TRUE(Pool.BeginCapture());
	FByteBuffer Pixels;
	std::string Error;
	ASSERT_EQ(Tests::FinishThumbnailCapture(Pool, Pixels, Error), EThumbnailCaptureState::Ready) << Error;
	EXPECT_FALSE(Pixels.empty());
	EXPECT_FALSE(Component->GetPSOPrecacheComponentData().IsPSOPrecacheCalled());
}

TEST_F(FThumbnailVulkanTests, CaptureDoesNotWaitForUnrelatedOptionalComponentPSOs)
{
	Durin::Tests::FScopedComponentPSOPrecacheMode PrecacheMode(Durin::EComponentPSOPrecacheMode::Enabled);
    using namespace Durin;
    static const FVertexFactoryType Factory("ThumbnailOptionalPSOFixture",
        EVertexFactoryFlags::SupportsPSOPrecaching);
    FVertexDeclarationElementList Elements{};
    Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 12);
    FMaterialPSOPrecacheParams Params;
    Params.VertexFactoryData = FPSOPrecacheVertexFactoryData(&Factory, Elements);
    FMaterialPSORequestManager OptionalWork;
    auto Request = OptionalWork.PrecachePSOs(Params);
    OptionalWork.Pump([](const auto&, auto& Out, EPSOPrecacheCollectionPhase) -> std::expected<bool, std::string> {
        Out.bRequiredPSOsCollected = true;
        return false;
    });
    ASSERT_TRUE(Request.GetProgress().bRequiredReady);
    ASSERT_EQ(Request.GetProgress().State, EMaterialPSOPrecacheState::Pending);

    FThumbnailPreviewScene Scene(Contract);

    FThumbnailCapture Pool(Contract.Output);
    ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
    auto* Actor = Scene.GetPreviewActor();
    ASSERT_NE(Actor, nullptr);
    auto* Component = Cast<DPrimitiveComponent>(Actor->AddInstanceComponent(DPrimitiveComponent::StaticClass()));
    ASSERT_NE(Component, nullptr);
    Component->SetMaterialPSOPrecacheData({Request});
    ASSERT_TRUE(Component->IsPSOPrecaching());
    ASSERT_TRUE(Pool.SetScene(Scene));
    ASSERT_TRUE(Pool.BeginCapture());
    FByteBuffer Pixels;
    std::string Error;
    EXPECT_EQ(Tests::FinishThumbnailCapture(Pool, Pixels, Error), EThumbnailCaptureState::Ready) << Error;
    EXPECT_FALSE(Pixels.empty());
    EXPECT_TRUE(Component->IsPSOPrecaching());
    Component->GetPSOPrecacheComponentData().Reset();
}

TEST_F(FThumbnailVulkanTests, RendererOwnsReusableSceneAndCaptureOnlyBorrowsIt)
{
    using namespace Durin;
    auto First = MakeThumbnailRenderer<StaticMeshEditor::DStaticMeshThumbnailRenderer>();
    auto Second = MakeThumbnailRenderer<StaticMeshEditor::DStaticMeshThumbnailRenderer>();
    auto& Scene = First->GetPreviewScene();
    auto& OtherScene = Second->GetPreviewScene();
    ASSERT_TRUE(Scene.IsAvailable()) << Scene.GetDiagnostic();
    ASSERT_TRUE(OtherScene.IsAvailable()) << OtherScene.GetDiagnostic();
    EXPECT_NE(Scene.GetWorld(), OtherScene.GetWorld());
    auto* Actor = Scene.GetPreviewActor();
    ASSERT_NE(Actor, nullptr);
    TWeakObjectPtr<DWorld> World(Scene.GetWorld());
    FThumbnailCapture Capture(Contract.Output);
    FThumbnailCapture OtherCapture(Contract.Output);
    ASSERT_TRUE(Capture.SetScene(Scene));
    EXPECT_FALSE(OtherCapture.SetScene(Scene));
    ASSERT_TRUE(OtherCapture.SetScene(OtherScene));
    Capture.Reset();
    EXPECT_EQ(Scene.GetPreviewActor(), Actor);
    OtherCapture.Reset();
    ASSERT_TRUE(OtherCapture.SetScene(Scene));
    OtherCapture.Reset();
    First.Reset();
    CollectGarbage(EObjectFlags::Standalone);
    EXPECT_FALSE(World.IsValid());
    EXPECT_TRUE(OtherScene.IsAvailable());
}

TEST_F(FThumbnailVulkanTests, CaptureValidatesConfigurationAtSubmission)
{
	using namespace Durin;
	FThumbnailPreviewScene Scene(Contract);
	FThumbnailCapture Capture(Contract.Output);
	ASSERT_TRUE(Capture.SetScene(Scene));
	FThumbnailPreviewView View;
	View.VerticalFieldOfViewDegrees = 0.0;
	Capture.SetView(View);
	auto InvalidView = Capture.BeginCapture();
	ASSERT_FALSE(InvalidView);
	EXPECT_NE(InvalidView.error().find("view is invalid"), std::string::npos);
	Capture.Reset();
	ASSERT_TRUE(Capture.SetScene(Scene));
	Capture.SetViewEnvironment({});
	auto InvalidEnvironment = Capture.BeginCapture();
	ASSERT_FALSE(InvalidEnvironment);
	EXPECT_NE(InvalidEnvironment.error().find("environment is invalid"), std::string::npos);
	Capture.Reset();
	ASSERT_TRUE(Capture.SetScene(Scene));
	ASSERT_TRUE(Capture.BeginCapture());
	FByteBuffer Pixels;
	std::string Error;
	EXPECT_EQ(Tests::FinishThumbnailCapture(Capture, Pixels, Error), EThumbnailCaptureState::Ready) << Error;
}
