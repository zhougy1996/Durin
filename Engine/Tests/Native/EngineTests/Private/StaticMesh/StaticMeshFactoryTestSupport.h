#pragma once

#include "Asset/PackageSerialization.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/DObjectGlobals.h"
#include "FactoryImportTestSupport.h"
#include "Modules/ModuleManager.h"

namespace Durin::AssetForge::Builtins
{
	// Test-only result adapter that keeps existing assertions concise while
	// exercising the production Factory/AssetTools creation path.
	inline auto ImportStaticMeshForTest(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FStaticMeshImportSettings& Settings = {})
		-> Durin::Testing::TFactoryImportResult<Durin::DStaticMesh>
	{
		FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
		FPackagePath ParsedPath;
		std::string Error;
		if (!FPackagePath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		auto* Factory = NewObject<DStaticMeshFactory>(
			nullptr, "StaticMeshTestFactory", EObjectFlags::Transient);
		Factory->SetImportSettings(Settings);
		const FAssetToolsResult Imported = IAssetTools::Get().ImportPackageLeafAssetForTesting(
			ParsedPath, DStaticMesh::StaticClass(), FilePath, Factory);
		auto* Mesh = Cast<DStaticMesh>(Imported.Asset);
		if (!Imported || !Mesh)
			return {false, Imported.Message, Mesh};
		const FAssetResult Saved = SavePackage(Imported.Package);
		return Saved
			? Durin::Testing::TFactoryImportResult<Durin::DStaticMesh>{true, {}, Mesh}
			: Durin::Testing::TFactoryImportResult<Durin::DStaticMesh>{false, Saved.Message, Mesh};
	}
}
