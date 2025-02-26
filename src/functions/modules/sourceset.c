/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#include "functions/modules/sourceset.h"
#include "lang/typecheck.h"

static bool
func_module_sourceset_source_set(struct workspace *wk, obj self, obj *res)
{
	if (!pop_args(wk, NULL, NULL)) {
		return false;
	}

	*res = make_obj(wk, obj_source_set);
	struct obj_source_set *ss = get_obj_source_set(wk, *res);
	ss->rules = make_obj(wk, obj_array);
	return true;
}

const struct func_impl impl_tbl_module_sourceset[] = {
	{
		"source_set",
		func_module_sourceset_source_set,
		tc_source_set,
	},
	{ NULL, NULL },
};
