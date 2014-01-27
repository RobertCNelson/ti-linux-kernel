/*
 * OV2659 camera sensors driver
 *
 * Copyright (C) 2013 Benoit Parrot <bparrot@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef OV2659_H_
#define OV2659_H_

/**
 * struct ov2659_platform_data - ov2659 driver platform data
 * @mclk_frequency: the sensor's master clock frequency in Hz
 * @gpio_pwdn:	    number of a GPIO connected to OV2659 PWDN pin
 * @gpio_reset:     number of a GPIO connected to OV2659 RESET pin
 *
 * If any of @gpio_pwdn or @gpio_reset are unused then they should be
 * set to a negative value. @mclk_frequency must always be specified.
 */
struct ov2659_platform_data {
	unsigned int mclk_frequency;
	int gpio_pwdn;
	int gpio_reset;
};
#endif /* OV2659_H_ */
