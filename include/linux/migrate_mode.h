#ifndef MIGRATE_MODE_H_INCLUDED
#define MIGRATE_MODE_H_INCLUDED
/*
 * MIGRATE_ASYNC means never block
 * MIGRATE_SYNC_LIGHT in the current implementation means to allow blocking
 *	on most operations but not ->writepage as the potential stall time
 *	is too significant
 * MIGRATE_SYNC will block when migrating pages
 * MIGRATE_SHMEM_RECOVERY is a MIGRATE_SYNC specific to huge tmpfs recovery.
 */
enum migrate_mode {
	MIGRATE_ASYNC,
	MIGRATE_SYNC_LIGHT,
	MIGRATE_SYNC,
	MIGRATE_SHMEM_RECOVERY,
};

#endif		/* MIGRATE_MODE_H_INCLUDED */
