/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_db_close --
 *	Close a DB handle.
 */
int
__wt_db_close(DB *db, u_int32_t flags)
{
	ENV *env;
	IDB *idb;
	int ret, tret;

	env = db->env;
	idb = db->idb;
	ret = 0;

	DB_FLAG_CHK_NOTFATAL(db, "Db.close", flags, WT_APIMASK_DB_CLOSE, ret);

	/* Free associated memory. */
	__wt_free(env, idb->file_name);

	/* Close the underlying Btree. */
	if ((tret = __wt_bt_close(db)) != 0 && ret == 0)
		ret = tret;

	/* Re-cycle the underlying IDB structure */
	__wt_idb_destroy(db, 1);

	/* Reset the methods that are permitted. */
	__wt_db_config_methods(db);

	return (ret);
}
