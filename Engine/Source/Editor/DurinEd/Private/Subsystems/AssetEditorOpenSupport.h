#pragma once

#include "CoreMinimal.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "DObject/Class.h"

namespace Durin
{
	// Shared admission policy for reflected editors and direct Toolkits.
	inline auto GetAssetSelectionOpenSupport(std::span<DObject* const> Assets,
		EAssetOpenMethod Method, FAssetOpenSupport Support) -> FAssetOpenSupport
	{
		if (Support.OpenMethod != Method) return {Method, false};
		for (auto* Asset : Assets)
		{
			if (!IsValid(Asset)) return {Method, false};
			if (const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Asset->GetClass()))
			{
				const auto AssetSupport = Definition->GetAssetOpenSupport({Method});
				if (!AssetSupport.IsSupported || AssetSupport.OpenMethod != Method
					|| (Support.RequiredToolkitMode && AssetSupport.RequiredToolkitMode && Support.RequiredToolkitMode != AssetSupport.RequiredToolkitMode)) return {Method, false};
				if (AssetSupport.RequiredToolkitMode) Support.RequiredToolkitMode = AssetSupport.RequiredToolkitMode;
			}
		}
		return Support;
	}
}
