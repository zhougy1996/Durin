#pragma once

#include <memory>

template<typename T>
using TWeakPtr = std::weak_ptr<T>;

template<typename T, typename D = std::default_delete<T>>
using TUniquePtr = std::unique_ptr<T, D>;

template<typename T>
using TSharedPtr = std::shared_ptr<T>;

template<typename T>
class CORE_API TSharedFromThis : public std::enable_shared_from_this<T>
{
public:
	FORCEINLINE auto AsShared() -> std::shared_ptr<T>
	{
		return this->shared_from_this();
	}

	template<typename OtherType>
	FORCEINLINE auto SharedThis(OtherType* ThisPtr) -> std::shared_ptr<OtherType>
	{
		return std::static_pointer_cast<OtherType>(ThisPtr->AsShared());
	}
};
