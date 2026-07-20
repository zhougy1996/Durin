#include "DObject/MathStructs.h"

#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "Math/Color.h"
#include "Math/Transform.h"

namespace Durin
{
	namespace
	{
		template<typename T, auto Member>
		auto MutableMember(void* Container, uint32 ArrayIndex) -> void*
		{
			return &(static_cast<T*>(Container)[ArrayIndex].*Member);
		}

		template<typename T, auto Member>
		auto ConstMember(const void* Container, uint32 ArrayIndex) -> const void*
		{
			return &(static_cast<const T*>(Container)[ArrayIndex].*Member);
		}

		auto MakeStruct(std::string_view QualifiedName, std::string_view ShortName, uint32 Size, uint32 Alignment) -> DStruct*
		{
			auto* Struct = new DStruct(EC_StaticConstructor, FName(QualifiedName), FName(ShortName), Size, Alignment, EObjectFlags::Intrinsic);
			Struct->Register(DStruct::StaticClass, "/Cpp/CoreDObject", std::string(QualifiedName).c_str());
			return Struct;
		}
	}

	auto Z_Construct_DStruct_Durin_FVector2() -> DStruct*
	{
		static DStruct* Singleton = nullptr;
		if (Singleton) return Singleton;
		Singleton = MakeStruct("Durin::FVector2", "FVector2", sizeof(FVector2), alignof(FVector2));
		static const DurinCodeGen::FPropertyParamsBase X = {"x", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector2, &FVector2::x>, &ConstMember<FVector2, &FVector2::x>};
		static const DurinCodeGen::FPropertyParamsBase Y = {"y", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector2, &FVector2::y>, &ConstMember<FVector2, &FVector2::y>};
		static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y};
		static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector2", "FVector2", sizeof(FVector2), alignof(FVector2), Properties, std::size(Properties)};
		return DurinCodeGen::ConstructDStruct(Params);
	}

	auto Z_Construct_DStruct_Durin_FVector3() -> DStruct*
	{
		static DStruct* Singleton = nullptr;
		if (Singleton) return Singleton;
		Singleton = MakeStruct("Durin::FVector3", "FVector3", sizeof(FVector3), alignof(FVector3));
		static const DurinCodeGen::FPropertyParamsBase X = {"x", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector3, &FVector3::x>, &ConstMember<FVector3, &FVector3::x>};
		static const DurinCodeGen::FPropertyParamsBase Y = {"y", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector3, &FVector3::y>, &ConstMember<FVector3, &FVector3::y>};
		static const DurinCodeGen::FPropertyParamsBase Z = {"z", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector3, &FVector3::z>, &ConstMember<FVector3, &FVector3::z>};
		static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y, &Z};
		static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector3", "FVector3", sizeof(FVector3), alignof(FVector3), Properties, std::size(Properties)};
		return DurinCodeGen::ConstructDStruct(Params);
	}

	auto Z_Construct_DStruct_Durin_FVector4() -> DStruct*
	{
		static DStruct* Singleton = nullptr;
		if (Singleton) return Singleton;
		Singleton = MakeStruct("Durin::FVector4", "FVector4", sizeof(FVector4), alignof(FVector4));
		static const DurinCodeGen::FPropertyParamsBase X = {"x", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector4, &FVector4::x>, &ConstMember<FVector4, &FVector4::x>};
		static const DurinCodeGen::FPropertyParamsBase Y = {"y", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector4, &FVector4::y>, &ConstMember<FVector4, &FVector4::y>};
		static const DurinCodeGen::FPropertyParamsBase Z = {"z", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector4, &FVector4::z>, &ConstMember<FVector4, &FVector4::z>};
		static const DurinCodeGen::FPropertyParamsBase W = {"w", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FVector4, &FVector4::w>, &ConstMember<FVector4, &FVector4::w>};
		static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y, &Z, &W};
		static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector4", "FVector4", sizeof(FVector4), alignof(FVector4), Properties, std::size(Properties)};
		return DurinCodeGen::ConstructDStruct(Params);
	}

	auto Z_Construct_DStruct_Durin_FQuat() -> DStruct*
	{
		static DStruct* Singleton = nullptr;
		if (Singleton) return Singleton;
		Singleton = MakeStruct("Durin::FQuat", "FQuat", sizeof(FQuat), alignof(FQuat));
		static const DurinCodeGen::FPropertyParamsBase W = {"w", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FQuat, &FQuat::w>, &ConstMember<FQuat, &FQuat::w>};
		static const DurinCodeGen::FPropertyParamsBase X = {"x", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FQuat, &FQuat::x>, &ConstMember<FQuat, &FQuat::x>};
		static const DurinCodeGen::FPropertyParamsBase Y = {"y", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FQuat, &FQuat::y>, &ConstMember<FQuat, &FQuat::y>};
		static const DurinCodeGen::FPropertyParamsBase Z = {"z", EPropertyFlags::None, 1, 0, sizeof(double), DurinCodeGen::EPropertyGenFlags::Double, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, &MutableMember<FQuat, &FQuat::z>, &ConstMember<FQuat, &FQuat::z>};
		static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&W, &X, &Y, &Z};
		static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FQuat", "FQuat", sizeof(FQuat), alignof(FQuat), Properties, std::size(Properties)};
		return DurinCodeGen::ConstructDStruct(Params);
	}

	auto Z_Construct_DStruct_Durin_FTransform() -> DStruct*
	{
		static DStruct* Singleton = nullptr;
		if (Singleton) return Singleton;
		Singleton = MakeStruct("Durin::FTransform", "FTransform", sizeof(FTransform), alignof(FTransform));
		static const DurinCodeGen::FPropertyParamsBase Rotation = {"Rotation", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FTransform, Rotation)), sizeof(FQuat), DurinCodeGen::EPropertyGenFlags::Struct, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &Z_Construct_DStruct_Durin_FQuat};
		static const DurinCodeGen::FPropertyParamsBase Translation = {"Translation", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FTransform, Translation)), sizeof(FVector3), DurinCodeGen::EPropertyGenFlags::Struct, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &Z_Construct_DStruct_Durin_FVector3};
		static const DurinCodeGen::FPropertyParamsBase Scale = {"Scale3D", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FTransform, Scale3D)), sizeof(FVector3), DurinCodeGen::EPropertyGenFlags::Struct, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr, &Z_Construct_DStruct_Durin_FVector3};
		static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&Rotation, &Translation, &Scale};
		static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FTransform", "FTransform", sizeof(FTransform), alignof(FTransform), Properties, std::size(Properties)};
		return DurinCodeGen::ConstructDStruct(Params);
	}

	auto Z_Construct_DStruct_Durin_FLinearColor() -> DStruct*
	{
		static DStruct* Singleton = nullptr;
		if (Singleton) return Singleton;
		Singleton = MakeStruct("Durin::FLinearColor", "FLinearColor", sizeof(FLinearColor), alignof(FLinearColor));
		static const DurinCodeGen::FPropertyParamsBase R = {"R", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, R)), sizeof(float), DurinCodeGen::EPropertyGenFlags::Float};
		static const DurinCodeGen::FPropertyParamsBase G = {"G", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, G)), sizeof(float), DurinCodeGen::EPropertyGenFlags::Float};
		static const DurinCodeGen::FPropertyParamsBase B = {"B", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, B)), sizeof(float), DurinCodeGen::EPropertyGenFlags::Float};
		static const DurinCodeGen::FPropertyParamsBase A = {"A", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, A)), sizeof(float), DurinCodeGen::EPropertyGenFlags::Float};
		static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&R, &G, &B, &A};
		static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FLinearColor", "FLinearColor", sizeof(FLinearColor), alignof(FLinearColor), Properties, std::size(Properties)};
		return DurinCodeGen::ConstructDStruct(Params);
	}
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector2() -> Durin::DStruct*
{
	return Durin::Z_Construct_DStruct_Durin_FVector2();
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector3() -> Durin::DStruct*
{
	return Durin::Z_Construct_DStruct_Durin_FVector3();
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FVector4() -> Durin::DStruct*
{
	return Durin::Z_Construct_DStruct_Durin_FVector4();
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FQuat() -> Durin::DStruct*
{
	return Durin::Z_Construct_DStruct_Durin_FQuat();
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FTransform() -> Durin::DStruct*
{
	return Durin::Z_Construct_DStruct_Durin_FTransform();
}

COREDOBJECT_API auto Z_Construct_DStruct_Durin_FLinearColor() -> Durin::DStruct*
{
	return Durin::Z_Construct_DStruct_Durin_FLinearColor();
}
