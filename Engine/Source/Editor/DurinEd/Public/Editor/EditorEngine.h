#pragma once

#include "DurinEdAPI.h"
#include "Engine/Engine.h"

#include "EditorEngine.gen.h"

namespace Durin
{
	class FEditorTransactionManager;
	class FEditorNotificationManager;

	DCLASS()
	class DEditorEngine : public DEngine
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorEngine(const FObjectInitializer& ObjectInitializer);
		DURINED_API ~DEditorEngine() override;
		DURINED_API auto Init() -> void override;
		DURINED_API auto GetTransactionManager() -> FEditorTransactionManager&;
		DURINED_API auto GetNotificationManager() -> FEditorNotificationManager&;

	private:
		std::unique_ptr<FEditorTransactionManager> TransactionManager;
		std::unique_ptr<FEditorNotificationManager> NotificationManager;
	};

	extern DURINED_API DEditorEngine* GEditor;
}
