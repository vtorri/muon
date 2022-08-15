#include "posix.h"

#include <assert.h>
#include <string.h>

#include "coerce.h"
#include "error.h"
#include "functions/kernel/install.h"
#include "install.h"
#include "lang/interpreter.h"
#include "options.h"
#include "platform/path.h"

bool
func_install_subdir(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_install_tag,
		kw_exclude_directories,
		kw_exclude_files,
		kw_strip_directory,
	};
	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string, .required = true },
		[kw_install_mode] = { "install_mode", tc_install_mode_kw },
		[kw_install_tag] = { "install_tag", obj_string }, // TODO
		[kw_exclude_directories] = { "exclude_directories", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_exclude_files] = { "exclude_files", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_strip_directory] = { "strip_directory", obj_bool },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	bool strip_directory = akw[kw_strip_directory].set
		? get_obj_bool(wk, akw[kw_strip_directory].val)
		: false;

	obj dest = akw[kw_install_dir].val;
	if (!strip_directory) {
		char path[PATH_MAX], name[PATH_MAX] = { 0 }, *sep;
		const char *name_tail;

		strncpy(name, get_cstr(wk, an[0].val), PATH_MAX - 1);
		name_tail = name;

		// strip the first part of the name
		if ((sep = strchr(name, PATH_SEP))) {
			*sep = 0;
			name_tail = sep + 1;
		}

		if (!path_join(path, PATH_MAX, get_cstr(wk, dest), name_tail)) {
			return false;
		}
		dest = make_str(wk, path);
	}

	char path[PATH_MAX];
	if (!path_join(path, PATH_MAX, get_cstr(wk, current_project(wk)->cwd), get_cstr(wk, an[0].val))) {
		return false;
	}
	obj src = make_str(wk, path);

	struct obj_install_target *tgt;

	if (!(tgt = push_install_target(wk, src, dest, akw[kw_install_mode].val))) {
		return false;
	}

	tgt->exclude_directories = akw[kw_exclude_directories].val;
	tgt->exclude_files = akw[kw_exclude_files].val;
	tgt->type = install_target_subdir;
	return true;
}

struct install_man_ctx {
	obj mode;
	obj install_dir;
	obj locale;
	uint32_t err_node;
	bool default_install_dir;
};

static enum iteration_result
install_man_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct install_man_ctx *ctx = _ctx;

	obj src = *get_obj_file(wk, val);
	char man[PATH_MAX];
	if (!path_basename(man, PATH_MAX, get_cstr(wk, src))) {
		return ir_err;
	}
	size_t len = strlen(man);
	assert(len > 0);
	--len;

	if (len <= 1 || man[len - 1] != '.' || man[len] < '0' || man[len] > '9') {
		interp_error(wk, ctx->err_node, "invalid path to man page");
		return ir_err;
	}

	obj install_dir;
	if (ctx->default_install_dir) {
		install_dir = make_strf(wk, "%s/man%c", get_cstr(wk, ctx->install_dir), man[len]);
	} else {
		install_dir = ctx->install_dir;
	}

	const char *basename = man;
	if (ctx->locale) {
		char *dot = strchr(man, '.');
		assert(dot);
		if (str_startswith(&WKSTR(dot + 1), get_str(wk, ctx->locale))) {
			*dot = '\0';
			obj new_man = make_strf(wk, "%s.%c", man, man[len]);
			basename = get_cstr(wk, new_man);
		}
	}

	char path[PATH_MAX];
	if (!path_join(path, PATH_MAX, get_cstr(wk, install_dir), basename)) {
		return ir_err;
	}

	if (!push_install_target(wk, src, make_str(wk, path), ctx->mode)) {
		return ir_err;
	}
	return ir_cont;
}

bool
func_install_man(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB | tc_coercible_files }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_locale,
	};
	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_install_mode] = { "install_mode", tc_install_mode_kw },
		[kw_locale] = { "locale", obj_string },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	struct install_man_ctx ctx = {
		.err_node = an[0].node,
		.mode = akw[kw_install_mode].val,
		.install_dir = akw[kw_install_dir].val,
		.default_install_dir = false,
	};

	if (!akw[kw_install_dir].set) {
		obj mandir;
		get_option_value(wk, current_project(wk), "mandir", &mandir);

		if (akw[kw_locale].set) {
			char path[PATH_MAX];
			if (!path_join(path, PATH_MAX, get_cstr(wk, mandir), get_cstr(wk, akw[kw_locale].val))) {
				return false;
			}
			ctx.install_dir = make_str(wk, path);
			ctx.locale = akw[kw_locale].val;
		} else {
			ctx.install_dir = mandir;
		}

		ctx.default_install_dir = true;
	}

	obj manpages;
	if (!coerce_files(wk, an[0].node, an[0].val, &manpages)) {
		return false;
	}
	return obj_array_foreach(wk, manpages, &ctx, install_man_iter);
}

bool
func_install_symlink(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_tag,
		kw_pointing_to,
	};
	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string, .required = true },
		[kw_install_tag] = { "install_tag", obj_string }, // TODO
		[kw_pointing_to] = { "pointing_to", obj_string, .required = true },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	char path[PATH_MAX];
	if (!path_join(path, PATH_MAX, get_cstr(wk, akw[kw_install_dir].val), get_cstr(wk, an[0].val))) {
		return false;
	}

	struct obj_install_target *tgt;
	if (!(tgt = push_install_target(wk, akw[kw_pointing_to].val, make_str(wk, path), 0))) {
		return false;
	}

	tgt->type = install_target_symlink;
	return true;
}

struct install_emptydir_ctx {
	obj mode;
};

static enum iteration_result
install_emptydir_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct install_emptydir_ctx *ctx = _ctx;
	struct obj_install_target *tgt;

	if (!(tgt = push_install_target(wk, make_str(wk, ""), val, ctx->mode))) {
		return ir_err;
	}

	tgt->type = install_target_emptydir;
	return ir_cont;
}

bool
func_install_emptydir(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB | obj_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_mode,
		kw_install_tag,
	};
	struct args_kw akw[] = {
		[kw_install_mode] = { "install_mode", tc_install_mode_kw },
		[kw_install_tag] = { "install_tag", obj_string }, // TODO
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	struct install_emptydir_ctx ctx = {
		.mode = akw[kw_install_mode].val,
	};
	return obj_array_foreach(wk, an[0].val, &ctx, install_emptydir_iter);
}

struct install_data_rename_ctx {
	obj rename;
	obj mode;
	obj dest;
	uint32_t i;
	uint32_t node;
};

static enum iteration_result
install_data_rename_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct install_data_rename_ctx *ctx = _ctx;

	obj src = *get_obj_file(wk, val);
	obj dest;

	obj rename;
	obj_array_index(wk, ctx->rename, ctx->i, &rename);

	char d[PATH_MAX];
	if (!path_join(d, PATH_MAX, get_cstr(wk, ctx->dest), get_cstr(wk, rename))) {
		return false;
	}

	dest = make_str(wk, d);

	push_install_target(wk, src, dest, ctx->mode);

	++ctx->i;
	return ir_cont;
}

bool
func_install_data(struct workspace *wk, obj _, uint32_t args_node, obj *res)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB | tc_file | tc_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_install_tag,
		kw_rename,
		kw_sources,
		kw_preserve_path,
	};

	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_install_mode] = { "install_mode", tc_install_mode_kw },
		[kw_install_tag] = { "install_tag", obj_string }, // TODO
		[kw_rename] = { "rename", ARG_TYPE_ARRAY_OF | obj_string },
		[kw_sources] = { "sources", ARG_TYPE_ARRAY_OF | tc_file | tc_string },
		[kw_preserve_path] = { "preserve_path", obj_bool },
		0
	};

	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	if (akw[kw_rename].set && akw[kw_preserve_path].set) {
		interp_error(wk, akw[kw_preserve_path].node, "rename keyword conflicts with preserve_path");
		return false;
	}

	obj install_dir;
	if (akw[kw_install_dir].set) {
		install_dir = akw[kw_install_dir].val;
	} else {
		obj install_dir_base;
		get_option_value(wk, current_project(wk), "datadir", &install_dir_base);

		char buf[PATH_MAX];
		if (!path_join(buf, PATH_MAX, get_cstr(wk, install_dir_base), get_cstr(wk, current_project(wk)->cfg.name))) {
			return false;
		}

		install_dir = make_str(wk, buf);

	}

	obj sources = an[0].val;
	uint32_t err_node = an[0].node;

	if (akw[kw_sources].set) {
		obj_array_extend_nodup(wk, sources, akw[kw_sources].val);
		err_node = akw[kw_sources].node;
	}

	if (akw[kw_rename].set) {
		if (get_obj_array(wk, akw[kw_rename].val)->len !=
		    get_obj_array(wk, sources)->len) {
			interp_error(wk, akw[kw_rename].node, "number of elements in rename != number of sources");
			return false;
		}

		struct install_data_rename_ctx ctx = {
			.node = err_node,
			.mode = akw[kw_install_mode].val,
			.rename = akw[kw_rename].val,
			.dest = install_dir,
		};

		obj coerced;
		if (!coerce_files(wk, err_node, sources, &coerced)) {
			return false;
		}

		return obj_array_foreach(wk, coerced, &ctx, install_data_rename_iter);
	} else {
		bool preserve_path =
			akw[kw_preserve_path].set
			&& get_obj_bool(wk, akw[kw_preserve_path].val);

		return push_install_targets(wk, err_node, sources, install_dir,
			akw[kw_install_mode].val, preserve_path);
	}
}

bool
func_install_headers(struct workspace *wk, obj _, uint32_t args_node, obj *ret)
{
	struct args_norm an[] = { { ARG_TYPE_GLOB | tc_file | tc_string }, ARG_TYPE_NULL };
	enum kwargs {
		kw_install_dir,
		kw_install_mode,
		kw_subdir,
		kw_preserve_path,
	};
	struct args_kw akw[] = {
		[kw_install_dir] = { "install_dir", obj_string },
		[kw_install_mode] = { "install_mode", tc_install_mode_kw },
		[kw_subdir] = { "subdir", obj_string },
		[kw_preserve_path] = { "preserve_path", obj_bool },
		0
	};
	if (!interp_args(wk, args_node, an, NULL, akw)) {
		return false;
	}

	if (akw[kw_install_dir].set && akw[kw_subdir].set) {
		interp_error(wk, akw[kw_subdir].node, "subdir may not be set if install_dir is set");
		return false;
	}

	obj install_dir_base;
	if (akw[kw_install_dir].set) {
		install_dir_base = akw[kw_install_dir].val;
	} else {
		get_option_value(wk, current_project(wk), "includedir", &install_dir_base);
	}

	obj install_dir;
	if (akw[kw_subdir].set) {
		char buf[PATH_MAX];
		if (!path_join(buf, PATH_MAX, get_cstr(wk, install_dir_base), get_cstr(wk, akw[kw_subdir].val))) {
			return false;
		}

		install_dir = make_str(wk, buf);
	} else {
		install_dir = install_dir_base;
	}

	bool preserve_path =
		akw[kw_preserve_path].set
		&& get_obj_bool(wk, akw[kw_preserve_path].val);

	return push_install_targets(wk, an[0].node, an[0].val,
		install_dir, akw[kw_install_mode].val, preserve_path);
}