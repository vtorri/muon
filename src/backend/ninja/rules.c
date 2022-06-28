#include "posix.h"

#include <string.h>

#include "args.h"
#include "backend/ninja/rules.h"
#include "backend/output.h"
#include "lang/workspace.h"
#include "log.h"
#include "platform/path.h"

struct write_compiler_rule_ctx {
	FILE *out;
	struct project *proj;
	obj rule_prefix_arr;
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

	obj args;
	make_obj(wk, &args, obj_array);
	obj_array_push(wk, args, comp->name);
	obj_array_push(wk, args, make_str(wk, "$ARGS"));
	if (compilers[t].deps) {
		push_args(wk, args, compilers[t].args.deps("$out", "$DEPFILE"));
	}
	push_args(wk, args, compilers[t].args.output("$out"));
	push_args(wk, args, compilers[t].args.compile_only());
	obj_array_push(wk, args, make_str(wk, "$in"));
	obj command = join_args_plain(wk, args);

	fprintf(ctx->out, "rule %s%s_COMPILER\n"
		" command = %s\n",
		get_cstr(wk, ctx->proj->rule_prefix),
		compiler_language_to_s(l),
		get_cstr(wk, command));
	if (compilers[t].deps) {
		fprintf(ctx->out,
			" deps = %s\n"
			" depfile = $DEPFILE_UNQUOTED\n",
			deps);
	}
	fprintf(ctx->out,
		" description = compiling %s $out\n\n",
		compiler_language_to_s(l));

	fprintf(ctx->out, "rule %s%s_LINKER\n"
		" command = %s $ARGS -o $out $in $LINK_ARGS\n"
		" description = linking $out\n\n",
		get_cstr(wk, ctx->proj->rule_prefix),
		compiler_language_to_s(l),
		get_cstr(wk, comp->name));

	return ir_cont;
}

bool
ninja_write_rules(FILE *out, struct workspace *wk, struct project *main_proj, bool need_phony)
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
		struct write_compiler_rule_ctx ctx = {
			.proj = proj,
			.out = out,
			.rule_prefix_arr = rule_prefix_arr,
		};


		{ // determine project rule prefix
			const char *proj_name = get_cstr(wk, proj->cfg.name);
			char buf[PATH_MAX] = { 0 }, *p;
			strncpy(buf, proj_name, PATH_MAX - 1);
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
