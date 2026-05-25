#include "Widgets/MCompoundWidget.h"

namespace Durin::Mona
{
	void MCompoundWidget::Draw()
	{
		if (ChildWidget)
		{
			ChildWidget->Draw();
		}
	}
} // namespace Durin::Mona
