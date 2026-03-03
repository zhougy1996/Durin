#include "Widgets/MCompoundWidget.h"

namespace Doge::Mona
{
	void MCompoundWidget::OnPaint()
	{
		ChildWidget->Paint();
	}
} // namespace Doge::Mona