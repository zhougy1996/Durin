#include "MaterialEditorModule.h"
#include "Settings/MaterialEditorSessionSettings.h"
#include "MaterialParameterCollectionDetails.h"
#include "Graph/MaterialGraphCanvas.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transaction.h"
#include "MaterialAssetCreation.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialParameterCollection.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Materials/MaterialFunction.h"

namespace Durin
{
	using namespace Editor::Material;
	namespace
	{
		template<typename TMaterial>
		auto CreateMaterialAsset(const FTopLevelAssetPath& AssetPath,
			std::string& OutError) -> bool
		{
			const FAssetToolsResult Created = IAssetTools::Get().CreateAsset(
				AssetPath, TMaterial::StaticClass());
			if (!Created)
			{
				OutError = Created.Message;
				return false;
			}
			const auto Discard = [&] {
				if (!IAssetTools::Get().DiscardPackage(Created.Package))
					OutError += " The unsaved material package could not be discarded.";
			};
			if constexpr (std::same_as<TMaterial, DMaterial>)
			{
				if (!PrepareNewMaterialForEditing(*Cast<DMaterial>(Created.Asset), OutError))
				{
					Discard();
					return false;
				}
			}
			const FAssetWriteResult Result = SavePackage(Created.Package);
			if (!Result)
			{
				OutError = Result.Message;
				Discard();
				return false;
			}
			return true;
		}

	}

	IMPLEMENT_MODULE(FMaterialEditorModule, MaterialEditor)

	struct FMaterialEditorModule::FIntegrationState : IAssetMoveObserver
	{
		FAssetMoveObserverHandle MoveObserver = 0;
		auto OnAssetsRelocated(std::span<const FAssetRelocationMapping> Mappings) -> void override
		{ FMaterialEditorSessionSettings::RemapStoredViewports(Mappings); }
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration>
			ContentBrowserExtensions;
	};

	FMaterialEditorModule::FMaterialEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}

	FMaterialEditorModule::~FMaterialEditorModule() = default;

	auto FMaterialEditorModule::StartupModule() -> void
	{
		Integration->MoveObserver = RegisterAssetMoveObserver(Integration.get());
		DetailCustomizationRegistration = Editor::FDetailCustomizationRegistry::Get().RegisterDetailCustomization(
			DMaterialParameterCollection::StaticClass(), Editor::Material::CreateMaterialParameterCollectionDetails());
		require(static_cast<bool>(DetailCustomizationRegistration));
		require(RegisterContributions(Editor::GetDefaultThumbnailManager()));
	}

	auto FMaterialEditorModule::ShutdownModule() -> void
	{
		UnregisterAssetMoveObserver(Integration->MoveObserver);
		Integration->MoveObserver = 0;
		UnregisterContributions();
		Editor::FDetailCustomizationRegistry::Get().Unregister(DetailCustomizationRegistration);
		DetailCustomizationRegistration = {};
		Editor::Material::FMaterialGraphCanvas::ClearSharedClipboard();
		if (GEditor)
			checkf(GEditor->GetTransactor()
				->DiscardCustomChangesByModule("MaterialEditor"),
				"MaterialEditor cannot retire while one of its custom changes is active");
	}

	auto FMaterialEditorModule::RegisterContributions(Editor::DThumbnailManager& ThumbnailManager) -> bool
	{
		check(!MaterialThumbnailRegistration && !MaterialInstanceThumbnailRegistration);

		auto MaterialHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DMaterialThumbnailRenderer>(
				DMaterial::StaticClass()->GetQualifiedName().ToString()));
		if (!MaterialHandle)
		{
			return false;
		}
		MaterialThumbnailRegistration =
			std::make_unique<Editor::FThumbnailRendererRegistrationHandle>(
				std::move(*MaterialHandle));
		auto InstanceHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DMaterialThumbnailRenderer>(
				DMaterialInstance::StaticClass()->GetQualifiedName().ToString()));
		if (!InstanceHandle)
		{
			MaterialThumbnailRegistration.reset();
			return false;
		}
		MaterialInstanceThumbnailRegistration =
			std::make_unique<Editor::FThumbnailRendererRegistrationHandle>(
				std::move(*InstanceHandle));
		const auto RegisterCreate = [this](std::string Id, std::string Label,
			std::string BaseName, bool bInstance) {
			std::string Error;
			auto Handle = Editor::ContentBrowser::RegisterAssetCreation({
				.Id = std::move(Id),
				.Label = std::move(Label),
				.DefaultName = std::move(BaseName),
				.Order = bInstance ? 210 : 200,
				.Create = bInstance ? CreateMaterialAsset<DMaterialInstance>
					: CreateMaterialAsset<DMaterial>,
				.AssetClassNameToOpen = (bInstance ? DMaterialInstance::StaticClass()
					: DMaterial::StaticClass())->GetQualifiedName().ToString(),
				}, Error);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register Content Browser material creation: {}", Error);
				return false;
			}
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));
			return true;
		};
		if (!RegisterCreate("material.create-material", "Material", "NewMaterial", false)
			|| !RegisterCreate("material.create-instance", "Material Instance",
				"NewMaterialInstance", true))
		{
			Integration->ContentBrowserExtensions.clear();
			MaterialInstanceThumbnailRegistration.reset();
			MaterialThumbnailRegistration.reset();
			return false;
		}
		std::string PresentationError;
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetCreation({
				.Id = "material.create-function", .Label = "Material Function", .DefaultName = "NewMaterialFunction", .Order = 220,
				.Create = CreateMaterialAsset<DMaterialFunction>,
				.AssetClassNameToOpen = DMaterialFunction::StaticClass()->GetQualifiedName().ToString()}, PresentationError);
			if (!Handle.IsValid()) { UnregisterContributions(); return false; }
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));

		}
		{
			auto Handle = Editor::ContentBrowser::RegisterAssetCreation({
				.Id = "material.create-parameter-collection",
				.Label = "Material Parameter Collection",
				.DefaultName = "NewMaterialParameterCollection", .Order = 230,
				.Create = CreateMaterialAsset<DMaterialParameterCollection>,
				.AssetClassNameToOpen = DMaterialParameterCollection::StaticClass()
					->GetQualifiedName().ToString()},
				PresentationError);
			if (!Handle.IsValid()) { UnregisterContributions(); return false; }
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));

		}


		return true;
	}

	auto FMaterialEditorModule::UnregisterContributions() -> void
	{
		Integration->ContentBrowserExtensions.clear();
		MaterialInstanceThumbnailRegistration.reset();
		MaterialThumbnailRegistration.reset();
	}
}
