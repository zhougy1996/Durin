#pragma once

class DObject;
class DClass;

struct FStaticConstructObjectParameters
{
	DClass* Class = nullptr;

	DObject* Outer = nullptr;

	FName Name;

	size_t Size = 0;
};

class FObjectInitializer
{
public:
	FORCEINLINE auto GetObj() const -> DObject* { return Obj; }

	static CORE_API auto Get() -> const FObjectInitializer&;

	DObject* Obj;
};

CORE_API auto DObjectForceRegistration(DObject* Object) -> void;

CORE_API auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*;

template<typename T>
auto NewObject(DObject* Outer, FName Name) -> T*
{
	static_assert(std::is_base_of<DObject, T>::value, "T must be derived from DObject");

	FStaticConstructObjectParameters Params;
	Params.Class = T::StaticClass();
	Params.Outer = Outer;
	Params.Name = Name;
	Params.Size = sizeof(T);

	DObject* Obj = StaticConstructObject(Params);

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
