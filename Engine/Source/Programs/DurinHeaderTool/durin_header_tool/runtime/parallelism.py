MIN_PARALLEL_TASKS = 8
MEDIUM_PARALLEL_TASKS = 16
LARGE_PARALLEL_TASKS = 32


def resolve_worker_count(task_count: int, max_workers: int) -> int:
    if max_workers < 1:
        raise ValueError("max_workers must be at least 1")
    # Process startup and libclang initialization cost more than they save for
    # the small reflected modules that Ninja can already schedule in parallel.
    if task_count < MIN_PARALLEL_TASKS:
        return 1
    if task_count < MEDIUM_PARALLEL_TASKS:
        return min(task_count, max_workers, 2)
    if task_count < LARGE_PARALLEL_TASKS:
        return min(task_count, max_workers, 4)
    return min(task_count, max_workers)
