#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserFileDataSourceAPI.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/StrongObjectPtr.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserFileProviderSubsystem.gen.h"
namespace Durin
{
	DCLASS()
	class DContentBrowserFileProviderSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSERFILEDATASOURCE_API explicit DContentBrowserFileProviderSubsystem(const FObjectInitializer&);
		CONTENTBROWSERFILEDATASOURCE_API auto Initialize(FSubsystemCollection&) -> void override;
		CONTENTBROWSERFILEDATASOURCE_API auto Deinitialize() noexcept -> void override;
	private:
		TStrongObjectPtr<DContentBrowserDataSource> Source;
	};
}
