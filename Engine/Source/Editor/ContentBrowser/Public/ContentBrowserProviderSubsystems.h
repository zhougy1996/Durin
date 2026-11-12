#pragma once
#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/StrongObjectPtr.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserProviderSubsystems.gen.h"
namespace Durin
{
	DCLASS()
	class DContentBrowserAssetProviderSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserAssetProviderSubsystem(const FObjectInitializer&);
		CONTENTBROWSER_API auto Initialize(FSubsystemCollection&) -> void override;
		CONTENTBROWSER_API auto Deinitialize() noexcept -> void override;
	private:
		TStrongObjectPtr<DContentBrowserDataSource> Source;
	};
	DCLASS()
	class DContentBrowserFileProviderSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserFileProviderSubsystem(const FObjectInitializer&);
		CONTENTBROWSER_API auto Initialize(FSubsystemCollection&) -> void override;
		CONTENTBROWSER_API auto Deinitialize() noexcept -> void override;
	private:
		TStrongObjectPtr<DContentBrowserDataSource> Source;
	};
}
