#include "builtin.h"
#include "config.h"
#include "lockfile.h"
#include "repository.h"
#include "dir.h"
#include "diff.h"
#include "revision.h"
#include "reachable.h"
#include "worktree.h"
#include "reflog.h"

/* NEEDSWORK: switch to using parse_options */
static const char reflog_expire_usage[] =
N_("git reflog expire [--expire=<time>] "
   "[--expire-unreachable=<time>] "
   "[--rewrite] [--updateref] [--stale-fix] [--dry-run | -n] "
   "[--verbose] [--all] <refs>...");
static const char reflog_delete_usage[] =
N_("git reflog delete [--rewrite] [--updateref] "
   "[--dry-run | -n] [--verbose] <refs>...");
static const char reflog_exists_usage[] =
N_("git reflog exists <ref>");

static timestamp_t default_reflog_expire;
static timestamp_t default_reflog_expire_unreachable;

struct worktree_reflogs {
	struct worktree *worktree;
	struct string_list reflogs;
};

static int collect_reflog(const char *ref, const struct object_id *oid, int unused, void *cb_data)
{
	struct worktree_reflogs *cb = cb_data;
	struct worktree *worktree = cb->worktree;
	struct strbuf newref = STRBUF_INIT;

	/*
	 * Avoid collecting the same shared ref multiple times because
	 * they are available via all worktrees.
	 */
	if (!worktree->is_current && ref_type(ref) == REF_TYPE_NORMAL)
		return 0;

	strbuf_worktree_ref(worktree, &newref, ref);
	string_list_append_nodup(&cb->reflogs, strbuf_detach(&newref, NULL));

	return 0;
}

static struct reflog_expire_cfg {
	struct reflog_expire_cfg *next;
	timestamp_t expire_total;
	timestamp_t expire_unreachable;
	char pattern[FLEX_ARRAY];
} *reflog_expire_cfg, **reflog_expire_cfg_tail;

static struct reflog_expire_cfg *find_cfg_ent(const char *pattern, size_t len)
{
	struct reflog_expire_cfg *ent;

	if (!reflog_expire_cfg_tail)
		reflog_expire_cfg_tail = &reflog_expire_cfg;

	for (ent = reflog_expire_cfg; ent; ent = ent->next)
		if (!strncmp(ent->pattern, pattern, len) &&
		    ent->pattern[len] == '\0')
			return ent;

	FLEX_ALLOC_MEM(ent, pattern, pattern, len);
	*reflog_expire_cfg_tail = ent;
	reflog_expire_cfg_tail = &(ent->next);
	return ent;
}

/* expiry timer slot */
#define EXPIRE_TOTAL   01
#define EXPIRE_UNREACH 02

static int reflog_expire_config(const char *var, const char *value, void *cb)
{
	const char *pattern, *key;
	size_t pattern_len;
	timestamp_t expire;
	int slot;
	struct reflog_expire_cfg *ent;

	if (parse_config_key(var, "gc", &pattern, &pattern_len, &key) < 0)
		return git_default_config(var, value, cb);

	if (!strcmp(key, "reflogexpire")) {
		slot = EXPIRE_TOTAL;
		if (git_config_expiry_date(&expire, var, value))
			return -1;
	} else if (!strcmp(key, "reflogexpireunreachable")) {
		slot = EXPIRE_UNREACH;
		if (git_config_expiry_date(&expire, var, value))
			return -1;
	} else
		return git_default_config(var, value, cb);

	if (!pattern) {
		switch (slot) {
		case EXPIRE_TOTAL:
			default_reflog_expire = expire;
			break;
		case EXPIRE_UNREACH:
			default_reflog_expire_unreachable = expire;
			break;
		}
		return 0;
	}

	ent = find_cfg_ent(pattern, pattern_len);
	if (!ent)
		return -1;
	switch (slot) {
	case EXPIRE_TOTAL:
		ent->expire_total = expire;
		break;
	case EXPIRE_UNREACH:
		ent->expire_unreachable = expire;
		break;
	}
	return 0;
}

static void set_reflog_expiry_param(struct cmd_reflog_expire_cb *cb, int slot, const char *ref)
{
	struct reflog_expire_cfg *ent;

	if (slot == (EXPIRE_TOTAL|EXPIRE_UNREACH))
		return; /* both given explicitly -- nothing to tweak */

	for (ent = reflog_expire_cfg; ent; ent = ent->next) {
		if (!wildmatch(ent->pattern, ref, 0)) {
			if (!(slot & EXPIRE_TOTAL))
				cb->expire_total = ent->expire_total;
			if (!(slot & EXPIRE_UNREACH))
				cb->expire_unreachable = ent->expire_unreachable;
			return;
		}
	}

	/*
	 * If unconfigured, make stash never expire
	 */
	if (!strcmp(ref, "refs/stash")) {
		if (!(slot & EXPIRE_TOTAL))
			cb->expire_total = 0;
		if (!(slot & EXPIRE_UNREACH))
			cb->expire_unreachable = 0;
		return;
	}

	/* Nothing matched -- use the default value */
	if (!(slot & EXPIRE_TOTAL))
		cb->expire_total = default_reflog_expire;
	if (!(slot & EXPIRE_UNREACH))
		cb->expire_unreachable = default_reflog_expire_unreachable;
}

static int cmd_reflog_expire(int argc, const char **argv, const char *prefix)
{
	struct cmd_reflog_expire_cb cmd = { 0 };
	timestamp_t now = time(NULL);
	int i, status, do_all, all_worktrees = 1;
	int explicit_expiry = 0;
	unsigned int flags = 0;
	int verbose = 0;
	reflog_expiry_should_prune_fn *should_prune_fn = should_expire_reflog_ent;

	default_reflog_expire_unreachable = now - 30 * 24 * 3600;
	default_reflog_expire = now - 90 * 24 * 3600;
	git_config(reflog_expire_config, NULL);

	save_commit_buffer = 0;
	do_all = status = 0;

	cmd.expire_total = default_reflog_expire;
	cmd.expire_unreachable = default_reflog_expire_unreachable;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--dry-run") || !strcmp(arg, "-n"))
			flags |= EXPIRE_REFLOGS_DRY_RUN;
		else if (skip_prefix(arg, "--expire=", &arg)) {
			if (parse_expiry_date(arg, &cmd.expire_total))
				die(_("'%s' is not a valid timestamp"), arg);
			explicit_expiry |= EXPIRE_TOTAL;
		}
		else if (skip_prefix(arg, "--expire-unreachable=", &arg)) {
			if (parse_expiry_date(arg, &cmd.expire_unreachable))
				die(_("'%s' is not a valid timestamp"), arg);
			explicit_expiry |= EXPIRE_UNREACH;
		}
		else if (!strcmp(arg, "--stale-fix"))
			cmd.stalefix = 1;
		else if (!strcmp(arg, "--rewrite"))
			flags |= EXPIRE_REFLOGS_REWRITE;
		else if (!strcmp(arg, "--updateref"))
			flags |= EXPIRE_REFLOGS_UPDATE_REF;
		else if (!strcmp(arg, "--all"))
			do_all = 1;
		else if (!strcmp(arg, "--single-worktree"))
			all_worktrees = 0;
		else if (!strcmp(arg, "--verbose"))
			verbose = 1;
		else if (!strcmp(arg, "--")) {
			i++;
			break;
		}
		else if (arg[0] == '-')
			usage(_(reflog_expire_usage));
		else
			break;
	}

	if (verbose)
		should_prune_fn = should_expire_reflog_ent_verbose;

	/*
	 * We can trust the commits and objects reachable from refs
	 * even in older repository.  We cannot trust what's reachable
	 * from reflog if the repository was pruned with older git.
	 */
	if (cmd.stalefix) {
		struct rev_info revs;

		repo_init_revisions(the_repository, &revs, prefix);
		revs.do_not_die_on_missing_tree = 1;
		revs.ignore_missing = 1;
		revs.ignore_missing_links = 1;
		if (verbose)
			printf(_("Marking reachable objects..."));
		mark_reachable_objects(&revs, 0, 0, NULL);
		if (verbose)
			putchar('\n');
	}

	if (do_all) {
		struct worktree_reflogs collected = {
			.reflogs = STRING_LIST_INIT_DUP,
		};
		struct string_list_item *item;
		struct worktree **worktrees, **p;

		worktrees = get_worktrees();
		for (p = worktrees; *p; p++) {
			if (!all_worktrees && !(*p)->is_current)
				continue;
			collected.worktree = *p;
			refs_for_each_reflog(get_worktree_ref_store(*p),
					     collect_reflog, &collected);
		}
		free_worktrees(worktrees);

		for_each_string_list_item(item, &collected.reflogs) {
			struct expire_reflog_policy_cb cb = {
				.cmd = cmd,
				.dry_run = !!(flags & EXPIRE_REFLOGS_DRY_RUN),
			};

			set_reflog_expiry_param(&cb.cmd, explicit_expiry, item->string);
			status |= reflog_expire(item->string, flags,
						reflog_expiry_prepare,
						should_prune_fn,
						reflog_expiry_cleanup,
						&cb);
		}
		string_list_clear(&collected.reflogs, 0);
	}

	for (; i < argc; i++) {
		char *ref;
		struct expire_reflog_policy_cb cb = { .cmd = cmd };

		if (!dwim_log(argv[i], strlen(argv[i]), NULL, &ref)) {
			status |= error(_("%s points nowhere!"), argv[i]);
			continue;
		}
		set_reflog_expiry_param(&cb.cmd, explicit_expiry, ref);
		status |= reflog_expire(ref, flags,
					reflog_expiry_prepare,
					should_prune_fn,
					reflog_expiry_cleanup,
					&cb);
		free(ref);
	}
	return status;
}

static int cmd_reflog_delete(int argc, const char **argv, const char *prefix)
{
	int i, status = 0;
	unsigned int flags = 0;
	int verbose = 0;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (!strcmp(arg, "--dry-run") || !strcmp(arg, "-n"))
			flags |= EXPIRE_REFLOGS_DRY_RUN;
		else if (!strcmp(arg, "--rewrite"))
			flags |= EXPIRE_REFLOGS_REWRITE;
		else if (!strcmp(arg, "--updateref"))
			flags |= EXPIRE_REFLOGS_UPDATE_REF;
		else if (!strcmp(arg, "--verbose"))
			verbose = 1;
		else if (!strcmp(arg, "--")) {
			i++;
			break;
		}
		else if (arg[0] == '-')
			usage(_(reflog_delete_usage));
		else
			break;
	}

	if (argc - i < 1)
		return error(_("no reflog specified to delete"));

	for ( ; i < argc; i++) {
		status |= reflog_delete(argv[i], flags, verbose);
	}
	return status;
}

static int cmd_reflog_exists(int argc, const char **argv, const char *prefix)
{
	int i, start = 0;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (!strcmp(arg, "--")) {
			i++;
			break;
		}
		else if (arg[0] == '-')
			usage(_(reflog_exists_usage));
		else
			break;
	}

	start = i;

	if (argc - start != 1)
		usage(_(reflog_exists_usage));

	if (check_refname_format(argv[start], REFNAME_ALLOW_ONELEVEL))
		die(_("invalid ref format: %s"), argv[start]);
	return !reflog_exists(argv[start]);
}

/*
 * main "reflog"
 */

static const char reflog_usage[] =
N_("git reflog [ show | expire | delete | exists ]");

int cmd_reflog(int argc, const char **argv, const char *prefix)
{
	if (argc > 1 && !strcmp(argv[1], "-h"))
		usage(_(reflog_usage));

	/* With no command, we default to showing it. */
	if (argc < 2 || *argv[1] == '-')
		return cmd_log_reflog(argc, argv, prefix);

	if (!strcmp(argv[1], "show"))
		return cmd_log_reflog(argc - 1, argv + 1, prefix);

	if (!strcmp(argv[1], "expire"))
		return cmd_reflog_expire(argc - 1, argv + 1, prefix);

	if (!strcmp(argv[1], "delete"))
		return cmd_reflog_delete(argc - 1, argv + 1, prefix);

	if (!strcmp(argv[1], "exists"))
		return cmd_reflog_exists(argc - 1, argv + 1, prefix);

	return cmd_log_reflog(argc, argv, prefix);
}
