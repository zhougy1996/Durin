#include "PackageTools.h"
#include "Misc/FeedbackContext.h"
#include "MaterialEditorModule.h"
#include "Settings/MaterialEditorProfileSettings.h"
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
	using namespace MaterialEditor;
	namespace
	{
		template<typename TMaterial>
		auto CreateMaterialAsset(const FTopLevelAssetPath& AssetPath,
			std::string& OutError) -> bool
		{
			std::string CreatedError;
			FFeedbackContext CreatedFeedback({}, [&](ELogLevel Level, std::string_view Message) {
				if (Level >= ELogLevel::Error)
				{
					if (!CreatedError.empty()) CreatedError += "\n";
					CreatedError += Message;
				}
			});
			Durin::FScopedFeedbackContext CreationFeedbackScope(CreatedFeedback);
			const auto Created = Durin::IAssetTools::Get().CreateAsset((AssetPath).GetAssetName(), (AssetPath).GetPackagePath().ToString().substr(0, (AssetPath).GetPackagePath().ToString().find_last_of('/')), TMaterial::StaticClass(), nullptr);
			if (!Created)
			{
				OutError = CreatedError;
				return false;
			}
			const auto Discard = [&] {
				if (!([&] { std::string Error; return Durin::DPackageTools::UnloadPackages(std::array{Created->GetPackage()}, Error, true) && Error.empty(); }()))
					OutError += " The unsaved material package could not be discarded.";
			};
			if constexpr (std::same_as<TMaterial, DMaterial>)
			{
				if (!PrepareNewMaterialForEditing(*Cast<DMaterial>(Created), OutError))
				{
					Discard();
					return false;
				}
			}
			FAssetWriteResult Result;
			if (!SavePackage(Created->GetPackage(), {}, &Result))
			{
				OutError = (Result.Error ? Result.Error->ToString() : std::string{});
				Discard();
				return false;
			}
			return true;
		}

	}

	IMPLEMENT_MODULE(FMaterialEditorModule, MaterialEditor)

	struct FMaterialEditorModule::FIntegrationState
	{
		FDelegateHandle RenameHandle;
		std::vector<ContentBrowser::FScopedExtensionRegistration>
			ContentBrowserExtensions;
	};

	FMaterialEditorModule::FMaterialEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}

	FMaterialEditorModule::~FMaterialEditorModule() = default;

	auto FMaterialEditorModule::StartupModule() -> void
	{
		Integration->RenameHandle = OnAssetRenamed().AddLambda([](const FAssetData& Asset, const FSoftObjectPath& Old) { FMaterialEditorProfileSettings::RemapStoredViewports(Asset, Old); });
		DetailCustomizationRegistration = FDetailCustomizationRegistry::Get().RegisterDetailCustomization(
			DMaterialParameterCollection::StaticClass(), MaterialEditor::CreateMaterialParameterCollectionDetails());
		require(static_cast<bool>(DetailCustomizationRegistration));
		require(RegisterContributions(GetDefaultThumbnailManager()));
	}

	auto FMaterialEditorModule::ShutdownModule() -> void
	{
		OnAssetRenamed().Remove(Integration->RenameHandle);
		Integration->RenameHandle = {};
		UnregisterContributions();
		FDetailCustomizationRegistry::Get().Unregister(DetailCustomizationRegistration);
		DetailCustomizationRegistration = {};
		MaterialEditor::FMaterialGraphCanvas::ClearSharedClipboard();
		if (GEditor)
			checkf(GEditor->GetTransactor()
				->DiscardCustomChangesByModule("MaterialEditor"),
				"MaterialEditor cannot retire while one of its custom changes is active");
	}

	auto FMaterialEditorModule::RegisterContributions(DThumbnailManager& ThumbnailManager) -> bool
	{
		check(!MaterialThumbnailRegistration && !MaterialInstanceThumbnailRegistration);

		auto MaterialHandle = ThumbnailManager.RegisterCustomRenderer(DMaterial::StaticClass(), DMaterialThumbnailRenderer::StaticClass());
		if (!MaterialHandle)
		{
			return false;
		}
		MaterialThumbnailRegistration =
			std::make_unique<FThumbnailRendererRegistrationHandle>(
				std::move(MaterialHandle));
		auto InstanceHandle = ThumbnailManager.RegisterCustomRenderer(DMaterialInstance::StaticClass(), DMaterialThumbnailRenderer::StaticClass());
		if (!InstanceHandle)
		{
			MaterialThumbnailRegistration.reset();
			return false;
		}
		MaterialInstanceThumbnailRegistration =
			std::make_unique<FThumbnailRendererRegistrationHandle>(
				std::move(InstanceHandle));
		const auto RegisterCreate = [this](std::string Id, std::string Label,
			std::string BaseName, bool bInstance) {
			std::string Error;
			auto Handle = ContentBrowser::RegisterAssetCreation({
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
			auto Handle = ContentBrowser::RegisterAssetCreation({
				.Id = "material.create-function", .Label = "Material Function", .DefaultName = "NewMaterialFunction", .Order = 220,
				.Create = CreateMaterialAsset<DMaterialFunction>,
				.AssetClassNameToOpen = DMaterialFunction::StaticClass()->GetQualifiedName().ToString()}, PresentationError);
			if (!Handle.IsValid()) { UnregisterContributions(); return false; }
			Integration->ContentBrowserExtensions.push_back(std::move(Handle));

		}
		{
			auto Handle = ContentBrowser::RegisterAssetCreation({
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
