#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "stringop.h"
#include "swinewm/config.h"
#include "swinewm/swaynag.h"
#include "swinewm/tree/output.h"
#include "swinewm/desktop/transaction.h"
#include "swinewm/platform.h"
#include "swinewm/server.h"

static const char usage[] =
	"Usage: swinewm [options] [command]\n"
	"\n"
	"  -h, --help             Show help message and quit.\n"
	"  -c, --config <config>  Specify a config file.\n"
	"  -C, --validate         Check the validity of the config file, then exit.\n"
	"  -d, --debug            Enables full logging, including debug information.\n"
	"  -v, --version          Show the version number and quit.\n"
	"  -V, --verbose          Enables more verbose logging.\n"
	"      --recover          Show windows a crashed swinewm left hidden, then exit.\n"
	"      --shell            Act as the Windows shell (not while Explorer runs).\n"
	"      --no-shell         Never act as the Windows shell.\n"
	"      --set-autostart    Start swinewm at logon, with Explorer as the shell\n"
	"                         behind it (recommended).\n"
	"      --unset-autostart  Do not start swinewm at logon.\n"
	"      --set-shell        Start swinewm instead of Explorer at logon.\n"
	"      --unset-shell      Start Explorer again at logon.\n"
	"\n";

int main(int argc, char **argv) {
	char *config_path = NULL;
	int shell_mode = -1;
	bool validate = false;
	bool recover = false;
	enum swine_log_importance verbosity = SWINE_ERROR;
	bool verbosity_given = false;

	for (int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			printf("%s", usage);
			return EXIT_SUCCESS;
		} else if ((strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) &&
				i + 1 < argc) {
			free(config_path);
			config_path = swine_strdup(argv[++i]);
		} else if (strcmp(arg, "-C") == 0 || strcmp(arg, "--validate") == 0) {
			validate = true;
		} else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--debug") == 0) {
			verbosity = SWINE_DEBUG;
			verbosity_given = true;
		} else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--verbose") == 0) {
			verbosity = SWINE_INFO;
			verbosity_given = true;
		} else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
			printf("swinewm version " SWINEWM_VERSION "\n");
			return EXIT_SUCCESS;
		} else if (strcmp(arg, "--recover") == 0) {
			recover = true;
		} else if (strcmp(arg, "--shell") == 0) {
			shell_mode = 1;
		} else if (strcmp(arg, "--no-shell") == 0) {
			shell_mode = 0;
		} else if (strcmp(arg, "--set-shell") == 0) {
			return platform_set_login_shell(true);
		} else if (strcmp(arg, "--unset-shell") == 0) {
			return platform_set_login_shell(false);
		} else if (strcmp(arg, "--set-autostart") == 0) {
			return platform_set_autostart(true);
		} else if (strcmp(arg, "--unset-autostart") == 0) {
			return platform_set_autostart(false);
		} else {
			fprintf(stderr, "%s", usage);
			return EXIT_FAILURE;
		}
	}
	// Started by Windows at logon there are no options: SWINEWM_LOG
	// (error, info or debug) sets the log level instead.
	const char *env_level = getenv("SWINEWM_LOG");
	if (!verbosity_given && env_level) {
		verbosity_given = swine_log_parse_importance(env_level, &verbosity);
	}
	swine_log_init(verbosity);
	platform_set_shell_mode(shell_mode);
	if (!recover) {
		platform_setup_environment();
	}
	if (!verbosity_given && platform_is_shell()) {
		// The log file is all there is to go on after a problem at logon.
		swine_log_init(SWINE_INFO);
	}
	swine_log(SWINE_INFO, "swinewm %s%s", SWINEWM_VERSION,
		platform_is_shell() ? ", as the Windows shell" : "");

	if (recover) {
		int n = platform_recover_windows();
		printf("Restored %d window(s)\n", n);
		return EXIT_SUCCESS;
	}

	if (!config_path) {
		config_path = find_config_file("config");
	}

	server_init();

	if (validate) {
		bool ok = load_main_config(config_path, false, true);
		printf("Configuration %s\n", ok ? "OK" : "has errors");
		free(config_path);
		free_config(config);
		server_fini();
		return ok ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (!load_main_config(config_path, false, false)) {
		swine_log(SWINE_ERROR, "Error(s) loading config; continuing");
	}
	free(config_path);

	// Outputs and existing windows are discovered here, after the config so
	// that gaps and layouts apply to the first workspaces.
	if (!platform_init()) {
		swine_log(SWINE_ERROR, "Unable to initialize the platform layer");
		free_config(config);
		server_fini();
		return EXIT_FAILURE;
	}
	config->active = true;
	config_run_deferred();
	output_apply_configs();
	platform_config_changed();
	swaynag_show_config_errors();
	transaction_commit_dirty();

	swine_log(SWINE_INFO, "Running swinewm " SWINEWM_VERSION);
	int status = platform_run();

	swine_log(SWINE_INFO, "Shutting down swinewm");
	platform_finish();
	server_fini();
	free_config(config);
	config = NULL;
	return status;
}
