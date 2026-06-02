#pragma once

#include "CoreStd.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	template<std::integral IndexType>
	inline constexpr IndexType TIndexNone = static_cast<IndexType>(-1);

	inline constexpr int32 INDEX_NONE = TIndexNone<int32>;
	inline constexpr uint32 INDEX_NONE_U32 = std::numeric_limits<uint32>::max();

	enum EForceInit
	{
		ForceInit,
		ForceInitToZero
	};

	enum ENoInit {NoInit};
	enum EInPlace {InPlace};
}


// Helper macro to make a class non-copyable and non-movable
// Should be placed in the public section of the class for better error messages
#define DURIN_NONCOPYABLE(TypeName) \
	TypeName(TypeName&&) = delete; \
	TypeName(const TypeName&) = delete; \
	TypeName& operator=(const TypeName&) = delete; \
	TypeName& operator=(TypeName&&) = delete;

#define STRUCT_OFFSET(struc, member)	((::size_t)&reinterpret_cast<char const volatile&>((((struc*)0)->member)))
#define STRUCT_OFFSET_UINT16(struc, member)	static_cast<uint16>(STRUCT_OFFSET(struc, member))

