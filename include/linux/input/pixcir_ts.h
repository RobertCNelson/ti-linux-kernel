#ifndef	_PIXCIR_I2C_TS_H
#define	_PIXCIR_I2C_TS_H

struct pixcir_ts_platform_data {
	int (*attb_read_val)(void);	/* deprecate this */
	unsigned int x_max;
	unsigned int y_max;
	int gpio_attb;		/* GPIO connected to ATTB line */
};

#endif
