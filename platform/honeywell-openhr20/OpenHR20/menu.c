/*
 *  Open HR20
 *
 *  target:     ATmega169 @ 4 MHz in Honnywell Rondostat HR20E
 *
 *  compiler:   WinAVR-20071221
 *              avr-libc 1.6.0
 *              GCC 4.2.2
 *
 *  copyright:  2008 Jiri Dobry (jdobry-at-centrum-dot-cz)
 *
 *  license:    This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU Library General Public
 *              License as published by the Free Software Foundation; either
 *              version 2 of the License, or (at your option) any later version.
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *              GNU General Public License for more details.
 *
 *              You should have received a copy of the GNU General Public License
 *              along with this program. If not, see http:*www.gnu.org/licenses
 */

/*!
 * \file       menu.c
 * \brief      menu view & controler for free HR20E project
 * \author     Jiri Dobry <jdobry-at-centrum-dot-cz>
 * \date       $Date: 2011-03-21 16:18:05 +0100 (Mon, 21 Mar 2011) $
 * $Rev: 347 $
 */

#include <stdint.h>

#include "config.h"
#include "main.h"
#include "keyboard.h"
#include "adc.h"
#include "lcd.h"
#include "../common/rtc.h"
#include "motor.h"
#include "eeprom.h"
#include "debug.h"
#include "controller.h"
#include "menu.h"
#include "watch.h"

static uint8_t service_idx=CONFIG_RAW_SIZE;

/*!
 *******************************************************************************
 * \brief menu_auto_update_timeout is timer for autoupdates of menu
 * \note val<0 means no autoupdate 
 ******************************************************************************/
int8_t menu_auto_update_timeout = 2;
int8_t wheel_tick_counter = 0;
uint8_t wheel_tick_time = 0;

typedef enum {
    // startup
    menu_startup, menu_version,

    // home screens
    menu_home_no_alter, menu_home, menu_home2, menu_home3, menu_home4,

    menu_home5,

    // lock message
    menu_lock,

    // service menu
    menu_service1, menu_service2, menu_service_watch
    
} menu_t;

menu_t menu_state;
bool menu_locked = false; 
uint32_t hourbar_buff;

/*!
 *******************************************************************************
 * \brief kb_events common reactions
 * 
 * \returns true for controler restart
 ******************************************************************************/

static int8_t wheel_proccess(void) {
    int8_t ret=0;
    if (kb_events & KB_EVENT_WHEEL_PLUS) {
        ret= 1;
    } else if (kb_events & KB_EVENT_WHEEL_MINUS) {
        ret= -1;
    } 
    return ret;
}

/*!
 *******************************************************************************
 * \brief kb_events common reactions
 * 
 * \returns true for controler restart
 ******************************************************************************/

static bool events_common(void) {
    bool ret=false;
    if (kb_events & KB_EVENT_LOCK_LONG) {
        menu_auto_update_timeout=LONG_PRESS_THLD+1;
        menu_locked = ! menu_locked;
        menu_state = (menu_locked)?menu_lock:menu_home;
        ret=true;
    } else if (!menu_locked) {    
        if (kb_events & KB_EVENT_ALL_LONG) { // service menu
            menu_state = menu_service1;
            ret=true;
        } else if (( kb_events & KB_EVENT_NONE_LONG ) 
#if NO_AUTORETURN_FROM_ALT_MENUES 
           && ! (((menu_state>=menu_home2) && (menu_state<=menu_home5)) || menu_state==menu_service_watch)
#endif  
		   ) {
	        menu_state=menu_home; // retun to main home screen
            ret=true;
        } else if ( kb_events & KB_EVENT_C_LONG ) {
            menu_state=menu_version;
            ret=true;
        }
	}
    return ret;
}

/*!
 *******************************************************************************
 * local menu static variables
 ******************************************************************************/
static uint8_t service_watch_n = 0;

/*!
 *******************************************************************************
 * \brief menu Controller
 * 
 * \returns true for controler restart  
 ******************************************************************************/
bool menu_controller(bool new_state) {
	int8_t wheel = wheel_proccess(); //signed number
	bool ret = false;
    switch (menu_state) {
    case menu_startup:
        if (new_state) {
            menu_auto_update_timeout=2;
        }
        if (menu_auto_update_timeout==0) {
            menu_state = menu_version;
            ret=true;
        }
        break;
    case menu_version:
        if (new_state) {
            menu_auto_update_timeout=2;
        }
        if (menu_auto_update_timeout==0) {
            menu_state = menu_home;
            ret=true;
        }
        break;
    case menu_home_no_alter: // same as home screen, but without alternate contend
    case menu_home:         // home screen
    case menu_home2:        // alternate version, real temperature
    case menu_home3:        // alternate version, valve pos
    case menu_home4:        // alternate version, time    
    case menu_home5:        // alternate version, battery
        if ( kb_events & KB_EVENT_C ) {
            menu_state++;       // go to next alternate home screen
            if (menu_state > menu_home5) menu_state=menu_home;
            ret=true; 
        } else {
            if (menu_locked) {
                if ( kb_events & (
                        KB_EVENT_WHEEL_PLUS  | KB_EVENT_WHEEL_MINUS | KB_EVENT_PROG
                        | KB_EVENT_AUTO | KB_EVENT_PROG_REWOKE | KB_EVENT_C_REWOKE | KB_EVENT_AUTO_REWOKE
                        | KB_EVENT_PROG_LONG | KB_EVENT_C_LONG | KB_EVENT_AUTO_LONG )) {
                    menu_auto_update_timeout=LONG_PRESS_THLD+1;
                    menu_state=menu_lock;
                    ret=true;
                    }
            } else { // not locked
				if ((menu_state == menu_home) || (menu_state == menu_home_no_alter)) {
					if (wheel != 0) {   //Handle wheel changes 
						if (CTL_thermostat_mode==radio_target || CTL_thermostat_mode==radio_valve) {
							wheel_tick_counter += wheel;
							wheel_tick_time = 0;
						}
						else if (CTL_thermostat_mode==manual_target){
							CTL_temp_change_inc(wheel);
						}
						else if (CTL_thermostat_mode==manual_timer){
							CTL_change_mode(manual_target);
							CTL_temp_change_inc(wheel);
						}
						menu_state = menu_home_no_alter;
						ret=true; 
					}

					if ( kb_events & KB_EVENT_AUTO ) {
						CTL_change_mode(CTL_CHANGE_AUTO); // change mode
						menu_state=menu_home_no_alter;
						ret=true; 
					}
					
					else if ( kb_events & KB_EVENT_PROG ) {
						CTL_change_mode(CTL_CHANGE_MINOR_MODE); // change mode
						menu_state=menu_home_no_alter;
						ret=true; 
					}

				}
				else {
					if ( kb_events & (
			                        KB_EVENT_WHEEL_PLUS  | KB_EVENT_WHEEL_MINUS | KB_EVENT_PROG
                			        | KB_EVENT_AUTO | KB_EVENT_PROG_REWOKE | KB_EVENT_C_REWOKE | KB_EVENT_AUTO_REWOKE
                        			| KB_EVENT_PROG_LONG | KB_EVENT_C_LONG | KB_EVENT_AUTO_LONG )) {
							menu_state = menu_home;
							ret = true;
					}
				}
                // TODO openHR ....  
            }
        } 
        break;

    default:
    case menu_lock:        // "bloc" message
        if (menu_auto_update_timeout==0) { menu_state=menu_home; ret=true; } 
        break;

    case menu_service1:     // service menu        
    case menu_service2:        
        if (kb_events & KB_EVENT_AUTO) { 
            menu_state=menu_home; 
            ret=true;
        } else if (kb_events & KB_EVENT_C) { 
            menu_state=menu_service_watch; 
            ret=true;
        } else if (kb_events & KB_EVENT_PROG) {
            if (menu_state == menu_service2) {
                eeprom_config_save(service_idx); // save current value
                menu_state = menu_service1;
            } else {
                menu_state = menu_service2;
            }
        } else {
            if (menu_state == menu_service1) {
                // change index
                service_idx = (service_idx+wheel+CONFIG_RAW_SIZE)%CONFIG_RAW_SIZE;
            } else {
                // change value in RAM, to save press PROG
                int16_t min = (int16_t)config_min(service_idx);
                int16_t max_min_1 = (int16_t)(config_max(service_idx))-min+1;
                config_raw[service_idx] = (uint8_t) (
                        ((int16_t)(config_raw[service_idx])+(int16_t)wheel-min+max_min_1)%max_min_1+min);
                if (service_idx==0) LCD_Init();
            }
        }
        break;
	case menu_service_watch:
        if (kb_events & KB_EVENT_AUTO) { 
            menu_state=menu_home; 
            ret=true;
        } else if (kb_events & KB_EVENT_C) { 
            menu_state=menu_service1; 
            ret=true;
        } else {
            service_watch_n=(service_watch_n+wheel+WATCH_N)%WATCH_N;
            if (wheel != 0) ret=true;
        }
        break;
    }
    if (events_common()) ret=true;
    if (ret && (service_idx<CONFIG_RAW_SIZE)) {
        // back config to default value
        config_raw[service_idx] = config_value(service_idx);
        service_idx = CONFIG_RAW_SIZE;
    }
    kb_events = 0; // clear unused keys
    return ret;
} 

/*!
 *******************************************************************************
 * view helper funcions for clear display
 *******************************************************************************/
/*
    // not used generic version, easy to read but longer code
    static void clr_show(uint8_t n, ...) {
        uint8_t i;
        uint8_t *p;
        LCD_AllSegments(LCD_MODE_OFF);
        p=&n+1;
        for (i=0;i<n;i++) {
            LCD_SetSeg(*p, LCD_MODE_ON);
            p++;
        }
    }
*/


static void clr_show1(uint8_t seg1) {
    LCD_AllSegments(LCD_MODE_OFF);
    LCD_SetSeg(seg1, LCD_MODE_ON);
}

/*!
 *******************************************************************************
 * \brief menu View
 ******************************************************************************/
void menu_view(bool update) {
  switch (menu_state) {
    case menu_startup:
        LCD_AllSegments(LCD_MODE_ON);                   // all segments on
        break;
    case menu_version:
        clr_show1(LCD_SEG_COL1);
        LCD_PrintHexW(VERSION_N,LCD_MODE_ON);
        break; 
	case menu_home5:        // battery 
		LCD_AllSegments(LCD_MODE_OFF);
        LCD_PrintDec(bat_average/100, 2, LCD_MODE_ON);
        LCD_PrintDec(bat_average%100, 0, LCD_MODE_ON);
       break;
    case menu_home: // wanted temp / error code / adaptation status
        if (MOTOR_calibration_step>0) {
            clr_show1(LCD_SEG_BAR24);
            LCD_PrintChar(LCD_CHAR_A,3,LCD_MODE_ON);
            if (MOTOR_ManuCalibration==-1) LCD_PrintChar(LCD_CHAR_d,2,LCD_MODE_ON);
            LCD_PrintChar(MOTOR_calibration_step%10, 0, LCD_MODE_ON);
            goto MENU_COMMON_STATUS; // optimization
        } else {
            if (update) clr_show1(LCD_SEG_BAR24);
            if (CTL_error!=0) {
                if (CTL_error & CTL_ERR_BATT_LOW) {
                    LCD_PrintStringID(LCD_STRING_BAtt,LCD_MODE_BLINK_1);
                } else if (CTL_error & CTL_ERR_MONTAGE) {
                    LCD_PrintStringID(LCD_STRING_E2,LCD_MODE_ON);
                } else if (CTL_error & CTL_ERR_MOTOR) {
                    LCD_PrintStringID(LCD_STRING_E3,LCD_MODE_ON);
                } else if (CTL_error & CTL_ERR_BATT_WARNING) {
                    LCD_PrintStringID(LCD_STRING_BAtt,LCD_MODE_ON);
                } else if (CTL_error & CTL_ERR_RFM_SYNC) {
                    LCD_PrintStringID(LCD_STRING_E4,LCD_MODE_ON);
                }
                goto MENU_COMMON_STATUS; // optimization
            } else {
                if (mode_window()) {
                    LCD_PrintStringID(LCD_STRING_OPEn,LCD_MODE_ON);
                    goto MENU_COMMON_STATUS; // optimization
                }
            }
        } 
        // do not use break at this position / optimization
    case menu_home_no_alter: // wanted temp
       	if (update) clr_show1(LCD_SEG_BAR24);
   	if (CTL_thermostat_mode >= 2){
		if ( wheel_tick_counter < -12){
			LCD_PrintStringID(LCD_STRING_4xminus,LCD_MODE_ON);
		}
		else if (wheel_tick_counter < 0){
			LCD_PrintStringID(LCD_STRING_BigMinus,LCD_MODE_ON);
		}
		else if (wheel_tick_counter > 12){
			LCD_PrintStringID(LCD_STRING_PlusPlus,LCD_MODE_ON);
		}
		else if (wheel_tick_counter > 0){
			LCD_PrintStringID(LCD_STRING_Plus,LCD_MODE_ON);
		}
		else{
			LCD_PrintTempInt(temp_average,LCD_MODE_ON);
		}
	}
	else {
		LCD_PrintTemp(CTL_temp_wanted,LCD_MODE_ON);
	}

/*	Not used: Target Valve value	
	if (CTL_mode_auto == auto_valve){
		LCD_PrintChar(LCD_CHAR_NULL, 3, LCD_MODE_ON);
       		LCD_PrintDec3(CTL_valve_wanted, 0 ,LCD_MODE_ON);
		LCD_SetSeg(LCD_SEG_COL1, LCD_MODE_OFF);
	}
*/
        //! \note hourbar status calculation is complex we don't want calculate it every view, use chache
        MENU_COMMON_STATUS:
        //LCD_SetSeg(LCD_SEG_AUTO, (CTL_test_auto()?LCD_MODE_ON:LCD_MODE_OFF));
        //LCD_SetSeg(LCD_SEG_MANU, (CTL_mode_auto?LCD_MODE_OFF:LCD_MODE_ON));
		switch(CTL_thermostat_mode){
			case manual_timer:
				LCD_SetSeg(LCD_SEG_AUTO, LCD_MODE_ON);
				LCD_SetSeg(LCD_SEG_MANU, LCD_MODE_OFF);
				LCD_SetSeg(LCD_SEG_PROG, LCD_MODE_OFF);
				break;
			case manual_target:
				LCD_SetSeg(LCD_SEG_AUTO, LCD_MODE_OFF);
				LCD_SetSeg(LCD_SEG_MANU, LCD_MODE_ON);
				LCD_SetSeg(LCD_SEG_PROG, LCD_MODE_OFF);
				break;
			case radio_target:
				LCD_SetSeg(LCD_SEG_AUTO, LCD_MODE_OFF);
				LCD_SetSeg(LCD_SEG_MANU, LCD_MODE_OFF);
				LCD_SetSeg(LCD_SEG_PROG, LCD_MODE_ON);
				break;
			case radio_valve:
				LCD_SetSeg(LCD_SEG_AUTO, LCD_MODE_OFF);
				LCD_SetSeg(LCD_SEG_MANU, LCD_MODE_OFF);
				LCD_SetSeg(LCD_SEG_PROG, LCD_MODE_ON);
				break;
		}

	       LCD_HourBarBitmap(hourbar_buff);
	       break;

    case menu_home2: // real temperature
   	if (CTL_thermostat_mode >= 2){
		menu_state++;
	}
	else {
        	if (update) clr_show1(LCD_SEG_COL1);           // decimal point
        	LCD_PrintTempInt(temp_average,LCD_MODE_ON);
        	break;
	}

    case menu_home3: // valve pos
        if (update) LCD_AllSegments(LCD_MODE_OFF);
        // LCD_PrintDec3(MOTOR_GetPosPercent(), 1 ,LCD_MODE_ON);
        // LCD_PrintChar(LCD_CHAR_2lines,0,LCD_MODE_ON);
        {
            uint8_t prc = MOTOR_GetPosPercent();
            if (prc<=100) {
                LCD_PrintDec3(MOTOR_GetPosPercent(), 0 ,LCD_MODE_ON);
            } else {
                LCD_PrintStringID(LCD_STRING_minusCminus,LCD_MODE_ON);
            }
        }
        break;

    case menu_lock:        // "bloc" message
        LCD_AllSegments(LCD_MODE_OFF); // all segments off
        LCD_PrintStringID(LCD_STRING_bloc,LCD_MODE_ON);
        break;
    case menu_service1: 
    case menu_service2:
        // service menu; left side index, right value
        LCD_AllSegments(LCD_MODE_ON);
        LCD_PrintHex(service_idx, 2, ((menu_state == menu_service1) ? LCD_MODE_BLINK_1 : LCD_MODE_ON));
        LCD_PrintHex(config_raw[service_idx], 0, ((menu_state == menu_service2) ? LCD_MODE_BLINK_1 : LCD_MODE_ON));
       break;
    case menu_service_watch:
        LCD_AllSegments(LCD_MODE_ON);
        LCD_PrintHexW(watch(service_watch_n),LCD_MODE_ON);
        LCD_SetHourBarSeg(service_watch_n, LCD_MODE_BLINK_1);
        break;

    default:
        break;                   
    }                                          
}