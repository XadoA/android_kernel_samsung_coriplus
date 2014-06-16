/*
 * Battery driver for Dialog D2083
 *   
 * Copyright(c) 2012 Dialog Semiconductor Ltd.
 *  
 * Author: Dialog Semiconductor Ltd. D. Chen, A Austin, E Jeong
 *  
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/power_supply.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/i2c.h>
#include <linux/proc_fs.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>

#include "linux/err.h"	// test only

#include <linux/d2083/core.h>
#include <linux/d2083/d2083_battery.h>
#include <linux/d2083/d2083_reg.h>

#include <linux/broadcom/csl_types.h>


static const char __initdata d2083_battery_banner[] = \
    "D2083 Battery, (c) 2012 Dialog Semiconductor Ltd.\n";

/***************************************************************************
 Pre-definition
***************************************************************************/
#define FALSE								(0)
#define TRUE								(1)

#define DETACHED							(0)
#define ATTACHED							(1)

#define ADC_RES_MASK_LSB					(0x0F)
#define ADC_RES_MASK_MSB					(0xF0)

#define POWER_SUPPLY_BATTERY 				"battery"
#define POWER_SUPPLY_WALL 					"ac"
#define POWER_SUPPLY_USB 					"usb"


//////////////////////////////////////////////////////////////////////////////
//    External Function Protorype
//////////////////////////////////////////////////////////////////////////////
#if 1  // TSU6111 IC
extern int fsa9480_read_charger_status(u8 *val);
#endif

extern UInt16 SYSPARM_GetDefault4p2VoltReading(void);
extern UInt16 SYSPARM_GetBattEmptyThresh(void);
extern UInt8 SYSPARM_GetIsInitialized(void);

//////////////////////////////////////////////////////////////////////////////
//    Static Function Prototype
//////////////////////////////////////////////////////////////////////////////
//static void d2083_external_event_handler(int category, int event);
static int  d2083_read_adc_in_auto(struct d2083_battery *pbat, adc_channel channel);
static int  d2083_read_adc_in_manual(struct d2083_battery *pbat, adc_channel channel);
static void d2083_start_charge(struct d2083_battery *pbat, u32 timer_type);
static void d2083_stop_charge(struct d2083_battery *pbat, u8 end_of_charge);
static void d2083_set_battery_health(struct d2083_battery *pbat, u32 health);
static void d2083_sleep_monitor(struct d2083_battery *pbat);
static u8   d2083_clear_end_of_charge(struct d2083_battery *pbat, u8 end_of_charge);
static void d2083_set_ta_attached(struct d2083_battery *pbat, u8 state);
static s8   d2083_check_end_of_charge(struct d2083_battery *pbat, u8 end_of_charge);
static void d2083_set_usb_attached(struct d2083_battery *pbat, u8 state);
static void d2083_set_jig_attached(struct d2083_battery *pbat, u8 state);
static void d2083_battery_charge_full(struct d2083_battery *pbat);
static void d2083_ovp_charge_stop(struct d2083_battery *pbat);
static void d2083_ovp_charge_restart(struct d2083_battery *pbat);
static void d2083_external_event_handler(int category, int event);


//////////////////////////////////////////////////////////////////////////////
//    Static Variable Declaration
//////////////////////////////////////////////////////////////////////////////
static struct d2083_battery *gbat = NULL;
static u8 tick_count = 0;
static u8 is_called_by_ticker = 0;


static enum power_supply_property d2083_ta_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property d2083_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property d2083_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_BATT_TEMP_ADC,
};


// TODO: Please Check...
// Is there no TJUNC channel in auto mode ?
static struct adc_cont_in_auto adc_cont_inven[D2083_ADC_CHANNEL_MAX - 1] = {
	// VBAT_S channel
	[D2083_ADC_VOLTAGE] = {
		.adc_cont_val = (D2083_ADCCONT_ADC_AUTO_EN | D2083_ADCCONT_ADC_MODE 
							| D2083_ADCCONT_AUTO_VBAT_EN),
		.adc_msb_res = D2083_VBAT_RES_REG,
		.adc_lsb_res = D2083_ADC_RES_AUTO1_REG,
		.adc_lsb_mask = ADC_RES_MASK_LSB,
	},
	// TEMP_1 channel
	[D2083_ADC_TEMPERATURE_1] = {
		.adc_cont_val = (D2083_ADCCONT_ADC_AUTO_EN | D2083_ADCCONT_ADC_MODE 
							/*| D2083_ADCCONT_TEMP1_ISRC_EN*/),   // 50uA Current Source disabled
		.adc_msb_res = D2083_TEMP1_RES_REG,
		.adc_lsb_res = D2083_ADC_RES_AUTO1_REG,
		.adc_lsb_mask = ADC_RES_MASK_MSB,
	},
	// TEMP_2 channel
	[D2083_ADC_TEMPERATURE_2] = {
		.adc_cont_val = (D2083_ADCCONT_ADC_AUTO_EN | D2083_ADCCONT_ADC_MODE 
							/*| D2083_ADCCONT_TEMP2_ISRC_EN*/),   // 50uA Current Source disabled
		.adc_msb_res = D2083_TEMP2_RES_REG,
		.adc_lsb_res = D2083_ADC_RES_AUTO3_REG,
		.adc_lsb_mask = ADC_RES_MASK_LSB,
	},
	// VF channel
	[D2083_ADC_VF] = {
		.adc_cont_val = (D2083_ADCCONT_ADC_AUTO_EN | D2083_ADCCONT_ADC_MODE 
							| D2083_ADCCONT_VF_ISRC_EN | D2083_ADCCONT_AUTO_VF_EN),
		.adc_msb_res = D2083_VF_RES_REG,
		.adc_lsb_res = D2083_ADC_RES_AUTO2_REG,
		.adc_lsb_mask = ADC_RES_MASK_LSB,
	},
	// AIN channel
	[D2083_ADC_AIN] = {
		.adc_cont_val = (D2083_ADCCONT_ADC_AUTO_EN | D2083_ADCCONT_ADC_MODE
							| D2083_ADCCONT_AUTO_AIN_EN),
		.adc_msb_res = D2083_AIN_RES_REG,
		.adc_lsb_res = D2083_ADC_RES_AUTO2_REG,
		.adc_lsb_mask = ADC_RES_MASK_MSB
	},
};


// LUT for NCP15XW223 thermistor with 10uA current source selected
static struct adc2temp_lookuptbl adc2temp_lut = {


	// Case of NCP03XH223
	.adc  = {  // ADC-12 input value
		2459,	   1933,	  1398, 	1221,	  981, 	    900,	 794,
		646,	   529, 	  435,		360,	  300,		250,	 210,
		190,	   177, 	  150,		127,	  109,		99, 	 90,
		88, 
	},
	.temp = {	// temperature (degree K)
		C2K(-200), C2K(-150), C2K(-80), C2K(-50), C2K(0),	C2K(20),  C2K(50),
		C2K(100),  C2K(150),  C2K(200), C2K(250), C2K(300), C2K(350), C2K(400),
		C2K(430),  C2K(450),  C2K(500), C2K(550), C2K(600), C2K(630), C2K(650),
		C2K(670),
	},
};

static u16 temp_lut_length = (u16)sizeof(adc2temp_lut.adc)/sizeof(u16);

// adc = (vbat-2500)/2000*2^12
// vbat (mV) = 2500 + adc*2000/2^12
static struct adc2vbat_lookuptbl adc2vbat_lut = {
#if 1
	.adc	 = {1843, 1946, 2148, 2253, 2458, 2662, 2867, 2683, 3072, 3482,}, // ADC-12 input value
	.offset  = {   0,	 0,    0,	 0,    0,	 0,    0,	 0,    0,    0,}, // charging mode ADC offset
	.vbat	 = {3400, 3450, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200,}, // VBAT (mV)
#else
    .adc     = {1843, 1946, 2148, 2253, 2458, 2662, 2867, 2683, 3072, 3482,}, // ADC-12 input value
    .offset  = {   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,}, // charging mode ADC offset
    .vbat    = {3400, 3450, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200,}, // VBAT (mV)
#endif
};

#ifdef BATTERY_CAPACITY_1300mAH
////////////////////1300mA 

//charging_0731_L1_1300
static struct adc2soc_lookuptbl adc2soc_lut = {
#if 1 //battery 1300	
	.adc_ht  = {1800, 1870, 2060, 2270, 2400, 2510, 2585, 2635, 2685, 2781, 2933, 3064, 3230, 3444,}, // ADC input @ high temp
	.adc_rt  = {1800, 1870, 2060, 2270, 2400, 2510, 2585, 2635, 2685, 2781, 2933, 3064, 3230, 3444,}, // ADC input @ room temp
#else //battery 1350
	.adc_ht  = {1800, 1870, 2060, 2270, 2430, 2510, 2590, 2655, 2715, 2791, 2933, 3064, 3230, 3444,}, // ADC input @ high temp
	.adc_rt  = {1800, 1870, 2060, 2270, 2430, 2510, 2590, 2655, 2715, 2791, 2933, 3064, 3230, 3444,}, // ADC input @ room temp
#endif	
	.adc_lt  = {1800, 1865, 2020, 2220, 2330, 2430, 2500, 2560, 2620, 2730, 2890, 3030, 3160, 3300,}, // ADC input @ low temp
	.adc_llt = {1800, 1860, 1998, 2159, 2270, 2368, 2436, 2486, 2536, 2622, 2753, 2869, 3010, 3190,}, // ADC input @ low low temp  <-- Will be tested. 10th Aug 16:26
	.soc	 = {   0,	 1,    3,	 5,   10,	20,   30,   40,	  50,   60,	  70,   80,	  90,  100,}, // SoC in %
};

//Discharging Weight(Room/Low/low low)          //    0,    1,    3,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100
static u16 adc_weight_section_discharge[]       = {3000, 2000,  700,  450,  240,  210,  170,  170,  270,  420,  380,  500,  600, 1000}; 
static u16 adc_weight_section_discharge_lt[]    = {2900, 1900,  690,  440,  230,  215,  175,  175,  275,  425,  390,  510,  320,  900};
static u16 adc_weight_section_discharge_llt[]   = {2600, 1700,  650,  400,  145,  120,  110,  110,  130,  140,  152,  190,  290,  560};   // <-- Will be tested. 10th Aug 16:26


//Charging Weight(Room/Low/low low)             //    0,    1,    3,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100
static u16 adc_weight_section_charge[]			= {7000, 3000, 1300,	330,  180,	120,   81,	 85,  125,	185,  190,	245,  300,	600}; //L
static u16 adc_weight_section_charge_lt[]       = {3050, 1150,  250,  150,   85,   49,   39,   37,   59,   94,   90,  114,  150,  300};
static u16 adc_weight_section_charge_llt[]      = {1500,  575,  125,   75,   46,   25,   20,   19,   30,   47,   45,   57,   75,  150};

//Charging Offset                               //    0,    1,    3,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100
//static u16 adc_diff_charge[]                  = {  60,   60,  200,  200,  205,  212,  255,  245,  247,  235,  180,  175,  175,    0};
//static u16 adc_diff_charge[]                  = {  60,   60,  200,  200,  210,  230,  245,  240,  240,  235,  180,  175,  175,    0};
//static u16 adc_diff_charge[]                   = { 60,   60,  200,  200,  210,  220,  245,  230,  230,  220,  175,  165,  165,   0};
static u16 adc_diff_charge[]                   = { 60,   60,  200,  210,  225,  225,  248,  240,  235,  220,  175,  165,  165,   0};


#elif defined(BATTERY_CAPACITY_1500mAH)


////////////////////////// 1500mA
/* charging_0731_1500
static struct adc2soc_lookuptbl adc2soc_lut = {
	.adc_ht  = {1800, 1870, 2060, 2270, 2450, 2550, 2655, 2705, 2785, 2891, 2993, 3104, 3270, 3444,}, // ADC input @ high temp
	.adc_rt  = {1800, 1870, 2060, 2270, 2450, 2550, 2655, 2705, 2785, 2891, 2993, 3104, 3270, 3444,}, // ADC input @ room temp
	.adc_lt  = {1800, 1830, 1870, 1910, 2040, 2140, 2240, 2340, 2440, 2550, 2680, 2850, 3080, 3300,}, // ADC input @ low temp
	.adc_llt = {1800, 1820, 1860, 1910, 1970, 2090, 2210, 2340, 2480, 2610, 2740, 2880, 3030, 3170,}, // ADC input @ low low temp
	.soc	 = {   0,	 1,    3,	 5,   10,	20,   30,   40,	  50,   60,	  70,   80,	  90,  100,}, // SoC in %
};
*/
/* charging_0801_4_1500_New
static struct adc2soc_lookuptbl adc2soc_lut = {
	.adc_ht  = {1800, 1870, 2060, 2270, 2460, 2550, 2645, 2700, 2755, 2831, 2973, 3104, 3235, 3444,}, // ADC input @ high temp
	.adc_rt  = {1800, 1870, 2060, 2270, 2460, 2550, 2645, 2700, 2755, 2831, 2973, 3104, 3235, 3444,}, // ADC input @ room temp
	.adc_lt  = {1800, 1830, 1870, 1910, 2040, 2140, 2240, 2340, 2440, 2550, 2680, 2850, 3080, 3300,}, // ADC input @ low temp
	.adc_llt = {1800, 1820, 1860, 1910, 1970, 2090, 2210, 2340, 2480, 2610, 2740, 2880, 3030, 3170,}, // ADC input @ low low temp
	.soc	 = {   0,	 1,    3,	 5,   10,	20,   30,	40,   50,	60,   70,	80,   90,  100,}, // SoC in %
};
*/
// 	.adc_rt  = {1800, 1870, 2060, 2270, 2400, 2510, 2585, 2635, 2685, 2781, 2933, 3064, 3230, 3444,}, // 1300mA
//  .adc_rt  = {1800, 1850, 2020, 2200, 2444, 2556, 2660, 2715, 2765, 2840, 2985, 3110, 3245, 3444,}, // test1
//  .adc_rt  = {1800, 1850, 2020, 2200, 2444, 2553, 2630, 2785, 2735, 2840, 2995, 3120, 3245, 3444,}, // test2
//  .adc_ht  = {1800, 1850, 2020, 2200, 2440, 2550, 2630, 2685, 2735, 2840, 2995, 3120, 3245, 3444,}, //charging_0803_1_1500
static struct adc2soc_lookuptbl adc2soc_lut = {
	.adc_ht  = {1800, 1850, 2020, 2200, 2440, 2550, 2630, 2685, 2735, 2840, 2990, 3115, 3245, 3444,}, // ADC input @ high temp
	.adc_rt  = {1800, 1850, 2020, 2200, 2440, 2550, 2630, 2685, 2735, 2840, 2990, 3115, 3245, 3444,}, // ADC input @ room temp
	.adc_lt  = {1800, 1830, 1870, 1910, 2040, 2140, 2240, 2340, 2440, 2550, 2680, 2850, 3080, 3300,}, // ADC input @ low temp
	.adc_llt = {1800, 1820, 1860, 1910, 1970, 2090, 2210, 2340, 2480, 2610, 2740, 2880, 3030, 3170,}, // ADC input @ low low temp
	.soc	 = {   0,	 1,    3,	 5,   10,	20,   30,	40,   50,	60,   70,	80,   90,  100,}, // SoC in %
};

//Discharging Weight(Room/Low/low low)          //    0,    1,    3,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100
//static u16 adc_weight_section_discharge[]     = {3000, 1800,  600,  410,  220,  190,  160,  160,  250,  400,  350,  450,  550, 1000}; 
//static u16 adc_weight_section_discharge[]     = {2538, 1692,  592,  380,  203,  177,  143,  143,  228,  355,  321,  423,  507,  800}; 
static u16 adc_weight_section_discharge[]       = {2288, 1532,  543,  500,  400,  160,  130,  130,  200,  320,  290,  380,  450, 800}; 
static u16 adc_weight_section_discharge_lt[]    = {2900, 1900,  690,  440,  230,  215,  175,  175,  275,  425,  390,  510,  300,  900};
static u16 adc_weight_section_discharge_llt[]   = {2500, 1600,  245,  212,  180,  155,  148,  147,  162,  215,  208,  220,  225,  600};

//Charging Weight(Room/Low/low low)             //    0,    1,    3,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100
//static u16 adc_weight_section_charge[]        = {5000, 2200,  490,  285,  160,   98,   73,   75,  115,  180,  180,  230,  300,  600}; 
//static u16 adc_weight_section_charge[]        = {7000, 2000,  700,  450,  240,  210,  170,  170,  270,  420,  380,  500,  600, 1000};
static u16 adc_weight_section_charge[]          = {3000, 2000,  700,  450,  240,  210,  170,  170,  270,  420,  380,  500,  600, 1000};
static u16 adc_weight_section_charge_lt[]       = {3050, 1150,  250,  150,   85,   49,   39,   37,   59,   94,   90,  114,  150,  300};
static u16 adc_weight_section_charge_llt[]      = {1500,  575,  125,   75,   46,   25,   20,   19,   30,   47,   45,   57,   75,  150};

//Charging Offset                               //    0,    1,    3,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100
//static u16 adc_diff_charge[]                  = {  60,   60,  200,  200,  210,  220,  245,  230,  230,  220,  175,  165,  165,    0};
static u16 adc_diff_charge[]                    = {  60,   60,  200,  200,  210,  220,  245,  230,  230,  220,  175,  165,  165,    0};
#endif /* BATTERY_CAPACITY_1300mAH */

static u16 adc2soc_lut_length = (u16)sizeof(adc2soc_lut.soc)/sizeof(u16);
static u16 adc2vbat_lut_length = (u16)sizeof(adc2vbat_lut.offset)/sizeof(u16);

#ifdef USE_ANTO_ALGORITHM
// For Antonello's algorithm
#include "battery.c"
#endif


/* 
 * Name : chk_lut
 *
 */
static int chk_lut (u16* x, u16* y, u16 v, u16 l) {
	int i;
	//u32 ret;
	int ret;

	if (v < x[0])
		ret = y[0];
	else if (v >= x[l-1])
		ret = y[l-1]; 
	else {          
		for (i = 1; i < l; i++) {          
			if (v < x[i])               
				break;      
		}       
		ret = y[i-1];       
		ret = ret + ((v-x[i-1])*(y[i]-y[i-1]))/(x[i]-x[i-1]);   
	}   
	//return (u16) ret;
	return ret;
}

/* 
 * Name : chk_lut_temp
 * return : The return value is Kelvin degree
 */
static int chk_lut_temp (u16* x, u16* y, u16 v, u16 l) {
	int i, ret;

	if (v >= x[0])
		ret = y[0];
	else if (v < x[l-1])
		ret = y[l-1]; 
	else {			
		for (i=1; i < l; i++) { 		 
			if (v > x[i])				
				break;		
		}		
		ret = y[i-1];		
		ret = ret + ((v-x[i-1])*(y[i]-y[i-1]))/(x[i]-x[i-1]);	
	}

	//pr_info("%s. Result (%d)\n", __func__, ret);
	
	return ret;
}


/* 
 * Name : adc_to_soc_with_temp_compensat
 *
 */
u32 adc_to_soc_with_temp_compensat(u16 adc, u16 temp) {	
	int sh, sl;

	//pr_info("%s. adc = %d. temp = %d\n", __func__, adc, temp);

	if (temp < BAT_LOW_LOW_TEMPERATURE)		
		temp = BAT_LOW_LOW_TEMPERATURE;
	else if (temp > BAT_HIGH_TEMPERATURE)
		temp = BAT_HIGH_TEMPERATURE;
	
	if ((temp <= BAT_HIGH_TEMPERATURE) && (temp > BAT_ROOM_TEMPERATURE)) {  
		sh = chk_lut(adc2soc_lut.adc_ht, adc2soc_lut.soc, adc, adc2soc_lut_length);    
		sl = chk_lut(adc2soc_lut.adc_rt, adc2soc_lut.soc, adc, adc2soc_lut_length);
		sh = sl + (temp - BAT_ROOM_TEMPERATURE)*(sh - sl)
								/ (BAT_HIGH_TEMPERATURE - BAT_ROOM_TEMPERATURE);
	}
	else if ((temp <= BAT_ROOM_TEMPERATURE) && (temp > BAT_LOW_TEMPERATURE)) {      
		sh = chk_lut(adc2soc_lut.adc_rt, adc2soc_lut.soc, adc, adc2soc_lut_length);        
		sl = chk_lut(adc2soc_lut.adc_lt, adc2soc_lut.soc, adc, adc2soc_lut_length);        
		sh = sl + (temp - BAT_LOW_TEMPERATURE)*(sh - sl)
								/ (BAT_ROOM_TEMPERATURE-BAT_LOW_TEMPERATURE);    
	} else {        
		sh = chk_lut(adc2soc_lut.adc_lt, adc2soc_lut.soc,  adc, adc2soc_lut_length);        
		sl = chk_lut(adc2soc_lut.adc_llt, adc2soc_lut.soc, adc, adc2soc_lut_length);
		sh = sl + (temp - BAT_LOW_LOW_TEMPERATURE)*(sh - sl)
								/ (BAT_LOW_TEMPERATURE-BAT_LOW_LOW_TEMPERATURE); 
	}

	return sh;
}


static u16 pre_soc = 0xffff;
u16 soc_filter(u16 new_soc, u8 is_charging) {
	u16 soc = new_soc;

	if(pre_soc == 0xffff)
		pre_soc = soc;
	else {
		if( soc > pre_soc)
		{
			if(is_charging)
			{
				if(soc <= pre_soc + 2)
					pre_soc = soc;
				else {
					soc = pre_soc + 1;
					pre_soc = soc;
				}
			} else
				soc = pre_soc; //in discharge, SoC never goes up
		} else {
			if(soc >= pre_soc - 2)
				pre_soc = soc;
			else {
				soc = pre_soc - 1;
				pre_soc = soc;
			}
		}
	}
	return (soc);
}


/* 
 * Name : adc_to_degree
 *
 */
//u16 adc_to_degree_k(u16 adc) {
int adc_to_degree_k(u16 adc) {

    return (chk_lut_temp(adc2temp_lut.adc, adc2temp_lut.temp, adc, temp_lut_length));
}

int degree_k2c(u16 k) {
	return (K2C(k));
}

/* 
 * Name : get_adc_offset
 *
 */
//u16 get_adc_offset(u16 adc) {	
int get_adc_offset(u16 adc) {	

    return (chk_lut(adc2vbat_lut.adc, adc2vbat_lut.offset, adc, adc2vbat_lut_length));
}

/* 
 * Name : adc_to_vbat
 *
 */
u16 adc_to_vbat(u16 adc, u8 is_charging) {    
	u16 a = adc;

	if(is_charging)
		a = adc - get_adc_offset(adc); // deduct charging offset
	// return (chk_lut(adc2vbat_lut.adc, adc2vbat_lut.vbat, a, adc2vbat_lut_length));
	return (2500 + ((a * 2000) >> 12));
}

/* 
 * Name : adc_to_soc
 * get SOC (@ room temperature) according ADC input
 */
//u16 adc_to_soc(u16 adc, u8 is_charger) { 
int adc_to_soc(u16 adc, u8 is_charging) { 

	u16 a = adc;

	if(is_charging)
		a = adc - get_adc_offset(adc); // deduct charging offset
	return (chk_lut(adc2soc_lut.adc_rt, adc2soc_lut.soc, a, adc2soc_lut_length));
}


int degree_to_bcmpmu_adc(struct d2083_battery_data *pbat_data) {
	int degree, length, ret, i;
	struct temp2adc_map *bcm_map;
	
	degree = pbat_data->average_temperature;
	length = pbat_data->bcmpmu_temp_map_len;
	bcm_map = pbat_data->bcmpmu_temp_map;

	if (degree < bcm_map[0].temp)
		ret = bcm_map[0].adc;
	else if (degree >= bcm_map[length-1].temp)
		ret = bcm_map[length-1].adc; 
	else {			
		for (i = 1; i < length; i++) { 		 
			if (degree < bcm_map[i].temp)				
				break;		
		}

		ret = bcm_map[i-1].adc;
		ret = ret + ((degree - bcm_map[i-1].temp) 
						* (bcm_map[i].adc - bcm_map[i-1].adc)) 
						/ ( bcm_map[i].temp - bcm_map[i-1].temp);
	}

	
	return ret;
}



//////////////////////////////////////////////////////////////////////////////////
// External Function 
//////////////////////////////////////////////////////////////////////////////////

/* 
 * Name : d2083_battery_reset_tick_count
 */
void d2083_battery_reset_tick_count(void)
{
	tick_count = 0;
	pr_info("%s. TICK count was rested to 0\n", __func__);
}
EXPORT_SYMBOL(d2083_battery_reset_tick_count);


/* 
 * Name : d2083_register_enable_charge
 */
int d2083_register_enable_charge(void (*enable_charge)(enum power_supply_type))
{
	pr_info("%s. Start\n", __func__);

	if(unlikely(!gbat)) {
		pr_err("%s. Platfrom data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&gbat->api_lock);
	gbat->charger_data.enable_charge = enable_charge;
	mutex_unlock(&gbat->api_lock);

	return 0; 
}
EXPORT_SYMBOL(d2083_register_enable_charge);

/* 
 * Name : d2083_register_disable_charge
 */
int d2083_register_disable_charge(void (*disable_charge)(unsigned char))
{
	pr_info("%s. Start\n", __func__);

	if(unlikely(!gbat)) {
		pr_err("%s. Platfrom data is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&gbat->api_lock);
	gbat->charger_data.disable_charge = disable_charge;
	mutex_unlock(&gbat->api_lock);

	return 0; 
}
EXPORT_SYMBOL(d2083_register_disable_charge);



/* 
 * Name : d2083_get_external_event_handler
 */
void (*d2083_get_external_event_handler(void))(int, int)
{
	return d2083_external_event_handler;
}
EXPORT_SYMBOL(d2083_get_external_event_handler);


/* 
 * Name : d2083_external_event_handler
 */
static void d2083_external_event_handler(int category, int event)
{
	if(unlikely(!gbat)) {
		pr_err("%s. Invalid data.\n", __func__);
		return;
	}

	switch(category)
	{
		case D2083_CATEGORY_DEVICE:
			switch(event)
			{
				case D2083_EVENT_TA_ATTACHED:
					d2083_set_ta_attached(gbat, ATTACHED);
					break;
				case D2083_EVENT_TA_DETACHED:
					d2083_set_ta_attached(gbat, DETACHED);
					break;
				case D2083_EVENT_USB_ATTACHED:
					d2083_set_usb_attached(gbat, ATTACHED);
					break;
				case D2083_EVENT_USB_DETACHED:
					d2083_set_usb_attached(gbat, DETACHED);
					break;
				case D2083_EVENT_JIG_ATTACHED:
					d2083_set_jig_attached(gbat, ATTACHED);
					break;
				case D2083_EVENT_JIG_DETACHED:
					d2083_set_jig_attached(gbat, DETACHED);
					break;
				default:
					break;
			}
			break;

		case D2083_CATEGORY_BATTERY:
			switch(event)
			{
				case D2083_EVENT_CHARGE_FULL:
					d2083_battery_charge_full(gbat);
					break;
				case D2083_EVENT_OVP_CHARGE_STOP:
					d2083_ovp_charge_stop(gbat);	
					break;
				case D2083_EVENT_OVP_CHARGE_RESTART:
					d2083_ovp_charge_restart(gbat);
					break;
				case D2083_EVENT_SLEEP_MONITOR:
					d2083_sleep_monitor(gbat);
					break;
				default:
					break;
			}
			break;

		default:
			break;
	}
}
EXPORT_SYMBOL(d2083_external_event_handler);


/* 
 * Name : d2083_get_last_adc
 */
int d2083_get_last_vbat_adc(void)
{
	int last_adc;
	
	if(unlikely(!gbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&gbat->lock);
	last_adc = gbat->battery_data.average_volt_adc;
	mutex_unlock(&gbat->lock);

	return last_adc;
}

EXPORT_SYMBOL(d2083_get_last_vbat_adc);

int d2083_get_last_temp_adc(void)
{
	int last_temp;
	
	if(unlikely(!gbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&gbat->lock);
	last_temp = gbat->battery_data.average_temp_adc;
	mutex_unlock(&gbat->lock);

	return last_temp;
}

EXPORT_SYMBOL(d2083_get_last_temp_adc);


/* 
 * Name : d2083_get_charger_type
 */
static int d2083_get_charger_type(struct d2083_battery *pbat)
{
	int charger_type = CHARGER_TYPE_NONE;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	charger_type = pbat->charger_data.current_charger;
	mutex_unlock(&pbat->lock);

	return charger_type;
}


/* 
 * Name : d2083_get_battery_status
 */
static int d2083_get_battery_status(struct d2083_battery *pbat)
{
	int battery_status;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}
	
	mutex_lock(&pbat->lock);
	battery_status = pbat->battery_data.status;
	mutex_unlock(&pbat->lock);

	return battery_status;
}


/* 
 * Name : d2083_get_battery_health
 */
static int d2083_get_battery_health(struct d2083_battery *pbat)
{
	int battery_health;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	battery_health = pbat->battery_data.health;
	mutex_unlock(&pbat->lock);

	return battery_health;
}


/* 
 * Name : d2083_get_battery_capacity
 */
static int d2083_get_battery_capacity(struct d2083_battery *pbat)
{
	int battery_soc = 0;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}


	mutex_lock(&pbat->lock);
	battery_soc = pbat->battery_data.soc;
	mutex_unlock(&pbat->lock);

	return battery_soc;
}


/* 
 * Name : d2083_get_battery_technology
 */
static int d2083_get_battery_technology(struct d2083_battery *pbat)
{
	int battery_technology;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	battery_technology = pbat->battery_data.battery_technology;
	mutex_unlock(&pbat->lock);

	return battery_technology;
}


/* 
 * Name : d2083_get_current_voltage
 */
static int d2083_get_current_voltage(struct d2083_battery *pbat)
{
	int current_voltage;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	current_voltage = pbat->battery_data.current_voltage;
	mutex_unlock(&pbat->lock);

	return current_voltage;
}


/* 
 * Name : d2083_get_average_voltage
 */
static int d2083_get_average_voltage(struct d2083_battery *pbat)
{
	int average_voltage;
	
	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	average_voltage = pbat->battery_data.average_voltage;
	mutex_unlock(&pbat->lock);

	return average_voltage;
}


/* 
 * Name : d2083_get_average_temperature
 */
static int d2083_get_average_temperature(struct d2083_battery *pbat)
{
	int average_temperature;
	
	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	average_temperature = pbat->battery_data.average_temperature;
	mutex_unlock(&pbat->lock);

	return average_temperature;
}


/* 
 * Name : d2083_get_average_temperature_adc
 */
static int d2083_get_average_temperature_adc(struct d2083_battery *pbat)
{
	int average_temperature_adc;
	
	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	average_temperature_adc = pbat->battery_data.average_temp_adc;
	mutex_unlock(&pbat->lock);

	return average_temperature_adc;
}


/* 
 * Name : d2083_get_pmic_vbus_status
 */
static int d2083_get_pmic_vbus_status(struct d2083_battery *pbat)
{
	int pmic_vbus_state;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&pbat->lock);
	pmic_vbus_state = pbat->charger_data.pmic_vbus_state;
	mutex_unlock(&pbat->lock);

	return pmic_vbus_state;
}


/* 
 * Name : d2083_get_soc
 */
static int d2083_get_soc(struct d2083_battery *pbat)
{
	//u8 is_charger = 0;
	int soc, anto_soc, battery_status;
 	struct d2083_battery_data *pbat_data = NULL;
	struct d2083_charger_data *pchg_data = NULL;

	if(pbat == NULL) {
		pr_err("%s. Invalid parameter. \n", __func__);
	}

	pbat_data = &pbat->battery_data;
	pchg_data = &pbat->charger_data;
	battery_status = d2083_get_battery_status(pbat);

	if(pbat_data->soc)
		pbat_data->prev_soc = pbat_data->soc;

#ifdef USE_ANTO_ALGORITHM
	if(pbat_data->anto_soc)
		pbat_data->anto_prev_soc = pbat_data->anto_soc;
#endif /* USE_ANTO_ALGORITHM */


	soc = adc_to_soc_with_temp_compensat(pbat_data->average_volt_adc, 
										C2K(pbat_data->average_temperature));
	if(soc <= 0) {
		pbat_data->soc = 0;
		if(pbat_data->current_voltage >= BAT_POWER_OFF_VOLTAGE
			|| (pchg_data->is_charging == TRUE)) {
			soc = 1;
		}
	}
	else if(soc >= 100) {
		soc = 100;
	}

#ifdef USE_ANTO_ALGORITHM
	anto_soc = adc_to_soc_with_temp_compensat_ant(pbat_data->origin_volt_adc, 
									   				C2K((pbat_data->current_temperature/10)),
									   				pchg_data->is_charging);
	if(anto_soc <= 0) {
		pbat_data->anto_soc = 0;
		if(pbat_data->current_voltage >= BAT_POWER_OFF_VOLTAGE
			|| (pchg_data->is_charging)) {
			anto_soc = 1;
		}
	}
	else if(anto_soc >= 1000) {
		anto_soc = 1000;
	}
#endif /* USE_ANTO_ALGORITHM */

	// Don't allow soc goes up when battery is dicharged.
	// and also don't allow soc goes down when battey is charged.
	if(pchg_data->is_charging != TRUE 
		&& (soc > pbat_data->prev_soc && pbat_data->prev_soc )) {
		soc = pbat_data->prev_soc;
	}
	else if(pchg_data->is_charging
		&& (soc < pbat_data->prev_soc) && pbat_data->prev_soc) {
		soc = pbat_data->prev_soc;
	}
	pbat_data->soc = soc;

#ifdef USE_ANTO_ALGORITHM
	if(pchg_data->is_charging != TRUE
		&& (anto_soc > pbat_data->anto_prev_soc && pbat_data->anto_prev_soc )) {
		anto_soc = pbat_data->anto_prev_soc;
	}
	pbat_data->anto_soc = anto_soc;
#endif /* USE_ANTO_ALGORITHM */

	return soc;
}


/* 
 * Name : d2083_get_jig_state
 */
static int d2083_get_jig_state(struct d2083_battery *pbat)
{
	int jig_connected = 0;

	mutex_lock(&pbat->lock);
	jig_connected = pbat->charger_data.jig_connected;
	mutex_unlock(&pbat->lock);

	return jig_connected;
}


/* 
 * Name : d2083_set_adc_mode
 * get resistance (ohm) of VF from ADC input, using 10uA current source
 */ 
static u32 d2083_get_vf_ohm (u16 adc) {
	u32 ohm;
	ohm = (2500 * adc * 100000); // R = 2.5*adc/(10*10^-6)/2^D2083_ADC_RESOLUTION
	ohm >>= D2083_ADC_RESOLUTION;
	ohm /= 1000;
	return (ohm);
}

static u16 d2083_get_target_adc_from_lookup_at_charging(u16 tempk, u16 average_adc, u8 is_charging)
{
	u8 i = 0;
	u16 *plut = NULL;
	int diff;

	if (tempk < BAT_LOW_LOW_TEMPERATURE)		
		plut = &adc2soc_lut.adc_llt[0];
	else if (tempk > BAT_ROOM_TEMPERATURE)
		plut = &adc2soc_lut.adc_rt[0];
	else
		plut = &adc2soc_lut.adc_lt[0];	
	
	for(i = adc2soc_lut_length - 1; i; i--) {
		if(plut[i] <= average_adc)
			break;
	}
	diff = adc_diff_charge[i] + ((average_adc - plut[i])*(adc_diff_charge[i+1]-adc_diff_charge[i])) 
				/ (plut[i+1] - plut[i]);
	
	if(diff < 0)
	{
		pr_info ("Diff can NEVER be less than 0!");
		diff = 0;
	}
	return (u16)diff;
}

static u16 d2083_get_weight_from_lookup(u16 tempk, u16 average_adc, u8 is_charging)
{
	u8 i = 0;
	u16 *plut = NULL;
	int /*percent, */weight = 0;

	// Sanity check.
	if (tempk < BAT_LOW_LOW_TEMPERATURE)		
		tempk = BAT_LOW_LOW_TEMPERATURE;
	else if (tempk > BAT_HIGH_TEMPERATURE)
		tempk = BAT_HIGH_TEMPERATURE;

	// Get the SOC look-up table
	if (tempk > BAT_HIGH_TEMPERATURE) {
		plut = &adc2soc_lut.adc_ht[0];
	}
	else if(tempk < BAT_HIGH_TEMPERATURE && tempk >= BAT_ROOM_TEMPERATURE) {
		plut = &adc2soc_lut.adc_rt[0];
	}
	else if(tempk < BAT_ROOM_TEMPERATURE && tempk >= BAT_LOW_TEMPERATURE) {
		plut = &adc2soc_lut.adc_lt[0];
	}
	else 
		plut = &adc2soc_lut.adc_llt[0];

	for(i = adc2soc_lut_length - 1; i; i--) {
		if(plut[i] <= average_adc)
			break;
	}
	
	if ((tempk <= BAT_HIGH_TEMPERATURE) && (tempk > BAT_ROOM_TEMPERATURE)) {  
		if(is_charging)
		{
			if(average_adc < plut[0]) {
				// under 1% -> fast charging
				weight = adc_weight_section_charge[0];
			}
			else
				weight = adc_weight_section_charge[i];
		}
		else
			weight = adc_weight_section_discharge[i];
	}
	else if ((tempk <= BAT_ROOM_TEMPERATURE) && (tempk > BAT_LOW_TEMPERATURE)) {

		if(is_charging) {
			if(average_adc < plut[0]) {
				i=0;
			}
		
			weight=adc_weight_section_charge_lt[i];
			weight = weight + ((tempk-BAT_LOW_TEMPERATURE)*(adc_weight_section_charge[i]-adc_weight_section_charge_lt[i]))
				/(BAT_ROOM_TEMPERATURE-BAT_LOW_TEMPERATURE);
		
		}
		else {

			weight=adc_weight_section_discharge_lt[i];
			weight = weight + ((tempk-BAT_LOW_TEMPERATURE)*(adc_weight_section_discharge[i]-adc_weight_section_discharge_lt[i]))
				/(BAT_ROOM_TEMPERATURE-BAT_LOW_TEMPERATURE); 
		}
	} else {        

		if(is_charging) {

			if(average_adc < plut[0]) {
				i=0;
			}
			weight=adc_weight_section_charge_llt[i];
			weight = weight + ((tempk-BAT_LOW_LOW_TEMPERATURE)*(adc_weight_section_charge_lt[i]-adc_weight_section_charge_llt[i]))
			/(BAT_LOW_TEMPERATURE-BAT_LOW_LOW_TEMPERATURE); 
		}
		else {
			weight=adc_weight_section_discharge_llt[i];
			weight = weight + ((tempk-BAT_LOW_LOW_TEMPERATURE)*(adc_weight_section_discharge_lt[i]-adc_weight_section_discharge_llt[i]))
				/(BAT_LOW_TEMPERATURE-BAT_LOW_LOW_TEMPERATURE); 
		}
	}

	return weight;	

}


/* 
 * Name : d2083_set_adc_mode
 */
static int d2083_set_adc_mode(struct d2083_battery *pbat, adc_mode type)
{
	if(unlikely(!pbat)) {
		pr_err("%s. Invalid parameter.\n", __func__);
		return -EINVAL;
	}

	if(pbat->adc_mode != type)
	{
		if(type == D2083_ADC_IN_AUTO) {
			pbat->d2083_read_adc = d2083_read_adc_in_auto;
			pbat->adc_mode = D2083_ADC_IN_AUTO;
		}
		else if(type == D2083_ADC_IN_MANUAL) {
			pbat->d2083_read_adc = d2083_read_adc_in_manual;
			pbat->adc_mode = D2083_ADC_IN_MANUAL;
		}
	}
	else {
		pr_info("%s: ADC mode is same before was set \n", __func__);
	}

	return 0;
}


/* 
 * Name : d2083_set_end_of_charge
 */
static void d2083_set_end_of_charge(struct d2083_battery *pbat, u8 end_of_charge)
{
	unsigned char result = 0;

	mutex_lock(&pbat->lock); 
	if(end_of_charge == BAT_END_OF_CHARGE_NONE)
		pbat->battery_data.end_of_charge = end_of_charge;
	else
		result = pbat->battery_data.end_of_charge |= end_of_charge; 		
	mutex_unlock(&pbat->lock);

	pr_info("%s. end_of_charge(%d) result(%d)\n", __func__, end_of_charge, result);
}


/* 
 * Name : d2083_set_battery_health
 */
static void d2083_set_battery_health(struct d2083_battery *pbat, u32 health)
{
	mutex_lock(&pbat->lock);
	pbat->battery_data.health = health;
	mutex_unlock(&pbat->lock);
}


/* 
 * Name : d2083_set_battery_status
 */
static void d2083_set_battery_status(struct d2083_battery *pbat, u32 status)
{
	mutex_lock(&pbat->lock);
	pbat->battery_data.status = status;
	mutex_unlock(&pbat->lock);

	power_supply_changed(&pbat->battery);	
}


/* 
 * Name : d2083_set_charger_type
 */
static void d2083_set_charger_type(struct d2083_battery *pbat, u8 charger_type)
{
	if(charger_type >= CHARGER_TYPE_MAX) {
		pr_err("%s. Invalid parameter(%d)\n", __func__, charger_type);
		return ;
	}

	mutex_lock(&pbat->lock);
	pbat->charger_data.current_charger = charger_type;
	mutex_unlock(&pbat->lock);
}


/* 
 * Name : d2083_set_jig_connected
 */
static void d2083_set_jig_connected(struct d2083_battery *pbat, int jig_connected)
{
	if(jig_connected == pbat->charger_data.jig_connected) {
		pr_info("%s. JIG states is same before was set\n", __func__);	
		return;
	}
	
	pbat->charger_data.jig_connected = jig_connected;
	return;
}


/* 
 * Name : d2083_set_ta_attached
 */
static void d2083_set_ta_attached(struct d2083_battery *pbat, u8 state)
{
	int charger_type = d2083_get_charger_type(pbat);

	pr_info("%s. Start. state(%d), charger_type(%d)\n", __func__, state, charger_type);
	
	if(state) {
		// TA was attached
		if(charger_type == CHARGER_TYPE_NONE)
		{
			wake_lock(&pbat->charger_data.charger_wakeup);
			d2083_set_charger_type(pbat, CHARGER_TYPE_TA);
			d2083_start_charge(pbat, BAT_CHARGE_START_TIMER);
			d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_CHARGING); 
			power_supply_changed(&pbat->wall);
		}
	}
	else {
		// TA was detached
		if(charger_type == CHARGER_TYPE_TA)
        {
			d2083_set_charger_type(pbat, CHARGER_TYPE_NONE);
			d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_NONE);
			d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_DISCHARGING);
			d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_GOOD);
			power_supply_changed(&pbat->wall);
			d2083_stop_charge(pbat, BAT_END_OF_CHARGE_NONE);
			wake_unlock(&pbat->charger_data.charger_wakeup);
		}
	}
}


/* 
 * Name : d2083_set_usb_attached
 */
static void d2083_set_usb_attached(struct d2083_battery *pbat, u8 state)
{
	int charger_type = d2083_get_charger_type(pbat);

	pr_info("%s. Start. state(%d), charger_type(%d)\n", __func__, state, charger_type);

	if(state) {
		if(charger_type == CHARGER_TYPE_NONE)
		{
			wake_lock(&pbat->charger_data.charger_wakeup);
			d2083_set_charger_type(pbat, CHARGER_TYPE_USB);
			d2083_start_charge(pbat, BAT_CHARGE_START_TIMER);
			d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_CHARGING);
			power_supply_changed(&pbat->usb);
		}
	}
	else {
		if(charger_type == CHARGER_TYPE_USB)
		{
			d2083_set_charger_type(pbat, CHARGER_TYPE_NONE);
			d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_NONE);
			d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_DISCHARGING);
			d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_GOOD);
			power_supply_changed(&pbat->usb);
			d2083_stop_charge(pbat, BAT_END_OF_CHARGE_NONE);
			wake_unlock(&pbat->charger_data.charger_wakeup);
		}
	}
}


/* 
 * Name : d2083_set_jig_attached
 */
static void d2083_set_jig_attached(struct d2083_battery *pbat, u8 state)
{
	pr_info("%s. Start. state(%d)\n", __func__, state);

	if(state) {
		d2083_set_jig_connected(pbat, state);
	}
	else {
		d2083_set_jig_connected(pbat, state);
	}
}


/* 
 * Name : d2083_clear_end_of_charge
 */
static u8 d2083_clear_end_of_charge(struct d2083_battery *pbat, u8 end_of_charge)
{
	unsigned char ret = 0;
	
	mutex_lock(&pbat->lock);
	ret = pbat->battery_data.end_of_charge;
	ret &= (~end_of_charge);
	pbat->battery_data.end_of_charge = ret;
	mutex_unlock(&pbat->lock);

	pr_info("%s. end_of_charge(%d) ret(%d)\n", __func__, end_of_charge, ret);

	return ret; 
}


/* 
 * Name : d2083_check_end_of_charge
 */
static s8 d2083_check_end_of_charge(struct d2083_battery *pbat, u8 end_of_charge)
{
	char ret = 0;

	mutex_lock(&pbat->lock);
    ret = pbat->battery_data.end_of_charge;
    mutex_unlock(&pbat->lock);

	ret &= end_of_charge; 
	ret = (ret ? 0 : -1);

	return ret;
}


/* 
 * Name : d2083_check_enable_charge
 */
static int d2083_check_enable_charge(struct d2083_battery *pbat)
{
	int charger_type, ret = 0;
	enum power_supply_type current_charger = POWER_SUPPLY_TYPE_MAINS;


	charger_type = d2083_get_charger_type(pbat);
	if(charger_type == CHARGER_TYPE_TA) {
		current_charger = POWER_SUPPLY_TYPE_MAINS;
	}
	else if(charger_type == CHARGER_TYPE_USB) {
		current_charger = POWER_SUPPLY_TYPE_USB;
	}

	mutex_lock(&pbat->api_lock);
	if(pbat->charger_data.enable_charge) {
		pbat->charger_data.enable_charge(current_charger);
		pbat->charger_data.is_charging = TRUE;
	}
	else {
		ret = -1;
		pr_warn("%s. enable_charge function is NULL\n", __func__);
	}
	mutex_unlock(&pbat->api_lock);

	return ret; 
}


/* 
 * Name : d2083_check_disable_charge
 */
static int d2083_check_disable_charge(struct d2083_battery *pbat, u8 end_of_charge)
{
	int ret = 0;

	mutex_lock(&pbat->api_lock);
	if(pbat->charger_data.disable_charge) {
		pbat->charger_data.disable_charge(end_of_charge);
		pbat->charger_data.is_charging = FALSE;
	}
	else {
		ret = -1;
		pr_warn("%s disable_charge function is NULL\n", __func__);
	}
	mutex_unlock(&pbat->api_lock);
	return ret; 
}


/* 
 * Name : d2083_read_adc_in_auto
 * Desc : Read ADC raw data for each channel.
 * Param : 
 *    - d2083 : 
 *    - channel : voltage, temperature 1, temperature 2, VF and TJUNC
 */
static int d2083_read_adc_in_auto(struct d2083_battery *pbat, adc_channel channel)
{
	u8 adc_channel, adc_res_msb_reg;
	u8 adc_res_lsb_reg, adc_res_lsb_mask;
	u8 msb_res, lsb_res;
	int ret = 0;
	struct d2083_battery_data *pbat_data = &pbat->battery_data;
	struct d2083 *d2083 = pbat->pd2083;

	if(unlikely(!pbat || !pbat_data || !d2083)) {
		pr_err("%s. Invalid argument\n", __func__);
		return -EINVAL;
	}

	// The valid channel is from ADC_VOLTAGE to ADC_AIN in auto mode.
	if(channel >= D2083_ADC_CHANNEL_MAX - 1) {
		pr_err("%s. Invalid channel(%d) in auto mode\n", __func__, channel);
		return -EINVAL;
	}

	mutex_lock(&pbat->meoc_lock);

	pbat_data->adc_res[channel].is_adc_eoc = FALSE;
	pbat_data->adc_res[channel].read_adc = 0;

	adc_channel = adc_cont_inven[channel].adc_cont_val;
	adc_res_msb_reg = adc_cont_inven[channel].adc_msb_res;
	adc_res_lsb_reg = adc_cont_inven[channel].adc_lsb_res;
	adc_res_lsb_mask = adc_cont_inven[channel].adc_lsb_mask;

	// Set ADC_CONT register to select a channel.
	if((ret = d2083_reg_write(d2083, D2083_ADC_CONT_REG, adc_channel)) < 0) {
		ret = -EIO;
		goto out;
	}
	msleep(5);
	// Read result register for requested adc channel
	ret = d2083_reg_read(d2083, adc_res_msb_reg, &msb_res);
	ret |= d2083_reg_read(d2083, adc_res_lsb_reg, &lsb_res);
	lsb_res &= adc_res_lsb_mask;
	if((ret = d2083_reg_write(d2083, D2083_ADC_CONT_REG, 0x00)) < 0)
		goto out;

	// Make ADC result
	pbat_data->adc_res[channel].is_adc_eoc = TRUE;
	pbat_data->adc_res[channel].read_adc =
		((msb_res << 4) | (lsb_res >> (adc_res_lsb_mask == ADC_RES_MASK_MSB ? 4 : 0)));

out:
	mutex_unlock(&pbat->meoc_lock);

	return ret;
}


/* 
 * Name : d2083_read_adc_in_manual
 */
static int d2083_read_adc_in_manual(struct d2083_battery *pbat, adc_channel channel)
{
	u8 mux_sel, flag = FALSE;
	int ret, retries = D2083_MANUAL_READ_RETRIES;
	struct d2083_battery_data *pbat_data = &pbat->battery_data;
	struct d2083 *d2083 = pbat->pd2083;

	mutex_lock(&pbat->meoc_lock);

	pbat_data->adc_res[channel].is_adc_eoc = FALSE;
	pbat_data->adc_res[channel].read_adc = 0;

	switch(channel) {
		case D2083_ADC_VOLTAGE:
			mux_sel = D2083_ADCMAN_MUXSEL_VBAT;
			break;
		case D2083_ADC_TEMPERATURE_1:
			mux_sel = D2083_ADCMAN_MUXSEL_TEMP1;
			break;
		case D2083_ADC_TEMPERATURE_2:
			mux_sel = D2083_ADCMAN_MUXSEL_TEMP2;
			break;
		case D2083_ADC_VF:
			mux_sel = D2083_ADCMAN_MUXSEL_VF;
			break;
		case D2083_ADC_TJUNC:
			mux_sel = D2083_ADCMAN_MUXSEL_TJUNC;
			break;
		default :
			pr_err("%s. Invalid channel(%d) \n", __func__, channel);
			ret = -EINVAL;
			goto out;
	}

	mux_sel |= D2083_ADC_MAN_CONV;
	if((ret = d2083_reg_write(d2083, D2083_ADC_MAN_REG, mux_sel)) < 0)
		goto out;

	do {
		schedule_timeout_interruptible(msecs_to_jiffies(1));
		flag = pbat_data->adc_res[channel].is_adc_eoc;
	} while(retries-- && (flag == FALSE));

	if(flag == FALSE) {
		pr_warn("%s. Failed manual ADC conversion. channel(%d)\n", __func__, channel);
		ret = -EIO;
	}

out:
	mutex_unlock(&pbat->meoc_lock);

	return ret;    
}



/* 
 * Name : d2083_read_voltage
 */
static int d2083_read_voltage(struct d2083_battery *pbat)
{
	int new_vol_adc = 0, base_weight,base_diff_adc, new_vol_orign;
	int battery_status, offset_with_old, offset_with_new = 0;
	int ret = 0;
	static int is_sysparm_init_done = 0;
	struct d2083_battery_data *pbat_data = &pbat->battery_data;
	struct d2083_charger_data *pchg_data = &pbat->charger_data;

	// Read voltage ADC
	ret = pbat->d2083_read_adc(pbat, D2083_ADC_VOLTAGE);

	if(SYSPARM_GetIsInitialized()) {
		// The follwing codes will give information for battery calibraion offset.
		pr_info("%s. SYSPARM_GetDefault4p2VoltReading = %d\n", __func__, SYSPARM_GetDefault4p2VoltReading());
		pr_info("%s. SYSPARM_GetBattEmptyThresh = %d\n", __func__, SYSPARM_GetBattEmptyThresh());
	}

	if(pbat_data->adc_res[D2083_ADC_VOLTAGE].is_adc_eoc) {

		new_vol_orign = new_vol_adc = pbat_data->adc_res[D2083_ADC_VOLTAGE].read_adc;

		if(pbat->battery_data.volt_adc_init_done) {

			battery_status = d2083_get_battery_status(pbat);

			base_weight = d2083_get_weight_from_lookup(
											C2K(pbat_data->average_temperature),
											pbat_data->average_volt_adc,
											pchg_data->is_charging);

			if(pchg_data->is_charging) {


				base_diff_adc = d2083_get_target_adc_from_lookup_at_charging(
											C2K(pbat_data->average_temperature),
											pbat_data->average_volt_adc, //same to SOC
											//new_vol_adc,
											pchg_data->is_charging);
				
				offset_with_new = new_vol_adc - pbat_data->average_volt_adc; 
				// Case of Charging
				// The battery may be discharged, even if a charger is attached.
				if(offset_with_new > base_diff_adc) //charging without load
				{
					new_vol_adc = new_vol_adc - base_diff_adc;
				}
				else //charging with load (over base_diff_adc/2 mV)
				{					
					//new_vol_adc=new_vol_adc - offset_with_new; //by charging
					
					//offset_with_new= base_diff_adc-offset_with_new;
					new_vol_adc = pbat_data->average_volt_adc + (offset_with_new * base_weight / 10000); //by discharge
				}					

				pbat_data->current_volt_adc = new_vol_adc;
				pbat_data->sum_voltage_adc += new_vol_adc;
				pbat_data->sum_voltage_adc -= 
								pbat_data->voltage_adc[pbat_data->voltage_idx];
				pbat_data->voltage_adc[pbat_data->voltage_idx] = new_vol_adc;


			}
			else {
				// Case of Discharging.
				offset_with_new = pbat_data->average_volt_adc - new_vol_adc;
				offset_with_old = pbat_data->voltage_adc[pbat_data->voltage_idx] 
								- pbat_data->average_volt_adc;
			
				#if 0
				if(is_called_by_ticker) {
					base_weight *= 2;
				}
				#endif

				if(offset_with_new > 0) {
					// Battery was discharged by some reason. 
					// So, ADC will be calculated again.
					if(offset_with_new <= 14) {
						new_vol_adc = new_vol_adc + (offset_with_new * 95 / 100);
					}else {
						new_vol_adc = pbat_data->average_volt_adc
										- (offset_with_new * base_weight / 10000) 
										+ (offset_with_old * base_weight / 10000);
					}
				} 

				if(is_called_by_ticker) {
					u8 i = 0;
					
					pbat_data->current_volt_adc = new_vol_adc;
#if 0					
					for(i = 0; i < 16; i++) {
						pbat_data->sum_voltage_adc += new_vol_adc;
						pbat_data->sum_voltage_adc -= 
										pbat_data->voltage_adc[pbat_data->voltage_idx];
						pbat_data->voltage_adc[pbat_data->voltage_idx] = new_vol_adc;
						pbat_data->voltage_idx = (pbat_data->voltage_idx+1) % AVG_SIZE;
					}
#else
					for(i=0 ; i<12 ; i++)
					{
						pbat_data->current_volt_adc = new_vol_adc;
						pbat_data->sum_voltage_adc += new_vol_adc;
						pbat_data->sum_voltage_adc -= 
										pbat_data->voltage_adc[pbat_data->voltage_idx];
						pbat_data->voltage_adc[pbat_data->voltage_idx] = new_vol_adc;	
						pbat_data->voltage_idx = (pbat_data->voltage_idx+1) % AVG_SIZE;
					}
#endif
					
					is_called_by_ticker = 0;
				}
				else {
					pbat_data->current_volt_adc = new_vol_adc;
					pbat_data->sum_voltage_adc += new_vol_adc;
					pbat_data->sum_voltage_adc -= 
									pbat_data->voltage_adc[pbat_data->voltage_idx];
					pbat_data->voltage_adc[pbat_data->voltage_idx] = new_vol_adc;
				}

			}
		}
		else {
			u8 i = 0;
			u8 res_msb, res_lsb;
			int result=0,result_vol_adc=0,result_temp_adc=0;
			int X1 = 200, X0 = -200;
			int Y1 = 330, Y0 = 110;
			int X = pbat_data->average_temperature;

			d2083_reg_read(pbat->pd2083, D2083_GPID5_REG, &res_msb);
			d2083_reg_read(pbat->pd2083, D2083_GPID6_REG, &res_lsb);
			if(res_msb != 0 || res_lsb != 0)
			{
				result_temp_adc = (((res_msb&0xF) << 8) | (res_lsb & 0xFF));
			}

			result=result_temp_adc - pbat_data->average_temp_adc;

			if(result < 0)
				result=-result;
			
			if( result < 1000) //in case the difference of temp is under 10C.
			{				
				d2083_reg_read(pbat->pd2083, D2083_GPID4_REG, &res_msb);
				d2083_reg_read(pbat->pd2083, D2083_GPID3_REG, &res_lsb);
				if(res_msb != 0 || res_lsb != 0)
				{
					result_vol_adc = (((res_msb&0xF) << 8) | (res_lsb & 0xFF));
				}
			}
			
			//new_vol_adc += 80; //52.5mV -> 40mV
			if(C2K(pbat_data->average_temperature) <= BAT_LOW_LOW_TEMPERATURE) {
				new_vol_adc += Y1;
				pr_info("A. Calculated new ADC is %4d \n", new_vol_adc);
			} else if(C2K(pbat_data->average_temperature) >= BAT_ROOM_TEMPERATURE) {
				new_vol_adc += Y0;
				pr_info("B. Calculated new ADC is %4d \n", new_vol_adc);
			} else {
				new_vol_adc = new_vol_adc + Y0 
					+ ((X - X0) * Y1 - (X - X0) * Y0) / (X1 - X0);
				pr_info("C. Calculated new ADC is %4d \n", new_vol_adc);
			}

			// 1. read general_register MSB and LSB of voltage ADC which was stored at bootloader
			// 2. set to new_temp_adc from general register
			// 3. make 12Bits ADC raw data.
			d2083_reg_read(pbat->pd2083, D2083_GPID1_REG, &res_msb);
			d2083_reg_read(pbat->pd2083, D2083_GPID2_REG, &res_lsb);

			result = ((res_msb << 4) | (res_lsb & 0xF));

			// Conpensate charging current.
			if(pchg_data->is_charging) {
				u8 is_cv_charging = 0;
			#if 1   // TSU6111
				u8 read_status;

				fsa9480_read_charger_status(&read_status);
				is_cv_charging = (read_status & (0x1 << 3));
			#else
				if(new_vol_adc > D2083_CHARGE_CV_ADC_LEVEL)
					is_cv_charging = 1;
			#endif

				if(is_cv_charging && new_vol_adc > 3419) {
					// Charging CV                //3565x+y=110, 3420x+y=190
					new_vol_adc= new_vol_adc-(2076-(new_vol_adc*80)/145); //from (190)95mV to (80)40mV (3340 +80 )(4180) (3485 +80 )(4200)
				}
				else {
					// Charging CC
					base_diff_adc = d2083_get_target_adc_from_lookup_at_charging(
											C2K(pbat_data->average_temperature),											
											new_vol_adc-200, //190,
											pchg_data->is_charging);
					new_vol_adc -= base_diff_adc; //95mV TI charge 450mA charging 
				}
			}

			result=result_vol_adc - new_vol_adc;

			if(result < 0)
				result=-result;
			
			if(result < 100) //50mV
				new_vol_adc=(result_vol_adc*40+new_vol_adc*60)/100;
				
			for(i = 0; i < AVG_SIZE; i++) {
				pbat_data->voltage_adc[i] = new_vol_adc;
				pbat_data->sum_voltage_adc += new_vol_adc;
			}
			
			pbat_data->current_volt_adc = new_vol_adc;
			pbat->battery_data.volt_adc_init_done = TRUE;
			power_supply_changed(&pbat->battery);
		}

		pbat_data->origin_volt_adc = new_vol_orign;
		pbat_data->average_volt_adc = pbat_data->sum_voltage_adc >> AVG_SHIFT;
		pbat_data->voltage_idx = (pbat_data->voltage_idx+1) % AVG_SIZE;
		pbat_data->current_voltage = adc_to_vbat(pbat_data->current_volt_adc,
											 pbat->charger_data.pmic_vbus_state);
		pbat_data->average_voltage = adc_to_vbat(pbat_data->average_volt_adc,
											 pbat->charger_data.pmic_vbus_state);

		pr_info("# SOC = %3d %%, Weight = %4d, ADC(oV) = %4d, ADC(aV) = %4d, OFST = %4d, Voltage = %4d mV, ADC(T) = %4d, ADC(VF) = %4d, Temp = %2d.%d C\n",
					pbat->battery_data.soc,
					base_weight,
					pbat->battery_data.origin_volt_adc,
					pbat->battery_data.average_volt_adc,
					offset_with_new,
					pbat->battery_data.average_voltage, 
					pbat->battery_data.average_temp_adc,
					pbat->battery_data.vf_adc,
					(pbat->battery_data.average_temperature/10),
					(pbat->battery_data.average_temperature%10));
	}
	else {
		pr_err("%s. Voltage ADC read failure \n", __func__);
		ret = -EIO;
	}
	//mutex_unlock(&pbat->lock);

	return ret;
}


/*
 * Name : d2083_read_temperature
 */
static int d2083_read_temperature(struct d2083_battery *pbat)
{
	u16 new_temp_adc = 0;
	int ret = 0;
	struct d2083_battery_data *pbat_data = &pbat->battery_data;

	//mutex_lock(&pbat->lock);

	// Set temperature ISRC bit.
	if(pbat->adc_mode == D2083_ADC_IN_MANUAL)
		d2083_set_bits(pbat->pd2083, D2083_ADC_CONT_REG, D2083_ADCCONT_TEMP2_ISRC_EN);

	// Read temperature ADC
	ret = pbat->d2083_read_adc(pbat, D2083_ADC_TEMPERATURE_2);

	if(pbat_data->adc_res[D2083_ADC_TEMPERATURE_2].is_adc_eoc) {
		new_temp_adc = pbat_data->adc_res[D2083_ADC_TEMPERATURE_2].read_adc;


		pbat_data->current_temp_adc = new_temp_adc;

		if(pbat_data->temp_adc_init_done) {
			pbat_data->sum_temperature_adc += new_temp_adc;
			pbat_data->sum_temperature_adc -= 
						pbat_data->temperature_adc[pbat_data->temperature_idx];
			pbat_data->temperature_adc[pbat_data->temperature_idx] = new_temp_adc;
		}
		else {
			u8 i = 0;

			for(i = 0; i < AVG_SIZE; i++) {
				pbat_data->temperature_adc[i] = new_temp_adc;
				pbat_data->sum_temperature_adc += new_temp_adc;
			}

			pbat->battery_data.temp_adc_init_done = TRUE;
		}

		pbat_data->average_temp_adc =
								pbat_data->sum_temperature_adc >> AVG_SHIFT;
		pbat_data->temperature_idx = (pbat_data->temperature_idx+1) % AVG_SIZE;
		pbat_data->average_temperature = 
					degree_k2c(adc_to_degree_k(pbat_data->average_temp_adc));
		pbat_data->current_temperature = 
									degree_k2c(adc_to_degree_k(new_temp_adc)); 

	}
	else {
		pr_err("%s. Temperature ADC read failed \n", __func__);
		ret = -EIO;
	}

	// Set temperature ISRC bit.
	if(pbat->adc_mode == D2083_ADC_IN_MANUAL)
		d2083_clear_bits(pbat->pd2083, D2083_ADC_CONT_REG, D2083_ADCCONT_TEMP2_ISRC_EN);

	return ret;
}


/* 
 * Name : d2083_read_vf
 */
static int d2083_read_vf(struct d2083_battery *pbat)
{
	int i, ret = 0;
	unsigned int sum = 0, read_adc;
	struct d2083_battery_data *pbat_data = &pbat->battery_data;

	if(pbat == NULL || pbat_data == NULL) {
		pr_err("%s. Invalid Parameter \n", __func__);
		return -EINVAL;
	}

	// Read VF ADC
	for(i = 4; i; i--) {
		ret = pbat->d2083_read_adc(pbat, D2083_ADC_VF);

		if(pbat_data->adc_res[D2083_ADC_VF].is_adc_eoc) {
			mutex_lock(&pbat->api_lock);
			read_adc = pbat_data->adc_res[D2083_ADC_VF].read_adc;
			sum += read_adc;
			pbat_data->vf_ohm = d2083_get_vf_ohm(pbat_data->vf_adc);
			mutex_unlock(&pbat->api_lock);
		}
		else {
			pr_err("%s. VF ADC read failure \n", __func__);
			ret = -EIO;
		}
	}

	if(i == 0) {
		// getting average and store VF ADC to member of structure.
		pbat_data->vf_adc = sum >> 2;
	}

	return 0;
}


/* 
 * Name : d2083_start_charge
 */
static void d2083_start_charge(struct d2083_battery *pbat, u32 timer_type)
{
	u32 time = 0;

	pr_info("%s. Start\n", __func__);

	if(d2083_check_enable_charge(pbat) < 0) {
		pr_err("%s. Failed to enable charge\n", __func__);
	}

	if(timer_pending(&pbat->charge_timer)) {
		pr_info("%s Charge_timer is running. Delete charge_timer\n", __func__);
		del_timer(&pbat->charge_timer);
	}

	if(timer_pending(&pbat->recharge_start_timer)) {
		pr_info("%s. recharge_start_timer is running. Delete reCharge_start_timer\n", __func__);
		if(d2083_check_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER) == 0) {
			d2083_clear_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER);
		}
		del_timer(&pbat->recharge_start_timer);
	}

	if(timer_type == BAT_CHARGE_START_TIMER) {
		if(pbat->battery_data.capacity < BAT_CAPACITY_1500MA)
			time = BAT_CHARGE_TIMER_5HOUR;
		else if(pbat->battery_data.capacity < BAT_CAPACITY_2000MA)
			time = BAT_CHARGE_TIMER_6HOUR;
		else if(pbat->battery_data.capacity < BAT_CAPACITY_4500MA)
			time = BAT_CHARGE_TIMER_8HOUR;
		else if(pbat->battery_data.capacity < BAT_CAPACITY_7000MA)
			time = BAT_CHARGE_TIMER_10HOUR;
		else {
			time = BAT_CHARGE_TIMER_5HOUR;
			pr_warn("%s. wrong battery capacity %d\n", __func__,
													pbat->battery_data.capacity);
		}
	}
	else if(timer_type == BAT_CHARGE_RESTART_TIMER)
		time = BAT_CHARGE_TIMER_90MIN;

	pbat->charge_timer.expires = jiffies + time; 
	add_timer(&pbat->charge_timer);	

	return;
}


/* 
 * Name : d2083_stop_charge
 */
static void d2083_stop_charge(struct d2083_battery *pbat, u8 end_of_charge)
{
	pr_info("%s. Start\n", __func__);
	if(d2083_check_disable_charge(pbat,end_of_charge) < 0) {
		pr_warn("%s. Failed to disable_charge\n", __func__);
	}

	if(timer_pending(&pbat->charge_timer)) {
		printk("%s Charge_timer is running. Delete Charge_timer\n", __func__);
		del_timer(&pbat->charge_timer);
	}

	if(timer_pending(&pbat->recharge_start_timer)) 	{
		pr_info("%s recharge_start_timer is running. Delete recharge_start_timer\n", __func__);
		if(d2083_check_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER)==0)
		{
			d2083_clear_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER);
		}
		del_timer(&pbat->recharge_start_timer);
	}
}


/* 
 * Name : d2083_battery_charge_full
 */
static void d2083_battery_charge_full(struct d2083_battery *pbat)
{
	int soc = 0;

	pr_info("%s. charger_type (%d)\n", __func__, d2083_get_charger_type(pbat));

	if(d2083_get_charger_type(pbat) != CHARGER_TYPE_NONE) {

		soc = d2083_get_battery_capacity(pbat);	
		if(soc != 100) {
			if(soc >= 99) {
				pbat->battery_data.soc = 100;
			}
			pr_info("%s. SOC : %d\n", __func__, soc);
		}

		d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_FULL);
		d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_FULL);
		d2083_stop_charge(pbat, BAT_END_OF_CHARGE_BY_FULL);
		power_supply_changed(&pbat->battery);
	}
	else {
		pr_info("%s. Can not make battery full. There is no charger\n", __func__);
	}
}


/* 
 * Name : d2083_ovp_charge_stop
 */
static void d2083_ovp_charge_stop(struct d2083_battery *pbat)
{
	int battery_status;

	if(d2083_get_charger_type(pbat) != CHARGER_TYPE_NONE)
	{
		d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_OVERVOLTAGE);

		battery_status = d2083_get_battery_status(pbat);

		if(battery_status == POWER_SUPPLY_STATUS_CHARGING) {                                                          
			pr_info("%s. Stop charging by OVP\n", __func__);                                                         
			d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_OVP);          
			d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_NOT_CHARGING);
			d2083_stop_charge(pbat, BAT_END_OF_CHARGE_BY_OVP);                         
		}                                                          
		else if((battery_status == POWER_SUPPLY_STATUS_NOT_CHARGING)
				|| (battery_status == POWER_SUPPLY_STATUS_FULL))                                                                     
		{       
			pr_info("%s. Charging had already been stopped\n", __func__);         
			d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_OVP);          
		}
	}
}


/* 
 * Name : d2083_ovp_charge_restart
 */
static void d2083_ovp_charge_restart(struct d2083_battery *pbat)
{
	if(d2083_get_charger_type(pbat) != CHARGER_TYPE_NONE)
	{
		if(d2083_check_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_OVP)==0)
		{
			pr_info("%s. Restart charge. Device recorver OVP status\n", __func__);
			d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_NONE);
			d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_GOOD);
			d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_CHARGING);
			d2083_start_charge(pbat, BAT_CHARGE_START_TIMER);
		}
	}
}


/* 
 * Name : d2083_vf_charge_stop
 */
static void d2083_vf_charge_stop(struct d2083_battery *pbat)
{
	int battery_status;
	
	if(d2083_get_charger_type(pbat) != CHARGER_TYPE_NONE)
	{
		d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_UNSPEC_FAILURE);
		battery_status = d2083_get_battery_status(pbat);

		if(battery_status == POWER_SUPPLY_STATUS_CHARGING)
		{                                                          
			pr_info("%s. Stop charging by vf open\n", __func__);                                                         
			d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_VF_OPEN);          
			d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_NOT_CHARGING);
			d2083_stop_charge(pbat, BAT_END_OF_CHARGE_BY_VF_OPEN);                         
		}                                                          
		else if((battery_status == POWER_SUPPLY_STATUS_NOT_CHARGING)
			|| (battery_status == POWER_SUPPLY_STATUS_FULL))                                                                     
		{       
			pr_info("%s. Charging had been stopped\n", __func__);         
			d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_VF_OPEN);          
		}
	}
}


/* 
 * Name : d2083_ta_get_property
 */
static int d2083_ta_get_property(struct power_supply *psy,
                                    enum power_supply_property psp,
                                    union power_supply_propval *val)
{
	int charger_type, ret = 0;
	struct d2083_battery *pbat = dev_get_drvdata(psy->dev->parent);
	struct d2083_battery_data *pbat_data = &pbat->battery_data;

	if(unlikely(!pbat || !pbat_data)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	switch(psp) {
		case POWER_SUPPLY_PROP_ONLINE:
				charger_type = d2083_get_charger_type(pbat);
				if(charger_type == CHARGER_TYPE_TA)
					val->intval = 1;
				else
					val->intval = 0;
				pr_info("%s. charger_type (%d), intval (%d) \n", __func__, charger_type, val->intval);
			break;
		default:
			pr_info("%s. TA : Property(%d) was not implemented\n", __func__, psp);
			ret = -EINVAL;
			break;
	}

	return ret;
}


/* 
 * Name : d2083_usb_get_property
 */
static int d2083_usb_get_property(struct power_supply *psy,
                                    enum power_supply_property psp,
                                    union power_supply_propval *val)
{
	int charger_type, ret = 0;
	struct d2083_battery *pbat = dev_get_drvdata(psy->dev->parent);
	struct d2083_battery_data *pbat_data = &pbat->battery_data;

	if(unlikely(!pbat || !pbat_data)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	switch(psp) {
		case POWER_SUPPLY_PROP_ONLINE:
			charger_type = d2083_get_charger_type(pbat);\
			if(charger_type == CHARGER_TYPE_USB)
				val->intval = 1;
			else
				val->intval = 0;
			pr_info("%s. charger_type (%d), intval (%d)\n", __func__, charger_type, val->intval);
			break;
		default:
			pr_info("%s. USB : Property(%d) was not implemented\n", __func__, psp);
			ret = -EINVAL;
			break;
	}

	return ret;
}



/* 
 * Name : d2083_battery_get_property
 */
static int d2083_battery_get_property(struct power_supply *psy,
                                    enum power_supply_property psp,
                                    union power_supply_propval *val)
{
	int ret = 0;
	struct d2083_battery *pbat = dev_get_drvdata(psy->dev->parent);

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	switch(psp) {
		case POWER_SUPPLY_PROP_STATUS:
			val->intval = d2083_get_battery_status(pbat);
			if(val->intval < 0)
				ret = val->intval;
			break;
		case POWER_SUPPLY_PROP_HEALTH:
			val->intval = d2083_get_battery_health(pbat);
			if(val->intval < 0)
				ret = val->intval;
			break;
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = pbat->battery_data.battery_present;
			break;
		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = d2083_get_battery_capacity(pbat);
			if(val->intval < 0)
				ret = val->intval;
			break;
		case POWER_SUPPLY_PROP_TECHNOLOGY:
			val->intval = d2083_get_battery_technology(pbat);
			if(val->intval < 0)
				ret = val->intval;
			break;
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
			val->intval = (d2083_get_average_voltage(pbat) * 1000);
			if(val->intval < 0)
				ret = val->intval;
			break;
		case POWER_SUPPLY_PROP_VOLTAGE_AVG:
			val->intval = (d2083_get_average_voltage(pbat) * 1000);
			if(val->intval < 0)
				ret = val->intval;
			break;
		case POWER_SUPPLY_PROP_TEMP:
			val->intval = d2083_get_average_temperature(pbat);
			if(val->intval < 0)
				ret = val->intval;
			break;
		case POWER_SUPPLY_PROP_BATT_TEMP_ADC:
			val->intval = d2083_get_average_temperature_adc(pbat);
			if(val->intval < 0)
				ret = val->intval;
			break;
		default:
			pr_info("%s. Battery : Property(%d) was not implemented\n", __func__, psp);
			ret = -EINVAL;
			break;
	}

	return ret;    
}


/******************************************************************************
    Interrupt Handler
******************************************************************************/
/* 
 * Name : d2083_battery_vf_handler
 */
static irqreturn_t d2083_battery_vf_handler(int irq, void *data)
{
	struct d2083_battery *pbat = (struct d2083_battery *)data;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	//pr_warn("WARNING !!! Invalid Battery inserted. \n");
	

	return IRQ_HANDLED;
}

/* 
 * Name : d2083_battery_tbat2_handler
 */
static irqreturn_t d2083_battery_tbat2_handler(int irq, void *data)
{
	struct d2083_battery *pbat = (struct d2083_battery *)data;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	//pr_warn("WARNING !!! Invalid Battery inserted. \n");
	

	return IRQ_HANDLED;
}

/* 
 * Name : d2083_battery_vdd_low_handler
 */
static irqreturn_t d2083_battery_vdd_low_handler(int irq, void *data)
{
	struct d2083_battery *pbat = (struct d2083_battery *)data;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	//pr_warn("WARNING !!! Low Battery... \n");
	

	return IRQ_HANDLED;
}

/* 
 * Name : d2083_battery_vdd_mon_handler
 */
static irqreturn_t d2083_battery_vdd_mon_handler(int irq, void *data)
{
	struct d2083_battery *pbat = (struct d2083_battery *)data;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	//pr_warn("WARNING !!! Invalid Battery inserted. \n");
	

	return IRQ_HANDLED;
}

/* 
 * Name : d2083_battery_tbat1_handler
 */
static irqreturn_t d2083_battery_tbat1_handler(int irq, void *data)
{
	struct d2083_battery *pbat = (struct d2083_battery *)data;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	//pr_warn("WARNING !!! Invalid Battery inserted. \n");
	

	return IRQ_HANDLED;
}

/* 
 * Name : d2083_battery_adceom_handler
 */
static irqreturn_t d2083_battery_adceom_handler(int irq, void *data)
{
	u8 read_msb, read_lsb, channel;
	int ret = 0;
	struct d2083_battery *pbat = (struct d2083_battery *)data;
	struct d2083 *d2083 = NULL;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	d2083 = pbat->pd2083;
	
	/* A manual ADC has 12 bit resolution */
	ret = d2083_reg_read(d2083, D2083_ADC_RES_H_REG, &read_msb);
	ret |= d2083_reg_read(d2083, D2083_ADC_RES_L_REG, &read_lsb);
	ret |= d2083_reg_read(d2083, D2083_ADC_MAN_REG, &channel);
	
	channel = (channel & 0xF);
	
	switch(channel) {
		case D2083_ADCMAN_MUXSEL_VBAT:
			channel = D2083_ADC_VOLTAGE;
			break;
		case D2083_ADCMAN_MUXSEL_TEMP1:
			channel = D2083_ADC_TEMPERATURE_1;
			break;
		case D2083_ADCMAN_MUXSEL_TEMP2:
			channel = D2083_ADC_TEMPERATURE_2;
			break;
		case D2083_ADCMAN_MUXSEL_VF:
			channel = D2083_ADC_VF;
			break;
		case D2083_ADCMAN_MUXSEL_TJUNC:
			channel = D2083_ADC_TJUNC;
			break;
		default :
			pr_err("%s. Invalid channel(%d) \n", __func__, channel);
			goto out;
	}

	pbat->battery_data.adc_res[channel].is_adc_eoc = TRUE;
	pbat->battery_data.adc_res[channel].read_adc = 
						((read_msb << 4) | (read_lsb & ADC_RES_MASK_LSB));

out:
	//pr_info("%s. Manual ADC (%d) = %d\n", 
	//			__func__, channel,
	//			pbat->battery_data.adc_res[channel].read_adc);

	return IRQ_HANDLED;
}

/* 
 * Name : d2083_battery_ta_handler
 */
static irqreturn_t d2083_battery_ta_handler(int irq, void *data)
{
	struct d2083_battery *pbat = (struct d2083_battery *)data;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	return IRQ_HANDLED;
}

/* 
 * Name : d2083_battery_njigon_handler
 */
static irqreturn_t d2083_battery_njigon_handler(int irq, void *data)
{
	struct d2083_battery *pbat = (struct d2083_battery *)data;

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return -EINVAL;
	}

	//pr_warn("WARNING !!! Invalid Battery inserted. \n");
	

	return IRQ_HANDLED;
}


/* 
 * Name : d2083_charge_timer_expired
 */
static void d2083_charge_timer_expired(unsigned long data)
{
	struct d2083_battery *pbat = (struct d2083_battery*)data; 

	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return;
	}

	pr_info("%s\n", __func__);

	schedule_delayed_work(&pbat->charge_timer_work, 0);

	return;
}


/* 
 * Name : d2083_recharge_timer_expired
 */
static void d2083_recharge_timer_expired(unsigned long data)
{
	struct d2083_battery *pbat = (struct d2083_battery*)data; 


	if(unlikely(!pbat)) {
		pr_err("%s. Invalid driver data\n", __func__);
		return;
	}

	pr_info("%s\n", __func__);

	schedule_delayed_work(&pbat->recharge_start_timer_work, 0);

	return;
}


/* 
 * Name : d2083_sleep_monitor
 */
static void d2083_sleep_monitor(struct d2083_battery *pbat)
{
	schedule_delayed_work(&pbat->sleep_monitor_work, 0);

	return;
}


/* 
 * Name : d2083_monitor_voltage_work
 */
extern void d2083_set_adc_rpc(int result);

static void d2083_monitor_voltage_work(struct work_struct *work)
{
	u8 end_of_charge = 0;
	static u8 loop_count = 0;
	int ret=0, charger_type = CHARGER_TYPE_NONE;
	struct d2083_battery *pbat = container_of(work, struct d2083_battery, monitor_volt_work.work);
	struct d2083_battery_data *pbat_data = &pbat->battery_data;
	int result, bcm_temp_result, bcm_volt_result;

	if(unlikely(!pbat || !pbat_data)) {
		pr_err("%s. Invalid driver data\n", __func__);
		goto err_adc_read;
	}

	if(pbat->charger_data.enable_charge == NULL) {
		pr_warn("Wait to register enable and disable charge function\n");
		goto err_adc_read;
	}

	ret = d2083_read_voltage(pbat);
	if(ret < 0)
	{
		pr_err("%s. Read voltage ADC failure\n", __func__);
		goto err_adc_read;
	}

	ret = d2083_get_soc(pbat);

	if((d2083_get_charger_type(pbat) != CHARGER_TYPE_NONE) 
		&& (d2083_get_battery_health(pbat) != POWER_SUPPLY_HEALTH_UNSPEC_FAILURE)
		&& (d2083_get_jig_state(pbat) == 0)) {

		ret = d2083_read_vf(pbat); 
		if(!ret) {

			if(pbat_data->vf_lower >= pbat_data->vf_upper)
			{
				pr_err("%s. Please check battery. vf_lower(%d) vf_upper(%d)\n",
							__func__, pbat_data->vf_lower, pbat_data->vf_upper);
			}
			else if(pbat_data->vf_adc == 0xFFF) {
				// In case of, battery was removed.
				pbat_data->battery_present = FALSE;
				power_supply_changed(&pbat->battery);
			}
			else
			{
				if((pbat_data->vf_adc < pbat_data->vf_lower) 
					|| (pbat_data->vf_adc > pbat_data->vf_upper))
				{
					pr_err("%s. Wrong battery detected. VF(%d), vf_lower(%d), vf_upper(%d)\n",
						 __func__, pbat_data->vf_adc, pbat_data->vf_lower, pbat_data->vf_upper);
					d2083_vf_charge_stop(pbat);
				}
			}
		}
		else {
			pr_err("%s. Read VF ADC failure\n", __func__);
		}
	}

	if(pbat->battery_data.volt_adc_init_done) {

		charger_type = d2083_get_charger_type(pbat);
		// Sampling time is 3 seconds for charging with Wall(TA) and USB charger
		if(charger_type == CHARGER_TYPE_TA || charger_type == CHARGER_TYPE_USB)
			schedule_delayed_work(&pbat->monitor_volt_work, D2083_VOLTAGE_MONITOR_FAST);
		else {
			if(loop_count <= 1) {
				if(pbat_data->voltage_idx == AVG_SIZE - 1)
					loop_count++;
				schedule_delayed_work(&pbat->monitor_volt_work, (20 * HZ));
			}
			else {
				schedule_delayed_work(&pbat->monitor_volt_work, D2083_VOLTAGE_MONITOR_NORMAL);
			}
		}
	}
	else {
		schedule_delayed_work(&pbat->monitor_volt_work, D2083_VOLTAGE_MONITOR_FAST);
	}

	// check recharge condition 
	if((d2083_get_battery_status(pbat) == POWER_SUPPLY_STATUS_FULL) 
		&& (adc_to_vbat(pbat_data->origin_volt_adc, pbat->charger_data.is_charging) < BAT_CHARGING_RESTART_VOLTAGE) 
		&& (d2083_check_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_FULL)==0)) {
		end_of_charge = d2083_clear_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_FULL);

		if(end_of_charge == BAT_END_OF_CHARGE_NONE) {
			pr_info("%s. Restart charging. Voltage is lower than %04d mV\n", 
						__func__, BAT_CHARGING_RESTART_VOLTAGE);
			d2083_start_charge(pbat, BAT_CHARGE_RESTART_TIMER);			
			// TODO: Need to check more. d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_CHARGING);
		}
		else {
			pr_info("%s. Can't restart charge. Reason is %d\n", __func__, end_of_charge); 
		}
	}

	// Get BCM PMU ADC level for temperature and voltage
#if 1   // Modified by Eric at 2012/08/23 
	//In case of 12Bits
	result = (((pbat_data->average_volt_adc & 0xFFF) << 12) 
				| (pbat_data->average_temp_adc & 0xFFF));
#else
	// The resolution of result are 10 Bits.
	bcm_temp_result = degree_to_bcmpmu_adc(pbat_data);
	bcm_volt_result = (((pbat_data->average_voltage - 2500) << 10) /2000);

	
	result = ((bcm_volt_result & 0x3FF) << 20)
				| ((bcm_temp_result & 0x3FF) << 10) | (bcm_temp_result & 0x3FF);
#endif
	d2083_set_adc_rpc(result);

	return;

err_adc_read:
	schedule_delayed_work(&pbat->monitor_volt_work, D2083_VOLTAGE_MONITOR_START);
	return;
}


static void d2083_monitor_temperature_work(struct work_struct *work)
{
	struct d2083_battery *pbat = container_of(work, struct d2083_battery, monitor_temp_work.work);
	int ret = 0;
	int battery_health, average_temperature;
	unsigned char end_of_charge = 0;

	if(pbat->charger_data.enable_charge == NULL) {
		pr_warn(" ### Wait to register enable and disable charge function ### \n");
		goto err_adc_read;
	}

	ret = d2083_read_temperature(pbat);
	if(ret < 0) {
		pr_err("%s. Failed to read_temperature\n", __func__);
		schedule_delayed_work(&pbat->monitor_temp_work, D2083_TEMPERATURE_MONITOR_NORMAL);
		return;
	}

	if(pbat->battery_data.temp_adc_init_done) {
		schedule_delayed_work(&pbat->monitor_temp_work, D2083_TEMPERATURE_MONITOR_NORMAL);
	}
	else {
		schedule_delayed_work(&pbat->monitor_temp_work, D2083_TEMPERATURE_MONITOR_FAST);
	}


	battery_health = d2083_get_battery_health(pbat);
	if((battery_health == POWER_SUPPLY_HEALTH_COLD) 
		|| (battery_health == POWER_SUPPLY_HEALTH_OVERHEAT))
	{
		average_temperature = d2083_get_average_temperature(pbat);

		if((average_temperature <= CHARGING_RESTART_HIGH_TEMPERATURE)
			 && (average_temperature >= CHARGING_RESTART_LOW_TEMPERATURE))
		 {

			d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_GOOD);

			if(d2083_check_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TEMPERATURE) == 0)
			{	
				end_of_charge = d2083_clear_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TEMPERATURE);
				
				if(end_of_charge == BAT_END_OF_CHARGE_NONE) {
					pr_info("%s. Restart charge. Temperature is in normal\n", 
																	__func__);
					d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_CHARGING);
					d2083_start_charge(pbat, BAT_CHARGE_START_TIMER);
				}				
				else {
					pr_warn("%s. Can't restart charge. Reason is %d\n", 
													__func__, end_of_charge); 
				}
			}
			else {
				pr_info("%s. Temperature is in normal\n", __func__);
			}
		}
		else
		{
			if(d2083_get_battery_status(pbat) == POWER_SUPPLY_STATUS_CHARGING)
			{
				pr_info("%s. Stop charging. Insert TA during HEALTH_COLD or HEALTH_OVERHEAT\n", __func__);
				d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TEMPERATURE);
				d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_NOT_CHARGING);
				d2083_stop_charge(pbat, BAT_END_OF_CHARGE_BY_TEMPERATURE);
			}
		}
	}
	else {
		int average_temperature = d2083_get_average_temperature(pbat);

		if((d2083_get_charger_type(pbat) != CHARGER_TYPE_NONE)  
			 && ((average_temperature >= CHARGING_STOP_HIGH_TEMPERATURE) 
			 	|| (average_temperature <= CHARGING_STOP_LOW_TEMPERATURE)))
	 	{

			if(average_temperature >= CHARGING_STOP_HIGH_TEMPERATURE)
				d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_OVERHEAT);
			else if(average_temperature <= CHARGING_STOP_LOW_TEMPERATURE)
				d2083_set_battery_health(pbat, POWER_SUPPLY_HEALTH_COLD);

			if(d2083_get_battery_status(pbat) == POWER_SUPPLY_STATUS_CHARGING)
			{
				pr_info("%s. Stop charging by HIGH_TEMPERATURE or LOW_TEMPERATURE\n", __func__);
				d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TEMPERATURE);
				d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_NOT_CHARGING);
				d2083_stop_charge(pbat, BAT_END_OF_CHARGE_BY_TEMPERATURE);
			}					
			else if((d2083_get_battery_status(pbat) == POWER_SUPPLY_STATUS_NOT_CHARGING)
					|| (d2083_get_battery_status(pbat) == POWER_SUPPLY_STATUS_FULL))
			{
				pr_info("%s. Already charging had been stopped\n", __func__);
				d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TEMPERATURE);
			}
		}
	}

	return ;

err_adc_read :
	schedule_delayed_work(&pbat->monitor_temp_work, D2083_TEMPERATURE_MONITOR_FAST);
	return ;
}


/* 
 * Name : d2083_info_notify_work
 */
static void d2083_info_notify_work(struct work_struct *work)
{
	struct d2083_battery *pbat = container_of(work, 
												struct d2083_battery, 
												info_notify_work.work);

	power_supply_changed(&pbat->battery);	
	schedule_delayed_work(&pbat->info_notify_work, D2083_NOTIFY_INTERVAL);
}


/* 
 * Name : d2083_charge_timer_work
 */
static void d2083_charge_timer_work(struct work_struct *work)
{
	struct d2083_battery *pbat = container_of(work, struct d2083_battery, charge_timer_work.work);

	pr_info("%s. Start\n", __func__);

	d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER);
	d2083_set_battery_status(pbat, POWER_SUPPLY_STATUS_FULL);
	d2083_stop_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER);

	pbat->recharge_start_timer.expires = jiffies + BAT_RECHARGE_CHECK_TIMER_30SEC;
	add_timer(&pbat->recharge_start_timer);

	return;
}


/* 
 * Name : d2083_recharge_start_timer_work
 */
static void d2083_recharge_start_timer_work(struct work_struct *work)
{
	u8 end_of_charge = 0;	
	struct d2083_battery *pbat = container_of(work, 
												struct d2083_battery, 
												recharge_start_timer_work.work);

	pr_info("%s. Start\n", __func__);

	if(d2083_check_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER) == 0)
	{
		end_of_charge = d2083_clear_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_TIMER);
		if(end_of_charge == BAT_END_OF_CHARGE_NONE)
		{
			if((d2083_get_battery_status(pbat) == POWER_SUPPLY_STATUS_FULL) 
				&& (d2083_get_average_voltage(pbat) < BAT_CHARGING_RESTART_VOLTAGE))
			{
				pr_info("%s. Restart charge. Voltage is lower than %04d mV\n", 
									__func__, BAT_CHARGING_RESTART_VOLTAGE);
				d2083_start_charge(pbat, BAT_CHARGE_RESTART_TIMER);
			}
			else
			{
				pr_info("%s. set BAT_END_OF_CHARGE_BY_FULL. Voltage is higher than %04d mV\n", 
								__func__, BAT_CHARGING_RESTART_VOLTAGE);
				d2083_set_end_of_charge(pbat, BAT_END_OF_CHARGE_BY_FULL);	
			}
		}
		else {
			pr_info("%s. Can't restart charge. The reason why is %d\n", __func__, end_of_charge);	
		}
	}
	else {
		pr_info("%s. SPA_END_OF_CHARGE_BY_TIMER had been cleared by other reason\n", __func__); 
	}
}


/* 
 * Name : d2083_sleep_monitor_work
 */
static void d2083_sleep_monitor_work(struct work_struct *work)
{
	struct d2083_battery *pbat = container_of(work, struct d2083_battery, 
												sleep_monitor_work.work);

	is_called_by_ticker = 1;
	wake_lock_timeout(&pbat->battery_data.sleep_monitor_wakeup, 
									D2083_SLEEP_MONITOR_WAKELOCK_TIME);
	pr_info("%s. Start. Ticker was set to 1\n", __func__);
	if(schedule_delayed_work(&pbat->monitor_volt_work, 0) == 0) {
		cancel_delayed_work_sync(&pbat->monitor_volt_work);
		schedule_delayed_work(&pbat->monitor_volt_work, 0);
	}
	if(schedule_delayed_work(&pbat->monitor_temp_work, 0) == 0) {
		cancel_delayed_work_sync(&pbat->monitor_temp_work);
		schedule_delayed_work(&pbat->monitor_temp_work, 0);
	}
	if(schedule_delayed_work(&pbat->info_notify_work, 0) == 0) {
		cancel_delayed_work_sync(&pbat->info_notify_work);
		schedule_delayed_work(&pbat->info_notify_work, 0);
	}

	return ;	
}


/* 
 * Name : d2083_battery_init
 */
static void d2083_battery_data_init(struct d2083_battery *pbat)
{
	struct d2083_battery_data *pbat_data = &pbat->battery_data;
	struct d2083_charger_data *pchg_data = &pbat->charger_data;

	if(unlikely(!pbat_data || !pchg_data)) {
		pr_err("%s. Invalid platform data\n", __func__);
		return;
	}

	tick_count = 0;
	pbat->adc_mode = D2083_ADC_MODE_MAX;

	pbat_data->vdd_hwmon_level = 0;
	pbat_data->volt_adc_init_done = FALSE;
	pbat_data->temp_adc_init_done = FALSE;
	pbat_data->battery_present = TRUE;
	pbat_data->health = POWER_SUPPLY_HEALTH_GOOD;
	pbat_data->status = POWER_SUPPLY_STATUS_DISCHARGING;
	pbat_data->end_of_charge = BAT_END_OF_CHARGE_NONE;

	pchg_data->is_charging = FALSE;
	pchg_data->current_charger = CHARGER_TYPE_NONE;

	// TODO: Please, Checking about naming.
	wake_lock_init(&pchg_data->charger_wakeup, WAKE_LOCK_SUSPEND, "charger_wakeups");
	wake_lock_init(&pbat_data->sleep_monitor_wakeup, WAKE_LOCK_SUSPEND, "sleep_monitor");

	init_timer(&pbat->charge_timer);
	pbat->charge_timer.function = d2083_charge_timer_expired;
	pbat->charge_timer.data = (u_long)pbat; 

	init_timer(&pbat->recharge_start_timer);
	pbat->recharge_start_timer.function = d2083_recharge_timer_expired;
	pbat->recharge_start_timer.data = (u_long)pbat; 

	pbat_data->capacity = pbat->pd2083->pdata->pbat_platform->battery_capacity;
	pbat_data->battery_technology = pbat->pd2083->pdata->pbat_platform->battery_technology;

	/* These two members are synchronized with BCM PMU temperature map */
	pbat_data->bcmpmu_temp_map = pbat->pd2083->pdata->pbat_platform->bcmpmu_temp_map;
	pbat_data->bcmpmu_temp_map_len = pbat->pd2083->pdata->pbat_platform->bcmpmu_temp_map_len;

	if(pbat->pd2083->pdata->pbat_platform->vf_lower >= pbat->pd2083->pdata->pbat_platform->vf_upper)
	{
		printk("%s. Please check vf_lower(%d) and vf_upper(%d)\n", 
					__func__, 
					pbat->pd2083->pdata->pbat_platform->vf_lower,
					pbat->pd2083->pdata->pbat_platform->vf_upper);
	}
	else
	{
		pbat_data->vf_lower = pbat->pd2083->pdata->pbat_platform->vf_lower;
		pbat_data->vf_upper = pbat->pd2083->pdata->pbat_platform->vf_upper;
	}

	return;
}


// The following definition is for functionality of power-off charging
#define CONFIG_SEC_BATT_EXT_ATTRS

#if defined(CONFIG_SEC_BATT_EXT_ATTRS)
#define BATT_TYPE 							"SDI_SDI"
enum
{
	SS_BATT_LP_CHARGING,
	SS_BATT_CHARGING_SOURCE,
	SS_BATT_TEMP_AVER,
	SS_BATT_TEMP_ADC_AVER,
	SS_BATT_TYPE,
	SS_BATT_READ_ADJ_SOC,
	SS_BATT_RESET_SOC,
};

static ssize_t ss_batt_ext_attrs_show(struct device *pdev, struct device_attribute *attr, char *buf);
static ssize_t ss_batt_ext_attrs_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t count);

static struct device_attribute ss_batt_ext_attrs[]=
{
	__ATTR(batt_lp_charging, 0644, ss_batt_ext_attrs_show, ss_batt_ext_attrs_store),
	__ATTR(batt_charging_source, 0644, ss_batt_ext_attrs_show, ss_batt_ext_attrs_store),
	__ATTR(batt_temp_aver, 0644, ss_batt_ext_attrs_show, ss_batt_ext_attrs_store),
	__ATTR(batt_temp_adc_aver, 0644, ss_batt_ext_attrs_show, ss_batt_ext_attrs_store),
	__ATTR(batt_type, 0644, ss_batt_ext_attrs_show, NULL),
	__ATTR(batt_read_adj_soc, 0644, ss_batt_ext_attrs_show , NULL),
	__ATTR(batt_reset_soc, 0664, NULL, ss_batt_ext_attrs_store),
};

static ssize_t ss_batt_ext_attrs_show(struct device *pdev, struct device_attribute *attr, char *buf)
{
	ssize_t count=0;
	int lp_charging=0;

	struct d2083_battery *pbat = pdev->platform_data;
	struct d2083_charger_data *pchg_data = &pbat->charger_data;

	const ptrdiff_t off = attr - ss_batt_ext_attrs;

	//struct power_supply *ps;
	union power_supply_propval propval;
	propval.intval = 0;
	propval.strval = 0;

	if(pbat == NULL)
	{
		printk("%s: Failed to get drive_data\n",__func__);
		return 0;
	}

	switch(off)
	{
		case SS_BATT_LP_CHARGING:
			lp_charging = pchg_data->lp_charging;
			count += scnprintf(buf+count, PAGE_SIZE-count, "%d\n", lp_charging);
			break;
		case SS_BATT_CHARGING_SOURCE:
			{
				unsigned int charger_type = 0;
				unsigned int charging_source = 0;

				charging_source = d2083_get_charger_type(pbat);
				switch(charging_source)
				{
					case CHARGER_TYPE_TA :
						charger_type = POWER_SUPPLY_TYPE_USB_DCP;
						break;
					case CHARGER_TYPE_USB :
						charger_type = POWER_SUPPLY_TYPE_USB;
						break;
					default:
						break;
				}
				count+=scnprintf(buf+count, PAGE_SIZE-count, "%d\n", charger_type);
			}
			break;
		case SS_BATT_TEMP_AVER:
			{
				int temp_aver = 0;

				temp_aver = d2083_get_average_temperature(pbat);
				count+=scnprintf(buf+count, PAGE_SIZE-count, "%d\n", temp_aver);
			}
			break;
		case SS_BATT_TEMP_ADC_AVER:
			{
				int temp_adc_aver = 0;

				temp_adc_aver = d2083_get_average_temperature_adc(pbat);
				count+=scnprintf(buf+count, PAGE_SIZE-count, "%d\n", temp_adc_aver);
			}
			break;
		case SS_BATT_TYPE:
			count+=scnprintf(buf+count, PAGE_SIZE-count, "%s\n", BATT_TYPE);
			break;
		case SS_BATT_READ_ADJ_SOC:
			{
				int capacity = 0;

				capacity = d2083_get_soc(pbat);
				count+=scnprintf(buf+count, PAGE_SIZE-count, "%d\n", capacity);
			}
			break;
		default:
			break;
	}


	return count;
}

static ssize_t ss_batt_ext_attrs_store(struct device *pdev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct d2083_battery *pbat = pdev->platform_data;

	const ptrdiff_t off = attr - ss_batt_ext_attrs;

	//struct power_supply *ps;
	union power_supply_propval propval;
	propval.intval = 0;
	propval.strval = 0;

	if(pbat == NULL)
	{
		printk("%s: Failed to get drive_data\n",__func__);
		return 0;
	}

	switch(off)
	{
		case SS_BATT_RESET_SOC:
			{
				// TODO: Check.
				//count = 0;

				//unsigned long val = simple_strtoul(buf, NULL, 0);
				//if ((val == 1) && (bcmpmu->em_reset))
				//	bcmpmu->em_reset(bcmpmu);	
			}
			break;
		default:
			break;
	}

	return count;
}

unsigned int lp_boot_mode;
static int get_boot_mode(char *str)
{
	get_option(&str, &lp_boot_mode);

	return 1;
}
__setup("lpcharge=",get_boot_mode);

#endif /* CONFIG_SEC_BATT_EXT_ATTRS */


/* 
 * Name : d2083_battery_probe
 */
static __devinit int d2083_battery_probe(struct platform_device *pdev)
{
	struct d2083 *d2083 = platform_get_drvdata(pdev);
	struct d2083_battery *pbat = &d2083->batt;
	int ret, i;

	pr_info("Start %s\n", __func__);

	if(unlikely(!d2083 || !pbat)) {
		pr_err("%s. Invalid platform data\n", __func__);
		return -EINVAL;
	}

	gbat = pbat;
	pbat->pd2083 = d2083;

	// Initialize a resource locking
	mutex_init(&pbat->lock);
	mutex_init(&pbat->api_lock);
	mutex_init(&pbat->meoc_lock);

	// Store a driver data structure to platform.
	platform_set_drvdata(pdev, pbat);

	d2083_battery_data_init(pbat);
	d2083_set_adc_mode(pbat, D2083_ADC_IN_AUTO);

	pbat->wall.name = POWER_SUPPLY_WALL;
	pbat->wall.type = POWER_SUPPLY_TYPE_MAINS;
	pbat->wall.properties = d2083_ta_props;
	pbat->wall.num_properties = ARRAY_SIZE(d2083_ta_props);
	pbat->wall.get_property = d2083_ta_get_property;
	ret = power_supply_register(&pdev->dev, &pbat->wall);
	if(ret) {
		pr_err("%s. The wall charger registration failed\n", __func__);
		goto err_reg_wall_supply;
	}

	pbat->usb.name = POWER_SUPPLY_USB;
	pbat->usb.type = POWER_SUPPLY_TYPE_USB;
	pbat->usb.properties = d2083_usb_props;
	pbat->usb.num_properties = ARRAY_SIZE(d2083_usb_props);
	pbat->usb.get_property = d2083_usb_get_property;
	ret = power_supply_register(&pdev->dev, &pbat->usb);
	if(ret) {
		pr_err("%s. The USB registration failed\n", __func__);
		goto err_reg_usb_supply;        
	}

	pbat->battery.name = POWER_SUPPLY_BATTERY;
	pbat->battery.type = POWER_SUPPLY_TYPE_BATTERY;
	pbat->battery.properties = d2083_battery_props;
	pbat->battery.num_properties = ARRAY_SIZE(d2083_battery_props);
	pbat->battery.get_property = d2083_battery_get_property;
	ret = power_supply_register(&pdev->dev, &pbat->battery);
	if(ret) {
		pr_err("%s. The battery registration failed\n", __func__);
		goto err_reg_battery_supply;        
	}

	// Register event handler
#ifdef D2083_REG_VF_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_EVF, d2083_battery_vf_handler,
							0, "d2083-vf", pbat);
	if(ret < 0) {
		pr_err("%s. VF IRQ register failed\n", __func__);
		goto err_reg_vf;
	}
#endif /* D2083_REG_VF_IRQ */
#ifdef D2083_REG_TBAT2_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_ETBAT2, d2083_battery_tbat2_handler,
							0, "d2083-tbat2", pbat);
	if(ret < 0) {		
		pr_err("%s. TBAT2 IRQ register failed\n", __func__);
		goto err_reg_tbat2;
	}
#endif /* D2083_REG_TBAT2_IRQ */
#ifdef D2083_REG_VDD_LOW_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_EVDD_LOW, d2083_battery_vdd_low_handler,
							0, "d2083-vddlow", pbat);
	if(ret < 0) {		
		pr_err("%s. VDD_LOW IRQ register failed\n", __func__);
		goto err_reg_vdd_low;
	}
#endif /* D2083_REG_VDD_LOW_IRQ */
#ifdef D2083_REG_VDD_MON_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_EVDD_MON, d2083_battery_vdd_mon_handler,
							0, "d2083-vddmon", pbat);
	if(ret < 0) {		
		pr_err("%s. VDD_MON IRQ register failed\n", __func__);
		goto err_reg_vdd_mon;
	}
#endif /* D2083_REG_VDD_MON_IRQ */
#ifdef D2083_REG_TBAT1_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_ETBAT1, d2083_battery_tbat1_handler,
							0, "d2083-tbat1", pbat);
	if(ret < 0) {		
		pr_err("%s. TBAT1 IRQ register failed\n", __func__);
		goto err_reg_tbat1;
	}
#endif /* D2083_REG_TBAT1_IRQ */
#ifdef D2083_REG_EOM_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_EADCEOM, d2083_battery_adceom_handler,
							0, "d2083-eom", pbat);
	if(ret < 0) {		
		pr_err("%s. ADCEOM IRQ register failed\n", __func__);
		goto err_reg_eadeom;
	}
#endif /* D2083_REG_EOM_IRQ */
#ifdef D2083_REG_TA_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_ETA, d2083_battery_ta_handler,
							0, "d2083-ta", pbat);
	if(ret < 0) {		
		pr_err("%s. TA IRQ register failed\n", __func__);
		goto err_reg_eta;
	}
#endif /* D2083_REG_TA_IRQ */
#ifdef D2083_REG_JIG_IRQ
	ret = d2083_register_irq(d2083, D2083_IRQ_ENJIGON, d2083_battery_njigon_handler,
							0, "d2083-jig", pbat);
	if(ret < 0) {		
		pr_err("%s. TBAT1 IRQ register failed\n", __func__);
		goto err_reg_njig;
	}
#endif /* D2083_REG_JIG_IRQv */

	pbat->battery.dev->platform_data = (void *)pbat;

#if defined(CONFIG_SEC_BATT_EXT_ATTRS)
	pbat->charger_data.lp_charging = lp_boot_mode;
	pr_info("%s. lp_charging = %d\n", __func__, lp_boot_mode);
	for(i = 0; i < ARRAY_SIZE(ss_batt_ext_attrs) ; i++)
	{
		ret = device_create_file(pbat->battery.dev, &ss_batt_ext_attrs[i]);
	}
#endif

	INIT_DELAYED_WORK(&pbat->monitor_volt_work, d2083_monitor_voltage_work);
	INIT_DELAYED_WORK(&pbat->monitor_temp_work, d2083_monitor_temperature_work);
	INIT_DELAYED_WORK(&pbat->info_notify_work, d2083_info_notify_work);
	INIT_DELAYED_WORK(&pbat->charge_timer_work, d2083_charge_timer_work);
	INIT_DELAYED_WORK(&pbat->recharge_start_timer_work, d2083_recharge_start_timer_work);
	INIT_DELAYED_WORK(&pbat->sleep_monitor_work, d2083_sleep_monitor_work);

	// Start schedule of dealyed work for monitoring voltage and temperature.
	schedule_delayed_work(&pbat->info_notify_work, D2083_NOTIFY_INTERVAL);
	schedule_delayed_work(&pbat->monitor_temp_work, D2083_TEMPERATURE_MONITOR_START);
	schedule_delayed_work(&pbat->monitor_volt_work, D2083_VOLTAGE_MONITOR_START);

	device_init_wakeup(&pdev->dev, 1);	

	pr_info("%s. End...\n", __func__);

	return 0;

#ifdef D2083_REG_JIG_IRQ
err_reg_njig:
#endif /* D2083_REG_JIG_IRQ */
#ifdef D2083_REG_TA_IRQ
	d2083_free_irq(d2083, D2083_IRQ_ETA);
err_reg_eta:
#endif /* D2083_REG_TA_IRQ */
#ifdef D2083_REG_EOM_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EADCEOM);
err_reg_eadeom:
#endif /* D2083_REG_EOM_IRQ */
#ifdef D2083_REG_TBAT1_IRQ
	d2083_free_irq(d2083, D2083_IRQ_ETBAT1);
err_reg_tbat1:
#endif /* D2083_REG_TBAT1_IRQ */
#ifdef D2083_REG_VDD_MON_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EVDD_MON);
err_reg_vdd_mon:
#endif /* D2083_REG_VDD_MON_IRQ */
#ifdef D2083_REG_VDD_LOW_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EVDD_LOW);
err_reg_vdd_low:
#endif /* D2083_REG_VDD_LOW_IRQ */
#ifdef D2083_REG_TBAT2_IRQ
	d2083_free_irq(d2083, D2083_IRQ_ETBAT2);
err_reg_tbat2:
#endif /* D2083_REG_TBAT2_IRQ */
#ifdef D2083_REG_VF_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EVF);
err_reg_vf:
#endif /* D2083_REG_VF_IRQ */
	power_supply_unregister(&pbat->battery);
err_reg_battery_supply:
	power_supply_unregister(&pbat->usb);
err_reg_usb_supply:
	power_supply_unregister(&pbat->wall);
err_reg_wall_supply:
	kfree(pbat);

	return ret;

}


/*
 * Name : d2083_battery_suspend
 */
static int d2083_battery_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct d2083_battery *pbat = platform_get_drvdata(pdev);
	struct d2083 *d2083 = pbat->pd2083;
	int ret;

	pr_info("%s. Enter\n", __func__);

	if(unlikely(!pbat || !d2083)) {
		pr_err("%s. Invalid parameter\n", __func__);
		return -EINVAL;
	}

	ret = d2083_reg_write(d2083, D2083_BUCKA_REG, 0x9A);//force pwm mode
	
	cancel_delayed_work(&pbat->info_notify_work);
	cancel_delayed_work(&pbat->monitor_temp_work);
	cancel_delayed_work(&pbat->monitor_volt_work);

	pr_info("%s. Leave\n", __func__);
	
	return 0;
}


/*
 * Name : d2083_battery_resume
 */
static int d2083_battery_resume(struct platform_device *pdev, pm_message_t state)
{
	struct d2083_battery *pbat = platform_get_drvdata(pdev);
	struct d2083 *d2083 = pbat->pd2083;
	int ret;

	pr_info("%s. Enter\n", __func__);

	if(unlikely(!pbat || !d2083)) {
		pr_err("%s. Invalid parameter\n", __func__);
		return -EINVAL;
	}

	ret = d2083_reg_write(d2083, D2083_BUCKA_REG, 0x99); // auto mode
	
	// Start schedule of dealyed work for monitoring voltage and temperature.
	if(!is_called_by_ticker) {
		wake_lock_timeout(&pbat->battery_data.sleep_monitor_wakeup, 
										D2083_SLEEP_MONITOR_WAKELOCK_TIME);
		schedule_delayed_work(&pbat->monitor_temp_work, 0);
		schedule_delayed_work(&pbat->monitor_volt_work, 0);
		schedule_delayed_work(&pbat->info_notify_work, 0);
	}

	pr_info("%s. Leave\n", __func__);

	return 0;
}


/*
 * Name : d2083_battery_remove
 */
static __devexit int d2083_battery_remove(struct platform_device *pdev)
{
	struct d2083_battery *pbat = platform_get_drvdata(pdev);
	struct d2083 *d2083 = pbat->pd2083;
	int i;

	if(unlikely(!pbat || !d2083)) {
		pr_err("%s. Invalid parameter\n", __func__);
		return -EINVAL;
	}

	// Free IRQ
#ifdef D2083_REG_JIG_IRQ
	d2083_free_irq(d2083, D2083_IRQ_ENJIGON);
#endif /* D2083_REG_JIG_IRQ */
#ifdef D2083_REG_TA_IRQ
	d2083_free_irq(d2083, D2083_IRQ_ETA);
#endif /* D2083_REG_TA_IRQ */
#ifdef D2083_REG_EOM_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EADCEOM);
#endif /* D2083_REG_EOM_IRQ */
#ifdef D2083_REG_TBAT1_IRQ
	d2083_free_irq(d2083, D2083_IRQ_ETBAT1);
#endif /* D2083_REG_TBAT1_IRQ */
#ifdef D2083_REG_VDD_MON_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EVDD_MON);
#endif /* D2083_REG_VDD_MON_IRQ */
#ifdef D2083_REG_VDD_LOW_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EVDD_LOW);
#endif /* D2083_REG_VDD_LOW_IRQ */
#ifdef D2083_REG_TBAT2_IRQ
	d2083_free_irq(d2083, D2083_IRQ_ETBAT2);
#endif /* D2083_REG_TBAT2_IRQ */
#ifdef D2083_REG_VF_IRQ
	d2083_free_irq(d2083, D2083_IRQ_EVF);
#endif /* D2083_REG_VF_IRQ */


#if defined(CONFIG_SEC_BATT_EXT_ATTRS)
	for(i = 0; i < ARRAY_SIZE(ss_batt_ext_attrs) ; i++)
	{
		device_remove_file(pbat->battery.dev, &ss_batt_ext_attrs[i]);
	}
#endif


	return 0;
}

static struct platform_driver d2083_battery_driver = {
	.probe    = d2083_battery_probe,
	.suspend  = d2083_battery_suspend,
	.resume   = d2083_battery_resume,
	.remove   = d2083_battery_remove,
	.driver   = {
		.name  = "d2083-battery",
		.owner = THIS_MODULE,
    },
};

static int __init d2083_battery_init(void)
{
	printk(d2083_battery_banner);
	return platform_driver_register(&d2083_battery_driver);
}
//module_init(d2083_battery_init);
subsys_initcall(d2083_battery_init);



static void __exit d2083_battery_exit(void)
{
	flush_scheduled_work();
	platform_driver_unregister(&d2083_battery_driver);
}
module_exit(d2083_battery_exit);


MODULE_AUTHOR("Dialog Semiconductor Ltd. < eric.jeong@diasemi.com >");
MODULE_DESCRIPTION("Battery driver for the Dialog D2083 PMIC");
MODULE_LICENSE("GPL");
MODULE_ALIAS("Power supply : d2083-battery");

