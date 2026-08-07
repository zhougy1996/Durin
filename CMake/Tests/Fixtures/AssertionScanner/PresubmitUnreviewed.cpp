void PerformRequiredWork();

void PresubmitMustRejectUnreviewedSideEffects()
{
	check(PerformRequiredWork());
}
