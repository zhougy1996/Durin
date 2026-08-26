#include "DObject/MathStructs.h"

#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "Math/Color.h"
#include "Math/Transform.h"

namespace Durin
{
	template<>
	struct TDStructOpsTraits<FVector3f> : TDStructOpsTraitsBase<FVector3f>
	{
		static auto DefaultConstruct(void* Destination) -> void
		{
			std::construct_at(static_cast<FVector3f*>(Destination), 0.0f);
		}
	};

	template<>
	struct TDStructOpsTraits<FVector3> : TDStructOpsTraitsBase<FVector3>
	{
		static auto DefaultConstruct(void* Destination) -> void
		{
			std::construct_at(static_cast<FVector3*>(Destination), 0.0);
		}
	};

	template<>
	struct TDStructOpsTraits<FQuatf> : TDStructOpsTraitsBase<FQuatf>
	{
		static auto DefaultConstruct(void* Destination) -> void
		{
			std::construct_at(static_cast<FQuatf*>(Destination), 1.0f, 0.0f, 0.0f, 0.0f);
		}
	};

	template<>
	struct TDStructOpsTraits<FMatrix4f> : TDStructOpsTraitsBase<FMatrix4f>
	{
		static auto DefaultConstruct(void* Destination) -> void
		{
			std::construct_at(static_cast<FMatrix4f*>(Destination), 1.0f);
		}
	};

	template<>
	struct TDStructOpsTraits<FLinearColor> : TDStructOpsTraitsBase<FLinearColor>
	{
		static auto DefaultConstruct(void* Destination) -> void
		{
			std::construct_at(static_cast<FLinearColor*>(Destination), ForceInitToZero);
		}
	};
} // namespace Durin

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

	template<typename T, size_t Index>
	auto MutableIndex(void* Container, uint32 ArrayIndex) -> void*
	{
		return &static_cast<T*>(Container)[ArrayIndex][Index];
	}

	template<typename T, size_t Index>
	auto ConstIndex(const void* Container, uint32 ArrayIndex) -> const void*
	{
		return &static_cast<const T*>(Container)[ArrayIndex][Index];
	}

	auto MakeStruct(std::string_view QualifiedName, std::string_view ShortName, uint32 Size, uint32 Alignment) -> Durin::DStruct*
	{
		auto* Struct = new Durin::DStruct(Durin::EC_StaticConstructor, Durin::FName(QualifiedName), Durin::FName(ShortName), Size, Alignment, Durin::EObjectFlags::Intrinsic);
		Struct->Register(Durin::DStruct::StaticClass, "/Cpp/CoreDObject", std::string(QualifiedName).c_str());
		return Struct;
	}
} // namespace

namespace Durin
{

auto Z_Construct_DStruct_FVector2f() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FVector2f", "FVector2f", sizeof(FVector2f), alignof(FVector2f));
	static const DurinCodeGen::FFloatPropertyParams X = DurinCodeGen::FFloatPropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FVector2f, &FVector2f::x>, &ConstMember<FVector2f, &FVector2f::x>);
	static const DurinCodeGen::FFloatPropertyParams Y = DurinCodeGen::FFloatPropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FVector2f, &FVector2f::y>, &ConstMember<FVector2f, &FVector2f::y>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector2f", "FVector2f", sizeof(FVector2f), alignof(FVector2f), Properties, std::size(Properties), &GetDStructOps<FVector2f>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FVector3f() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FVector3f", "FVector3f", sizeof(FVector3f), alignof(FVector3f));
	static const DurinCodeGen::FFloatPropertyParams X = DurinCodeGen::FFloatPropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FVector3f, &FVector3f::x>, &ConstMember<FVector3f, &FVector3f::x>);
	static const DurinCodeGen::FFloatPropertyParams Y = DurinCodeGen::FFloatPropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FVector3f, &FVector3f::y>, &ConstMember<FVector3f, &FVector3f::y>);
	static const DurinCodeGen::FFloatPropertyParams Z = DurinCodeGen::FFloatPropertyParams::WithAccessors("z", EPropertyFlags::None, 1, &MutableMember<FVector3f, &FVector3f::z>, &ConstMember<FVector3f, &FVector3f::z>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y, &Z};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector3f", "FVector3f", sizeof(FVector3f), alignof(FVector3f), Properties, std::size(Properties), &GetDStructOps<FVector3f>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FVector4f() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FVector4f", "FVector4f", sizeof(FVector4f), alignof(FVector4f));
	static const DurinCodeGen::FFloatPropertyParams X = DurinCodeGen::FFloatPropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FVector4f, &FVector4f::x>, &ConstMember<FVector4f, &FVector4f::x>);
	static const DurinCodeGen::FFloatPropertyParams Y = DurinCodeGen::FFloatPropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FVector4f, &FVector4f::y>, &ConstMember<FVector4f, &FVector4f::y>);
	static const DurinCodeGen::FFloatPropertyParams Z = DurinCodeGen::FFloatPropertyParams::WithAccessors("z", EPropertyFlags::None, 1, &MutableMember<FVector4f, &FVector4f::z>, &ConstMember<FVector4f, &FVector4f::z>);
	static const DurinCodeGen::FFloatPropertyParams W = DurinCodeGen::FFloatPropertyParams::WithAccessors("w", EPropertyFlags::None, 1, &MutableMember<FVector4f, &FVector4f::w>, &ConstMember<FVector4f, &FVector4f::w>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y, &Z, &W};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector4f", "FVector4f", sizeof(FVector4f), alignof(FVector4f), Properties, std::size(Properties), &GetDStructOps<FVector4f>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FVector2() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FVector2", "FVector2", sizeof(FVector2), alignof(FVector2));
	static const DurinCodeGen::FDoublePropertyParams X = DurinCodeGen::FDoublePropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FVector2, &FVector2::x>, &ConstMember<FVector2, &FVector2::x>);
	static const DurinCodeGen::FDoublePropertyParams Y = DurinCodeGen::FDoublePropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FVector2, &FVector2::y>, &ConstMember<FVector2, &FVector2::y>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector2", "FVector2", sizeof(FVector2), alignof(FVector2), Properties, std::size(Properties), &GetDStructOps<FVector2>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FVector3() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FVector3", "FVector3", sizeof(FVector3), alignof(FVector3));
	static const DurinCodeGen::FDoublePropertyParams X = DurinCodeGen::FDoublePropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FVector3, &FVector3::x>, &ConstMember<FVector3, &FVector3::x>);
	static const DurinCodeGen::FDoublePropertyParams Y = DurinCodeGen::FDoublePropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FVector3, &FVector3::y>, &ConstMember<FVector3, &FVector3::y>);
	static const DurinCodeGen::FDoublePropertyParams Z = DurinCodeGen::FDoublePropertyParams::WithAccessors("z", EPropertyFlags::None, 1, &MutableMember<FVector3, &FVector3::z>, &ConstMember<FVector3, &FVector3::z>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y, &Z};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector3", "FVector3", sizeof(FVector3), alignof(FVector3), Properties, std::size(Properties), &GetDStructOps<FVector3>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FVector4() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FVector4", "FVector4", sizeof(FVector4), alignof(FVector4));
	static const DurinCodeGen::FDoublePropertyParams X = DurinCodeGen::FDoublePropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FVector4, &FVector4::x>, &ConstMember<FVector4, &FVector4::x>);
	static const DurinCodeGen::FDoublePropertyParams Y = DurinCodeGen::FDoublePropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FVector4, &FVector4::y>, &ConstMember<FVector4, &FVector4::y>);
	static const DurinCodeGen::FDoublePropertyParams Z = DurinCodeGen::FDoublePropertyParams::WithAccessors("z", EPropertyFlags::None, 1, &MutableMember<FVector4, &FVector4::z>, &ConstMember<FVector4, &FVector4::z>);
	static const DurinCodeGen::FDoublePropertyParams W = DurinCodeGen::FDoublePropertyParams::WithAccessors("w", EPropertyFlags::None, 1, &MutableMember<FVector4, &FVector4::w>, &ConstMember<FVector4, &FVector4::w>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&X, &Y, &Z, &W};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FVector4", "FVector4", sizeof(FVector4), alignof(FVector4), Properties, std::size(Properties), &GetDStructOps<FVector4>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FQuat() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FQuat", "FQuat", sizeof(FQuat), alignof(FQuat));
	static const DurinCodeGen::FDoublePropertyParams W = DurinCodeGen::FDoublePropertyParams::WithAccessors("w", EPropertyFlags::None, 1, &MutableMember<FQuat, &FQuat::w>, &ConstMember<FQuat, &FQuat::w>);
	static const DurinCodeGen::FDoublePropertyParams X = DurinCodeGen::FDoublePropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FQuat, &FQuat::x>, &ConstMember<FQuat, &FQuat::x>);
	static const DurinCodeGen::FDoublePropertyParams Y = DurinCodeGen::FDoublePropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FQuat, &FQuat::y>, &ConstMember<FQuat, &FQuat::y>);
	static const DurinCodeGen::FDoublePropertyParams Z = DurinCodeGen::FDoublePropertyParams::WithAccessors("z", EPropertyFlags::None, 1, &MutableMember<FQuat, &FQuat::z>, &ConstMember<FQuat, &FQuat::z>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&W, &X, &Y, &Z};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FQuat", "FQuat", sizeof(FQuat), alignof(FQuat), Properties, std::size(Properties), &GetDStructOps<FQuat>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FQuatf() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FQuatf", "FQuatf", sizeof(FQuatf), alignof(FQuatf));
	static const DurinCodeGen::FFloatPropertyParams W = DurinCodeGen::FFloatPropertyParams::WithAccessors("w", EPropertyFlags::None, 1, &MutableMember<FQuatf, &FQuatf::w>, &ConstMember<FQuatf, &FQuatf::w>);
	static const DurinCodeGen::FFloatPropertyParams X = DurinCodeGen::FFloatPropertyParams::WithAccessors("x", EPropertyFlags::None, 1, &MutableMember<FQuatf, &FQuatf::x>, &ConstMember<FQuatf, &FQuatf::x>);
	static const DurinCodeGen::FFloatPropertyParams Y = DurinCodeGen::FFloatPropertyParams::WithAccessors("y", EPropertyFlags::None, 1, &MutableMember<FQuatf, &FQuatf::y>, &ConstMember<FQuatf, &FQuatf::y>);
	static const DurinCodeGen::FFloatPropertyParams Z = DurinCodeGen::FFloatPropertyParams::WithAccessors("z", EPropertyFlags::None, 1, &MutableMember<FQuatf, &FQuatf::z>, &ConstMember<FQuatf, &FQuatf::z>);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&W, &X, &Y, &Z};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FQuatf", "FQuatf", sizeof(FQuatf), alignof(FQuatf), Properties, std::size(Properties), &GetDStructOps<FQuatf>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FMatrix4f() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FMatrix4f", "FMatrix4f", sizeof(FMatrix4f), alignof(FMatrix4f));
	static constexpr FPropertyMetadataParams ColumnMetadata{.ToolTip = "Column-major matrix column", .Category = "Matrix"};
	static const auto Column0 = DurinCodeGen::WithTypedMetadata(DurinCodeGen::FStructPropertyParams::WithAccessors("Column0", EPropertyFlags::None, 1, &Z_Construct_DStruct_FVector4f, &MutableIndex<FMatrix4f, 0>, &ConstIndex<FMatrix4f, 0>), &ColumnMetadata);
	static const auto Column1 = DurinCodeGen::WithTypedMetadata(DurinCodeGen::FStructPropertyParams::WithAccessors("Column1", EPropertyFlags::None, 1, &Z_Construct_DStruct_FVector4f, &MutableIndex<FMatrix4f, 1>, &ConstIndex<FMatrix4f, 1>), &ColumnMetadata);
	static const auto Column2 = DurinCodeGen::WithTypedMetadata(DurinCodeGen::FStructPropertyParams::WithAccessors("Column2", EPropertyFlags::None, 1, &Z_Construct_DStruct_FVector4f, &MutableIndex<FMatrix4f, 2>, &ConstIndex<FMatrix4f, 2>), &ColumnMetadata);
	static const auto Column3 = DurinCodeGen::WithTypedMetadata(DurinCodeGen::FStructPropertyParams::WithAccessors("Column3", EPropertyFlags::None, 1, &Z_Construct_DStruct_FVector4f, &MutableIndex<FMatrix4f, 3>, &ConstIndex<FMatrix4f, 3>), &ColumnMetadata);
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&Column0, &Column1, &Column2, &Column3};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FMatrix4f", "FMatrix4f", sizeof(FMatrix4f), alignof(FMatrix4f), Properties, std::size(Properties), &GetDStructOps<FMatrix4f>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FTransform() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FTransform", "FTransform", sizeof(FTransform), alignof(FTransform));
	static constexpr FPropertyMetadataParams RotationMetadata{.ToolTip = "Local quaternion rotation", .Category = "Transform"};
	static constexpr FPropertyMetadataParams TranslationMetadata{.ToolTip = "Local translation", .Category = "Transform"};
	static const auto Rotation = DurinCodeGen::WithTypedMetadata(DurinCodeGen::FStructPropertyParams{"Rotation", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FTransform, Rotation)), &Z_Construct_DStruct_FQuat}, &RotationMetadata);
	static const auto Translation = DurinCodeGen::WithTypedMetadata(DurinCodeGen::FStructPropertyParams{"Translation", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FTransform, Translation)), &Z_Construct_DStruct_FVector3}, &TranslationMetadata);
	static const DurinCodeGen::FStructPropertyParams Scale = {"Scale3D", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FTransform, Scale3D)), &Z_Construct_DStruct_FVector3};
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&Rotation, &Translation, &Scale};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FTransform", "FTransform", sizeof(FTransform), alignof(FTransform), Properties, std::size(Properties), &GetDStructOps<FTransform>()};
	return DurinCodeGen::ConstructDStruct(Params);
}

auto Z_Construct_DStruct_FLinearColor() -> DStruct*
{
	static DStruct* Singleton = nullptr;
	if (Singleton) return Singleton;
	Singleton = MakeStruct("Durin::FLinearColor", "FLinearColor", sizeof(FLinearColor), alignof(FLinearColor));
	static const DurinCodeGen::FFloatPropertyParams R = {"R", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, R))};
	static const DurinCodeGen::FFloatPropertyParams G = {"G", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, G))};
	static const DurinCodeGen::FFloatPropertyParams B = {"B", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, B))};
	static const DurinCodeGen::FFloatPropertyParams A = {"A", EPropertyFlags::None, 1, static_cast<uint16>(STRUCT_OFFSET(FLinearColor, A))};
	static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&R, &G, &B, &A};
	static const DurinCodeGen::FStructParams Params = {[]() -> DStruct* { return Singleton; }, "Durin::FLinearColor", "FLinearColor", sizeof(FLinearColor), alignof(FLinearColor), Properties, std::size(Properties), &GetDStructOps<FLinearColor>()};
	return DurinCodeGen::ConstructDStruct(Params);
}
} // namespace Durin
