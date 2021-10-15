#include "cache.h"
#include "hook.h"
#include "run-command.h"
#include "config.h"

const char *find_hook(const char *name)
{
	static struct strbuf path = STRBUF_INIT;

	strbuf_reset(&path);
	strbuf_git_path(&path, "hooks/%s", name);
	if (access(path.buf, X_OK) < 0) {
		int err = errno;

#ifdef STRIP_EXTENSION
		strbuf_addstr(&path, STRIP_EXTENSION);
		if (access(path.buf, X_OK) >= 0)
			return path.buf;
		if (errno == EACCES)
			err = errno;
#endif

		if (err == EACCES && advice_enabled(ADVICE_IGNORED_HOOK)) {
			static struct string_list advise_given = STRING_LIST_INIT_DUP;

			if (!string_list_lookup(&advise_given, name)) {
				string_list_insert(&advise_given, name);
				advise(_("The '%s' hook was ignored because "
					 "it's not set as executable.\n"
					 "You can disable this warning with "
					 "`git config advice.ignoredHook false`."),
				       path.buf);
			}
		}
		return NULL;
	}
	return path.buf;
}

int hook_exists(const char *name)
{
	return !!find_hook(name);
}

void run_hooks_opt_clear(struct run_hooks_opt *o)
{
	strvec_clear(&o->env);
	strvec_clear(&o->args);
}

static int pick_next_hook(struct child_process *cp,
			  struct strbuf *out,
			  void *pp_cb,
			  void **pp_task_cb)
{
	struct hook_cb_data *hook_cb = pp_cb;
	const char *hook_path = hook_cb->hook_path;

	if (!hook_path)
		return 0;

	cp->no_stdin = 1;
	cp->env = hook_cb->options->env.v;
	cp->stdout_to_stderr = 1;
	cp->trace2_hook_name = hook_cb->hook_name;
	cp->dir = hook_cb->options->dir;

	strvec_push(&cp->args, hook_path);
	strvec_pushv(&cp->args, hook_cb->options->args.v);

	/* Provide context for errors if necessary */
	*pp_task_cb = (char *)hook_path;

	/*
	 * This pick_next_hook() will be called again, we're only
	 * running one hook, so indicate that no more work will be
	 * done.
	 */
	hook_cb->hook_path = NULL;

	return 1;
}

static int notify_start_failure(struct strbuf *out,
				void *pp_cb,
				void *pp_task_cp)
{
	struct hook_cb_data *hook_cb = pp_cb;
	const char *hook_path = pp_task_cp;

	hook_cb->rc |= 1;

	strbuf_addf(out, _("Couldn't start hook '%s'\n"),
		    hook_path);

	return 1;
}

static int notify_hook_finished(int result,
				struct strbuf *out,
				void *pp_cb,
				void *pp_task_cb)
{
	struct hook_cb_data *hook_cb = pp_cb;

	hook_cb->rc |= result;

	return 0;
}

int run_hooks(const char *hook_name, const char *hook_path,
	      struct run_hooks_opt *options)
{
	struct strbuf abs_path = STRBUF_INIT;
	struct hook_cb_data cb_data = {
		.rc = 0,
		.hook_name = hook_name,
		.hook_path = hook_path,
		.options = options,
	};
	int jobs = 1;

	if (!options)
		BUG("a struct run_hooks_opt must be provided to run_hooks");

	if (options->absolute_path) {
		strbuf_add_absolute_path(&abs_path, hook_path);
		hook_path = abs_path.buf;
	}

	run_processes_parallel_tr2(jobs,
				   pick_next_hook,
				   notify_start_failure,
				   notify_hook_finished,
				   &cb_data,
				   "hook",
				   hook_name);

	if (options->absolute_path)
		strbuf_release(&abs_path);

	return cb_data.rc;
}

int run_hooks_oneshot(const char *hook_name, struct run_hooks_opt *options)
{
	const char *hook_path;
	struct run_hooks_opt hook_opt_scratch = RUN_HOOKS_OPT_INIT;
	int ret = 0;

	if (!options)
		options = &hook_opt_scratch;

	hook_path = find_hook(hook_name);
	if (!hook_path)
		goto cleanup;

	ret = run_hooks(hook_name, hook_path, options);
cleanup:
	run_hooks_opt_clear(options);

	return ret;
}
