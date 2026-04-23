#include "sensor_print_ctrl.h"

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

static bool accel_prints_enabled;
static bool batt_prints_enabled;

bool sensor_print_ctrl_accel_enabled(void)
{
	return accel_prints_enabled;
}

bool sensor_print_ctrl_batt_enabled(void)
{
	return batt_prints_enabled;
}

void sensor_print_ctrl_set_accel_enabled(bool enabled)
{
	accel_prints_enabled = enabled;
}

void sensor_print_ctrl_set_batt_enabled(bool enabled)
{
	batt_prints_enabled = enabled;
}

static const char *enabled_text(bool enabled)
{
	return enabled ? "on" : "off";
}

static int parse_print_state(const struct shell *shell, const char *text, bool *enabled)
{
	if ((strcmp(text, "on") == 0) || (strcmp(text, "enable") == 0) ||
	    (strcmp(text, "enabled") == 0) || (strcmp(text, "true") == 0) ||
	    (strcmp(text, "1") == 0)) {
		*enabled = true;
		return 0;
	}

	if ((strcmp(text, "off") == 0) || (strcmp(text, "disable") == 0) ||
	    (strcmp(text, "disabled") == 0) || (strcmp(text, "false") == 0) ||
	    (strcmp(text, "0") == 0)) {
		*enabled = false;
		return 0;
	}

	shell_error(shell, "Expected on/off, enable/disable, true/false, or 1/0");
	return -EINVAL;
}

static void print_status(const struct shell *shell)
{
	shell_print(shell, "Accelerometer printouts: %s", enabled_text(accel_prints_enabled));
	shell_print(shell, "Battery printouts:       %s", enabled_text(batt_prints_enabled));
}

static int cmd_sensor_print_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	print_status(shell);
	return 0;
}

static int cmd_sensor_print_accel(const struct shell *shell, size_t argc, char **argv)
{
	bool enabled;
	int err;

	if (!IS_ENABLED(CONFIG_APP_SENSOR_ACCEL_DEMO)) {
		shell_error(shell, "Accelerometer module is not enabled in this build");
		return -ENOTSUP;
	}

	if (argc == 1) {
		shell_print(shell, "Accelerometer printouts: %s",
			    enabled_text(accel_prints_enabled));
		return 0;
	}

	err = parse_print_state(shell, argv[1], &enabled);
	if (err != 0) {
		return err;
	}

	sensor_print_ctrl_set_accel_enabled(enabled);
	shell_print(shell, "Accelerometer printouts: %s", enabled_text(enabled));
	return 0;
}

static int cmd_sensor_print_batt(const struct shell *shell, size_t argc, char **argv)
{
	bool enabled;
	int err;

	if (!IS_ENABLED(CONFIG_APP_SENSOR_BATT_DEMO)) {
		shell_error(shell, "Battery module is not enabled in this build");
		return -ENOTSUP;
	}

	if (argc == 1) {
		shell_print(shell, "Battery printouts: %s", enabled_text(batt_prints_enabled));
		return 0;
	}

	err = parse_print_state(shell, argv[1], &enabled);
	if (err != 0) {
		return err;
	}

	sensor_print_ctrl_set_batt_enabled(enabled);
	shell_print(shell, "Battery printouts: %s", enabled_text(enabled));
	return 0;
}

static int cmd_sensor_print_all(const struct shell *shell, size_t argc, char **argv)
{
	bool enabled;
	int err;

	if (argc == 1) {
		print_status(shell);
		return 0;
	}

	err = parse_print_state(shell, argv[1], &enabled);
	if (err != 0) {
		return err;
	}

	if (IS_ENABLED(CONFIG_APP_SENSOR_ACCEL_DEMO)) {
		sensor_print_ctrl_set_accel_enabled(enabled);
	}

	if (IS_ENABLED(CONFIG_APP_SENSOR_BATT_DEMO)) {
		sensor_print_ctrl_set_batt_enabled(enabled);
	}

	print_status(shell);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sensor_print_subcommands,
	SHELL_CMD_ARG(accel, NULL, "Show or set accelerometer printouts: accel <on|off>",
		      cmd_sensor_print_accel, 1, 1),
	SHELL_CMD_ARG(batt, NULL, "Show or set battery printouts: batt <on|off>",
		      cmd_sensor_print_batt, 1, 1),
	SHELL_CMD_ARG(all, NULL, "Show or set all sensor printouts: all <on|off>",
		      cmd_sensor_print_all, 1, 1),
	SHELL_CMD(status, NULL, "Show sensor printout status", cmd_sensor_print_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sensor_print, &sensor_print_subcommands,
		   "Control battery and accelerometer serial printouts", NULL);
