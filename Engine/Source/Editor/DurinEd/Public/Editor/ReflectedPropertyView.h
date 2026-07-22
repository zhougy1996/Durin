#pragma once

#include "Editor/ReflectedPropertyEditing.h"

namespace Durin
{
	class DObject;
	class FArrayProperty;
	class FMapProperty;
	class FProperty;

	// A logical reflected value whose address is resolved on demand. Bindings retain
	// stable selectors and snapshot roots, so container reallocation does not escape
	// into editor panels.
	class FReflectedPropertyBinding
	{
	public:
		auto IsValid() const -> bool { return Object != nullptr && MemberProperty != nullptr && LeafProperty != nullptr; }
		DURINED_API auto IsPresent() const -> bool;

	private:
		DObject* Object = nullptr;
		FMapProperty* MemberProperty = nullptr;
		FProperty* LeafProperty = nullptr;
		FPropertyValueSnapshot MapKey;
		std::vector<uint8> PathKeyData;

		friend class FReflectedPropertyView;
	};

	struct FReflectedPropertyViewContext
	{
		FEditorTransactionManager* Transactions = nullptr;
		std::function<void(std::string)> ReportError;
		bool bReadOnly = false;
	};

	struct FPropertyViewOptions
	{
		std::string Label;
	};

	struct FObjectPropertyViewOptions
	{
		std::string_view SearchText;
		std::function<bool(const FProperty&, uint32)> Filter;
		const char* PropertyTableId = "ReflectedPropertyTable";
		bool bCreatePropertyTable = true;
		bool bShowEmptyMessage = true;
	};

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
			const std::function<void()>& AssignValue,
			bool bContinuous
		) -> bool;
		DURINED_API auto BindStringMapValue(
			DObject* Object,
			FMapProperty* Property,
			std::string_view Key
		) const -> FReflectedPropertyBinding;
		DURINED_API auto SubmitBoundPropertyValueEdit(
			const FReflectedPropertyViewContext& Context,
			const FReflectedPropertyBinding& Binding,
			const std::function<void(FProperty*, void*)>& AssignValue,
			bool bContinuous
		) -> bool;
		DURINED_API auto SetBoundPropertyEnabled(
			const FReflectedPropertyViewContext& Context,
			const FReflectedPropertyBinding& Binding,
			bool bEnabled,
			const std::function<void(FProperty*, void*)>& InitializeValue
		) -> bool;

		DURINED_API auto FinishActiveEdit(const FReflectedPropertyViewContext* Context, bool bCancel) -> void;
		DURINED_API auto HandleOwnerContext(const FReflectedPropertyViewContext& Context, DObject* Object) -> void;
		auto IsEditing() const -> bool { return EditSession.IsActive(); }
		auto IsEditingObject(const DObject* Object) const -> bool { return EditSession.IsActive() && ActiveEditObject == Object; }
		auto IsEditingTarget(const FReflectedPropertyEditTarget& Target) const -> bool { return EditSession.MatchesTarget(Target); }

	private:
		auto EditPropertyValue(
			const FReflectedPropertyViewContext& Context,
			DObject* Object,
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			const std::string& Label,
			bool bReadOnly,
			const FReflectedPropertyEditTarget& EditTarget,
			bool bUseTransaction = true
		) -> bool;
		auto EditArrayProperty(const FReflectedPropertyViewContext& Context, DObject* Object, FArrayProperty* Property,
			void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
			const FReflectedPropertyEditTarget& EditTarget) -> bool;
		auto EditMapProperty(const FReflectedPropertyViewContext& Context, DObject* Object, FMapProperty* Property,
			void* Container, uint32 ArrayIndex, const std::string& Label, bool bReadOnly,
			const FReflectedPropertyEditTarget& EditTarget) -> bool;
		auto ReportError(const FReflectedPropertyViewContext& Context, std::string Error) const -> void;

		std::array<char, 256> AssetSearchText{};
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
}
