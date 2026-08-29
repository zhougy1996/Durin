#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "MaterialEditorModule.h"
#include "Editor/WorkspaceManager.h"

#include "Materials/MaterialTestSupport.h"

#include "Thumbnail/AssetThumbnailTestFixtures.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/Package.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeRequest(
		const Durin::Asset::FAssetData& Data,
		uint64 Serial = 1) -> Durin::Editor::FAssetThumbnailRequest
	{
		return {
			.Asset = {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
			.RequestSerial = Serial};
	}

	auto CaptureKey(
		Durin::Editor::Material::DMaterialThumbnailRenderer& Renderer,
		const Durin::Asset::FAssetData& Data,
		Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> std::string
	{
		const Durin::Editor::FThumbnailRenderingInfo Registration =
			Renderer.GetRegistration();
		if (!Renderer.CaptureGenerationRequest(
				MakeRequest(Data), 7, OutRequest, OutError))
			return {};
		OutRequest.KeyInput.Asset = MakeRequest(Data).Asset;
		OutRequest.KeyInput.RendererName = Registration.RendererName;
		OutRequest.KeyInput.GeneratorSchemaVersion =
			Registration.GeneratorSchemaVersion;
		return Durin::Editor::BuildAssetThumbnailCacheKey(OutRequest.KeyInput);
	}

	auto ContainsDependency(
		const Durin::Editor::FAssetThumbnailGenerationRequest& Request,
		std::string_view Path) -> bool
	{
		return std::ranges::any_of(
			Request.KeyInput.Dependencies,
			[Path](const Durin::Editor::FAssetThumbnailPackageFingerprint& Dependency) {
				return Dependency.VirtualPath.GetView() == Path;
			});
	}
}

TEST(FMaterialThumbnailRendererTests, ModuleOwnsBothExactRenderersAndWorkspaceLifecycle)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	Durin::FMaterialEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("MaterialEditor");
	ModuleHarness.Start(Module);
	const std::string MaterialClass =
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString();
	const std::string InstanceClass =
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString();
	ASSERT_TRUE(Module.RegisterMaterialEditor(Manager, ThumbnailManager));
	EXPECT_TRUE(ThumbnailManager.Find(MaterialClass));
	EXPECT_TRUE(ThumbnailManager.Find(InstanceClass));
	EXPECT_NE(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Module.RegisterMaterialEditor(Manager, ThumbnailManager));
	Module.UnregisterMaterialEditor();
	EXPECT_FALSE(ThumbnailManager.Find(MaterialClass));
	EXPECT_FALSE(ThumbnailManager.Find(InstanceClass));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
	ModuleHarness.Shutdown();
}

TEST(FMaterialThumbnailRendererTests, RendererConflictRollsBackWholeIntegration)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	std::string Error;
	auto Existing = ThumbnailManager.RegisterScoped(
		std::make_unique<Durin::Editor::Material::DMaterialThumbnailRenderer>(
			Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
		Error);
	ASSERT_TRUE(Existing) << Error;
	Durin::FMaterialEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("MaterialEditor");
	ModuleHarness.Start(Module);
	EXPECT_FALSE(Module.RegisterMaterialEditor(Manager, ThumbnailManager));
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(ThumbnailManager.Find(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
	ModuleHarness.Shutdown();
}

TEST(FMaterialThumbnailRendererTests, RendererCapturesSortedTransitiveMaterialDependencies)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;

	Durin::FAssetPath MaterialPath;
	Durin::FAssetPath InstancePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath, MaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialInstancePath, InstancePath));
	const Durin::Asset::FAssetCatalogEntry MaterialData =
		Durin::Asset::FindAssetExact(MaterialPath);
	const Durin::Asset::FAssetCatalogEntry InstanceData =
		Durin::Asset::FindAssetExact(InstancePath);
	ASSERT_NE(MaterialData, nullptr);
	ASSERT_NE(InstanceData, nullptr);

	Durin::Editor::Material::DMaterialThumbnailRenderer MaterialRenderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::Editor::Material::DMaterialThumbnailRenderer InstanceRenderer(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(
		MaterialRenderer.GetRegistration().GeneratorSchemaVersion, 5u);
	Durin::Editor::FAssetThumbnailGenerationRequest MaterialRequest;
	Durin::Editor::FAssetThumbnailGenerationRequest InstanceRequest;
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

	Durin::Editor::FAssetThumbnailKeyInput ChangedDependency = InstanceRequest.KeyInput;
	ASSERT_FALSE(ChangedDependency.Dependencies.empty());
	++ChangedDependency.Dependencies.front().LastWriteTimeTicks;
	EXPECT_NE(
		Durin::Editor::BuildAssetThumbnailCacheKey(ChangedDependency),
		Durin::Editor::BuildAssetThumbnailCacheKey(InstanceRequest.KeyInput));
}

TEST(FMaterialThumbnailRendererTests, RendererRejectsMissingRegistryData)
{
	Durin::Tests::RegisterAssetThumbnailFixtureMount();
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/ThumbnailFixtures/Materials/M_Missing", MissingPath));
	Durin::Editor::Material::DMaterialThumbnailRenderer Renderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Renderer.CaptureGenerationRequest({
		.Asset = {
			.VirtualPath = MissingPath,
			.AssetClassName =
				Durin::DMaterial::StaticClass()->GetQualifiedName().ToString(),
			.PackageFormatVersion = 1,
			.FileSize = 1,
			.LastWriteTimeTicks = 1},
		.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
		.RequestSerial = 1}, 1, Captured, Error));
	EXPECT_NE(Error.find(MissingPath.ToString()), std::string::npos);
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
	const Durin::FMaterialRenderBinding MaterialBinding =
		GetMaterialBinding(MaterialData);
	const Durin::FMaterialRenderBinding InstanceBinding =
		GetMaterialBinding(InstanceData);
	EXPECT_NE(MaterialBinding.BaseColor, InstanceBinding.BaseColor);
	EXPECT_NE(
		MaterialBinding.Textures[0],
		InstanceBinding.Textures[0]);

	MaterialPrimitive.reset();
	InstancePrimitive.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
	Durin::ShutdownRenderingThread();
}

TEST(FMaterialThumbnailRendererTests, InvalidInstancePublishesOneStableDiagnostic)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
	Durin::FAssetPath InvalidPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::InvalidMaterialInstancePath,
		InvalidPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(InvalidPath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::DThumbnailManager ThumbnailManager;
	auto Handle = ThumbnailManager.RegisterScoped(
		std::make_unique<Durin::Editor::Material::DMaterialThumbnailRenderer>(
			Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
		Error);
	ASSERT_TRUE(Handle) << Error;
	Durin::Editor::FAssetThumbnailPool Cache(ThumbnailManager);
	Cache.BeginFrame();
	Cache.Request(MakeRequest(*Data).Asset, Durin::Editor::EAssetThumbnailPriority::Visible);
	Cache.EndFrame();
	const Durin::Editor::FAssetThumbnailView First = Cache.Find(InvalidPath);
	ASSERT_EQ(First.State, Durin::Editor::EAssetThumbnailState::Failed);
	EXPECT_NE(First.Diagnostic.find("parent"), std::string::npos);

	Cache.BeginFrame();
	Cache.Request(MakeRequest(*Data).Asset, Durin::Editor::EAssetThumbnailPriority::Visible);
	Cache.EndFrame();
	const Durin::Editor::FAssetThumbnailView Repeated = Cache.Find(InvalidPath);
	EXPECT_EQ(Repeated.State, Durin::Editor::EAssetThumbnailState::Failed);
	EXPECT_EQ(Repeated.Diagnostic, First.Diagnostic);
}

TEST(FMaterialThumbnailRendererTests,
	LoadCapturesAuthoredRevisionInsteadOfRuntimeRenderRevision)
{
	Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture SavedMountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	std::string Error;
	ASSERT_TRUE(Durin::Asset::RefreshAssetRegistry());

	Durin::FAssetPath MaterialPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/Engine/Materials/ImportedSurface",
		MaterialPath));
	const Durin::Asset::FAssetCatalogEntry MaterialData =
		Durin::Asset::FindAssetExact(MaterialPath);
	ASSERT_NE(MaterialData, nullptr);
	Durin::DObject* LoadedObject = nullptr;
	const Durin::Asset::FAssetResult LoadResult =
		Durin::Asset::LoadAsset(MaterialPath, LoadedObject);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	auto* Material = Durin::Cast<Durin::DMaterial>(LoadedObject);
	ASSERT_NE(Material, nullptr);
	ASSERT_NE(Material->GetPackage(), nullptr);
	while (Material->GetPackage()->GetEditRevision()
		== Material->GetRenderStateVersion())
		Material->GetPackage()->MarkDirty();
	Material->GetPackage()->ClearDirty();
	const uint64 AuthoredRevision = Material->GetPackage()->GetEditRevision();
	ASSERT_NE(AuthoredRevision, Material->GetRenderStateVersion());

	Durin::Editor::Material::DMaterialThumbnailRenderer Renderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	ASSERT_FALSE(CaptureKey(Renderer, *MaterialData, Request, Error).empty())
		<< Error;
	auto Session = Renderer.CreateGenerationSession(
		Request, *Request.Input, Error);
	ASSERT_NE(Session, nullptr) << Error;
	const Durin::Editor::FThumbnailRendererSessionUpdate Loaded =
		Session->Load();
	ASSERT_EQ(Loaded.State,
		Durin::Editor::EThumbnailRendererSessionState::WaitingForResources)
		<< Loaded.Diagnostic;
	EXPECT_EQ(Loaded.AssetRevision, AuthoredRevision);
	EXPECT_NE(Loaded.AssetRevision, Material->GetRenderStateVersion());
	Session.reset();
}
