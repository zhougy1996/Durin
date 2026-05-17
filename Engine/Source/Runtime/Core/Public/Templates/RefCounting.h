#pragma once
#include "HAL/Platform.h"

namespace Durin
{
	template<typename ReferencedType>
	class TRefCountPtr
	{
	public:
		FORCEINLINE TRefCountPtr() = default;

		TRefCountPtr(ReferencedType* InReference, bool bAddRef = true)
			: Reference(InReference)
		{
			if (Reference && bAddRef)
			{
				Reference->AddRef();
			}
		}

		TRefCountPtr(const TRefCountPtr& Copy)
		{
			Reference = Copy.Reference;
			if (Reference)
			{
				Reference->AddRef();
			}
		}

		template<typename CopyReferencedType>
		TRefCountPtr(const TRefCountPtr<CopyReferencedType>& Copy)
		{
			Reference = static_cast<ReferencedType*>(Copy.GetReference());
			if (Reference)
			{
				Reference->AddRef();
			}
		}

		TRefCountPtr(TRefCountPtr&& Move) noexcept
		{
			Reference = Move.Reference;
			Move.Reference = nullptr;
		}

		template<typename MoveReferencedType>
		explicit TRefCountPtr(TRefCountPtr<MoveReferencedType>&& Move) noexcept
		{
			Reference = static_cast<ReferencedType*>(Move.GetReference());
			Move.Reference = nullptr;
		}

		~TRefCountPtr()
		{
			if (Reference)
			{
				Reference->Release();
			}
		}

		auto operator=(ReferencedType* InReference) -> TRefCountPtr&
		{
			if (Reference != InReference)
			{
				// Call AddRef() before Release(), in case InReference is the same object as Reference but with a different pointer value (e.g. due to multiple inheritance).
				// It also handles the case when the release of old reference causes the release of the new reference.
				ReferencedType* OldReference = Reference;
				Reference = InReference;
				if (Reference)
				{
					Reference->AddRef();
				}
				if (OldReference)
				{
					OldReference->Release();
				}
			}
			return *this;
		}

		FORCEINLINE auto operator=(const TRefCountPtr& Copy) -> TRefCountPtr&
		{
			if (this != &Copy)
			{
				*this = Copy.Reference;
			}
			return *this;
		}

		template<typename CopyReferencedType>
		FORCEINLINE auto operator=(const TRefCountPtr<CopyReferencedType>& Copy) -> TRefCountPtr&
		{
			*this = Copy.GetReference();
			return *this;
		}

		auto operator=(TRefCountPtr&& InMovePtr) noexcept -> TRefCountPtr&
		{
			if (this != &InMovePtr)
			{
				ReferencedType* OldReference = Reference;
				Reference = InMovePtr.Reference;
				InMovePtr.Reference = nullptr;
				if (OldReference)
				{
					OldReference->Release();
				}
			}
			return *this;
		}

		template<typename MoveReferencedType>
		auto operator=(TRefCountPtr<MoveReferencedType>&& InMovePtr) noexcept -> TRefCountPtr&
		{
			// InMovePtr is a different object, so we don't need to check if this != &InMovePtr. We can directly move the reference and release the old one.
			ReferencedType* OldReference = Reference;
			Reference = InMovePtr.Reference;
			InMovePtr.Reference = nullptr;
			if (OldReference)
			{
				OldReference->Release();
			}
			return *this;
		}

		FORCEINLINE auto operator->() const -> ReferencedType* { return Reference; }

		FORCEINLINE auto GetReference() const -> ReferencedType* { return Reference; }

		FORCEINLINE auto GetRefCount() const -> int32 { return Reference->GetRefCount(); }

		FORCEINLINE void Swap(TRefCountPtr& InPtr)
		{
			ReferencedType* OldReference = Reference;
			Reference = InPtr.Reference;
			InPtr.Reference = OldReference;
		}

	private:
		ReferencedType* Reference = nullptr;

		template<typename OtherType>
		friend class TRefCountPtr;

	public:
		FORCEINLINE operator bool() const { return Reference != nullptr; }

		FORCEINLINE operator ReferencedType*() const { return Reference; }

		FORCEINLINE auto operator==(const TRefCountPtr& Other) const -> bool { return Reference == Other.Reference; }

		FORCEINLINE auto operator==(ReferencedType* Other) const -> bool { return Reference == Other; }
	};

	template<typename T, typename... Args>
	auto MakeRefCount(Args&&... InArgs) -> TRefCountPtr<T>
	{
		return TRefCountPtr<T>(new T(std::forward<Args>(InArgs)...));
	}

} // namespace Durin