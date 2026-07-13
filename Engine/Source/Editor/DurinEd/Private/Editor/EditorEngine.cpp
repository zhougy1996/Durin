#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"

#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	DEditorEngine* GEditor = nullptr;

	DEditorEngine::DEditorEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, TransactionManager(std::make_unique<FEditorTransactionManager>())
	{
		GEditor = this;
	}

	DEditorEngine::~DEditorEngine()
	{
		if (GEditor == this) GEditor = nullptr;
	}

	auto DEditorEngine::Init() -> void
	{
		DEngine::Init();

		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		MainFrameModule.CreateDefaultMainFrame();
		DURIN_DEBUG("Editor initialized successfully");
	}

	auto DEditorEngine::GetTransactionManager() -> FEditorTransactionManager&
	{
		return *TransactionManager;
	}
}
