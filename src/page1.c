/*
 * STC1000+, improved firmware and Arduino based firmware uploader for the STC-1000 dual stage thermostat.
 *
 * Copyright 2014 Mats Staffansson
 *
 * This file is part of STC1000+.
 *
 * STC1000+ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * STC1000+ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with STC1000+.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define __16f1828
#include "pic14/pic16f1828.h"
#include "stc1000p.h"

#define reset() { __asm RESET __endasm; }

/* Helpful defines to handle buttons */
#define BTN_PWR			0x88
#define BTN_S			0x44
#define BTN_UP			0x22
#define BTN_DOWN		0x11

#define BTN_IDLE(btn)		((_buttons & (btn)) == 0x00)
#define BTN_PRESSED(btn)	((_buttons & (btn)) == ((btn) & 0x0f))
#define BTN_HELD(btn)		((_buttons & (btn)) == (btn))
#define BTN_RELEASED(btn)	((_buttons & (btn)) == ((btn) & 0xf0))

/* Help to convert menu item number and config item number to an EEPROM config address */
#define ITEM_TO_ADDRESS(mi, ci)	((mi)*19 + (ci))
//#define ITEM_TO_ADDRESS(mi, ci)	(((mi)<<4) + ((mi)<<1) + (mi) + (ci))


/* Reimplement menu with x macros */

#define SET_MENU_DATA(_) \
    _(hy, 		LED_h, 		LED_y, 		LED_OFF, 	0, 				TEMP_HYST_1_MAX) \
    _(hy2, 		LED_h, 		LED_y, 		LED_2, 		0, 				TEMP_HYST_2_MAX) \
    _(tc, 		LED_t, 		LED_c, 		LED_OFF, 	TEMP_CORR_MIN, 	TEMP_CORR_MAX) \
    _(tc2, 		LED_t, 		LED_c, 		LED_2, 		TEMP_CORR_MIN,	TEMP_CORR_MAX) \
    _(SP, 		LED_S, 		LED_P, 		LED_OFF, 	TEMP_MIN,		TEMP_MAX) \
    _(St, 		LED_S, 		LED_t, 		LED_OFF, 	0,				8) \
    _(dh, 		LED_d, 		LED_h, 		LED_OFF, 	0,				999) \
    _(cd, 		LED_c, 		LED_d, 		LED_OFF, 	0,				60) \
    _(hd, 		LED_h, 		LED_d, 		LED_OFF, 	0,				60) \
    _(rP, 		LED_r, 		LED_P, 		LED_OFF, 	0,				1) \
    _(Pb, 		LED_P, 		LED_b, 		LED_OFF, 	0,				1) \
    _(rn, 		LED_r, 		LED_n, 		LED_OFF, 	0,				6) \

#define TO_STRUCT(name, led10ch, led1ch, led01ch, minv, maxv) \
    { led10ch, led1ch, led01ch, minv, maxv},
    
#define TO_ENUM(name, led10ch, led1ch, led01ch, minv, maxv) \
    name,

enum set_menu_enum {
    SET_MENU_DATA(TO_ENUM)
};

#define EEADR(name)    (114 + ((name)<<2))

struct s_setmenu {
    unsigned char led_c_10;
    unsigned char led_c_1;
    unsigned char led_c_01;
//    unsigned char flags;
    int min;
    int max;
};

static const struct s_setmenu setmenu[] = {
	SET_MENU_DATA(TO_STRUCT)
};

#define SET_MENU_SIZE	(sizeof(setmenu)/sizeof(setmenu[0]))

/* End reimplement menu with x macros */

/* Helpers to constrain user input  */
//#define RANGE(x,min,max)	(((x)>(max))?(min):((x)<(min))?(max):(x));

int RANGE(int x, int min, int max){
	if(x>max)
		return min;
	if(x<min)
		return max;
	return x;
}

static int check_config_value(int config_value, unsigned char config_address){
	if(config_address < EEADR_PROFILE_SETPOINT(6,0)){
		if(config_address & 0x1){
			config_value = RANGE(config_value, 0, 999);
		} else {
			config_value = RANGE(config_value, TEMP_MIN, TEMP_MAX);
		}
	} else {
		config_address -= 114;
		config_value = RANGE(config_value, setmenu[config_address].min, setmenu[config_address].max);
	}
	return config_value;
}

static void prx_to_led(unsigned char run_mode, unsigned char is_menu){
	led_e.e_negative = 1;
	led_e.e_deg = 1;
	led_e.e_c = 1;
	if(run_mode<6){
		led_10.raw = LED_P;
		led_1.raw = LED_r;
		led_01.raw = led_lookup[run_mode];
	} else {
		if(is_menu){
			led_10.raw = LED_S;
			led_1.raw = LED_e;
			led_01.raw = LED_t;
		} else {
			led_10.raw = LED_t;
			led_1.raw = LED_h;
			led_01.raw = LED_OFF;
		}
	}
}

#define run_mode_to_led(x)	prx_to_led(x,0)
#define menu_to_led(x)	prx_to_led(x,1)

/* States for the menu FSM */
enum menu_states {
	state_idle = 0,

	state_power_down_wait,

	state_show_version,

	state_show_sp,

	state_show_profile,
	state_show_profile_st,
	state_show_profile_dh,

	state_show_menu_item,
	state_set_menu_item,
	state_show_config_item,
	state_set_config_item,
	state_show_config_value,
	state_set_config_value,

	state_up_pressed,
	state_down_pressed,
};

/* Due to a fault in SDCC, static local variables are not initialized
 * properly, so the variables below were moved from button_menu_fsm()
 * and made global.
 */
static unsigned char state=state_idle;
static unsigned char menu_item=0, config_item=0, countdown=0;
static int config_value;
static unsigned char _buttons = 0;

/* This is the button input and menu handling function.
 * arguments: none
 * returns: nothing
 */
void button_menu_fsm(){
	{
		unsigned char trisc, latb;

		// Disable interrups while reading buttons
		GIE = 0;

		// Save registers that interferes with LED's
		latb = LATB;
		trisc = TRISC;

		LATB = 0b00000000; // Turn off LED's
		TRISC = 0b11011000; // Enable input for buttons

		_buttons = (_buttons << 1) | RC7; // pwr
		_buttons = (_buttons << 1) | RC4; // s
		_buttons = (_buttons << 1) | RC6; // up
		_buttons = (_buttons << 1) | RC3; // down

		// Restore registers
		LATB = latb;
		TRISC = trisc;

		// Reenable interrups
		GIE = 1;
	}

	if(countdown){
		countdown--;
	}

	switch(state){
	case state_idle:
		if(BTN_PRESSED(BTN_PWR)){
			countdown = 27; // 3 sec
			state = state_power_down_wait;
		} else if(_buttons && eeprom_read_config(EEADR_POWER_ON)){
			if (BTN_PRESSED(BTN_UP | BTN_DOWN)) {
				state = state_show_version;
			} else if (BTN_PRESSED(BTN_UP)) {
				state = state_show_sp;
			} else if (BTN_PRESSED(BTN_DOWN)) {
				countdown = 13; // 1.5 sec
				state = state_show_profile;
			} else if (BTN_RELEASED(BTN_S)) {
				state = state_show_menu_item;
			}
		}
		break;

	case state_show_version:
		int_to_led(STC1000P_VERSION);
		led_10.decimal = 0;
		led_e.e_deg = 1;
		led_e.e_c = 1;
		if(!BTN_HELD(BTN_UP | BTN_DOWN)){
			state=state_idle;
		}
		break;

	case state_power_down_wait:
		if(countdown==0){
			unsigned char pwr_on = eeprom_read_config(EEADR_POWER_ON);
			eeprom_write_config(EEADR_POWER_ON, !pwr_on);
			if(pwr_on){
				LATA0 = 0;
				LATA4 = 0;
				LATA5 = 0;
				TMR4ON = 0;
				TMR4IF = 0;
			} else {
				reset();
			}
			state = state_idle;
		} else if(!BTN_HELD(BTN_PWR)){
//			if((unsigned char)eeprom_read_config(EEADR_2ND_PROBE)){
					TX9 = !TX9;
//			}
			state = state_idle;
		}
		break;

	case state_show_sp:
		temperature_to_led(eeprom_read_config(EEADR_SETPOINT));
		if(!BTN_HELD(BTN_UP)){
			state=state_idle;
		}
		break;

	case state_show_profile:
		{
			unsigned char run_mode = eeprom_read_config(EEADR_RUN_MODE);
			run_mode_to_led(run_mode);
			if(run_mode<6 && countdown==0){
				countdown=17;
				state = state_show_profile_st;
			}
			if(!BTN_HELD(BTN_DOWN)){
				state=state_idle;
			}
		}
		break;
	case state_show_profile_st:
		int_to_led(eeprom_read_config(EEADR_CURRENT_STEP));
		if(countdown==0){
			countdown=13;
			state = state_show_profile_dh;
		}
		if(!BTN_HELD(BTN_DOWN)){
			state=state_idle;
		}
		break;
	case state_show_profile_dh:
		int_to_led(eeprom_read_config(EEADR_CURRENT_STEP_DURATION));
		if(countdown==0){
			countdown=13;
			state = state_show_profile;
		}
		if(!BTN_HELD(BTN_DOWN)){
			state=state_idle;
		}
		break;

	case state_show_menu_item:
		menu_to_led(menu_item);
		countdown = 110;
		state = state_set_menu_item;
		break;
	case state_set_menu_item:
		if(countdown==0 || BTN_RELEASED(BTN_PWR)){
			state=state_idle;
		} else if(BTN_RELEASED(BTN_UP)){
			menu_item = (menu_item >= 6) ? 0 : menu_item+1;
			state = state_show_menu_item;
		} else if(BTN_RELEASED(BTN_DOWN)){
			menu_item = (menu_item == 0) ? 6 : menu_item-1;
			state = state_show_menu_item;
		} else if(BTN_RELEASED(BTN_S)){
			config_item = 0;
			state = state_show_config_item;
		}
		break;
	case state_show_config_item:
		led_e.e_negative = 1;
		led_e.e_deg = 1;
		led_e.e_c = 1;
		if(menu_item < 6){
			if(config_item & 0x1) {
				led_10.raw = LED_d;
				led_1.raw = LED_h;
			} else {
				led_10.raw = LED_S;
				led_1.raw = LED_P;
			}
			led_01.raw = led_lookup[config_item >> 1];
		} else /* if(menu_item == 6) */{
			led_10.raw = setmenu[config_item].led_c_10;
			led_1.raw = setmenu[config_item].led_c_1;
			led_01.raw = setmenu[config_item].led_c_01;
		}
		countdown = 110;
		state = state_set_config_item;
		break;
	case state_set_config_item:
		if(countdown==0){
			state=state_idle;
		} else if(BTN_RELEASED(BTN_PWR)){
			state = state_show_menu_item;
		} else if(BTN_RELEASED(BTN_UP)){
			if(menu_item < 6){
				config_item = (config_item >= 18) ? 0 : config_item+1;
			} else {
				config_item = (config_item >= SET_MENU_SIZE-1) ? 0 : config_item+1;
				if(config_item == St && (unsigned char)eeprom_read_config(EEADR_RUN_MODE) >= 6){
					config_item += 2;
				}
			}
			state = state_show_config_item;
		} else if(BTN_RELEASED(BTN_DOWN)){
			if(menu_item < 6){
				config_item = (config_item == 0) ? 18 : config_item-1;
			} else {
				config_item = (config_item == 0) ? SET_MENU_SIZE-1 : config_item-1;
				if(config_item == dh && (unsigned char)eeprom_read_config(EEADR_RUN_MODE) >= 6){
					config_item -= 2;
				}
			}
			state = state_show_config_item;
		} else if(BTN_RELEASED(BTN_S)){
			unsigned char adr = ITEM_TO_ADDRESS(menu_item, config_item);
			config_value = eeprom_read_config(adr);
			config_value = check_config_value(config_value, adr);
			countdown = 110;
			state = state_show_config_value;
		}
		break;
	case state_show_config_value:
		if(menu_item < 6){
			if(config_item & 0x1){
				int_to_led(config_value);
			} else {
				temperature_to_led(config_value);
			}
		} else if(menu_item == 6){
			if(config_item <= SP){
				temperature_to_led(config_value);
			} else if (config_item < SET_MENU_SIZE-1){
				int_to_led(config_value);
			} else {
				run_mode_to_led(config_value);
			}
		}
		countdown = 110;
		state = state_set_config_value;
		break;
	case state_set_config_value:
		{
			unsigned char adr = ITEM_TO_ADDRESS(menu_item, config_item);

			if(countdown==0){
				state=state_idle;
			} else if(BTN_RELEASED(BTN_PWR)){
				state = state_show_config_item;
			} else if(BTN_RELEASED(BTN_UP) || BTN_HELD(BTN_UP)) {
				config_value = ((config_value >= 1000) || (config_value < -1000)) ? (config_value + 10) : (config_value + 1);
				config_value = check_config_value(config_value, adr);
				if(PR6 > 30){
					PR6-=8;
				}
				state = state_show_config_value;
			} else if(BTN_RELEASED(BTN_DOWN) || BTN_HELD(BTN_DOWN)) {
				config_value = ((config_value > 1000) || (config_value <= -1000)) ? (config_value - 10) : (config_value - 1);
				config_value = check_config_value(config_value, adr);
				if(PR6 > 30){
					PR6-=8;
				}
				state = state_show_config_value;
			} else if(BTN_RELEASED(BTN_S)){
				if(menu_item == 6){
					if(config_item == rn){
						// When setting runmode, clear current step & duration
						eeprom_write_config(EEADR_CURRENT_STEP, 0);
						eeprom_write_config(EEADR_CURRENT_STEP_DURATION, 0);
						if(config_value < 6){
							// Set intial value for SP
							eeprom_write_config(EEADR_SETPOINT, eeprom_read_config(EEADR_PROFILE_SETPOINT(((unsigned char)config_value), 0)));
							// Hack in case inital step duration is '0'
							if(eeprom_read_config(EEADR_PROFILE_DURATION(((unsigned char)config_value), 0)) == 0){
								config_value = 6;
							}
						}
					}
				}
				eeprom_write_config(adr, config_value);
				state=state_show_config_item;
			} else {
				PR6 = 250;
			}
		}
		break;
	default:
		state=state_idle;
	}

	/* This is last resort...
	 * Start using unused registers for general purpose
	 * Use TMR1GE to flag if display should show temperature or not */
	TMR1GE = (state==0);

}