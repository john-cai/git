#ifndef HOOK_H
#define HOOK_H
#include "strvec.h"

struct run_hooks_opt
{
	/* Environment vars to be set for each hook */
	struct strvec env;

	/* Args to be passed to each hook */
	struct strvec args;

	/*
	 * Resolve and run the "absolute_path(hook)" instead of
	 * "hook". Used for "git worktree" hooks
	 */
	int absolute_path;

	/* Path to initial working directory for subprocess */
	const char *dir;
};

#define RUN_HOOKS_OPT_INIT { \
	.env = STRVEC_INIT, \
	.args = STRVEC_INIT, \
}

struct hook_cb_data {
	/* rc reflects the cumulative failure state */
	int rc;
	const char *hook_name;
	const char *hook_path;
	struct run_hooks_opt *options;
};

/*
 * Returns the path to the hook file, or NULL if the hook is missing
 * or disabled. Note that this points to static storage that will be
 * overwritten by further calls to find_hook and run_hook_*.
 */
const char *find_hook(const char *name);

/**
 * A boolean version of find_hook()
 */
int hook_exists(const char *hookname);

/**
 * Clear data from an initialized "struct run_hooks_opt".
 */
void run_hooks_opt_clear(struct run_hooks_opt *o);

/**
 * Takes an already resolved hook found via find_hook() and runs
 * it. Does not call run_hooks_opt_clear() for you.
 *
 * See run_hooks_oneshot() for the simpler one-shot API.
 */
int run_hooks(const char *hookname, const char *hook_path,
	      struct run_hooks_opt *options);

/**
 * Calls find_hook() on your "hook_name" and runs the hooks (if any)
 * with run_hooks().
 *
 * If "options" is provided calls run_hooks_opt_clear() on it for
 * you. If "options" is NULL the default options from
 * RUN_HOOKS_OPT_INIT will be used.
 */
int run_hooks_oneshot(const char *hook_name, struct run_hooks_opt *options);

#endif
