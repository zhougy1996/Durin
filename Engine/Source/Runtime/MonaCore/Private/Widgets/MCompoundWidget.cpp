#include "Widgets/MCompoundWidget.h"

namespace Doge::Mona
{
	void MCompoundWidget::Draw()
	{
		ChildWidget->Draw();
	}
} // namespace Doge::Mona