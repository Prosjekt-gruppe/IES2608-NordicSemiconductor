#ifndef SENSOR_PRINT_CTRL_H_
#define SENSOR_PRINT_CTRL_H_

#include <stdbool.h>

#if defined(CONFIG_APP_SENSOR_PRINT_SHELL)

bool sensor_print_ctrl_accel_enabled(void);
bool sensor_print_ctrl_batt_enabled(void);
void sensor_print_ctrl_set_accel_enabled(bool enabled);
void sensor_print_ctrl_set_batt_enabled(bool enabled);

#else

static inline bool sensor_print_ctrl_accel_enabled(void)
{
	return false;
}

static inline bool sensor_print_ctrl_batt_enabled(void)
{
	return false;
}

static inline void sensor_print_ctrl_set_accel_enabled(bool enabled)
{
	(void)enabled;
}

static inline void sensor_print_ctrl_set_batt_enabled(bool enabled)
{
	(void)enabled;
}

#endif

#endif /* SENSOR_PRINT_CTRL_H_ */
