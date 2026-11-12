#include "Thumbnail/ThumbnailManager.h"
#include "Asset/AssetCompilingManager.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "MaterialEditorModule.h"
#include "Documents/EditorDocumentManager.h"

#include "Materials/MaterialTestSupport.h"

#include "Thumbnail/AssetThumbnailTestFixtures.h"

#include "Asset/AssetCompilingManager.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Package.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace
{
	auto MakeAssetPath(const Durin::FPackagePath& PackagePath)
		-> Durin::FTopLevelAssetPath
	{
		return Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(PackagePath);
	}

	auto MakeRequest(
		const Durin::FPackageData& Data,
		uint64 Serial = 1) -> Durin::FAssetThumbnailRequest
	{
		return {
			.Asset = {
				.AssetPath = MakeAssetPath(Data.PackagePath),
				.PackagePath = Data.PackagePath,
				.AssetClassName = Data.TopLevelAssets.front().AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::EAssetThumbnailPriority::Visible,
			.RequestSerial = Serial};
	}

	auto CaptureKey(
		Durin::MaterialEditor::DMaterialThumbnailRenderer& Renderer,
		const Durin::FPackageData& Data,
		Durin::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> std::string
	{
		const Durin::FThumbnailRenderingInfo Registration =
			Renderer.GetRegistration();
		auto CaptureResult = Renderer.CaptureGenerationRequest(
				MakeRequest(Data), 7);
		if (!CaptureResult)
		{
			OutError = std::move(CaptureResult.error());
			return {};
		}
		OutRequest = std::move(*CaptureResult);
		OutRequest.KeyInput.Asset = MakeRequest(Data).Asset;
		OutRequest.KeyInput.RendererName = Registration.RendererName;
		OutRequest.KeyInput.GeneratorSchemaVersion =
			Registration.GeneratorSchemaVersion;
		return Durin::BuildAssetThumbnailCacheKey(OutRequest.KeyInput);
	}

	auto ContainsDependency(
		const Durin::FAssetThumbnailGenerationRequest& Request,
		std::string_view Path) -> bool
	{
		return std::ranges::any_of(
			Request.KeyInput.Dependencies,
			[Path](const Durin::FAssetThumbnailPackageFingerprint& Dependency) {
				return Dependency.PackagePath.GetView() == Path;
			});
	}
}

TEST(FMaterialThumbnailRendererTests, ModuleStartupPublishesIndependentRenderersWithoutSharedToolkits)
{
	InitializeDObjectSystem();
	Durin::FEditorDocumentManager Manager;
	auto& ThumbnailManager = Durin::GetDefaultThumbnailManager();
	Durin::FMaterialEditorModule Module;
	Durin::FModuleTestHarness Harness("MaterialEditor");
	Harness.Start(Module);
	EXPECT_TRUE(ThumbnailManager.Find(Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(ThumbnailManager.Find(Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(!Manager.GetToolkit());
	Harness.Shutdown();
	EXPECT_FALSE(ThumbnailManager.Find(Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_FALSE(ThumbnailManager.Find(Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
}


TEST(FMaterialThumbnailRendererTests, RendererConflictDoesNotAffectIndependentModuleContributions)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	Durin::FEditorDocumentManager Manager;
	Durin::DThumbnailManager ThumbnailManager;
	std::string Error;
	auto Existing = ThumbnailManager.RegisterScoped(
		std::make_unique<Durin::MaterialEditor::DMaterialThumbnailRenderer>(
			Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
	ASSERT_TRUE(Existing) << Existing.error();
	Durin::FMaterialEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("MaterialEditor");
	ModuleHarness.Start(Module);
	EXPECT_FALSE(ThumbnailManager.RegisterScoped(std::make_unique<Durin::MaterialEditor::DMaterialThumbnailRenderer>(Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString())));
	EXPECT_TRUE(Durin::GetDefaultThumbnailManager().Find(Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(ThumbnailManager.Find(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindToolkit(
		Durin::FAssetEditorToolkitId("MaterialEditor")), nullptr);
	ModuleHarness.Shutdown();
}

TEST(FMaterialThumbnailRendererTests, RendererCapturesSortedTransitiveMaterialDependencies)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;

	Durin::FPackagePath MaterialPath;
	Durin::FPackagePath InstancePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath, MaterialPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialInstancePath, InstancePath));
	const Durin::FPackageCatalogEntry MaterialData =
		Durin::FindPackageExact(MaterialPath);
	const Durin::FPackageCatalogEntry InstanceData =
		Durin::FindPackageExact(InstancePath);
	ASSERT_NE(MaterialData, nullptr);
	ASSERT_NE(InstanceData, nullptr);

	Durin::MaterialEditor::DMaterialThumbnailRenderer MaterialRenderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::MaterialEditor::DMaterialThumbnailRenderer InstanceRenderer(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(
		MaterialRenderer.GetRegistration().GeneratorSchemaVersion, 6u);
	Durin::FAssetThumbnailGenerationRequest MaterialRequest;
	Durin::FAssetThumbnailGenerationRequest InstanceRequest;
	const std::string MaterialKey =
		CaptureKey(MaterialRenderer, *MaterialData, MaterialRequest, Error);
	ASSERT_FALSE(MaterialKey.empty()) << Error;
	const std::string InstanceKey =
		CaptureKey(InstanceRenderer, *InstanceData, InstanceRequest, Error);
	ASSERT_FALSE(InstanceKey.empty()) << Error;

	EXPECT_TRUE(ContainsDependency(
		MaterialRequest,
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(ContainsDependency(
		InstanceRequest,
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(ContainsDependency(
		InstanceRequest,
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(ContainsDependency(
		InstanceRequest,
		Durin::Tests::FAssetThumbnailFixtureSet::OverrideTexturePath));
	EXPECT_NE(MaterialKey, InstanceKey);

	Durin::FAssetThumbnailKeyInput ChangedDependency = InstanceRequest.KeyInput;
	ASSERT_FALSE(ChangedDependency.Dependencies.empty());
	++ChangedDependency.Dependencies.front().LastWriteTimeTicks;
	EXPECT_NE(
		Durin::BuildAssetThumbnailCacheKey(ChangedDependency),
		Durin::BuildAssetThumbnailCacheKey(InstanceRequest.KeyInput));
}

TEST(FMaterialThumbnailRendererTests, RendererRejectsMissingRegistryData)
{
	Durin::Tests::RegisterAssetThumbnailFixtureMount();
	Durin::FPackagePath MissingPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/ThumbnailFixtures/Materials/M_Missing", MissingPath));
	Durin::MaterialEditor::DMaterialThumbnailRenderer Renderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	auto CapturedResult = Renderer.CaptureGenerationRequest({
		.Asset = {
			.AssetPath = MakeAssetPath(MissingPath),
			.PackagePath = MissingPath,
			.AssetClassName =
				Durin::DMaterial::StaticClass()->GetQualifiedName().ToString(),
			.PackageFormatVersion = 1,
			.FileSize = 1,
			.LastWriteTimeTicks = 1},
		.Priority = Durin::EAssetThumbnailPriority::Visible,
		.RequestSerial = 1}, 1);
	ASSERT_FALSE(CapturedResult);
	EXPECT_NE(CapturedResult.error().find(MissingPath.ToString()), std::string::npos);
}

TEST(FMaterialThumbnailRendererTests, PreviewComponentResolvesInstanceInheritanceAndOverrides)
{
	Durin::InitRenderingThread();
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);

	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(
		nullptr, "MaterialThumbnailPreviewComponent");
	Component->SetStaticMesh(Mesh);
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Fixtures.Material);
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Fixtures.MaterialInstance);
	Component->SetMaterial(Fixtures.Material);
	std::unique_ptr<Durin::FPrimitiveSceneProxy> MaterialPrimitive =
		Component->CreateSceneProxy();
	ASSERT_NE(MaterialPrimitive, nullptr) << Error;
	Component->SetMaterial(Fixtures.MaterialInstance);
	std::unique_ptr<Durin::FPrimitiveSceneProxy> InstancePrimitive =
		Component->CreateSceneProxy();
	ASSERT_NE(InstancePrimitive, nullptr) << Error;
	auto* MaterialProxy =
		dynamic_cast<Durin::FStaticMeshSceneProxy*>(MaterialPrimitive.get());
	auto* InstanceProxy =
		dynamic_cast<Durin::FStaticMeshSceneProxy*>(InstancePrimitive.get());
	ASSERT_NE(MaterialProxy, nullptr);
	ASSERT_NE(InstanceProxy, nullptr);

	Durin::FMaterialRenderData MaterialData;
	Durin::FMaterialRenderData InstanceData;
	struct FCaptureThumbnailMaterialProxiesCommand
	{
		static constexpr const char* GetName()
		{
			return "CaptureThumbnailMaterialProxies";
		}
	};
	Durin::EnqueueRenderCommand<FCaptureThumbnailMaterialProxiesCommand>(
		[MaterialProxy, InstanceProxy, &MaterialData, &InstanceData](
			Durin::FRHICommandListImmediate&) {
			MaterialData =
				MaterialProxy->ResolveMaterialRenderData_RenderThread(0);
			InstanceData =
				InstanceProxy->ResolveMaterialRenderData_RenderThread(0);
		});
	Durin::FlushRenderingCommands();
	const auto MaterialBinding =
		GetMaterialBinding(MaterialData);
	const auto InstanceBinding =
		GetMaterialBinding(InstanceData);
	EXPECT_NE(MaterialBinding.BaseColor, InstanceBinding.BaseColor);
	EXPECT_NE(
		MaterialBinding.Textures[0],
		InstanceBinding.Textures[0]);

	MaterialPrimitive.reset();
	InstancePrimitive.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialThumbnailRendererTests, InvalidInstancePublishesOneStableDiagnostic)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
	Durin::FPackagePath InvalidPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::InvalidMaterialInstancePath,
		InvalidPath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(InvalidPath);
	ASSERT_NE(Data, nullptr);

	Durin::DThumbnailManager ThumbnailManager;
	auto Handle = ThumbnailManager.RegisterScoped(
		std::make_unique<Durin::MaterialEditor::DMaterialThumbnailRenderer>(
			Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
	ASSERT_TRUE(Handle) << Handle.error();
	Durin::FAssetThumbnailPool Cache(ThumbnailManager);
	Durin::ProcessAsyncLoading();
	Cache.BeginFrame();
	Cache.Request(MakeRequest(*Data).Asset, Durin::EAssetThumbnailPriority::Visible);
	Cache.EndFrame();
	// The cache lookup completes asynchronously before renderer validation runs.
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (Cache.Find(MakeAssetPath(InvalidPath)).State != Durin::EAssetThumbnailState::Failed
		&& std::chrono::steady_clock::now() < Deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		Durin::ProcessAsyncLoading();
		Cache.BeginFrame();
		Cache.EndFrame();
	}
	const Durin::FAssetThumbnailView First = Cache.Find(MakeAssetPath(InvalidPath));
	ASSERT_EQ(First.State, Durin::EAssetThumbnailState::Failed);
	EXPECT_NE(First.Diagnostic.find("parent"), std::string::npos) << First.Diagnostic;

	Durin::ProcessAsyncLoading();
	Cache.BeginFrame();
	Cache.Request(MakeRequest(*Data).Asset, Durin::EAssetThumbnailPriority::Visible);
	Cache.EndFrame();
	const Durin::FAssetThumbnailView Repeated = Cache.Find(MakeAssetPath(InvalidPath));
	EXPECT_EQ(Repeated.State, Durin::EAssetThumbnailState::Failed);
	EXPECT_EQ(Repeated.Diagnostic, First.Diagnostic);
}

TEST(FMaterialThumbnailRendererTests,
	LoadedSessionRequiresPreparedInput)
{
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	Durin::Testing::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	std::string Error;
	ASSERT_TRUE(Durin::RefreshAssetRegistry());

	Durin::FPackagePath MaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/Engine/Materials/DefaultMaterial",
		MaterialPath));
	const Durin::FPackageCatalogEntry MaterialData =
		Durin::FindPackageExact(MaterialPath);
	ASSERT_NE(MaterialData, nullptr);
	Durin::DObject* LoadedObject = nullptr;
	const auto LoadResult =
		Durin::LoadObjectDetailed<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		LoadedObject = LoadResult.value_or(nullptr);
	ASSERT_TRUE(LoadResult) << (LoadResult ? std::string{} : LoadResult.error().Message);
	auto* Material = Durin::Cast<Durin::DMaterial>(LoadedObject);
	ASSERT_NE(Material, nullptr);
	ASSERT_NE(Material->GetPackage(), nullptr);

	Durin::MaterialEditor::DMaterialThumbnailRenderer Renderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::FAssetThumbnailGenerationRequest Request;
	ASSERT_FALSE(CaptureKey(Renderer, *MaterialData, Request, Error).empty())
		<< Error;
	auto Session = Renderer.CreateGenerationSession(
		Request, *Request.Input);
	ASSERT_TRUE(Session) << Session.error();
	ASSERT_NE(*Session, nullptr);
	const Durin::FThumbnailRendererSessionUpdate Loaded =
		(*Session)->Load();
	ASSERT_EQ(Loaded.State,
		Durin::EThumbnailRendererSessionState::WaitingForResources)
		<< Loaded.Diagnostic;
	EXPECT_FALSE((*Session)->ValidatePreparedInput());
	(*Session)->ResetPreview();
	EXPECT_FALSE((*Session)->ValidatePreparedInput());
	Session->reset();
}

TEST(FMaterialThumbnailRendererTests,
	RendererUsesCurrentMaterialWhileWaitingForSphereCompilation)
{
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	if (!Durin::IsMaterialCompilationAcceptingRequests())
		if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	Durin::Testing::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	std::string Error;
	ASSERT_TRUE(Durin::RefreshAssetRegistry());

	Durin::FPackagePath MaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/Engine/Materials/DefaultMaterial", MaterialPath));
	const Durin::FPackageCatalogEntry MaterialData =
		Durin::FindPackageExact(MaterialPath);
	ASSERT_NE(MaterialData, nullptr);
	Durin::DObject* LoadedObject = nullptr;
	const auto LoadResult =
		Durin::LoadObjectDetailed<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		LoadedObject = LoadResult.value_or(nullptr);
	ASSERT_TRUE(LoadResult) << (LoadResult ? std::string{} : LoadResult.error().Message);
	auto* Material = Durin::Cast<Durin::DMaterial>(LoadedObject);
	ASSERT_NE(Material, nullptr);
	(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
	ASSERT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());

	Durin::FSoftObjectPath SpherePath;
	ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(
		Durin::FThumbnailVisualContract::SphereAssetPath, SpherePath));
	Durin::DObject* SphereObject = nullptr;
	{
		auto LoadedValue = Durin::LoadObjectDetailed<Durin::DObject>(SpherePath);
		SphereObject = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	auto* Sphere = Durin::Cast<Durin::DStaticMesh>(SphereObject);
	ASSERT_NE(Sphere, nullptr);

	Durin::MaterialEditor::DMaterialThumbnailRenderer Renderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::FAssetThumbnailGenerationRequest Request;
	ASSERT_FALSE(CaptureKey(Renderer, *MaterialData, Request, Error).empty())
		<< Error;
	auto Session = Renderer.CreateGenerationSession(
		Request, *Request.Input);
	ASSERT_TRUE(Session) << Session.error();
	ASSERT_NE(*Session, nullptr);
	ASSERT_EQ((*Session)->Load().State,
		Durin::EThumbnailRendererSessionState::WaitingForResources);

	Durin::ProcessAsyncLoading(100.0, 16);
	EXPECT_EQ((*Session)->PollResources().State,
		Durin::EThumbnailRendererSessionState::WaitingForResources);
	Durin::ProcessAsyncLoading(100.0, 16);
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*Material, true));
	ASSERT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());
	EXPECT_FALSE(Material->IsCompilationFinished());
	EXPECT_TRUE(Material->HasValidGameThreadShaderMap());
	const Durin::FThumbnailRendererSessionUpdate Compiling =
		(*Session)->PollResources();
	EXPECT_EQ(Compiling.State,
		Durin::EThumbnailRendererSessionState::WaitingForResources)
		<< Compiling.Diagnostic;

	(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
	ASSERT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());

	ASSERT_TRUE(Sphere->Build(EStaticMeshBuildMode::Asynchronous, {.bMarkPackageDirty = false})) << Error;
	ASSERT_TRUE(Sphere->IsCompiling());
	ASSERT_EQ(Sphere->GetRenderResourceStatus().Readiness,
		Durin::EStaticMeshRenderResourceReadiness::Unavailable);
	const auto SphereCompiling = (*Session)->PollResources();
	EXPECT_EQ(SphereCompiling.State,
		Durin::EThumbnailRendererSessionState::WaitingForResources)
		<< SphereCompiling.Diagnostic;
	EXPECT_TRUE(SphereCompiling.Diagnostic.empty());
	(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Sphere);
	EXPECT_FALSE(Sphere->IsCompiling());
	EXPECT_NE(Sphere->GetRenderData(), nullptr);

	Session->reset();
}
