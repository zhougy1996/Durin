#pragma once

#include "LevelEditorAPI.h"

#include "DObject/ObjectMacros.h"
#include "DObject/Object.h"

#include "TestDHT.gen.h"

namespace Durin
{
	DENUM()
	enum class ETestDHTMode : uint8
	{
		None,
		Visible = 4,
		Hidden
	};

	DCLASS(AAA, bbb = ccc)
	class LEVELEDITOR_API TestDHT : DObject
	{
		GENERATED_BODY()

	public:
		DFUNCTION()
		void Func() {};


	private:
		const std::vector<const DObject*> a;

		DPROPERTY(Edit, Transient, zzz)
		uint16 a1 = 0;

		DPROPERTY(zzz)
		int32 a2 = 0;

		DPROPERTY(EditConst)
		uint8 a3[3] = {};

		DPROPERTY()
		DObject* ObjectRef = nullptr;

		DPROPERTY()
		std::string DisplayName;

		DPROPERTY()
		ETestDHTMode Mode = ETestDHTMode::None;

		DPROPERTY()
		std::vector<int32> Scores;

		DPROPERTY()
		std::vector<ETestDHTMode> Modes;

		DPROPERTY()
		std::vector<DObject*> ObjectRefs;

		DPROPERTY()
		std::unordered_map<std::string, int32> NamedScores;

		DPROPERTY()
		std::unordered_map<std::string, ETestDHTMode> NamedModes;

		DPROPERTY()
		std::unordered_map<ETestDHTMode, int32> ModeScores;

		DPROPERTY()
		std::vector<std::vector<int32>> NestedScores;

		DPROPERTY()
		std::vector<std::unordered_map<std::string, DObject*>> ObjectMapList;

		DPROPERTY()
		std::unordered_map<std::string, std::vector<int32>> ScoreGroups;

		DPROPERTY()
		std::vector<std::vector<std::vector<std::vector<std::vector<int32>>>>> UnsupportedTooDeep;

		DPROPERTY()
		std::unordered_map<DObject*, int32> UnsupportedObjectKeyMap;

		DPROPERTY()
		std::vector<std::unique_ptr<DObject>> UnsupportedUniqueObjects;
	};
}
