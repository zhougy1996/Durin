void OrderingA()
{
#if FEATURE_DISABLED
	check(VisitInactiveBranch());
#else
	check(IsReady());
#endif
}
