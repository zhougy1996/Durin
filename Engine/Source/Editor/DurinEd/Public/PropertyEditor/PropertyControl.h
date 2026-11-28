#pragma once

#include "CoreMinimal.h"

#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "PropertyEditor/PropertyHandle.h"

namespace Durin::PropertyEditor
{
	// Owns the handle and transaction for one active widget gesture.
	class FPropertyControl
	{
	public:
		static constexpr auto InteractiveFlags = EPropertyValueSetFlags::InteractiveChange | EPropertyValueSetFlags::NotTransactable;

		// Defers finishing the gesture until the outermost handle call returns.
		class FInput
		{
		public:
			DURINED_API FInput(FPropertyControl& Control, std::string* OutError = nullptr);
			DURINED_API ~FInput();
			FInput(const FInput&) = delete;
			explicit operator bool() const { return Handle != nullptr; }
			auto operator->() const -> IPropertyHandle* { return Handle; }

		private:
			FPropertyControl& Control;
			std::string* Error;
			IPropertyHandle* Handle = nullptr;
		};

		DURINED_API ~FPropertyControl();
		FPropertyControl() = default;
		FPropertyControl(const FPropertyControl&) = delete;
		auto operator=(const FPropertyControl&) -> FPropertyControl& = delete;
		DURINED_API auto BeginGesture(std::shared_ptr<IPropertyHandle> Handle, bool bShouldTransact,
			bool bColorPicker = false, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Finish(bool bCancelPicker, std::string* OutError = nullptr, bool* OutChanged = nullptr) -> bool;
		auto IsActive() const -> bool { return ActiveHandle != nullptr; }
		auto IsEditingObject(const DObject* Object) const -> bool { return ActiveHandle && Owner.Get() == Object; }
		auto MatchesTarget(const FPropertyEditTarget& Target) const -> bool
		{ return ActiveHandle && ActiveHandle->ResolveTarget().MatchesContinuousEdit(Target); }
		auto GetHandle() const -> const std::shared_ptr<IPropertyHandle>& { return ActiveHandle; }

	private:
		struct FColorPicker
		{
			FPropertyValueSnapshot Original;
			auto Capture(const FPropertyEditTarget& Target) -> std::expected<void, std::string>;
			auto Restore(IPropertyHandle& Handle, const FPropertyEditTarget& Target) const -> std::expected<bool, std::string>;
		};
		auto HasRecording() const -> bool;
		int32 InputDepth = 0;
		std::optional<bool> PendingFinish;
		std::shared_ptr<IPropertyHandle> ActiveHandle;
		TStrongObjectPtr<DObject> Owner;
		TWeakObjectPtr<DTransactor> History;
		FTransactionId Recording = 0;
		bool bOwnsRecording = false;
		std::optional<FColorPicker> Picker;
	};
}
