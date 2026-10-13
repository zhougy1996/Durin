#include "MaterialParameterCollectionDetails.h"
#include "DObject/Class.h"
#include "Materials/MaterialParameterCollection.h"

namespace Durin::Editor::Material
{
	namespace
	{
		// Places collection declarations in the shared layout without replacing their editor.
		class FMaterialParameterCollectionDetails final : public IDetailCustomization
		{
		public:
			auto CustomizeDetails(IDetailLayoutBuilder& Builder) -> void override
			{
				auto* Collection = Cast<DMaterialParameterCollection>(Builder.GetObject());
				if (!Collection) return;
				Builder.EditCategory("Parameters").AddProperty(Collection,
					Collection->GetClass()->FindPropertyByName("Declarations"));
			}
		};
	}

	auto CreateMaterialParameterCollectionDetails() -> std::shared_ptr<IDetailCustomization>
	{
		return std::make_shared<FMaterialParameterCollectionDetails>();
	}
}
