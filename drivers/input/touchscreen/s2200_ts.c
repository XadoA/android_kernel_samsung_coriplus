/*
 * s2200_ts.c
  * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

//#define DEBUG
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
//#include <mach/vreg.h>

#include <linux/s2200_ts.h>
/*
#include <asm/unaligned.h>
*/

#define MAX_WIDTH		255
#define MAX_PRESSURE		255
#define MAX_FINGERS	    2       /* 4 */
#define S2200_INIT_REG		0x14
#define S2200_FINGER_STATE	0X15

#define TOUCH_ON 1
#define TOUCH_OFF 0

#define TOUCH_EN 43 //PSJ

#define SYNAPTICS_TS_NAME "synaptics-S2200"
static DEFINE_SPINLOCK(s2200_spin_lock);
struct finger_data {
	int x;
	int y;
	int z;
	int w;
	u8 state;
//	int16_t component;
};

struct s2200_ts_data {
	struct i2c_client		*client;
	struct input_dev		*input_dev;
	char				phys[32];

	int				max_x;
	int				max_y;

	bool				invert_x;
	bool				invert_y;

	int				irq;

	struct s2200_ts_platform_data	*pdata;

//	char				*fw_name;
//	struct completion		init_done;
	struct early_suspend		early_suspend;
	
	struct finger_data		fingers[3];

	/* protects the enabled flag */
//	struct mutex			lock;
//	bool				enabled;
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void s2200_ts_early_suspend(struct early_suspend *h);
static void s2200_ts_late_resume(struct early_suspend *h);
#endif

#if defined(CONFIG_MFD_D2083_BRINGUP_RECHECK)
extern void touch_ldo_onoff(int onoff);
#endif

static void s2200_ts_power(int on_off)
{
	if(on_off==TOUCH_ON)
	{
#if defined(CONFIG_MFD_D2083_BRINGUP_RECHECK)
		touch_ldo_onoff(1);
#else
		gpio_request(TOUCH_EN,"Touch_en");
		gpio_direction_output(TOUCH_EN,1);
		gpio_set_value(TOUCH_EN,1);
		gpio_free(TOUCH_EN);
#endif

	}
	else
	{
#if defined(CONFIG_MFD_D2083_BRINGUP_RECHECK)
		touch_ldo_onoff(0);
#else
		gpio_request(TOUCH_EN,"Touch_en");
		gpio_direction_output(TOUCH_EN,0);
		gpio_set_value(TOUCH_EN,0);
		gpio_free(TOUCH_EN);
#endif
	}
}

inline s32 ts_read_data(struct i2c_client *client,
	u16 reg, u8 *values, u16 length)
{
	s32 ret;
	/* select register*/
	ret = i2c_master_send(client , (u8 *)&reg , 1);
	//zinitix_debug_msg("%s, %d, ret : %d\n", __func__, __LINE__, ret );	
	if (ret < 0)
    {
		printk("-----------------ts_read_datato - i2c_master_send, ret =%d \n", ret);
		return ret;
	}
	/* for setup tx transaction. */
	udelay(50);
	ret = i2c_master_recv(client , values , length);
	//zinitix_debug_msg("%s, %d, ret : %d\n", __func__, __LINE__, ret );		
	if (ret < 0)
    {
		printk("-----------------ts_read_datato - i2c_master_recv, ret =%d \n", ret);
		return ret;
	}
	udelay(10);
	return length;
}

static irqreturn_t  s2200_ts_interrupt(int irq, void *dev_id)
{
	struct s2200_ts_data *data = dev_id;
	struct i2c_client *client = data->client;
	u8 finger_state;
	u8 ic_state;
	u8 val;
	static int init_flag = 0;
	u8 buf[22] ={0,};
	u8 buf2[1] ={0,};
	int ret;
	int i;
	u8 temp;
      unsigned long flags;
//	dev_info(&client->dev, "%s(%d)(%d)\n", __func__, gpio_get_value(data->pdata->gpio_int), init_flag);

/*
      ret = ts_read_data(client, S2200_INIT_REG, buf, 22);

     	if (ret < 0) {
             printk("[TSP] fail to read touch data ret = %d \n",ret);
		dev_err (&client->dev, "fail to read touch data\m");
	}
	
*/
      //spin_lock_irqsave(&s2200_spin_lock, flags);

       //i2c_smbus_write_i2c_block_data(client,0xFF,1,0x00);
      //printk("[TSP] client name is %s\n", client->name);
     // printk("[TSP] client name is %x\n", client->addr);

     //gpio_direction_output(89,1);
     //gpio_direction_output(90,1);

	//ret = i2c_smbus_read_i2c_block_data(client, S2200_INIT_REG, 22, buf);
        //ret = ts_read_data(client, S2200_INIT_REG, buf, 22);
       ret = ts_read_data(client, 0x14, buf, 22);

       for(i=0 ; i < 6 ; i++)
      {
         printk("buf[%d] = %0x\n", i, buf[i]);
      }
    
	if (ret < 0) {
             printk("[TSP] fail to read touch data ret = %d \n",ret);
		dev_err (&client->dev, "fail to read touch data\n");
	}else{
             printk("[TSP] %d byte Read \n",ret);
	}
//	print_hex_dump(KERN_ERR, "s2200: ",
//	       DUMP_PREFIX_OFFSET, 22, 1, buf, 22, false);
	
	for (i = 0; i < MAX_FINGERS; i++) {
		temp = buf[1] >> i*2;
		temp &= 0x03;
//		dev_info(&client->dev, "0x15=%X\n", temp);
		//printk("[TSP] temp is %d\n", temp);

		if (temp == 0x0 || temp == 0x01) {
			if ( temp == 0 && data->fingers[i].state == 0)
				continue;
			if (temp == 1 && data->fingers[i].state == 0)
				data->fingers[i].state = 1;
			else if (temp == 0 && data->fingers[i].state == 1)
				data->fingers[i].state = 0;

			data->fingers[i].x = (buf[3 + i * 5 + 0] << 4) | (buf[3 + i * 5 + 2] & 0xf);
			data->fingers[i].y = (buf[3 + i * 5 + 1] << 4) | ((buf[3 + i * 5 + 2] & 0xf0) >> 4);
			data->fingers[i].w = 0;
			data->fingers[i].z = buf[3 + i * 5 + 4];

			printk(KERN_ERR "[TSP] id[%d],x=%d,y=%d,z=%d,s=%d\n",
				i , data->fingers[i].x, data->fingers[i].y, data->fingers[i].z, data->fingers[i].state); 

#if 0   /* MT protocol type A */
                   if(data->fingers[i].state)
                   {
                        input_report_abs(data->input_dev, ABS_MT_POSITION_X, data->fingers[i].x);
                        input_report_abs(data->input_dev, ABS_MT_POSITION_Y, data->fingers[i].y);
                        input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, data->fingers[i].z);
                        input_report_abs(data->input_dev, ABS_MT_WIDTH_MAJOR, data->fingers[i].w);
                        input_report_abs(data->input_dev, ABS_MT_TRACKING_ID, i);
                        printk(KERN_ERR "[TSP] pressed !!!!\n");
			
			
                   }

                    input_mt_sync(data->input_dev);
#else   /* MT protocol type B */
		input_mt_slot(data->input_dev, i);
		input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, data->fingers[i].state);
		
		if(data->fingers[i].state)
		{
			input_report_abs(data->input_dev, ABS_MT_POSITION_X, data->fingers[i].x);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y, data->fingers[i].y);
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, data->fingers[i].z);
			input_report_abs(data->input_dev, ABS_MT_WIDTH_MAJOR, data->fingers[i].w);			
		}

#endif

		}

	}
    
	input_sync(data->input_dev);

///////////////////////////////////////////////////////////////////////////////////////
#if 0
                ret = i2c_master_send(client, 0xFF, 1);
                ret = i2c_master_send(client, 0x02, 1);

                ret = ts_read_data(client, 0x00, buf2, 1);

                ret = i2c_master_send(client, 0xFF, 1);
                ret = i2c_master_send(client, 0x00, 1);

                printk("[TSP] button touch buf2 = %d \n", buf2);
#endif



///////////////////////////////////////////////////////////////////////////////////////

      //spin_unlock_irqrestore(&s2200_spin_lock, flags);
//	val = i2c_smbus_read_byte_data(data->client, S2200_INIT_REG);
//	dev_info(&client->dev, "%s: val=%X\n", __func__, val);
/*
	if (init_flag == 10) {
		i2c_smbus_read_byte_data(data->client, S2200_INIT_REG);
		dev_info(&client->dev, "%s: ic_state=%d\n", __func__, ic_state);
	}

	if (init_flag == 100) {
		s2200_ts_power(data, false);
		gpio_set_value(data->pdata->gpio_int , 1);
	}
	
	if (init_flag == 200) {
		disable_irq(data->irq);
	}
	init_flag++;
*/	
out:
	return IRQ_HANDLED;
	//return IRQ_WAKE_THREAD;
}

void TSP_forced_release_forkey(void)
{
	int i;
	int temp_value = 0;
	printk("[TSP] force release\n");

/*	
	for ( i= 0; i<MAX_USING_FINGER_NUM; i++ )
	{
		if(fingerInfo[i].id >=1)
		{
			fingerInfo[i].status = -2; // force release
		}

		if(fingerInfo[i].status != -2) continue;
		
		input_report_abs(ts_global->input_dev, ABS_MT_POSITION_X, fingerInfo[i].x);
		input_report_abs(ts_global->input_dev, ABS_MT_POSITION_Y, fingerInfo[i].y);
		input_report_abs(ts_global->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(ts_global->input_dev, ABS_MT_WIDTH_MAJOR, fingerInfo[i].z);
		input_mt_sync(ts_global->input_dev);

		printk("[TSP] force release\n");
		temp_value++;
	}

	if(temp_value>0)
		input_sync(ts_global->input_dev);
*/
}
EXPORT_SYMBOL(TSP_forced_release_forkey);



static int __devexit s2200_ts_remove(struct i2c_client *client)
{
	struct s2200_ts_data *data = i2c_get_clientdata(client);

	if (data->irq >= 0)
		free_irq(data->irq, data);
	input_unregister_device(data->input_dev);
	kfree(data);

	return 0;
}

#if defined(CONFIG_PM) || defined(CONFIG_HAS_EARLYSUSPEND)
static int s2200_ts_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct s2200_ts_data *data = i2c_get_clientdata(client);
	int i;

      s2200_ts_power(TOUCH_OFF);
	return 0;
}

static int s2200_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct s2200_ts_data *data = i2c_get_clientdata(client);
	int ret = 0;

	s2200_ts_power(TOUCH_ON);
	return ret;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void s2200_ts_early_suspend(struct early_suspend *h)
{
	struct s2200_ts_data *data;
	printk("[TSP] %s, %d\n", __func__, __LINE__ );
	data = container_of(h, struct s2200_ts_data, early_suspend);

       disable_irq(data->irq);
	s2200_ts_suspend(&data->client->dev);
}

static void s2200_ts_late_resume(struct early_suspend *h)
{
      int ret;
	struct s2200_ts_data *data;
      char buf[1];
	printk("[TSP] %s, %d\n", __func__, __LINE__ );
	data = container_of(h, struct s2200_ts_data, early_suspend);
     
	s2200_ts_resume(&data->client->dev);
      mdelay(1000);
      
      	//ret = i2c_smbus_read_byte_data(data->client, S2200_INIT_REG);
      ret =  ts_read_data(data->client, 0x13, buf, 1);
	if (ret < 0) {
		printk("-----------------Failed to init IC - 0x13 \n");
       }

      ret =  ts_read_data(data->client, S2200_INIT_REG, buf, 1);

	if (ret < 0) {
		printk("-----------------Failed to init IC\n");
		//goto err_req_irq;
	}else{
             printk("-----------------length is %d\n",ret);
	}
      enable_irq(data->irq);
}
#endif

static irqreturn_t ts_int_handler(int irq, void *dev)
{

      return IRQ_WAKE_THREAD;

}

static int __devinit s2200_ts_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct s2200_ts_data *data;
	struct input_dev *input_dev;
	char buf[1];
	int ret = 0;
	u8 ic_status;

	printk("[TSP] %s, %d\n", __func__, __LINE__ );

    	s2200_ts_power(TOUCH_ON);
     	mdelay(1000);

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -EIO;

	data = kzalloc(sizeof(struct s2200_ts_data), GFP_KERNEL);
	if (!data) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		goto err_alloc;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		goto err_alloc_input_dev;
	}

	data->client = client;
	data->input_dev = input_dev;
	data->pdata = client->dev.platform_data;
//	init_completion(&data->init_done);
	data->irq = -1;
//	mutex_init(&data->lock);

	if (data->pdata) {
		data->max_x = data->pdata->max_x;
		data->max_y = data->pdata->max_y;
		data->invert_x = data->pdata->invert_x;
		data->invert_y = data->pdata->invert_y;
	} else {
		data->max_x = 240;
		data->max_y = 320;
	}

	snprintf(data->phys, sizeof(data->phys), 
		"%s/input0", dev_name(&client->dev));
//	input_dev->name = "Synaptics S2200 Touchscreen";
	input_dev->name = "sec_ts";
	input_dev->phys = data->phys;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_ABS, input_dev->evbit);
	set_bit(MT_TOOL_FINGER, input_dev->keybit);	// for B type multi touch protocol    
	set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
	//set_bit(BTN_TOUCH, input_dev->keybit);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, 240, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, 320, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, MAX_WIDTH, 0, 0);
	//input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, MAX_PRESSURE, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);

	input_mt_init_slots(input_dev, 2); // for B type multi touch protocol


	input_set_drvdata(input_dev, data);

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&client->dev, "failed to register input dev (%d)\n",
			ret);
		goto err_reg_input_dev;
	}

	i2c_set_clientdata(client, data);


	/*init IC*/
	//ic_status = i2c_smbus_read_byte_data(data->client, S2200_INIT_REG);
	ret = ts_read_data(client, 0x13, buf, 1);

	if (ret < 0) {
             printk("[TSP] 1 fail to read touch data ret = %d \n",ret);
		dev_err (&client->dev, "fail to read touch data\n");
	}else{
             printk("[TSP] 1 %d byte Read \n",ret);
	}
    
	ret = ts_read_data(client, 0x14, buf, 1);

	if (ret < 0) {
             printk("[TSP] 2 fail to read touch data ret = %d \n",ret);
		dev_err (&client->dev, "fail to read touch data\n");
	}else{
             printk("[TSP] 2 %d byte Read \n",ret);
	}
	
	dev_info(&client->dev, "ic status=%x\n", ic_status);
	
	//interrupt register
	ret = request_threaded_irq(client->irq, ts_int_handler, s2200_ts_interrupt,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT ,
				   "synaptics_s2200_ts", data);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to register interrupt\n");
		goto err_req_irq;
	}
	data->irq = client->irq;

	dev_info(&client->dev, "Synaptics s2200 touchscreen driver is initialized\n");

	dev_info(&client->dev, "client->irq:%d, data->pdata->gpio_int:%d\n", client->irq, data->pdata->gpio_int);
	
#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	data->early_suspend.suspend = s2200_ts_early_suspend;
	data->early_suspend.resume = s2200_ts_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

	return 0;

err_req_irq:
err_config:
	input_unregister_device(input_dev);
	input_dev = NULL;
err_reg_input_dev:
	input_free_device(input_dev);
err_alloc_input_dev:
//	kfree(data->fw_name);
	kfree(data);
err_alloc:
	return ret;
}

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
static const struct dev_pm_ops s2200_ts_pm_ops = {
	.suspend	= s2200_ts_suspend,
	.resume		= s2200_ts_resume,
};
#endif

static const struct i2c_device_id s2200_ts_id[] = {
	{ "synaptics_s2200_ts", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, s2200_ts_id);

static struct i2c_driver s2200_ts_driver = {
	.probe		= s2200_ts_probe,
	.remove		= __devexit_p(s2200_ts_remove),
	.driver = {
		.name = "synaptics_s2200_ts",
#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
		.pm	= &s2200_ts_pm_ops,
#endif
	},
	.id_table	= s2200_ts_id,
};

static int __init s2200_ts_init(void)
{
	return i2c_add_driver(&s2200_ts_driver);
}

static void __exit s2200_ts_exit(void)
{
	i2c_del_driver(&s2200_ts_driver);
}

late_initcall(s2200_ts_init);
module_exit(s2200_ts_exit);

/* Module information */
MODULE_DESCRIPTION("Synaptics Touchscreen driver");
MODULE_LICENSE("GPL");
