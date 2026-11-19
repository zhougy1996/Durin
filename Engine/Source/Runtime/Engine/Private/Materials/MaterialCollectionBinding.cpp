#include "MaterialCollectionBinding.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialParameterCollection.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::MaterialBuildPrivate
{
	auto BindMaterialShaderMap(FMaterialProgram& Program, std::shared_ptr<const FMaterialShaderMap> ShaderMap,
		const MIR::FCompilerInput& Input, EMaterialCollectionSource Source,
		std::vector<FMaterialProgramDiagnostic>& Diagnostics) -> bool
	{
		if (!ShaderMap) {
			Diagnostics.push_back({.Message = "Material compilation did not produce a shader map."});
			return false;
		}
		FMaterialProgram Bound;
		Bound.Quality = Input.Environment.Quality;
		Bound.FeatureLevel = Input.Environment.FeatureLevel;
		Bound.StaticBools = Input.Environment.StaticBools;
		std::vector<DObject*> Objects;
		if (Source == EMaterialCollectionSource::LiveObjects && !ShaderMap->CollectionLayouts.empty()) {
			if (GIsGameThreadIdInitialized) CheckGameThread();
			Objects = GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
		}
		for (const auto& Layout : ShaderMap->CollectionLayouts) {
			const auto Captured = std::ranges::find(Input.Collections, Layout.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
			const auto Fail = [&](std::string_view Reason) {
				Diagnostics.push_back({.Message = std::format("Material parameter collection {} ({}): {}.",
					Layout.CollectionId.ToString(), Captured == Input.Collections.end() ? Layout.AssetPath : Captured->AssetPath, Reason)});
				return false;
			};
			if (Captured == Input.Collections.end()) return Fail("missing from compile request");
			if (!Layout.HasCompatibleSchema(*Captured)) return Fail("request schema differs from compiled shader map");
			if (Source == EMaterialCollectionSource::RequestSnapshot) {
				Bound.ActiveCollections.push_back(*Captured);
				continue;
			}
			DMaterialParameterCollection* Found = nullptr;
			for (auto* Object : Objects)
				if (auto* Collection = Cast<DMaterialParameterCollection>(Object); IsValid(Collection)
					&& Collection->GetCollectionId() == Layout.CollectionId
					&& (Captured->AssetPath.empty() || Collection->GetObjectPath() == Captured->AssetPath))
				{ Found = Collection; break; }
			if (!Found) return Fail("live collection is missing");
			auto Current = Found->BuildLayout();
			if (!Current || !Layout.HasCompatibleSchema(*Current)) return Fail("live schema differs from compiled shader map");
			Bound.ActiveCollections.push_back(std::move(*Current));
		}
		Bound.ShaderMap = std::move(ShaderMap);
		Program = std::move(Bound);
		return true;
	}
}
