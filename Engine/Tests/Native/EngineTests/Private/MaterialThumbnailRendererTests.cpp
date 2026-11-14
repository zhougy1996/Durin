#include "Thumbnail/AssetThumbnailKey.h"
#include "ThumbnailManagerTestSupport.h"
#include "DObject/WeakObjectPtr.h"
#include "NativePackageUnloadTestSupport.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/ThumbnailRenderer.h"
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

	auto CaptureKey(Durin::MaterialEditor::DMaterialThumbnailRenderer& Renderer, const Durin::FPackageData& Data, std::string& OutError) -> std::string
	{
		auto Input = Durin::CaptureThumbnailKeyInput(Data.TopLevelAssets.front(), Renderer.GetClass());
		if (!Input) { OutError = Input.error(); return {}; }
		return Durin::BuildAssetThumbnailCacheKey(*Input);
	}

	auto ContainsDependency(
		const Durin::FAssetThumbnailKeyInput& Request,
		std::string_view Path) -> bool
	{
		return std::ranges::any_of(
			Request.Packages,
			[Path](const Durin::FPackageData& Dependency) {
				return Dependency.PackagePath.GetView() == Path;
			});
	}
}

TEST(FMaterialThumbnailRendererTests, ReflectedRegistrationOwnsRendererUntilWithdrawalAndGarbageCollection)
{
	InitializeDObjectSystem();
	Durin::DThumbnailManager Manager;
	auto Registered = Manager.RegisterCustomRenderer(Durin::DMaterial::StaticClass(),
		Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass());
	ASSERT_TRUE(Registered);
	const auto Info = Manager.GetRenderingInfo(Durin::DMaterial::StaticClass());
	ASSERT_TRUE(Info);
	EXPECT_EQ(Info->ClassNeedingThumbnail, Durin::DMaterial::StaticClass());
	EXPECT_EQ(Info->RendererClass, Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass());
	EXPECT_EQ(Info->RendererClassName, Info->RendererClass->GetQualifiedName().ToString());
	Durin::TWeakObjectPtr<Durin::DThumbnailRenderer> Weak(Info->Renderer);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_NE(Weak.Get(), nullptr);
	Registered.Reset();
	EXPECT_FALSE(Manager.GetRenderingInfo(Durin::DMaterial::StaticClass()));
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Weak.Get(), nullptr);
}

TEST(FMaterialThumbnailRendererTests, ReflectedRegistrationRejectsAbstractAndUnrelatedClasses)
{
	InitializeDObjectSystem();
	Durin::DThumbnailManager Manager;
	EXPECT_FALSE(Manager.RegisterCustomRenderer(nullptr, Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass()));
	EXPECT_FALSE(Manager.RegisterCustomRenderer(Durin::DMaterial::StaticClass(), nullptr));
	EXPECT_FALSE(Manager.RegisterCustomRenderer(Durin::DMaterial::StaticClass(), Durin::DThumbnailRenderer::StaticClass()));
	EXPECT_FALSE(Manager.RegisterCustomRenderer(Durin::DMaterial::StaticClass(), Durin::DMaterial::StaticClass()));
	EXPECT_EQ(Manager.Num(), 0u);
}

TEST(FMaterialThumbnailRendererTests, OneReflectedRendererClassServesDistinctAssetRegistrations)
{
	InitializeDObjectSystem();
	Durin::DThumbnailManager Manager;
	auto Material = Manager.RegisterCustomRenderer(Durin::DMaterial::StaticClass(), Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass());
	auto Instance = Manager.RegisterCustomRenderer(Durin::DMaterialInstance::StaticClass(), Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass());
	ASSERT_TRUE(Material);
	ASSERT_TRUE(Instance);
	EXPECT_NE(Manager.GetRenderingInfo(Durin::DMaterial::StaticClass())->Renderer,
		Manager.GetRenderingInfo(Durin::DMaterialInstance::StaticClass())->Renderer);
	EXPECT_FALSE(Manager.RegisterCustomRenderer(Durin::DMaterial::StaticClass(), Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass()));
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
		Durin::InitializeAssetCompilingManager();
	Durin::FEditorDocumentManager Manager;
	Durin::DThumbnailManager ThumbnailManager;
	std::string Error;
	auto Existing = ThumbnailManager.RegisterCustomRenderer(Durin::DMaterialInstance::StaticClass(), Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass());
	ASSERT_TRUE(Existing);
	Durin::FMaterialEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("MaterialEditor");
	ModuleHarness.Start(Module);
	EXPECT_FALSE(ThumbnailManager.RegisterCustomRenderer(Durin::DMaterialInstance::StaticClass(), Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass()));
	EXPECT_TRUE(Durin::GetDefaultThumbnailManager().Find(Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(ThumbnailManager.Find(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindToolkit(
		Durin::FAssetEditorToolkitId("MaterialEditor")), nullptr);
	ModuleHarness.Shutdown();
}

TEST(FMaterialThumbnailRendererTests, CacheKeyIncludesTransitiveMaterialPackages)
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

	auto MaterialRendererOwner = Durin::MakeThumbnailRenderer<Durin::MaterialEditor::DMaterialThumbnailRenderer>();
	auto& MaterialRenderer = *MaterialRendererOwner;
	auto InstanceRendererOwner = Durin::MakeThumbnailRenderer<Durin::MaterialEditor::DMaterialThumbnailRenderer>();
	auto& InstanceRenderer = *InstanceRendererOwner;


	const std::string MaterialKey =
		CaptureKey(MaterialRenderer, *MaterialData, Error);
	ASSERT_FALSE(MaterialKey.empty()) << Error;
	const std::string InstanceKey =
		CaptureKey(InstanceRenderer, *InstanceData, Error);
	ASSERT_FALSE(InstanceKey.empty()) << Error;

	const auto MaterialInput = *Durin::CaptureThumbnailKeyInput(MaterialData->TopLevelAssets.front(), MaterialRenderer.GetClass());
	const auto InstanceInput = *Durin::CaptureThumbnailKeyInput(InstanceData->TopLevelAssets.front(), InstanceRenderer.GetClass());
	EXPECT_TRUE(ContainsDependency(
		MaterialInput,
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(ContainsDependency(
		InstanceInput,
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(ContainsDependency(
		InstanceInput,
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(ContainsDependency(
		InstanceInput,
		Durin::Tests::FAssetThumbnailFixtureSet::OverrideTexturePath));
	EXPECT_NE(MaterialKey, InstanceKey);

	Durin::FAssetThumbnailKeyInput ChangedDependency = *Durin::CaptureThumbnailKeyInput(InstanceData->TopLevelAssets.front(), InstanceRenderer.GetClass());
	ASSERT_FALSE(ChangedDependency.Packages.empty());
	++ChangedDependency.Packages.front().LastWriteTimeTicks;
	EXPECT_NE(
		Durin::BuildAssetThumbnailCacheKey(ChangedDependency),
		Durin::BuildAssetThumbnailCacheKey(*Durin::CaptureThumbnailKeyInput(InstanceData->TopLevelAssets.front(), InstanceRenderer.GetClass())));
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
	auto Handle = ThumbnailManager.RegisterCustomRenderer(Durin::DMaterialInstance::StaticClass(), Durin::MaterialEditor::DMaterialThumbnailRenderer::StaticClass());
	ASSERT_TRUE(Handle);
	Durin::FAssetThumbnailPool Cache(ThumbnailManager);
	Durin::ProcessAsyncLoading();

	Cache.AccessTexture(Data->TopLevelAssets.front(), Durin::EAssetThumbnailPriority::Visible);
	Cache.Tick();
	// The cache lookup completes asynchronously before renderer validation runs.
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (Cache.Find(MakeAssetPath(InvalidPath)).State != Durin::EAssetThumbnailState::Failed
		&& std::chrono::steady_clock::now() < Deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		Durin::ProcessAsyncLoading();

		Cache.Tick();
	}
	const Durin::FAssetThumbnailView First = Cache.Find(MakeAssetPath(InvalidPath));
	ASSERT_EQ(First.State, Durin::EAssetThumbnailState::Failed);
	EXPECT_NE(First.Diagnostic.find("parent"), std::string::npos) << First.Diagnostic;

	Durin::ProcessAsyncLoading();

	Cache.AccessTexture(Data->TopLevelAssets.front(), Durin::EAssetThumbnailPriority::Visible);
	Cache.Tick();
	const Durin::FAssetThumbnailView Repeated = Cache.Find(MakeAssetPath(InvalidPath));
	EXPECT_EQ(Repeated.State, Durin::EAssetThumbnailState::Failed);
	EXPECT_EQ(Repeated.Diagnostic, First.Diagnostic);
}

TEST(FMaterialThumbnailRendererTests,
	RendererUsesCurrentMaterialWhileWaitingForSphereCompilation)
{
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		Durin::InitializeAssetCompilingManager();
	if (!Durin::IsMaterialCompilationAcceptingRequests())
		if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		Durin::InitializeAssetCompilingManager();
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

	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::MaterialEditor::DMaterialThumbnailRenderer>();
	auto& Renderer = *RendererOwner;

	ASSERT_FALSE(CaptureKey(Renderer, *MaterialData, Error).empty())
		<< Error;
	{
		Durin::FThumbnailDrawContext Context(Material);
		Renderer.Draw(Material, Context);
		Durin::ProcessAsyncLoading(100.0, 16);
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*Material, true));
		ASSERT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());
		EXPECT_FALSE(Material->IsCompilationFinished());
		EXPECT_TRUE(Material->HasValidGameThreadShaderMap());
		Renderer.Draw(Material, Context);
		EXPECT_TRUE(Durin::FThumbnailManagerTestAccess::IsWaiting(Context))
			<< Durin::FThumbnailManagerTestAccess::GetDiagnostic(Context);

		(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		ASSERT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());

		ASSERT_TRUE(Sphere->Build(EStaticMeshBuildMode::Asynchronous, {.bMarkPackageDirty = false})) << Error;
		ASSERT_TRUE(Sphere->IsCompiling());
		ASSERT_EQ(Sphere->GetRenderResourceStatus().Readiness,
			Durin::EStaticMeshRenderResourceReadiness::Unavailable);
		Renderer.Draw(Material, Context);
		EXPECT_TRUE(Durin::FThumbnailManagerTestAccess::IsWaiting(Context));
		EXPECT_TRUE(Durin::FThumbnailManagerTestAccess::GetDiagnostic(Context).empty());
		(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Sphere);
		EXPECT_FALSE(Sphere->IsCompiling());
		EXPECT_NE(Sphere->GetRenderData(), nullptr);
	}
	// The fixture belongs to the renderer, independently of a cancelled draw.
	const auto SpherePackage = Sphere->GetPackage()->GetPackagePathIdentity();
	EXPECT_EQ(Durin::UnloadPackage(SpherePackage).Error, Durin::EAssetReadError::InUse);
	Durin::TWeakObjectPtr<Durin::DThumbnailRenderer> WeakRenderer(RendererOwner.Get());
	RendererOwner.Reset();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(WeakRenderer.IsValid());
	EXPECT_TRUE(Durin::UnloadPackage(SpherePackage));

}
