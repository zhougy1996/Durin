#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserAssetDataSourceAPI.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/StrongObjectPtr.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserAssetProviderSubsystem.gen.h"
namespace Durin
{
	DCLASS()
	class DContentBrowserAssetProviderSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSERASSETDATASOURCE_API explicit DContentBrowserAssetProviderSubsystem(const FObjectInitializer&);
		CONTENTBROWSERASSETDATASOURCE_API auto Initialize(FSubsystemCollection&) -> void override;
		CONTENTBROWSERASSETDATASOURCE_API auto Deinitialize() noexcept -> void override;
	private:
		TStrongObjectPtr<DContentBrowserDataSource> Source;
	};
}
