#include "AssetTools/IAssetTools.h"

#include "Asset/AssetOperations.h"
#include "Asset/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Factories/Factory.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ValidateFactory(
			const DFactory* Factory,
			const DClass* AssetClass,
			std::string& OutError) -> bool
		{
			if (!Factory)
			{
				OutError = "No factory supports the requested asset.";
				return false;
			}
			const DClass* SupportedClass = Factory->GetSupportedClass();
			if (!SupportedClass || !AssetClass->IsChildOf(SupportedClass))
			{
				OutError = "The selected factory does not support the requested asset class.";
				return false;
			}
			return true;
		}

		class FAssetTools final : public IAssetTools
		{
		public:
			auto CreateAsset(
				const FAssetPath& AssetPath,
				DClass* AssetClass,
				const DFactory* Factory,
				DObject* Context,
				EObjectFlags Flags) -> FAssetToolsResult override
			{
				return CreateWithFactory(
					AssetPath, AssetClass, Factory, {}, Context, Flags, false);
			}

			auto ImportAsset(
				const FAssetPath& AssetPath,
				DClass* AssetClass,
				std::string_view Filename,
				const DFactory* Factory,
				DObject* Context,
				EObjectFlags Flags) -> FAssetToolsResult override
			{
				return CreateWithFactory(
					AssetPath, AssetClass, Factory, Filename, Context, Flags, true);
			}

			auto DiscardPackage(DPackage* Package) -> bool override
			{
				checkf(IsInGameThread(), "Asset package discard must run on the game thread.");
				if (!Package || !Package->IsAssetPackage()) return false;
				FAssetPath Path;
				if (!FAssetPath::TryCreate(Package->GetPackagePath(), Path)) return false;
				return Asset::UnloadPackage(
					Package, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved).Succeeded();
			}

		private:
			static auto CreateWithFactory(
				const FAssetPath& AssetPath,
				DClass* AssetClass,
				const DFactory* RequestedFactory,
				std::string_view Filename,
				DObject* Context,
				EObjectFlags Flags,
				bool bFromFile) -> FAssetToolsResult
			{
				checkf(IsInGameThread(),
					"Asset tools creation must run on the game thread.");
				if (!AssetPath.IsValid())
					return {.Message = "The destination asset path is invalid."};
				if (!AssetClass || !AssetClass->IsChildOf(DObject::StaticClass())
					|| AssetClass->HasAnyClassFlags(EClassFlags::Abstract))
					return {.Message = "The requested asset class cannot be constructed."};
				if (FindPackage(AssetPath.GetView()) || Asset::FindAssetExact(AssetPath))
					return {.Message = std::format(
						"Asset {} already exists.", AssetPath.ToString())};
				if (bFromFile && Filename.empty())
					return {.Message = "A source filename is required for import."};

				const DFactory* Factory = RequestedFactory;
				if (!Factory && bFromFile)
				{
					const std::string Extension =
						std::filesystem::path(Filename).extension().generic_string();
					std::vector<const DFactory*> Candidates =
						DFactory::FindFactories(AssetClass, Extension);
					if (Candidates.size() > 1)
						return {.Message = std::format(
							"Multiple factories support {} for the requested asset class.",
							Extension)};
					if (!Candidates.empty()) Factory = Candidates.front();
				}
				if (!Factory) Factory = DFactory::FindFactory(AssetClass);
				std::string Error;
				if (!ValidateFactory(Factory, AssetClass, Error))
					return {.Message = std::move(Error)};

				DPackage* Package = CreatePackage(AssetPath);
				if (!Package)
					return {.Message = "The destination package could not be created."};
				if (Asset::FAssetResult AdoptResult = Asset::AdoptCreatedPackage(Package);
					!AdoptResult)
				{
					MarkObjectHierarchyAsGarbage(Package);
					CollectGarbage();
					return {.Message = AdoptResult.Message};
				}

				FFactoryDiagnostics Diagnostics;
				const FName AssetName(AssetPath.GetAssetName());
				DObject* Asset = bFromFile
					? Factory->FactoryCreateFromFile(
						AssetClass, Package, AssetName, Flags, Filename, Context, &Diagnostics)
					: Factory->FactoryCreateNew(
						AssetClass, Package, AssetName, Flags, Context, &Diagnostics);
				if (!Asset)
				{
					GetAssetTools().DiscardPackage(Package);
					std::string Message = Diagnostics.ToString();
					if (Message.empty()) Message = bFromFile
						? "The factory could not import the asset."
						: "The factory could not create the asset.";
					return {.Message = std::move(Message)};
				}
				if (!Asset->IsA(AssetClass) || !Package->SetAsset(Asset))
				{
					GetAssetTools().DiscardPackage(Package);
					return {.Message =
						"The factory returned an invalid package main asset."};
				}
				return {.Asset = Asset, .Package = Package,
					.Message = Diagnostics.ToString()};
			}
		};
	}

	auto GetAssetTools() -> IAssetTools&
	{
		static FAssetTools AssetTools;
		return AssetTools;
	}
}
