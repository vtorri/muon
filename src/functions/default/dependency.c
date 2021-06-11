#include "posix.h"

#include <string.h>

#include "coerce.h"
#include "functions/common.h"
#include "functions/default/dependency.h"
#include "interpreter.h"
#include "log.h"
#include "pkgconf.h"
#include "run_cmd.h"

bool
pkg_config(struct workspace *wk, struct run_cmd_ctx *ctx, uint32_t args_node, const char *arg, const char *depname)
{
	if (!run_cmd(ctx, "pkg-config", (char *[]){ "pkg-config", (char *)arg, (char *)depname, NULL })) {
		if (ctx->err_msg) {
			interp_error(wk, args_node, "error: %s", ctx->err_msg);
		} else {
			interp_error(wk, args_node, "error: %s", strerror(ctx->err_no));
		}
		return false;
	}

	return true;
}

static bool
get_dependency(struct workspace *wk, uint32_t *obj, uint32_t node, uint32_t name, enum requirement_type requirement)
{
	struct pkgconf_info info = { 0 };

	struct obj *dep = make_obj(wk, obj, obj_dependency);
	dep->dat.dep.name = name;

	if (!pkgconf_lookup(wk, wk_objstr(wk, name), &info)) {
		if (requirement == requirement_required) {
			interp_error(wk, node, "required dependency not found");
			return false;
		}

		LOG_I(log_interp, "dependency %s not found", wk_objstr(wk, dep->dat.dep.name));
		return true;
	}

	dep->dat.dep.version = wk_str_push(wk, info.version);

	LOG_I(log_interp, "dependency %s found: %s", wk_objstr(wk, dep->dat.dep.name), wk_str(wk, dep->dat.dep.version));

	dep->dat.dep.flags |= dep_flag_found;
	dep->dat.dep.flags |= dep_flag_pkg_config;

	get_obj(wk, *obj)->dat.dep.link_with = info.libs;
	get_obj(wk, *obj)->dat.dep.include_directories = info.includes;
	return true;
}

static bool
handle_special_dependency(struct workspace *wk, uint32_t node, uint32_t name,
	enum requirement_type requirement,  uint32_t *obj, bool *handled)
{
	if (strcmp(wk_objstr(wk, name), "threads") == 0) {
		*handled = true;
		struct obj *dep = make_obj(wk, obj, obj_dependency);
		dep->dat.dep.name = get_obj(wk, name)->dat.str;
		dep->dat.dep.flags |= dep_flag_found;
	} else if (strcmp(wk_objstr(wk, name), "curses") == 0) {
		*handled = true;
		uint32_t s;
		make_obj(wk, &s, obj_string)->dat.str = wk_str_push(wk, "ncurses");
		if (!get_dependency(wk, obj, node, s, requirement)) {
			return false;
		}
	} else {
		*handled = false;
	}

	return true;
}

struct parse_cflags_iter_ctx {
	uint32_t include_directories;
};

bool
func_dependency(struct workspace *wk, uint32_t rcvr, uint32_t args_node, uint32_t *obj)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_required,
		kw_native,
		kw_version,
		kw_static,
		kw_modules,
	};
	struct args_kw akw[] = {
		[kw_required] = { "required" },
		[kw_native] = { "native", obj_bool },
		[kw_version] = { "version", obj_string },
		[kw_static] = { "static", obj_bool },
		[kw_modules] = { "modules", obj_array },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	enum requirement_type requirement;
	if (!coerce_requirement(wk, &akw[kw_required], &requirement)) {
		return false;
	}

	if (requirement == requirement_skip) {
		struct obj *dep = make_obj(wk, obj, obj_dependency);
		dep->dat.dep.name = an[0].val;
		return true;
	}

	bool handled;
	if (!handle_special_dependency(wk, an[0].node, an[0].val, requirement, obj, &handled)) {
		return false;
	} else if (handled) {
		return true;
	}

	return get_dependency(wk, obj, an[0].node, an[0].val, requirement);
}
