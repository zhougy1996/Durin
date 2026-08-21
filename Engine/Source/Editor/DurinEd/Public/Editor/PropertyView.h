#pragma once

#include "DObject/AssetPath.h"
#include "Editor/PropertyEditing.h"

namespace Durin
{
	class DObject;
	class FArrayProperty;
	class FMapProperty;
	class FProperty;
	class FSoftObjectProperty;
	class FWeakObjectProperty;
}

namespace Durin::Editor
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

	// Describes a soft reference without loading its target.
	struct FSoftObjectViewState
	{
		ESoftObjectViewState State = ESoftObjectViewState::Null;
		FAssetPath Path;
		FAssetPath ResolvedPath;
		DObject* LoadedObject = nullptr;
		std::string Message;
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
		FTransactionManager* Transactions = nullptr;
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
	class FPropertyView
	{
	public:
		DURINED_API auto EditObject(
			const FPropertyViewContext& Context,
			DObject* Object,
			const FObjectPropertyViewOptions& Options = {}
		) -> FObjectPropertyViewResult;

		DURINED_API auto EditProperty(
			const FPropertyViewContext& Context,
			DObject* Object,
			FProperty* Property,
			uint32 ArrayIndex = 0,
			const FPropertyViewOptions& Options = {}
		) -> bool;

		DURINED_API auto SubmitPropertyEdit(
			const FPropertyViewContext& Context,
			const FPropertyEditTarget& Target,
			const FPropertyValueSnapshot& ProposedValue,
			bool bContinuous
		) -> bool;
		DURINED_API auto SubmitPropertyValueEdit(
			const FPropertyViewContext& Context,
			const FPropertyEditTarget& Target,
			const std::function<void(FProperty*, void*, uint32)>& AssignValue,
			bool bContinuous
		) -> bool;

		// Returns false only when the requested terminal action failed and the
		// recoverable edit must remain active.
		DURINED_API auto FinishActiveEdit(const FPropertyViewContext* Context, bool bCancel) -> bool;
		DURINED_API auto HandleOwnerContext(const FPropertyViewContext& Context, DObject* Object) -> bool;
		auto IsEditing() const -> bool { return EditSession.IsActive(); }
		auto IsEditingObject(const DObject* Object) const -> bool { return EditSession.IsActive() && ActiveEditObject == Object; }
		auto IsEditingTarget(const FPropertyEditTarget& Target) const -> bool { return EditSession.MatchesTarget(Target); }

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
			const FPropertyEditTarget& EditTarget
		) -> bool;
		auto EditPropertyWidget(
			const FPropertyViewContext& Context,
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			const std::string& Label,
			bool bReadOnly
		) -> FPropertyWidgetEditResult;
		auto EditStructPropertyWidget(FProperty* Property, void* Container, uint32 ArrayIndex,
			const std::string& Label, bool bReadOnly) -> FPropertyWidgetEditResult;
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
		auto ReportError(const FPropertyViewContext& Context, std::string Error) const -> void;

		std::array<char, 256> AssetSearchText{};
		FMapInsertDraft MapInsertDraft;
		FPropertyEditSession EditSession;
		DObject* OwnerContextObject = nullptr;
		DObject* ActiveEditOwnerObject = nullptr;
		DObject* ActiveEditObject = nullptr;
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
	DURINED_API auto LoadSoftObject(
		FSoftObjectProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		DObject*& OutObject,
		std::string* OutError = nullptr
	) -> bool;
	DURINED_API auto GetSoftObjectStateLabel(ESoftObjectViewState State) -> std::string_view;
	DURINED_API auto InspectWeakObject(
		FWeakObjectProperty* Property, void* Container, uint32 ArrayIndex = 0
	) -> FWeakObjectViewState;
	DURINED_API auto GetWeakObjectStateLabel(EWeakObjectViewState State) -> std::string_view;
}
