#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "PropertyEditor/DetailCustomization.h"

namespace Durin { class DClass; }

namespace Durin::Editor
{
	// Identifies a class registration owned by an editor module.
	struct FDetailCustomizationHandle
	{
		uint64 Id = 0;
		explicit operator bool() const { return Id != 0; }
	};

	// Resolves reflected object layouts in base-to-derived registration order.
	class FDetailCustomizationRegistry
	{
	public:
		DURINED_API static auto Get() -> FDetailCustomizationRegistry&;
		DURINED_API auto RegisterDetailCustomization(DClass* Class, std::shared_ptr<IDetailCustomization> Customization) -> FDetailCustomizationHandle;
		DURINED_API auto Unregister(FDetailCustomizationHandle Handle) -> bool;
		DURINED_API auto FindDetailCustomization(const DClass* Class) const -> std::shared_ptr<IDetailCustomization>;
		DURINED_API auto FindDetailCustomizations(const DClass* Class) const -> std::vector<std::shared_ptr<IDetailCustomization>>;
	private:
		// Couples a registration identity with its editor-owned customization.
		struct FEntry
		{
			uint64 Id;
			std::shared_ptr<IDetailCustomization> Customization;
		};
		uint64 NextId = 1;
		std::unordered_map<DClass*, FEntry> Entries;
	};
}
