#pragma once

namespace Doge
{
	enum { INDEX_NONE = -1 };

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
#define DOGE_NONCOPYABLE(TypeName) \
	TypeName(TypeName&&) = delete; \
	TypeName(const TypeName&) = delete; \
	TypeName& operator=(const TypeName&) = delete; \
	TypeName& operator=(TypeName&&) = delete;

#define STRUCT_OFFSET(struc, member)	((::size_t)&reinterpret_cast<char const volatile&>((((struc*)0)->member)))
#define STRUCT_OFFSET_UINT16(struc, member)	static_cast<uint16>(STRUCT_OFFSET(struc, member))

