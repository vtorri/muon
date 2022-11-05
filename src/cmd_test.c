/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "posix.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "args.h"
#include "backend/ninja.h"
#include "backend/output.h"
#include "cmd_test.h"
#include "error.h"
#include "formats/tap.h"
#include "functions/environment.h"
#include "lang/serial.h"
#include "log.h"
#include "platform/filesystem.h"
#include "platform/mem.h"
#include "platform/path.h"
#include "platform/run_cmd.h"
#include "platform/term.h"

enum test_result_status {
	test_result_status_running,
	test_result_status_ok,
	test_result_status_failed,
	test_result_status_timedout,
};

struct test_result {
	struct run_cmd_ctx cmd_ctx;
	struct obj_test *test;
	struct timespec start;
	float dur, timeout;
	enum test_result_status status;
	bool busy;
	struct {
		bool have;
		uint32_t pass, total;
	} subtests;
};

struct run_test_ctx {
	struct test_options *opts;
	obj proj_name;
	obj collected_tests;
	obj deps;
	uint32_t proj_i;
	struct {
		uint32_t test_i, test_len, error_count;
		uint32_t total_count, total_error_count, total_expect_fail_count;
		uint32_t total_skipped;
		uint32_t term_width;
		bool term;
		bool ran_tests;
	} stats;

	struct {
		obj env;
		obj exclude_suites;
		obj wrapper;
		float timeout_multiplier;
	} setup;

	struct darr test_results;

	struct test_result *jobs;
	uint32_t busy_jobs;
	bool serial;
};

/*
 * Test labeling and output
 */

static const char *
test_category_label(enum test_category cat)
{
	switch (cat) {
	case test_category_test:
		return "test";
	case test_category_benchmark:
		return "benchmark";
	default:
		UNREACHABLE_RETURN;
	}
}

static void
print_test_result(struct workspace *wk, const struct test_result *res)
{
	const char *name = get_cstr(wk, res->test->name);

	enum {
		status_failed,
		status_should_have_failed,
		status_ok,
		status_failed_ok,
		status_running,
		status_timedout,
	} status = status_ok;

	const char *status_msg[] = {
		[status_failed]             = "fail ",
		[status_should_have_failed] = "ok*  ",
		[status_ok]                 = "ok   ",
		[status_failed_ok]          = "fail*",
		[status_running]            = "start",
		[status_timedout]           = "timeout",
	};

	switch (res->status) {
	case test_result_status_running:
		status = status_running;
		break;
	case test_result_status_timedout:
		status = status_timedout;
		break;
	case test_result_status_failed:
		if (res->test->should_fail) {
			status = status_should_have_failed;
		} else {
			status = status_failed;
		}
		break;
	case test_result_status_ok:
		if (res->test->should_fail) {
			status = status_failed_ok;
		} else {
			status = status_ok;
		}
		break;
	}

	const char *suite_str = NULL;
	uint32_t suites_len = 0;
	if (res->test->suites) {
		suites_len = get_obj_array(wk, res->test->suites)->len;
		if (suites_len == 1) {
			obj s;
			obj_array_index(wk, res->test->suites, 0, &s);
			suite_str = get_cstr(wk, s);
		} else if (suites_len > 1) {
			obj s;
			obj_array_join(wk, true, res->test->suites, make_str(wk, ", "), &s);
			suite_str = get_cstr(wk, s);
		}
	}

	if (log_clr()) {
		uint32_t clr[] = {
			[status_failed] = 31,
			[status_should_have_failed] = 31,
			[status_ok] = 32,
			[status_failed_ok] = 33,
			[status_running] = 0,
			[status_timedout] = 31,
		};
		log_plain("[\033[%dm%s\033[0m]", clr[status], status_msg[status]);
	} else {
		log_plain("[%s]", status_msg[status]);
	}

	if (res->status == test_result_status_running) {
		log_plain("          ");
	} else {
		log_plain(" %6.2fs, ", res->dur);
	}

	if (res->subtests.have) {
		log_plain("%3d/%3d subtests, ", res->subtests.pass, res->subtests.total);
	}

	if (suite_str) {
		if (suites_len > 1) {
			log_plain("[%s]:", suite_str);
		} else {
			log_plain("%s:", suite_str);
		}
	}

	log_plain("%s", name);

	if (status == status_should_have_failed) {
		log_plain(" - passing test marked as should_fail");
	}
}

static void
print_test_progress(struct workspace *wk, struct run_test_ctx *ctx, const struct test_result *res, bool write_line)
{
	if (res->status != test_result_status_running) {
		++ctx->stats.total_count;
		++ctx->stats.test_i;
		if (res->status != test_result_status_ok) {
			++ctx->stats.total_error_count;
			++ctx->stats.error_count;
		}
	}

	if (!ctx->stats.term && !ctx->opts->verbosity) {
		if (res->status != test_result_status_running) {
			char c;
			switch (res->status) {
			case test_result_status_failed:
				c = 'E';
				break;
			case test_result_status_timedout:
				c = 'T';
				break;
			default:
				c = '.';
				break;
			}

			log_plain("%c", c);
		}
		return;
	} else if (ctx->stats.term) {
		log_plain("\r");
	}

	if (write_line && (ctx->opts->verbosity > 0 || res->test->verbose)) {
		print_test_result(wk, res);

		if (ctx->stats.term) {
			log_plain("\033[K");
		}

		log_plain("\n");
	}

	if (!ctx->stats.term) {
		return;
	}

	uint32_t i, pad = 2;

	char info[BUF_SIZE_4k];
	pad += snprintf(info, BUF_SIZE_4k, "%d/%d f: %d (%d) ", ctx->stats.test_i, ctx->stats.test_len, ctx->stats.error_count, ctx->busy_jobs);

	log_plain("%s[", info);
	uint32_t pct = (float)(ctx->stats.test_i) * (float)(ctx->stats.term_width - pad) / (float)ctx->stats.test_len;
	for (i = 0; i < ctx->stats.term_width - pad; ++i) {
		if (i < pct) {
			log_plain("=");
		} else if (i == pct) {
			log_plain(">");
		} else {
			log_plain(" ");
		}
	}
	log_plain("]");
}

/*
 * test setup / suites
 */

static bool
project_namespaced_name_matches(const char *name1, bool proj2_is_main,
	const struct str *proj2, const struct str *name2)
{
	struct str proj1 = { 0 };
	const char *sep;
	if ((sep = strchr(name1, ':'))) {
		proj1 = (struct str){ .s = name1, .len = sep - name1 };
		name1 = sep + 1;
	}

	if (proj1.len) {
		if (!str_eql(&proj1, proj2)) {
			return false;
		}
	} else {
		if (!proj2_is_main) {
			return false;
		}
	}

	return str_eql(&WKSTR(name1), name2);
}

struct test_in_suite_ctx {
	struct run_test_ctx *run_test_ctx;
	bool found;

	obj suite;
};

static enum iteration_result
test_in_suite_iter(struct workspace *wk, void *_ctx, obj s)
{
	struct test_in_suite_ctx *ctx = _ctx;
	uint32_t i;

	struct test_options *opts = ctx->run_test_ctx->opts;

	for (i = 0; i < opts->suites_len; ++i) {
		if (!project_namespaced_name_matches(opts->suites[i],
			ctx->run_test_ctx->proj_i == 0,
			get_str(wk, ctx->run_test_ctx->proj_name), get_str(wk, s))) {
			continue;
		}

		ctx->found = true;
		return ir_done;
	}

	return ir_cont;
}

static enum iteration_result
test_in_exclude_suites_exclude_suites_iter(struct workspace *wk, void *_ctx, obj exclude)
{
	struct test_in_suite_ctx *ctx = _ctx;

	if (project_namespaced_name_matches(get_cstr(wk, exclude),
		ctx->run_test_ctx->proj_i == 0,
		get_str(wk, ctx->run_test_ctx->proj_name), get_str(wk, ctx->suite))) {

		ctx->found = true;
		return ir_done;
	}

	return ir_cont;
}

static enum iteration_result
test_in_exclude_suites_iter(struct workspace *wk, void *_ctx, obj suite)
{
	struct test_in_suite_ctx *ctx = _ctx;
	ctx->suite = suite;

	obj_array_foreach(wk, ctx->run_test_ctx->setup.exclude_suites,
		ctx, test_in_exclude_suites_exclude_suites_iter);

	if (ctx->found) {
		return ir_done;
	}
	return ir_cont;
}

static bool
test_in_suite(struct workspace *wk, obj suites, struct run_test_ctx *run_test_ctx)
{
	struct test_in_suite_ctx ctx = {
		.run_test_ctx = run_test_ctx,
	};

	if (!run_test_ctx->opts->suites_len) {
		// no suites given on command line

		if (run_test_ctx->setup.exclude_suites) {
			obj_array_foreach(wk, suites, &ctx, test_in_exclude_suites_iter);
			return !ctx.found;
		} else {
			return true;
		}
	} else if (!suites) {
		// suites given on command line, but test has no suites
		return false;
	}

	obj_array_foreach(wk, suites, &ctx, test_in_suite_iter);
	return ctx.found;
}

struct find_test_setup_ctx {
	struct run_test_ctx *rtctx;
	bool found;
};

static enum iteration_result
find_test_setup_iter(struct workspace *wk, void *_ctx, obj arr)
{
	struct find_test_setup_ctx *ctx = _ctx;

	/* [name, env, exclude_suites, exe_wrapper, is_default, timeout_multiplier] */
	obj name, env, exclude_suites, exe_wrapper, is_default, timeout_multiplier;

	obj_array_index(wk, arr, 0, &name);
	obj_array_index(wk, arr, 1, &env);
	obj_array_index(wk, arr, 2, &exclude_suites);
	obj_array_index(wk, arr, 3, &exe_wrapper);
	obj_array_index(wk, arr, 4, &is_default);
	obj_array_index(wk, arr, 5, &timeout_multiplier);

	if (ctx->rtctx->opts->setup) {
		if (!project_namespaced_name_matches(ctx->rtctx->opts->setup,
			ctx->rtctx->proj_i == 0,
			get_str(wk, ctx->rtctx->proj_name), get_str(wk, name))) {
			return ir_cont;
		}
	} else if (!is_default || !get_obj_bool(wk, is_default)) {
		return ir_cont;
	}

	if (ctx->rtctx->opts->setup) {
		L("using test setup '%s'", ctx->rtctx->opts->setup);
	} else {
		L("using default test setup '%s'", get_cstr(wk, name));
	}

	ctx->rtctx->setup.env = env;
	ctx->rtctx->setup.exclude_suites = exclude_suites;
	ctx->rtctx->setup.wrapper = exe_wrapper;
	ctx->rtctx->setup.timeout_multiplier =
		timeout_multiplier ? get_obj_number(wk, timeout_multiplier)
			: 1.0f;
	ctx->found = true;
	return ir_done;
}

static enum iteration_result
find_test_setup_project_iter(struct workspace *wk, void *_ctx, obj project_name, obj arr)
{
	struct find_test_setup_ctx *ctx = _ctx;
	ctx->rtctx->proj_name = project_name;

	obj setups;
	obj_array_index(wk, arr, 1, &setups);

	if (!setups) {
		return ir_cont;
	}

	obj_array_foreach(wk, setups, ctx, find_test_setup_iter);

	if (ctx->found) {
		return ir_done;
	}

	++ctx->rtctx->proj_i;
	return ir_cont;
}

static bool
load_test_setup(struct workspace *wk, struct run_test_ctx *rtctx, obj tests_dict)
{
	bool res = false;
	struct find_test_setup_ctx ctx = {
		.rtctx = rtctx,
	};

	obj_dict_foreach(wk, tests_dict, &ctx, find_test_setup_project_iter);

	if (!ctx.found) {
		if (rtctx->opts->setup) {
			LOG_E("invalid test setup: '%s'", rtctx->opts->setup);
			goto ret;
		}
	}

	res = true;
ret:
	rtctx->proj_i = 0;
	return res;
}

/*
 * Test runner
 */

static void
calculate_test_duration(struct test_result *res)
{
	struct timespec end;

	if (clock_gettime(CLOCK_MONOTONIC, &end)) {
		LOG_E("error getting test end time: %s", strerror(errno));
		return;
	}

	double secs = (double)end.tv_sec - (double)res->start.tv_sec;
	double ns = ((secs * 1000000000.0) + end.tv_nsec) - res->start.tv_nsec;
	res->dur = ns / 1000000000.0;
}

static void
test_delay(void)
{
	struct timespec req = {
		.tv_nsec = 10000000,
	};
	nanosleep(&req, NULL);
}

static bool
check_test_result_tap(struct workspace *wk, struct run_test_ctx *ctx, struct test_result *res)
{
	struct tap_parse_result tap_result = { 0 };
	tap_parse(res->cmd_ctx.out.buf, res->cmd_ctx.out.len, &tap_result);

	res->subtests.have = true;
	res->subtests.pass = tap_result.pass + tap_result.skip;
	res->subtests.total = tap_result.total;

	return tap_result.all_ok && res->status == 0;
}

static bool
check_test_result_exitcode(struct workspace *wk, struct run_test_ctx *ctx, struct test_result *res)
{
	if (res->cmd_ctx.status == 0) {
		return true;
	} else if (res->cmd_ctx.status == 77) {
		++ctx->stats.total_skipped;
		return true;
	} else if (res->cmd_ctx.status == 99) {
		return false;
	} else {
		return false;
	}
}

static void
collect_tests(struct workspace *wk, struct run_test_ctx *ctx)
{
	uint32_t i;

	for (i = 0; i < ctx->opts->jobs; ++i) {
		if (!ctx->jobs[i].busy) {
			continue;
		}

		struct test_result *res = &ctx->jobs[i];
		calculate_test_duration(res);

		enum run_cmd_state state = run_cmd_collect(&res->cmd_ctx);

		if (state != run_cmd_running
		    && res->status == test_result_status_timedout) {
			run_cmd_ctx_destroy(&res->cmd_ctx);
			print_test_progress(wk, ctx, res, true);
			darr_push(&ctx->test_results, res);
			goto free_slot;
		}

		switch (state) {
		case run_cmd_running: {
			if (res->timeout > 0.0f && res->dur >= res->timeout) {
				bool force_kill = res->status == test_result_status_timedout
						  && (res->dur - res->timeout) > 0.5f;

				run_cmd_kill(&res->cmd_ctx, force_kill);

				if (!res->status) {
					res->status = test_result_status_timedout;
				}
			}

			continue;
		}
		case run_cmd_error:
			res->status = test_result_status_failed;
			print_test_progress(wk, ctx, res, true);
			darr_push(&ctx->test_results, res);
			break;
		case run_cmd_finished: {
			bool ok;

			switch (res->test->protocol) {
			case test_protocol_tap:
				ok = check_test_result_tap(wk, ctx, res);
				break;
			default:
				ok = check_test_result_exitcode(wk, ctx, res);
				break;
			}

			if (!ok && res->test->should_fail) {
				ok = true;
			}

			if (ok) {
				if (res->test->should_fail) {
					++ctx->stats.total_expect_fail_count;
				}

				res->status = test_result_status_ok;
				run_cmd_ctx_destroy(&res->cmd_ctx);
			} else {
				res->status = test_result_status_failed;
			}

			print_test_progress(wk, ctx, res, true);
			darr_push(&ctx->test_results, res);
			break;
		}
		}

free_slot:
		res->busy = false;
		--ctx->busy_jobs;

		if (!res->test->is_parallel) {
			ctx->serial = false;
		}

		if (ctx->opts->fail_fast && ctx->stats.total_error_count) {
			break;
		}
	}
}

static void
push_test(struct workspace *wk, struct run_test_ctx *ctx, struct obj_test *test,
	const char *argstr, uint32_t argc, const char *envstr, uint32_t envc)
{
	uint32_t i;
	while (true) {
		if (ctx->serial && ctx->busy_jobs) {
			goto cont;
		}

		if (test->is_parallel) {
			for (i = 0; i < ctx->opts->jobs; ++i) {
				if (!ctx->jobs[i].busy) {
					goto found_slot;
				}
			}
		} else {
			if (!ctx->busy_jobs) {
				ctx->serial = true;
				i = 0;
				goto found_slot;
			}
		}

cont:
		test_delay();
		collect_tests(wk, ctx);
	}
found_slot:
	++ctx->busy_jobs;

	struct test_result *res = &ctx->jobs[i];
	struct run_cmd_ctx *cmd_ctx = &res->cmd_ctx;

	*res = (struct test_result) {
		.busy = true,
		.test = test,
		.timeout = (test->timeout ? get_obj_number(wk, test->timeout) : 30.0f)
			   * ctx->setup.timeout_multiplier,

		.cmd_ctx = {
			.flags = run_cmd_ctx_flag_async,
		},
	};

	if (ctx->opts->verbosity > 1) {
		cmd_ctx->flags |= run_cmd_ctx_flag_dont_capture;
	}

	if (test->workdir) {
		cmd_ctx->chdir = get_cstr(wk, test->workdir);
	}

	if (clock_gettime(CLOCK_MONOTONIC, &res->start)) {
		LOG_E("error getting test start time: %s", strerror(errno));
	}

	print_test_progress(wk, ctx, res, ctx->serial);

	if (!run_cmd(cmd_ctx, argstr, argc, envstr, envc)) {
		res->busy = false;
		--ctx->busy_jobs;

		calculate_test_duration(res);
		res->status = test_result_status_failed;
		print_test_progress(wk, ctx, res, true);
		darr_push(&ctx->test_results, res);
	}
}

static enum iteration_result
run_test(struct workspace *wk, void *_ctx, obj t)
{
	struct run_test_ctx *ctx = _ctx;

	if (ctx->opts->fail_fast && ctx->stats.total_error_count) {
		return ir_done;
	}

	struct obj_test *test = get_obj_test(wk, t);

	obj cmdline;
	make_obj(wk, &cmdline, obj_array);

	if (ctx->setup.wrapper) {
		obj_array_extend(wk, cmdline, ctx->setup.wrapper);
	}

	obj_array_push(wk, cmdline, test->exe);

	if (test->args) {
		obj_array_extend_nodup(wk, cmdline, test->args);
	}

	const char *argstr, *envstr;
	uint32_t argc, envc;

	obj env;
	if (!environment_to_dict(wk, test->env, &env)) {
		UNREACHABLE;
	}

	if (ctx->setup.env) {
		obj setup_env;
		if (!environment_to_dict(wk, ctx->setup.env, &setup_env)) {
			UNREACHABLE;
		}

		obj merged;
		obj_dict_merge(wk, env, setup_env, &merged);
		env = merged;
	}

	join_args_argstr(wk, &argstr, &argc, cmdline);
	env_to_envstr(wk, &envstr, &envc, env);
	push_test(wk, ctx, test, argstr, argc, envstr, envc);
	return ir_cont;
}

/*
 * Test filtering and dispatch
 */

static enum iteration_result
gather_project_tests_iter(struct workspace *wk, void *_ctx, obj val)
{
	struct run_test_ctx *ctx = _ctx;
	struct obj_test *t = get_obj_test(wk, val);

	if (!(t->category == ctx->opts->cat
	      && test_in_suite(wk, t->suites, ctx))) {
		return ir_cont;
	}

	obj_array_push(wk, ctx->collected_tests, val);

	++ctx->stats.test_len;
	if (t->depends) {
		obj_array_extend_nodup(wk, ctx->deps, t->depends);
	}
	return ir_cont;
}

static int32_t
test_compare(struct workspace *wk, void *_ctx, obj t1_id, obj t2_id)
{
	struct obj_test *t1 = get_obj_test(wk, t1_id),
			*t2 = get_obj_test(wk, t2_id);

	int64_t p1 = t1->priority ? get_obj_number(wk, t1->priority) : 0,
		p2 = t2->priority ? get_obj_number(wk, t2->priority) : 0;

	if (p1 > p2) {
		return -1;
	} else if (p1 < p2) {
		return 1;
	} else if (t1->is_parallel && t2->is_parallel) {
		return 0;
	} else if (t1->is_parallel) {
		return 1;
	} else {
		return -1;
	}
}

static enum iteration_result
list_tests_iter(struct workspace *wk, void *_ctx, obj test)
{
	struct run_test_ctx *ctx = _ctx;
	struct obj_test *t = get_obj_test(wk, test);
	obj_printf(wk, "%#o", ctx->proj_name);
	if (t->suites) {
		obj_printf(wk, ":%o", t->suites);
	}
	obj_printf(wk, " - %#o\n", t->name);
	return ir_cont;
}

static enum iteration_result
run_project_tests(struct workspace *wk, void *_ctx, obj proj_name, obj arr)
{
	obj unfiltered_tests, tests;
	obj_array_index(wk, arr, 0, &unfiltered_tests);

	struct run_test_ctx *ctx = _ctx;
	make_obj(wk, &ctx->deps, obj_array);

	ctx->stats.test_i = 0;
	ctx->stats.error_count = 0;
	ctx->stats.test_len = 0;

	make_obj(wk, &ctx->collected_tests, obj_array);
	obj_array_foreach(wk, unfiltered_tests, ctx, gather_project_tests_iter);
	obj_array_sort(wk, NULL, ctx->collected_tests, test_compare, &tests);

	if (ctx->opts->list) {
		obj_array_foreach(wk, tests, ctx, list_tests_iter);
		return ir_cont;
	} else if (!ctx->stats.test_len) {
		return ir_cont;
	}

	if (get_obj_array(wk, ctx->deps)->len && !ctx->opts->no_rebuild) {
		obj ninja_cmd;
		obj_array_dedup(wk, ctx->deps, &ninja_cmd);
		if (ninja_run(wk, ninja_cmd, NULL, NULL) != 0) {
			LOG_W("failed to run ninja");
		}
	}

	LOG_I("running %ss for project '%s'", test_category_label(ctx->opts->cat), get_cstr(wk, proj_name));

	ctx->stats.ran_tests = true;

	ctx->proj_name = proj_name;

	if (!obj_array_foreach(wk, tests, ctx, run_test)) {
		return ir_err;
	}

	if (ctx->opts->fail_fast && ctx->stats.total_error_count) {
		return ir_done;
	}

	while (ctx->busy_jobs) {
		test_delay();
		collect_tests(wk, ctx);
	}

	log_plain("\n");

	++ctx->proj_i;

	return ir_cont;
}

bool
tests_run(struct test_options *opts)
{
	bool ret = false;
	SBUF_manual(tests_src);

	struct workspace wk;
	workspace_init_bare(&wk);

	if (!opts->jobs) {
		opts->jobs = 4;
	}

	{
		obj ninja_cmd;
		make_obj(&wk, &ninja_cmd, obj_array);
		obj_array_push(&wk, ninja_cmd, make_str(&wk, "build.ninja"));
		ninja_run(&wk, ninja_cmd, NULL, NULL);
	}

	path_join(NULL, &tests_src, output_path.private_dir, output_path.tests);

	FILE *f;
	f = fs_fopen(tests_src.buf, "r");

	sbuf_destroy(&tests_src);

	if (!f) {
		goto ret;
	}

	struct run_test_ctx ctx = {
		.opts = opts,
		.setup = { .timeout_multiplier = 1.0f, },
	};

	{
		int fd;
		if (!fs_fileno(log_file(), &fd)) {
			return false;
		}

		if (opts->display == test_display_auto) {
			opts->display = test_display_dots;
			if (term_isterm(fd)) {
				opts->display = test_display_bar;
			}
		}

		if (opts->display == test_display_bar) {
			uint32_t h;
			ctx.stats.term = true;
			term_winsize(fd, &h, &ctx.stats.term_width);
		} else if (opts->display == test_display_dots) {
			ctx.stats.term = false;
		} else {
			assert(false && "unreachable");
		}
	}

	darr_init(&ctx.test_results, 32, sizeof(struct test_result));
	ctx.jobs = z_calloc(ctx.opts->jobs, sizeof(struct test_result));

	obj tests_dict;
	if (!serial_load(&wk, &tests_dict, f)) {
		LOG_E("invalid data file");
		goto ret;
	} else if (!fs_fclose(f)) {
		goto ret;
	}

	if (!load_test_setup(&wk, &ctx, tests_dict)) {
		goto ret;
	}

	if (!obj_dict_foreach(&wk, tests_dict, &ctx, run_project_tests)) {
		goto ret;
	}

	if (opts->list) {
		ret = true;
		goto ret;
	}

	if (!ctx.stats.ran_tests) {
		LOG_I("no %ss defined", test_category_label(opts->cat));
	} else {
		LOG_I("finished %d %ss, %d expected fail, %d fail, %d skipped",
			ctx.stats.total_count,
			test_category_label(opts->cat),
			ctx.stats.total_expect_fail_count,
			ctx.stats.total_error_count,
			ctx.stats.total_skipped
			);
	}

	ret = true;
	uint32_t i;
	for (i = 0; i < ctx.test_results.len; ++i) {
		struct test_result *res = darr_get(&ctx.test_results, i);

		if (opts->print_summary
		    || (res->status == test_result_status_failed
			|| res->status == test_result_status_timedout)) {
			print_test_result(&wk, res);
			if (res->status == test_result_status_failed && res->cmd_ctx.err_msg) {
				log_plain(": %s", res->cmd_ctx.err_msg);
			}
			log_plain("\n");
		}

		if (res->status == test_result_status_failed) {
			if (res->test->should_fail) {
				ret = false;
			} else {
				ret = false;
				if (res->cmd_ctx.out.len) {
					log_plain("stdout: '%s'\n", res->cmd_ctx.out.buf);
				}
				if (res->cmd_ctx.err.len) {
					log_plain("stderr: '%s'\n", res->cmd_ctx.err.buf);
				}

			}

			run_cmd_ctx_destroy(&res->cmd_ctx);
		} else if (res->status == test_result_status_timedout) {
			ret = false;
		}
	}

ret:
	workspace_destroy_bare(&wk);
	darr_destroy(&ctx.test_results);
	z_free(ctx.jobs);
	return ret;
}
