#pragma once

#include "CoreMinimal.h"

#include "DObject/AssetPath.h"
#include "PropertyEditor/PropertyControl.h"
#include "PropertyEditor/PropertyControlFrame.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Widgets/MonaImGuiPropertyTable.h"

namespace Durin
{
	class DObject;

	class FArrayProperty;
	class FMapProperty;
	class FProperty;
	class FSoftObjectProperty;
	class FWeakObjectProperty;
}

namespace Durin
{
	enum class ESoftObjectViewState : uint8
	{
		Null,
		Unloaded,
		Loaded,
		Redirected,
		Missing,
		TypeMismatch,
	};

	enum class ESoftObjectViewError : uint8 { None, Storage, Accessor, Asset };

	// Describes a soft reference without loading its target.
	struct FSoftObjectViewState
	{
		ESoftObjectViewState State = ESoftObjectViewState::Null;
		FSoftObjectPath Path;
		FSoftObjectPath ResolvedPath;
		DObject* LoadedObject = nullptr;
		ESoftObjectViewError Error = ESoftObjectViewError::None;
		std::string PropertyName;
		uint32 ArrayIndex = 0;
		int32 ArrayDim = 0;
		std::string AssetDiagnostic;
	};

	enum class EWeakObjectViewState : uint8 { Null, Live, Expired, TypeMismatch };

	struct FWeakObjectViewState
	{
		EWeakObjectViewState State = EWeakObjectViewState::Null;
		DObject* Object = nullptr;
	};

	// Supplies transaction, error, and read-only policy to a property view.
	struct FPropertyViewContext
	{
		bool bShouldTransact = true;
		// Reports editor actions and host operations.
		std::function<void(std::string)> ReportError;
		// Overrides the default non-modal property edit feedback.
		std::function<void(std::string)> ReportPropertyError;
		std::function<bool(const FPackagePath&, std::string&)> RevealAsset;
		std::function<bool(const FPackagePath&, std::string&)> OpenAsset;
		bool bReadOnly = false;
	};

	// Overrides presentation for one reflected property row.
	struct FPropertyViewOptions
	{
		std::string Label;
		// Shows only the existing Location/Rotation/Scale controls when their category provides the header.
		bool bShowTransformHeader = true;
	};

	// An embeddable immediate-mode property view. It owns only transient widget/edit
	// state; committed history remains owned by the transactor supplied by its host.
	class FPropertyView
	{
	public:
		DURINED_API auto EditProperty(
			const FPropertyViewContext& Context,
			DObject* Object,
			FProperty* Property,
			uint32 ArrayIndex = 0,
			const FPropertyViewOptions& Options = {}
		) -> bool;

		DURINED_API auto SubmitPropertyValueEdit(
			const FPropertyViewContext& Context,
			const FPropertyEditTarget& Target,
			const std::function<void(FProperty*, void*, uint32)>& AssignValue,
			bool bContinuous
		) -> bool;
		DURINED_API auto UpdatePropertyValueEdit(
			const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
			const MonaImGui::PropertyEdit::FPropertyInteractionState& Interaction,
			const std::function<void(FProperty*, void*, uint32)>& AssignValue = {}
		) -> bool;
		DURINED_API auto EndInteractionFrame(const FPropertyViewContext& Context, bool* OutChanged = nullptr) -> bool;

		DURINED_API auto ReportEditError(const FPropertyViewContext& Context, std::string Error) -> void;

		// Reports whether the active edit has ended.
		DURINED_API auto FinishActiveEdit(const FPropertyViewContext* Context, bool bCancel) -> bool;
		DURINED_API auto HandleOwnerContext(const FPropertyViewContext& Context, DObject* Object) -> bool;
		auto IsEditing() const -> bool { return Gesture.IsActive(); }
		auto IsEditingObject(const DObject* Object) const -> bool
		{ return Gesture.IsEditingObject(Object); }
		auto IsEditingTarget(const FPropertyEditTarget& Target) const -> bool
		{ return Gesture.MatchesTarget(Target); }

	private:
		using FWidgetAssignment = std::function<void(FProperty*, void*, uint32)>;

		// Captures widget output without exposing a live reflected target to UI code.
		struct FPropertyWidgetEditResult
		{
			// The destination is always temporary or detached storage selected by the caller.
			// Widget code never receives an object or reflected edit target.
			FWidgetAssignment AssignValue;
			bool bChanged = false;
			bool bContinuous = false;
			MonaImGui::PropertyEdit::FPropertyInteractionState Interaction;
		};

		// Retains detached key/value drafts while a map insertion UI is active.
		struct FMapInsertDraft
		{
			FPropertyEditTarget Target;
			FPropertyValueSnapshot Key;
			FPropertyValueSnapshot Value;
			bool bActive = false;
		};

		auto EditPropertyValue(
			const FPropertyViewContext& Context,
			DObject* Object,
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			const std::string& Label,
			bool bReadOnly,
			const FPropertyEditTarget& EditTarget, bool bShowTransformHeader = true
		) -> bool;
		auto DrawAndSubmitPropertyWidget(const FPropertyViewContext& Context, DObject* Object,
			FProperty* Property, void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
			const FPropertyEditTarget* ExistingTarget = nullptr, bool bStructMember = false, bool bShowTransformHeader = true) -> bool;
		auto EditPropertyWidget(
			const FPropertyViewContext& Context,
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			const std::string& Label,
			bool bReadOnly, bool bShowTransformHeader = true
		) -> FPropertyWidgetEditResult;
		auto EditStructPropertyWidget(FProperty* Property, void* Container, uint32 ArrayIndex,
			const std::string& Label, bool bReadOnly, bool bShowTransformHeader = true) -> FPropertyWidgetEditResult;
		auto EditStructProperty(const FPropertyViewContext& Context, DObject* Object,
			FProperty* Property, void* Container, uint32 ArrayIndex, const std::string& Label,
			bool bReadOnly, const FPropertyEditTarget& EditTarget) -> bool;
		auto SubmitWidgetEdit(const FPropertyViewContext& Context,
			const FPropertyEditTarget& EditTarget, const FPropertyWidgetEditResult& Edit) -> bool;
		auto EditArrayProperty(const FPropertyViewContext& Context, DObject* Object, FArrayProperty* Property,
			void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
			const FPropertyEditTarget& EditTarget) -> bool;
		auto EditMapProperty(const FPropertyViewContext& Context, DObject* Object, FMapProperty* Property,
			void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
			const FPropertyEditTarget& EditTarget) -> bool;
		auto FinishSession(const FPropertyViewContext* Context, bool bCancel, bool* OutChanged = nullptr) -> bool;
		auto ReconcileInteraction(const FPropertyViewContext& Context, bool bEndOfFrame = false) -> bool;
		auto PreparePropertyWrite(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
			const MonaImGui::PropertyEdit::FPropertyInteractionState* Interaction = nullptr) -> bool;
		auto BeginEdit(const FPropertyViewContext& Context, const std::shared_ptr<PropertyEditor::IPropertyHandle>& Handle, bool bColorPicker = false) -> bool;
		auto WritePropertyValue(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
			const std::function<void(FProperty*, void*, uint32)>& AssignValue, bool bContinuous, bool* OutChanged) -> bool;
		template<typename TWrite>
		auto ApplyPropertyDraft(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
			bool bContinuous, bool* OutChanged, TWrite&& Write) -> bool;
		auto ApplyPropertyValueEdit(const FPropertyViewContext& Context, const FPropertyEditTarget& Target,
			const std::function<void(FProperty*, void*, uint32)>& AssignValue, bool bContinuous,
			const MonaImGui::PropertyEdit::FPropertyInteractionState* Interaction = nullptr, bool* OutChanged = nullptr) -> bool;

		std::array<char, 256> AssetSearchText{};
		FMapInsertDraft MapInsertDraft;
		struct FMapKeyInteraction
		{
			FPropertyValueSnapshot Value;
			ImGuiID Id = 0;
		} MapKeyInteraction;
		std::string LastTransientError;
		double LastTransientErrorTime = 0.0;
		PropertyEditor::FPropertyControl Gesture;
		struct FWidgetTarget
		{
			std::shared_ptr<PropertyEditor::IPropertyHandle> Handle;
			bool bColorPicker;
		};
		PropertyEditor::TPropertyControlFrame<ImGuiID, FWidgetTarget, FWidgetAssignment> Controls;
		DObject* OwnerContextObject = nullptr;
		DObject* ActiveEditOwnerObject = nullptr;
		MonaImGui::PropertyEdit::FPropertyEditInteraction EditInteraction;
	};

	DURINED_API auto MakePropertyDisplayName(
		std::string_view PropertyName,
		DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view ExplicitDisplayName = {}
	) -> std::string;
	DURINED_API auto MakePropertyLabel(const FProperty& Property, uint32 ArrayIndex = 0) -> std::string;
	DURINED_API auto InspectSoftObject(
		FSoftObjectProperty* Property, void* Container, uint32 ArrayIndex = 0
	) -> FSoftObjectViewState;
	enum class EPropertySoftLoadError : uint8 { None, Storage, Accessor, Asset, MissingObject };
	struct FPropertySoftLoadResult
	{
		EPropertySoftLoadError Error = EPropertySoftLoadError::None;
		std::string PropertyName;
		uint32 ArrayIndex = 0;
		int32 ArrayDim = 0;
		FSoftObjectPath Path;
		std::string AssetDiagnostic;
		explicit operator bool() const { return Error == EPropertySoftLoadError::None; }
	};
	DURINED_API auto FormatPropertySoftLoadResult(const FPropertySoftLoadResult& Result) -> std::string;
	DURINED_API auto LoadSoftObjectProperty(
		FSoftObjectProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		DObject*& OutObject
	) -> FPropertySoftLoadResult;
	DURINED_API auto GetSoftObjectStateLabel(ESoftObjectViewState State) -> std::string_view;
	DURINED_API auto InspectWeakObject(
		FWeakObjectProperty* Property, void* Container, uint32 ArrayIndex = 0
	) -> FWeakObjectViewState;
	DURINED_API auto GetWeakObjectStateLabel(EWeakObjectViewState State) -> std::string_view;
}
