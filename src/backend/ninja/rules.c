#include "posix.h"

#include <string.h>

#include "args.h"
#include "backend/ninja/rules.h"
#include "backend/output.h"
#include "error.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/path.h"

struct write_compiler_rule_ctx {
	FILE *out;
	struct project *proj;
	obj rule_prefix_arr;
	obj compiler_rule_arr;
};

static enum iteration_result
write_compiler_rule_iter(struct workspace *wk, void *_ctx, enum compiler_language l, obj comp_id)
{
	struct write_compiler_rule_ctx *ctx = _ctx;
	struct obj_compiler *comp = get_obj_compiler(wk, comp_id);

	enum compiler_type t = comp->type;

	const char *deps = NULL;
	switch (compilers[t].deps) {
	case compiler_deps_none:
		break;
	case compiler_deps_gcc:
		deps = "gcc";
		break;
	case compiler_deps_msvc:
		deps = "msvc";
		break;
	}

	obj args, link_args;
	make_obj(wk, &args, obj_array);
	obj_array_extend(wk, args, comp->cmd_arr);
	obj_array_push(wk, args, make_str(wk, "$ARGS"));

	obj_array_dup(wk, args, &link_args);

	if (compilers[t].deps) {
		push_args(wk, args, compilers[t].args.deps("$out", "$DEPFILE"));
	}
	push_args(wk, args, compilers[t].args.output("$out"));
	push_args(wk, args, compilers[t].args.compile_only());
	obj_array_push(wk, args, make_str(wk, "$in"));

	obj compile_command = join_args_plain(wk, args);

	obj compiler_rule = make_strf(wk, "%s%s_COMPILER",
		get_cstr(wk, ctx->proj->rule_prefix),
		compiler_language_to_s(l));
	obj_array_push(wk, ctx->compiler_rule_arr, compiler_rule);

	fprintf(ctx->out, "rule %s\n"
		" command = %s\n",
		get_cstr(wk, compiler_rule),
		get_cstr(wk, compile_command));
	if (compilers[t].deps) {
		fprintf(ctx->out,
			" deps = %s\n"
			" depfile = $DEPFILE_UNQUOTED\n",
			deps);
	}
	fprintf(ctx->out,
		" description = compiling %s $out\n\n",
		compiler_language_to_s(l));

	push_args(wk, link_args, compilers[t].args.output("$out"));
	obj_array_push(wk, link_args, make_str(wk, "$in"));
	obj_array_push(wk, link_args, make_str(wk, "$LINK_ARGS"));

	obj link_command = join_args_plain(wk, link_args);

	fprintf(ctx->out, "rule %s%s_LINKER\n"
		" command = %s\n"
		" description = linking $out\n\n",
		get_cstr(wk, ctx->proj->rule_prefix),
		compiler_language_to_s(l),
		get_cstr(wk, link_command));

	return ir_cont;
}

static enum iteration_result
add_global_opts_set_from_env_iter(struct workspace *wk, void *_ctx, obj key, obj val)
{
	obj regen_args = *(obj *)_ctx;

	struct obj_option *o = get_obj_option(wk, val);
	if (o->source != option_value_source_environment) {
		return ir_cont;
	}


	// NOTE: This only handles options of type str or [str], which is okay since
	// the only options that can be set from the environment are of this
	// type.
	// TODO: The current implementation of array stringification would
	// choke on spaces, etc.

	const char *sval;
	switch (get_obj_type(wk, o->val)) {
	case obj_string:
		sval = get_cstr(wk, o->val);
		break;
	case obj_array: {
		obj joined;
		obj_array_join(wk, true, o->val, make_str(wk, ","), &joined);
		sval = get_cstr(wk, joined);
		break;
	}
	default:
		UNREACHABLE;
	}

	obj_array_push(wk, regen_args, make_strf(wk, "-D%s=%s", get_cstr(wk, o->name), sval));
	return ir_cont;
}

bool
ninja_write_rules(FILE *out, struct workspace *wk, struct project *main_proj,
	bool need_phony,
	obj compiler_rule_arr)
{
	fprintf(
		out,
		"# This is the build file for project \"%s\"\n"
		"# It is autogenerated by the muon build system.\n"
		"ninja_required_version = 1.7.1\n\n",
		get_cstr(wk, main_proj->cfg.name)
		);

	fprintf(out,
		"rule STATIC_LINKER\n"
		" command = rm -f $out && ar $LINK_ARGS $out $in\n"
		" description = linking static $out\n"
		"\n"
		"rule CUSTOM_COMMAND\n"
		" command = $COMMAND\n"
		" description = $DESCRIPTION\n"
		" restat = 1\n"
		"\n"
		);

	obj regen_args;
	make_obj(wk, &regen_args, obj_array);

	obj_array_push(wk, regen_args, make_str(wk, wk->argv0));
	obj_array_push(wk, regen_args, make_str(wk, "-C"));
	obj_array_push(wk, regen_args, make_str(wk, wk->source_root));
	obj_array_push(wk, regen_args, make_str(wk, "setup"));

	SBUF(compiler_check_cache_path);
	path_join(wk, &compiler_check_cache_path,
		wk->muon_private, output_path.compiler_check_cache);

	obj_array_push(wk, regen_args, make_str(wk, "-c"));
	obj_array_push(wk, regen_args, make_str(wk, compiler_check_cache_path.buf));

	obj_dict_foreach(wk, wk->global_opts, &regen_args, add_global_opts_set_from_env_iter);

	uint32_t i;
	for (i = 0; i < wk->original_commandline.argc; ++i) {
		obj_array_push(wk, regen_args,
			make_str(wk, wk->original_commandline.argv[i]));
	}

	obj regen_cmd = join_args_shell(wk, regen_args);

	fprintf(out,
		"rule REGENERATE_BUILD\n"
		" command = %s", get_cstr(wk, regen_cmd));

	fputs("\n description = Regenerating build files.\n"
		" generator = 1\n"
		"\n", out);

	fprintf(out,
		"build build.ninja: REGENERATE_BUILD %s\n"
		" pool = console\n\n",
		get_cstr(wk, join_args_ninja(wk, wk->sources))
		);

	if (need_phony) {
		fprintf(out, "build build_always_stale: phony\n\n");
	}

	obj rule_prefix_arr;
	make_obj(wk, &rule_prefix_arr, obj_array);
	for (i = 0; i < wk->projects.len; ++i) {
		struct project *proj = darr_get(&wk->projects, i);
		if (proj->not_ok) {
			continue;
		}

		struct write_compiler_rule_ctx ctx = {
			.proj = proj,
			.out = out,
			.rule_prefix_arr = rule_prefix_arr,
			.compiler_rule_arr = compiler_rule_arr,
		};

		{ // determine project rule prefix
			const char *proj_name = get_cstr(wk, proj->cfg.name);
			char buf[BUF_SIZE_1k] = { 0 }, *p;
			strncpy(buf, proj_name, BUF_SIZE_1k - 1);
			for (p = buf; *p; ++p) {
				if (*p == '_'
				    || ('a' <= *p && *p <= 'z')
				    || ('A' <= *p && *p <= 'Z')
				    || ('0' <= *p && *p <= '9')) {
					continue;
				}

				*p = '_';
			}

			proj->rule_prefix = make_strf(wk, "%s_", buf);
			uint32_t x = 1;
			while (obj_array_in(wk, rule_prefix_arr, proj->rule_prefix)) {
				proj->rule_prefix = make_strf(wk, "%s%d_", buf, x);
				++x;
			}
			obj_array_push(wk, rule_prefix_arr, proj->rule_prefix);
		}

		if (!obj_dict_foreach(wk, proj->compilers, &ctx, write_compiler_rule_iter)) {
			return false;
		}
	}

	fprintf(out, "# targets\n\n");
	return true;
}
