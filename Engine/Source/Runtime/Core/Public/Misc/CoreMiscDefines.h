#pragma once

enum { INDEX_NONE = -1 };

enum EForceInit 
{
	ForceInit,
	ForceInitToZero
};

enum ENoInit {NoInit};
enum EInPlace {InPlace};


// Helper macro to make a class non-copyable and non-movable
// Should be placed in the public section of the class for better error messages
#define DOGE_NONCOPYABLE(TypeName) \
	TypeName(TypeName&&) = delete; \
	TypeName(const TypeName&) = delete; \
	TypeName& operator=(const TypeName&) = delete; \
	TypeName& operator=(TypeName&&) = delete;

