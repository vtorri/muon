#include "posix.h"

#include "functions/common.h"
#include "functions/disabler.h"
#include "lang/interpreter.h"
#include "log.h"

static bool
func_disabler_found(struct workspace *wk, obj rcvr, uint32_t args_node, obj *res)
{
	if (!interp_args(wk, args_node, NULL, NULL, NULL)) {
		return false;
	}

	make_obj(wk, res, obj_bool)->dat.boolean = false;
	return true;
}

const struct func_impl_name impl_tbl_disabler[] = {
	{ "found", func_disabler_found },
	{ NULL, NULL },
};