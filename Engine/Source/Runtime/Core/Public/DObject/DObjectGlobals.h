#pragma once

struct FStaticConstructObjectParameters
{
	DClass* Class = nullptr;
	DObject* Outer = nullptr;
	FName Name;
};

class FObjectInitializer
{
public:
	FORCEINLINE auto GetObj() const -> DObject* { return Obj; }

	static CORE_API auto Get() -> const FObjectInitializer&;

	DObject* Obj;

	FStaticConstructObjectParameters Params;
};

CORE_API auto DObjectForceRegistration(DObject* Object) -> void;

template <typename T>
auto NewObject(DObject* Outer, FName Name) -> T*
{
	static_assert(std::is_base_of<DObject, T>::value, "T must be derived from DObject");

	// Allocate memory and zero it out
	DObject* Obj = nullptr;
	Obj = (DObject*)std::malloc(sizeof(T));
	assert(Obj && "Memory allocation failed");
	std::memset(Obj, 0, sizeof(T));

	new (Obj) DObject(T::StaticClass(), Outer, Name);

	FObjectInitializer ObjectInitializer;
	ObjectInitializer.Obj = Obj;
	ObjectInitializer.Params = FStaticConstructObjectParameters{T::StaticClass(), Outer, Name};

	DObjectForceRegistration(Obj);
	return static_cast<T*>(Obj);
}

namespace DogeCodeGen
{

struct FClassParams
{
	DClass* (*ClassNoRegisterFunc)();
	const UTF8Char* ClassName;
};

CORE_API auto ConstructDClass(const FClassParams& Params) -> DClass*;

} // namespace DogeCodeGen
