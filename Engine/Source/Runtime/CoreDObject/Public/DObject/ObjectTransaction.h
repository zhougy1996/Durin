#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;

	// Binds object modification to the current editor-thread recording owner.
	struct FObjectTransaction
	{
		void* Owner = nullptr;
		bool (*SaveObject)(void* Owner, DObject* Object) = nullptr;
		DObject* ObjectOwner = nullptr;
	};
	COREDOBJECT_API auto RegisterObjectTransaction(const std::shared_ptr<FObjectTransaction>& Recorder) -> void;
	COREDOBJECT_API auto UnregisterObjectTransaction(const std::shared_ptr<FObjectTransaction>& Recorder) -> void;
	COREDOBJECT_API auto GetObjectTransactionRecorder() -> FObjectTransaction*;
	COREDOBJECT_API auto HasObjectTransactionBinding() -> bool;

	// Execution-only binding. It never changes the lifetime of a recording.
	class FScopedObjectTransactionBinding
	{
	public:
		COREDOBJECT_API explicit FScopedObjectTransactionBinding(std::shared_ptr<FObjectTransaction> Recorder);
		COREDOBJECT_API ~FScopedObjectTransactionBinding();
		FScopedObjectTransactionBinding(const FScopedObjectTransactionBinding&) = delete;
		auto operator=(const FScopedObjectTransactionBinding&) -> FScopedObjectTransactionBinding& = delete;
	private:
		std::shared_ptr<FObjectTransaction> Previous;
	};
}
