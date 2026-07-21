#pragma once

#include "DObject/Archive.h"
#include "DObject/PropertyChange.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class DObject;
	class FProperty;

	struct FReflectedPropertyEditPathSegment
	{
		const FProperty* Property = nullptr;
		EPropertyPathSelector Selector = EPropertyPathSelector::None;
		uint64 Index = 0;
		std::vector<uint8> MapKeyData;
	};

	struct FReflectedPropertyEditTarget
	{
		DObject* Object = nullptr;
		const FProperty* MemberProperty = nullptr;
		const FProperty* LeafProperty = nullptr;
		void* LeafContainer = nullptr;
		uint32 LeafArrayIndex = 0;
		std::vector<FReflectedPropertyEditPathSegment> Path;
		EPropertyChangeKind Kind = EPropertyChangeKind::ValueSet;

		DURINED_API static auto ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex = 0) -> FReflectedPropertyEditTarget;
	};

	class DURINED_API IReflectedPropertyMutationAdapter
	{
	public:
		virtual ~IReflectedPropertyMutationAdapter() = default;
		virtual auto Capture(const FReflectedPropertyEditTarget& Target, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool = 0;
		virtual auto Apply(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& ProposedValue, std::string* OutError) const -> bool = 0;
		virtual auto Restore(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool = 0;
	};

	DURINED_API auto GetGenericReflectedPropertyMutationAdapter() -> const IReflectedPropertyMutationAdapter&;

	enum class EReflectedPropertyEditResult : uint8
	{
		Failed,
		NoChange,
		Changed,
	};

	class FReflectedPropertyEditSession
	{
	public:
		DURINED_API ~FReflectedPropertyEditSession();
		FReflectedPropertyEditSession() = default;
		FReflectedPropertyEditSession(const FReflectedPropertyEditSession&) = delete;
		auto operator=(const FReflectedPropertyEditSession&) -> FReflectedPropertyEditSession& = delete;

		DURINED_API auto Begin(
			const FReflectedPropertyEditTarget& InTarget,
			std::string_view InDescription,
			// Custom adapters are registry-owned in production and must outlive the session.
			const IReflectedPropertyMutationAdapter* InAdapter = nullptr,
			std::string* OutError = nullptr
		) -> bool;
		DURINED_API auto Apply(const FPropertyValueSnapshot& ProposedValue, std::string* OutError = nullptr) -> EReflectedPropertyEditResult;
		DURINED_API auto Commit(std::string* OutError = nullptr) -> EReflectedPropertyEditResult;
		DURINED_API auto Cancel(std::string* OutError = nullptr) -> EReflectedPropertyEditResult;

		auto IsActive() const -> bool { return bActive; }
		auto HasChanges() const -> bool { return bActive && !(OriginalValue == CurrentValue); }
		auto GetDescription() const -> std::string_view { return Description; }
		auto GetOriginalValue() const -> const FPropertyValueSnapshot& { return OriginalValue; }
		auto GetCurrentValue() const -> const FPropertyValueSnapshot& { return CurrentValue; }

	private:
		auto Notify(EPropertyChangePhase Phase) const -> void;
		auto Reset() -> void;

		FReflectedPropertyEditTarget Target;
		const IReflectedPropertyMutationAdapter* Adapter = nullptr;
		FPropertyValueSnapshot OriginalValue;
		FPropertyValueSnapshot CurrentValue;
		std::string Description;
		bool bActive = false;
		bool bObjectRooted = false;
	};
}
