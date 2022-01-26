#ifndef REFLOG_H
#define REFLOG_H

#include "cache.h"
#include "commit.h"

/* Remember to update object flag allocation in object.h */
#define INCOMPLETE	(1u<<10)
#define STUDYING	(1u<<11)
#define REACHABLE	(1u<<12)

struct cmd_reflog_expire_cb {
	int stalefix;
	timestamp_t expire_total;
	timestamp_t expire_unreachable;
	int recno;
};

struct expire_reflog_policy_cb {
	enum {
		UE_NORMAL,
		UE_ALWAYS,
		UE_HEAD
	} unreachable_expire_kind;
	struct commit_list *mark_list;
	unsigned long mark_limit;
	struct cmd_reflog_expire_cb cmd;
	struct commit *tip_commit;
	struct commit_list *tips;
	unsigned int dry_run:1;
};

int reflog_delete(const char*, int, int);
void reflog_expiry_cleanup(void *);
void reflog_expiry_prepare(const char*, const struct object_id*,
			   void *);
int should_expire_reflog_ent(struct object_id *, struct object_id*,
				    const char *, timestamp_t, int,
				    const char *, void *);
int count_reflog_ent(struct object_id *ooid, struct object_id *noid,
		const char *email, timestamp_t timestamp, int tz,
		const char *message, void *cb_data);
int should_expire_reflog_ent_verbose(struct object_id *,
				     struct object_id *,
				     const char *,
				     timestamp_t, int,
				     const char *, void *);
#endif /* REFLOG_H */
