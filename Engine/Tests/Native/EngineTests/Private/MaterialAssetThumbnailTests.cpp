#include "Thumbnail/MaterialAssetThumbnail.h"

#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"

#include "Engine/PrimitiveSceneProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeRequest(
		const Durin::Asset::FAssetData& Data,
		Durin::uint64 Serial = 1) -> Durin::FAssetThumbnailRequest
	{
		return {
			.Asset = {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<Durin::uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::EAssetThumbnailPriority::Visible,
			.RequestSerial = Serial};
	}

	auto CaptureKey(
		Durin::FMaterialAssetThumbnailProvider& Provider,
		const Durin::Asset::FAssetData& Data,
		Durin::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> std::string
	{
		const Durin::FAssetThumbnailProviderRegistration Registration =
			Provider.GetRegistration();
		if (!Provider.CaptureGenerationRequest(
				MakeRequest(Data), 7, OutRequest, OutError))
			return {};
		OutRequest.KeyInput.Asset = MakeRequest(Data).Asset;
		OutRequest.KeyInput.ProviderName = Registration.ProviderName;
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
				return Dependency.VirtualPath.GetView() == Path;
			});
	}
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
	const Durin::Asset::FAssetData* MaterialData =
		Durin::Asset::GetAssetRegistry().FindAsset(MaterialPath);
	const Durin::Asset::FAssetData* InstanceData =
		Durin::Asset::GetAssetRegistry().FindAsset(InstancePath);
	ASSERT_NE(MaterialData, nullptr);
	ASSERT_NE(InstanceData, nullptr);

	Durin::FMaterialAssetThumbnailProvider MaterialProvider(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::FMaterialAssetThumbnailProvider InstanceProvider(
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString());
	Durin::FAssetThumbnailGenerationRequest MaterialRequest;
	Durin::FAssetThumbnailGenerationRequest InstanceRequest;
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

	Durin::FAssetThumbnailKeyInput ChangedDependency = InstanceRequest.KeyInput;
	ASSERT_FALSE(ChangedDependency.Dependencies.empty());
	++ChangedDependency.Dependencies.front().LastWriteTimeTicks;
	EXPECT_NE(
		Durin::BuildAssetThumbnailCacheKey(ChangedDependency),
		Durin::BuildAssetThumbnailCacheKey(InstanceRequest.KeyInput));
}

TEST(FMaterialAssetThumbnailTests, ProviderRejectsMissingRegistryData)
{
	Durin::Tests::RegisterRenderedAssetThumbnailFixtureMount();
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/RenderedThumbnailFixtures/Materials/M_Missing", MissingPath));
	Durin::FMaterialAssetThumbnailProvider Provider(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString());
	Durin::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Provider.CaptureGenerationRequest({
		.Asset = {
			.VirtualPath = MissingPath,
			.AssetClassName =
				Durin::DMaterial::StaticClass()->GetQualifiedName().ToString(),
			.PackageFormatVersion = 1,
			.FileSize = 1,
			.LastWriteTimeTicks = 1},
		.Priority = Durin::EAssetThumbnailPriority::Visible,
		.RequestSerial = 1}, 1, Captured, Error));
	EXPECT_NE(Error.find(MissingPath.ToString()), std::string::npos);
}

TEST(FMaterialAssetThumbnailTests, SharedPreviewPrimitiveResolvesInstanceInheritanceAndOverrides)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error)) << Error;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);

	std::unique_ptr<Durin::PrimitiveSceneProxy> MaterialPrimitive =
		Durin::CreateMaterialPreviewPrimitive(Mesh, Fixtures.Material, 10, Error);
	ASSERT_NE(MaterialPrimitive, nullptr) << Error;
	std::unique_ptr<Durin::PrimitiveSceneProxy> InstancePrimitive =
		Durin::CreateMaterialPreviewPrimitive(Mesh, Fixtures.MaterialInstance, 11, Error);
	ASSERT_NE(InstancePrimitive, nullptr) << Error;
	auto* MaterialProxy =
		dynamic_cast<Durin::FStaticMeshSceneProxy*>(MaterialPrimitive.get());
	auto* InstanceProxy =
		dynamic_cast<Durin::FStaticMeshSceneProxy*>(InstancePrimitive.get());
	ASSERT_NE(MaterialProxy, nullptr);
	ASSERT_NE(InstanceProxy, nullptr);

	const Durin::FMaterialRenderData& MaterialData =
		MaterialProxy->GetMaterialRenderData(0);
	const Durin::FMaterialRenderData& InstanceData =
		InstanceProxy->GetMaterialRenderData(0);
	EXPECT_NE(MaterialData.BaseColor, InstanceData.BaseColor);
	EXPECT_NE(MaterialData.SpecularStrength, InstanceData.SpecularStrength);
	EXPECT_NE(MaterialData.BaseColorTexture, InstanceData.BaseColorTexture);
	EXPECT_FLOAT_EQ(InstanceData.Shininess, MaterialData.Shininess);

	MaterialPrimitive.reset();
	InstancePrimitive.reset();
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
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
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAsset(InvalidPath);
	ASSERT_NE(Data, nullptr);

	Durin::FMaterialAssetThumbnailCache Cache;
	Cache.BeginFrame();
	Cache.Request(MakeRequest(*Data).Asset, Durin::EAssetThumbnailPriority::Visible);
	Cache.EndFrame();
	const Durin::FAssetThumbnailView First = Cache.Find(InvalidPath);
	ASSERT_EQ(First.State, Durin::EAssetThumbnailState::Failed);
	EXPECT_NE(First.Diagnostic.find("parent"), std::string::npos);

	Cache.BeginFrame();
	Cache.Request(MakeRequest(*Data).Asset, Durin::EAssetThumbnailPriority::Visible);
	Cache.EndFrame();
	const Durin::FAssetThumbnailView Repeated = Cache.Find(InvalidPath);
	EXPECT_EQ(Repeated.State, Durin::EAssetThumbnailState::Failed);
	EXPECT_EQ(Repeated.Diagnostic, First.Diagnostic);
}
