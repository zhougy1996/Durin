#pragma once

#include "DObject/Object.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "LevelEditorAPI.h"
#include "Viewport/TransformProxy.h"
#include "TransformGizmoStateTarget.gen.h"

namespace Durin
{
	class DTransactor;

	// Transaction and restore lifetime for one transform gesture.
	DCLASS(NoClassDefaultObject)
	class DTransformGizmoStateTarget : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DTransformGizmoStateTarget(const FObjectInitializer& Initializer) : Super(Initializer) {}
		LEVELEDITOR_API void Initialize(DTransformProxy* InProxy, DTransactor* Transactions, std::string_view InAction);
		LEVELEDITOR_API void BeginUpdate();
		LEVELEDITOR_API void EndUpdate();
		LEVELEDITOR_API void CancelUpdate();
		LEVELEDITOR_API bool CanContinueWith(const DTransactor* Transactions) const;
		void RequestCancel() { bCancelPending = true; }
		bool IsCancelPending() const { return bCancelPending; }

	private:
		bool OwnsRecording() const;
		void ReleaseGesture();
		TStrongObjectPtr<DTransformProxy> Proxy;
		std::optional<FTransformProxyChangeSource> ChangeSource;
		TWeakObjectPtr<DTransactor> TransactionManager;
		std::string Action;
		uint64 RecordingOperationId = 0;
		bool bCancelPending = false;
	};
}
