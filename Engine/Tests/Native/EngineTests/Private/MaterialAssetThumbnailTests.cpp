#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "MaterialEditorModule.h"
#include "Editor/WorkspaceManager.h"

#include "Materials/MaterialTestSupport.h"

#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeRequest(
		const Durin::Asset::FAssetData& Data,
		Durin::uint64 Serial = 1) -> Durin::Editor::FAssetThumbnailRequest
	{
		return {
			.Asset = {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<Durin::uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
			.RequestSerial = Serial};
	}

	auto CaptureKey(
		Durin::Editor::Material::FMaterialAssetThumbnailProvider& Provider,
		const Durin::Asset::FAssetData& Data,
		Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> std::string
	{
		const Durin::Editor::FAssetThumbnailProviderRegistration Registration =
			Provider.GetRegistration();
		if (!Provider.CaptureGenerationRequest(
				MakeRequest(Data), 7, OutRequest, OutError))
			return {};
		OutRequest.KeyInput.Asset = MakeRequest(Data).Asset;
		OutRequest.KeyInput.ProviderName = Registration.ProviderName;
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

TEST(FMaterialAssetThumbnailTests, ModuleOwnsBothExactProvidersAndWorkspaceLifecycle)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::FRenderedAssetThumbnailService Service;
	Durin::FMaterialEditorModule Module;
	const std::string MaterialClass =
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString();
	const std::string InstanceClass =
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString();
	ASSERT_TRUE(Module.RegisterMaterialEditor(Manager, Service));
	EXPECT_TRUE(Service.Find(MaterialClass));
	EXPECT_TRUE(Service.Find(InstanceClass));
	EXPECT_NE(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
	EXPECT_FALSE(Module.RegisterMaterialEditor(Manager, Service));
	Module.UnregisterMaterialEditor();
	EXPECT_FALSE(Service.Find(MaterialClass));
	EXPECT_FALSE(Service.Find(InstanceClass));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
}

TEST(FMaterialAssetThumbnailTests, ProviderConflictRollsBackWholeIntegration)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::FRenderedAssetThumbnailService Service;
	std::string Error;
	auto Existing = Service.RegisterScoped(
		std::make_unique<Durin::Editor::Material::FMaterialAssetThumbnailProvider>(
			Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
		Error);
	ASSERT_TRUE(Existing) << Error;
	Durin::FMaterialEditorModule Module;
	EXPECT_FALSE(Module.RegisterMaterialEditor(Manager, Service));
	EXPECT_FALSE(Service.Find(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(Service.Find(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("MaterialEditor")), nullptr);
}

TEST(FMaterialAssetThumbnailTests, ProviderCapturesSortedTransitiveMaterialDependencies)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error)) << Error;

	Durin::FAssetPath MaterialPath;
	Durin::FAssetPath InstancePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialPath, MaterialPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialInstancePath, InstancePath));
	const Durin::Asset::FAssetCatalogEntry MaterialData =
		Durin::Asset::FindAssetExact(MaterialPath);
	const Durin::Asset::FAssetCatalogEntry InstanceData =
		Durin::Asset::FindAssetExact(InstancePath);
	ASSERT_NE(MaterialData, nullptr);
	ASSERT_NE(InstanceData, nullptr);

	Durin::Editor::Material::FMaterialAssetThumbnailProvider MaterialProvider(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::Editor::Material::FMaterialAssetThumbnailProvider InstanceProvider(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString());
	Durin::Editor::FAssetThumbnailGenerationRequest MaterialRequest;
	Durin::Editor::FAssetThumbnailGenerationRequest InstanceRequest;
	const std::string MaterialKey =
		CaptureKey(MaterialProvider, *MaterialData, MaterialRequest, Error);
	ASSERT_FALSE(MaterialKey.empty()) << Error;
	const std::string InstanceKey =
		CaptureKey(InstanceProvider, *InstanceData, InstanceRequest, Error);
	ASSERT_FALSE(InstanceKey.empty()) << Error;

	EXPECT_TRUE(ContainsDependency(
		MaterialRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(ContainsDependency(
		InstanceRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(ContainsDependency(
		InstanceRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(ContainsDependency(
		InstanceRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::OverrideTexturePath));
	EXPECT_NE(MaterialKey, InstanceKey);

	Durin::Editor::FAssetThumbnailKeyInput ChangedDependency = InstanceRequest.KeyInput;
	ASSERT_FALSE(ChangedDependency.Dependencies.empty());
	++ChangedDependency.Dependencies.front().LastWriteTimeTicks;
	EXPECT_NE(
		Durin::Editor::BuildAssetThumbnailCacheKey(ChangedDependency),
		Durin::Editor::BuildAssetThumbnailCacheKey(InstanceRequest.KeyInput));
}

TEST(FMaterialAssetThumbnailTests, ProviderRejectsMissingRegistryData)
{
	Durin::Tests::RegisterRenderedAssetThumbnailFixtureMount();
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/RenderedThumbnailFixtures/Materials/M_Missing", MissingPath));
	Durin::Editor::Material::FMaterialAssetThumbnailProvider Provider(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Provider.CaptureGenerationRequest({
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

TEST(FMaterialAssetThumbnailTests, PreviewComponentResolvesInstanceInheritanceAndOverrides)
{
	Durin::InitRenderingThread();
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error)) << Error;
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

TEST(FMaterialAssetThumbnailTests, InvalidInstancePublishesOneStableDiagnostic)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error)) << Error;
	Durin::FAssetPath InvalidPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::InvalidMaterialInstancePath,
		InvalidPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(InvalidPath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::FRenderedAssetThumbnailService Service;
	auto Handle = Service.RegisterScoped(
		std::make_unique<Durin::Editor::Material::FMaterialAssetThumbnailProvider>(
			Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString()),
		Error);
	ASSERT_TRUE(Handle) << Error;
	Durin::Editor::FRenderedAssetThumbnailCache Cache(Service);
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
