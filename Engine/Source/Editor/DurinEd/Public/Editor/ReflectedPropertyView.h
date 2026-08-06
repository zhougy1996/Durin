#pragma once

#include "DObject/AssetPath.h"
#include "Editor/ReflectedPropertyEditing.h"

namespace Durin
{
	class DObject;
	class FArrayProperty;
	class FMapProperty;
	class FProperty;
	class FSoftObjectProperty;

	enum class ESoftObjectPropertyViewState : uint8
	{
		Null,
		Unloaded,
		Loaded,
		Redirected,
		Missing,
		TypeMismatch,
	};

	// Describes a soft reference without loading its target.
	struct FSoftObjectPropertyViewState
	{
		ESoftObjectPropertyViewState State = ESoftObjectPropertyViewState::Null;
		FAssetPath Path;
		FAssetPath ResolvedPath;
		DObject* LoadedObject = nullptr;
		std::string Message;
	};

	// Supplies transaction, error, and read-only policy to a property view.
	struct FReflectedPropertyViewContext
	{
		FEditorTransactionManager* Transactions = nullptr;
		std::function<void(std::string)> ReportError;
		std::function<bool(const FAssetPath&, std::string&)> RevealAsset;
		std::function<bool(const FAssetPath&, std::string&)> OpenAsset;
		bool bReadOnly = false;
	};

	// Overrides presentation for one reflected property row.
	struct FPropertyViewOptions
	{
		std::string Label;
	};

	// Configures filtering and table ownership for an object property view.
	struct FObjectPropertyViewOptions
	{
		std::string_view SearchText;
		std::function<bool(const FProperty&, uint32)> Filter;
		const char* PropertyTableId = "ReflectedPropertyTable";
		bool bCreatePropertyTable = true;
		bool bShowEmptyMessage = true;
	};

	// Summarizes visible rows and committed changes from one object draw.
	struct FObjectPropertyViewResult
	{
		uint32 VisiblePropertyCount = 0;
		bool bChanged = false;
	};

	// An embeddable immediate-mode property view. It owns only transient widget/edit
	// state; committed history remains owned by the transaction manager supplied by its host.
	class FReflectedPropertyView
	{
	public:
		DURINED_API auto EditObject(
			const FReflectedPropertyViewContext& Context,
			DObject* Object,
			const FObjectPropertyViewOptions& Options = {}
		) -> FObjectPropertyViewResult;

		DURINED_API auto EditProperty(
			const FReflectedPropertyViewContext& Context,
			DObject* Object,
			FProperty* Property,
			uint32 ArrayIndex = 0,
			const FPropertyViewOptions& Options = {}
		) -> bool;

		DURINED_API auto SubmitPropertyEdit(
			const FReflectedPropertyViewContext& Context,
			const FReflectedPropertyEditTarget& Target,
			const FPropertyValueSnapshot& ProposedValue,
			bool bContinuous
		) -> bool;
		DURINED_API auto SubmitPropertyValueEdit(
			const FReflectedPropertyViewContext& Context,
			const FReflectedPropertyEditTarget& Target,
			const std::function<void(FProperty*, void*, uint32)>& AssignValue,
			bool bContinuous
		) -> bool;

		// Returns false only when the requested terminal action failed and the
		// recoverable edit must remain active.
		DURINED_API auto FinishActiveEdit(const FReflectedPropertyViewContext* Context, bool bCancel) -> bool;
		DURINED_API auto HandleOwnerContext(const FReflectedPropertyViewContext& Context, DObject* Object) -> bool;
		auto IsEditing() const -> bool { return EditSession.IsActive(); }
		auto IsEditingObject(const DObject* Object) const -> bool { return EditSession.IsActive() && ActiveEditObject == Object; }
		auto IsEditingTarget(const FReflectedPropertyEditTarget& Target) const -> bool { return EditSession.MatchesTarget(Target); }

	private:
		// Captures widget output without exposing a live reflected target to UI code.
		struct FPropertyWidgetEditResult
		{
			// The destination is always temporary or detached storage selected by the caller.
			// Widget code never receives an object or reflected edit target.
			std::function<void(FProperty*, void*, uint32)> AssignValue;
			bool bChanged = false;
			bool bContinuous = false;
			bool bActive = false;
			bool bDeactivatedAfterEdit = false;
		};

		// Retains detached key/value drafts while a map insertion UI is active.
		struct FMapInsertDraft
		{
			FReflectedPropertyEditTarget Target;
			FPropertyValueSnapshot Key;
			FPropertyValueSnapshot Value;
			bool bActive = false;
		};

		auto EditPropertyValue(
			const FReflectedPropertyViewContext& Context,
			DObject* Object,
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			const std::string& Label,
			bool bReadOnly,
			const FReflectedPropertyEditTarget& EditTarget
		) -> bool;
		auto EditPropertyWidget(
			const FReflectedPropertyViewContext& Context,
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			const std::string& Label,
			bool bReadOnly
		) -> FPropertyWidgetEditResult;
		auto EditStructPropertyWidget(FProperty* Property, void* Container, uint32 ArrayIndex,
			const std::string& Label, bool bReadOnly) -> FPropertyWidgetEditResult;
		auto SubmitWidgetEdit(const FReflectedPropertyViewContext& Context,
			const FReflectedPropertyEditTarget& EditTarget, const FPropertyWidgetEditResult& Edit) -> bool;
		auto EditArrayProperty(const FReflectedPropertyViewContext& Context, DObject* Object, FArrayProperty* Property,
			void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
			const FReflectedPropertyEditTarget& EditTarget) -> bool;
		auto EditMapProperty(const FReflectedPropertyViewContext& Context, DObject* Object, FMapProperty* Property,
			void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
			const FReflectedPropertyEditTarget& EditTarget) -> bool;
		auto ReportError(const FReflectedPropertyViewContext& Context, std::string Error) const -> void;

		std::array<char, 256> AssetSearchText{};
		FMapInsertDraft MapInsertDraft;
		FReflectedPropertyEditSession EditSession;
		DObject* OwnerContextObject = nullptr;
		DObject* ActiveEditOwnerObject = nullptr;
		DObject* ActiveEditObject = nullptr;
	};

	DURINED_API auto MakeReflectedPropertyDisplayName(
		std::string_view PropertyName,
		DurinCodeGen::EPropertyGenFlags Kind,
		std::string_view ExplicitDisplayName = {}
	) -> std::string;
	DURINED_API auto MakeReflectedPropertyLabel(const FProperty& Property, uint32 ArrayIndex = 0) -> std::string;
	DURINED_API auto InspectSoftObjectProperty(
		FSoftObjectProperty* Property, void* Container, uint32 ArrayIndex = 0
	) -> FSoftObjectPropertyViewState;
	DURINED_API auto LoadSoftObjectProperty(
		FSoftObjectProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		DObject*& OutObject,
		std::string* OutError = nullptr
	) -> bool;
	DURINED_API auto GetSoftObjectPropertyStateLabel(ESoftObjectPropertyViewState State) -> std::string_view;
}
