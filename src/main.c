#include "posix.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "backend/ninja.h"
#include "compilers.h"
#include "external/libarchive.h"
#include "external/libcurl.h"
#include "external/libpkgconf.h"
#include "external/samu.h"
#include "formats/ini.h"
#include "functions/default/options.h"
#include "functions/default/setup.h"
#include "install.h"
#include "lang/eval.h"
#include "log.h"
#include "machine_file.h"
#include "opts.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"
#include "tests.h"
#include "version.h"
#include "wrap.h"

static bool
cmd_exe(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct {
		const char *capture;
		const char *const *cmd;
	} opts = { 0 };

	OPTSTART("c:") {
	case 'c':
		opts.capture = optarg;
		break;
	} OPTEND(argv[argi],
		" <cmd> [arg1[ arg2[...]]]",
		"  -c <file> - capture output to file\n",
		NULL, -1)

	if (argi >= argc) {
		LOG_E("missing command");
		return false;
	}

	opts.cmd = (const char *const *)&argv[argi];
	++argi;

	bool ret = false;
	struct run_cmd_ctx ctx = { 0 };

	if (!run_cmd(&ctx, opts.cmd[0], opts.cmd, NULL)) {
		LOG_E("failed to run command: %s", ctx.err);
		goto ret;
	}

	if (ctx.status != 0) {
		fputs(ctx.err, stderr);
		goto ret;
	}

	if (opts.capture) {
		ret = fs_write(opts.capture, (uint8_t *)ctx.out, strlen(ctx.out));
	} else {
		fputs(ctx.out, stdout);
		ret = true;
	}
ret:
	run_cmd_ctx_destroy(&ctx);
	return ret;
}

static const char *
get_filename_as_only_arg(uint32_t argc, uint32_t argi, char *const argv[])
{
	OPTSTART("") {
	} OPTEND(argv[argi], " <filename>", "", NULL, 1)

	return argv[argi];
}

static bool
cmd_check(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct {
		const char *filename;
		bool print_ast;
	} opts = { 0 };

	OPTSTART("p") {
	case 'p':
		opts.print_ast = true;
		break;
	} OPTEND(argv[argi],
		" <filename>",
		"  -p - print parsed ast\n",
		NULL, 1)

	opts.filename = argv[argi];

	bool ret = false;

	struct source src = { 0 };
	struct ast ast = { 0 };
	struct source_data sdata = { 0 };

	if (!fs_read_entire_file(opts.filename, &src)) {
		goto ret;
	}

	if (!parser_parse(&ast, &sdata, &src)) {
		goto ret;
	}

	if (opts.print_ast) {
		print_ast(&ast);
	}

	ret = true;
ret:
	fs_source_destroy(&src);
	ast_destroy(&ast);
	source_data_destroy(&sdata);
	return ret;
}

static bool
cmd_check_wrap(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct {
		const char *filename;
	} opts = { 0 };

	OPTSTART("") {
	} OPTEND(argv[argi],
		" <filename>",
		"",
		NULL, 1)

	opts.filename = argv[argi];

	bool ret = false;

	struct wrap wrap = { 0 };
	if (!wrap_parse(opts.filename, &wrap)) {
		goto ret;
	}

	ret = true;
ret:
	wrap_destroy(&wrap);
	return ret;
}

static bool
eval_internal(const char *filename, const char *argv0)
{
	bool ret = false;

	struct workspace wk;
	workspace_init(&wk);

	if (!workspace_setup_dirs(&wk, "dummy", argv0, false)) {
		goto ret;
	}

	wk.lang_mode = language_internal;

	struct source src = { 0 };

	if (!fs_read_entire_file(filename, &src)) {
		goto ret;
	}

	uint32_t id;
	make_project(&wk, &id, "dummy", wk.source_root, wk.build_root);

	uint32_t res;
	if (!eval(&wk, &src, &res)) {
		goto ret;
	}

	ret = true;
ret:
	fs_source_destroy(&src);
	workspace_destroy(&wk);
	return ret;
}

static bool
cmd_eval(uint32_t argc, uint32_t argi, char *const argv[])
{
	const char *filename;

	if (!(filename = get_filename_as_only_arg(argc, argi, argv))) {
		return false;
	}

	return eval_internal(filename, argv[0]);
}

static bool
cmd_repl(uint32_t argc, uint32_t argi, char *const argv[])
{
	char buf[2048];
	bool ret = false;
	struct source src = { .label = "repl", .src = buf };

	struct workspace wk;
	workspace_init(&wk);

	wk.lang_mode = language_internal;

	uint32_t id;
	make_project(&wk, &id, "dummy", wk.source_root, wk.build_root);

	fputs("> ", stderr);
	while (fgets(buf, 2048, stdin)) {
		src.len = strlen(buf);

		uint32_t res;
		if (eval(&wk, &src, &res)) {
			if (res) {
				obj_fprintf(&wk, stderr, "%o\n", res);
			}
		}
		fputs("> ", stderr);
	}

	ret = true;
	workspace_destroy(&wk);
	return ret;
}

static bool
cmd_internal(uint32_t argc, uint32_t argi, char *const argv[])
{
	static const struct command commands[] = {
		{ "eval", cmd_eval, "evaluate a file" },
		{ "exe", cmd_exe, "run an external command" },
		{ "repl", cmd_repl, "start a meson langauge repl" },
		0,
	};

	OPTSTART("") {
	} OPTEND(argv[argi], "", "", commands, -1);

	cmd_func cmd = NULL;;
	if (!find_cmd(commands, &cmd, argc, argi, argv, false)) {
		return false;
	}

	assert(cmd);
	return cmd(argc, argi, argv);
}

static bool
cmd_samu(uint32_t argc, uint32_t argi, char *const argv[])
{
	return muon_samu(argc - argi, (char **)&argv[argi]) == 0;
}

static bool
cmd_test(uint32_t argc, uint32_t argi, char *const argv[])
{
	OPTSTART("") {
	} OPTEND(argv[argi], " <build dir>", "", NULL, 1)

	return tests_run(argv[argi]);
}

static bool
cmd_install(uint32_t argc, uint32_t argi, char *const argv[])
{
	OPTSTART("") {
	} OPTEND(argv[argi], " <build dir>", "", NULL, 1)

	return install_run(argv[argi]);
}

static bool
cmd_setup(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct workspace wk;
	workspace_init(&wk);

	OPTSTART("D:m:") {
	case 'D':
		if (!parse_and_set_cmdline_option(&wk, optarg)) {
			return false;
		}
		break;
	case 'm':
		if (!machine_file_parse(&wk, optarg)) {
			return false;
		}
		break;
	} OPTEND(argv[argi],
		" <build dir>",
		"  -D <option>=<value> - set project options\n",
		NULL, 1)

	const char *build = argv[argi];
	++argi;

	if (!workspace_setup_dirs(&wk, build, argv[0], true)) {
		goto err;
	}

	uint32_t project_id;
	if (!eval_project(&wk, NULL, wk.source_root, wk.build_root, &project_id)) {
		goto err;
	}

	if (!ninja_write_all(&wk)) {
		goto err;
	}

	workspace_print_summaries(&wk);

	workspace_destroy(&wk);
	return true;
err:
	workspace_destroy(&wk);
	return false;
}

static bool
cmd_auto(uint32_t argc, uint32_t argi, char *const argv[])
{
	struct {
		const char *cfg;
	} opts = { .cfg = ".muon" };

	OPTSTART("c:rf") {
	case 'r':
		// HACK this should be redesigned as soon as more than one
		// function has command-line controllable behaviour.
		func_setup_flags |= func_setup_flag_force;
		func_setup_flags |= func_setup_flag_no_build;
		break;
	case 'f':
		// HACK this should be redesigned as soon as more than one
		// function has command-line controllable behaviour.
		func_setup_flags |= func_setup_flag_force;
		break;
	case 'c':
		opts.cfg = optarg;
		break;
	} OPTEND(argv[argi], "",
		"  -c config - load config alternate file (default: .muon)\n"
		"  -f - regenerate build file and rebuild\n"
		"  -r - regenerate build file only\n",
		NULL, 0)

	return eval_internal(opts.cfg, argv[0]);
}

static bool
cmd_version(uint32_t argc, uint32_t argi, char *const argv[])
{
	printf("muon v%s-%s\nenabled features:",
		muon_version.version, muon_version.vcs_tag);
	if (have_libcurl) {
		printf(" libcurl");
	}

	if (have_libpkgconf) {
		printf(" libpkgconf");
	}

	if (have_libarchive) {
		printf(" libarchive");
	}

	if (have_samu) {
		printf(" samu");
	}

	printf("\n");
	return true;
}

static bool
cmd_main(uint32_t argc, uint32_t argi, char *const argv[])
{
	static const struct command commands[] = {
		{ "auto", cmd_auto, "build the project with default options" },
		{ "check", cmd_check, "check if a meson file parses" },
		{ "check-wrap", cmd_check_wrap, "check if a meson wrap is valid" },
		{ "install", cmd_install, "install project" },
		{ "internal", cmd_internal, "internal subcommands" },
		{ "samu", cmd_samu, "run samurai" },
		{ "setup", cmd_setup, "setup a build directory" },
		{ "test", cmd_test, "run tests" },
		{ "version", cmd_version, "print version information" },
		{ 0 },
	};

	OPTSTART("vl") {
	case 'v':
		log_set_lvl(log_debug);
		break;
	case 'l':
		log_set_opts(log_show_source);
		break;
	} OPTEND(argv[0], "",
		"  -v - turn on debug messages\n"
		"  -l - show source locations for log messages\n",
		commands, -1)

	cmd_func cmd;
	if (!find_cmd(commands, &cmd, argc, argi, argv, true)) {
		return false;
	}

	if (cmd) {
		return cmd(argc, argi, argv);
	} else {
		/* subtract one from argi here since it gets incremented by 1
		 * implicitly during option parsing, but in this case there was
		 * no subcommand arg */
		return cmd_auto(argc, argi - 1, argv);
	}

	return true;
}

int
main(int argc, char *argv[])
{
	log_init();
	log_set_lvl(log_info);

	if (!path_init()) {
		return 1;
	}

	compilers_init();

	return cmd_main(argc, 0, argv) ? 0 : 1;
}
