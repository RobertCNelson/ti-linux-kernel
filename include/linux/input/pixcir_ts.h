#ifndef	_PIXCIR_I2C_TS_H
#define	_PIXCIR_I2C_TS_H

struct pixcir_ts_platform_data {
	int (*attb_read_val)(void);
	unsigned int x_size;	/* X axis resolution */
	unsigned int y_size;	/* Y axis resolution */
	int gpio_attb;		/* GPIO connected to ATTB line */
};

#endif
