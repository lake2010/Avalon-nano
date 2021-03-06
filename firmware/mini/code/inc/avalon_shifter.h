/*
 * @brief shifter head file
 *
 * @note
 *
 * @par
 */
#ifndef __AVALON_SHIFTER_H_
#define __AVALON_SHIFTER_H_

void shifter_init(void);
int set_voltage(uint16_t vol);
uint16_t get_voltage(void);
uint8_t read_power_good(void);

#endif /* __AVALON_SHIFTER_H_ */
