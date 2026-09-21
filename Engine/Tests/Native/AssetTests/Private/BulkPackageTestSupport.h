#pragma once

#include "Asset/EditorBulkData.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"

namespace
{
	class DBulkPackageAssetForTest : public Durin::DObject
	{
	public:
		explicit DBulkPackageAssetForTest(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer), Payload(Durin::FGuid{0x55112233, 0x44556677,
				0x8899aabb, 0xccddeeff})
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DBulkPackageAssetForTest(X);
		}

		static auto StaticClassNoRegister() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(Durin::EC_StaticConstructor,
					"DBulkPackageAssetForTest", sizeof(DBulkPackageAssetForTest),
					alignof(DBulkPackageAssetForTest), Durin::EObjectFlags::NoFlags,
					Durin::EClassFlags::None, Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DBulkPackageAssetForTest>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "",
					"DBulkPackageAssetForTest");
			}
			return Class;
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static const Durin::DurinCodeGen::FBulkDataPropertyParams PayloadProp =
				Durin::DurinCodeGen::FBulkDataPropertyParams::Create<
					Durin::FEditorBulkData>(
						"Payload", Durin::EPropertyFlags::None, 1,
						STRUCT_OFFSET_UINT16(DBulkPackageAssetForTest, Payload));
			static const Durin::DurinCodeGen::FPropertyParamsBase* Properties[] = {
				&PayloadProp};
			static const Durin::DurinCodeGen::FClassParams Params = {
				&StaticClassNoRegister, "Tests::DBulkPackageAssetForTest",
				"DBulkPackageAssetForTest", Properties, std::size(Properties)};
			static Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(Params);
			return Class;
		}

		Durin::FEditorBulkData Payload;
	};

}
