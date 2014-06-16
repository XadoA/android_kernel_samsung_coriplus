/*
 * This file is part of the PX3215C sensor driver.
 * Chip is proximity sensor only.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 *
 * Filename: px3215.c
 *
 * Summary:
 *	PX3215C sensor dirver.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/string.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/px3215.h>

//#define PX3215C_DRV_NAME	"px3215"
#define PX3215C_DRV_NAME		"dyna"
#define DRIVER_VERSION		"1.9"
#define CHIP_NAME	"PX3215"

#define PX3215C_NUM_CACHABLE_REGS	18

#define PX3215C_MODE_COMMAND	0x00
#define PX3215C_MODE_SHIFT	(0)
#define PX3215C_MODE_MASK	0x07

#define	AL3212_PX_LSB		0x0e
#define	AL3212_PX_MSB		0x0f
#define	AL3212_PX_LSB_MASK	0x0f
#define	AL3212_PX_MSB_MASK	0x3f

#define PX3215C_OBJ_COMMAND	0x0f
#define PX3215C_OBJ_MASK		0x80
#define PX3215C_OBJ_SHIFT	(7)

#define PX3215C_INT_COMMAND	0x01
#define PX3215C_INT_SHIFT	(0)
#define PX3215C_INT_MASK		0x03
#define PX3215C_INT_PMASK		0x02

#define PX3215C_PX_LTHL			0x2a
#define PX3215C_PX_LTHL_SHIFT	(0)
#define PX3215C_PX_LTHL_MASK		0x03

#define PX3215C_PX_LTHH			0x2b
#define PX3215C_PX_LTHH_SHIFT	(0)
#define PX3215C_PX_LTHH_MASK		0xff

#define PX3215C_PX_HTHL			0x2c
#define PX3215C_PX_HTHL_SHIFT	(0)
#define PX3215C_PX_HTHL_MASK		0x03

#define PX3215C_PX_HTHH			0x2d
#define PX3215C_PX_HTHH_SHIFT	(0)
#define PX3215C_PX_HTHH_MASK		0xff

#define PX3215C_PX_CONFIGURE	0x20

#define PX3215C_RETRY_COUNT	3

#define PX3215_DEBUG 0
#define error(fmt, arg...) printk("--------" fmt "\n", ##arg)
#if PX3215_DEBUG
#define PROXDBG(fmt, args...) printk(KERN_INFO fmt, ## args)
#define debug(fmt, arg...) printk("--------" fmt "\n", ##arg)
#else
#define PROXDBG(fmt, args...)
#define debug(fmt,arg...)
#endif


static struct i2c_client *this_client;
static struct input_dev *px3215_input_dev;
#if defined(CONFIG_SENSORS_CORE)
extern struct class *sensors_class;
#endif

struct px3215_data {
	struct i2c_client *client;
	struct mutex lock;
	u8 reg_cache[PX3215C_NUM_CACHABLE_REGS];
	u8 power_state_before_suspend;
	int irq;
        unsigned int irq_gpio;
	struct input_dev *input;
};

static u8 px3215_reg[PX3215C_NUM_CACHABLE_REGS] = 
	{0x00,0x01,0x02,0x0a,0x0b,0x0e,0x0f,
	 0x20,0x21,0x22,0x23,0x24,0x28,0x29,0x2a,0x2b,0x2c,0x2d};

#define ADD_TO_IDX(addr,idx)	{														\
									int i;												\
									for(i = 0; i < PX3215C_NUM_CACHABLE_REGS; i++)		\
									{													\
										if (addr == px3215_reg[i])						\
										{												\
											idx = i;									\
											break;										\
										}												\
									}													\
								}

/*
 * register access helpers
 */
static int __px3215_i2c_read(u8 *buf, int len)
{
	uint8_t i;
        u8 reg;
	struct i2c_msg msgs[] = {
		{
			.addr	= this_client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= buf,
		},
		{
			.addr	= this_client->addr,
			.flags	= I2C_M_RD,
			.len	= len,
			.buf	= buf,
		}
	};
        reg = buf[0];
        
	for (i = 0; i < PX3215C_RETRY_COUNT; i++) {
		if (i2c_transfer(this_client->adapter, msgs, 2) >= 0) {
			break;
		}
		mdelay(10);
	}

	debug("read reg=%X, value=%X", reg, (buf[1] << 8) | buf[0]);

	if (i >= PX3215C_RETRY_COUNT) {
		pr_err("%s: retry over %d\n", __FUNCTION__, PX3215C_RETRY_COUNT);
		return -EIO;
	}

	return 0;
}


static int __px3215_i2c_write(u8 *buf, int len)
{
	uint8_t i;       
	struct i2c_msg msg[] = {
		{
			.addr	= this_client->addr,
			.flags	= 0,
			.len	= len,
			.buf	= buf,
		}
	};

        debug("write reg=%X, value=%X", buf[0], buf[1]);
    
	for (i = 0; i < PX3215C_RETRY_COUNT; i++) {
		if (i2c_transfer(this_client->adapter, msg, 1) >= 0) {
			break;
		}
		mdelay(10);
	}

	if (i >= PX3215C_RETRY_COUNT) {
		pr_err("%s: retry over %d\n", __FUNCTION__, PX3215C_RETRY_COUNT);
		return -EIO;
	}
	return 0;
}


static int __px3215_read_reg(struct i2c_client *client,
			       u32 reg, u8 mask, u8 shift)
{
	struct px3215_data *data = i2c_get_clientdata(client);
	u8 idx = 0xff;

	ADD_TO_IDX(reg,idx)
	return (data->reg_cache[idx] & mask) >> shift;
}

static int __px3215_write_reg(struct i2c_client *client,
				u32 reg, u8 mask, u8 shift, u8 val)
{
	struct px3215_data *data = i2c_get_clientdata(client);
	int ret = 0;
	u8 tmp;
	u8 idx = 0xff;
        u8 value[2] = {0};

	ADD_TO_IDX(reg,idx)
	if (idx > 0x2D)
		return -EINVAL;

	mutex_lock(&data->lock);

	tmp = data->reg_cache[idx];
	tmp &= ~mask;
	tmp |= val << shift;

        value[0] = reg;
        value[1] = tmp;
	ret=__px3215_i2c_write(value, 2);       
	if (!ret)
		data->reg_cache[idx] = tmp;

	mutex_unlock(&data->lock);
	return ret;
}

/*
 * internally used functions
 */

/* mode */
static int px3215_get_mode(struct i2c_client *client)
{
	int ret;    
	ret = __px3215_read_reg(client, PX3215C_MODE_COMMAND,
			PX3215C_MODE_MASK, PX3215C_MODE_SHIFT);

	printk(KERN_INFO "[PX3215] %s : mode=%d\n",__func__, ret);     
	return ret;
}

static int px3215_set_mode(struct i2c_client *client, int mode)
{
	int ret;

	printk(KERN_INFO "[PX3215] %s : mode=%d\n",__func__, mode); 
    
	ret = __px3215_write_reg(client, PX3215C_MODE_COMMAND,
				PX3215C_MODE_MASK, PX3215C_MODE_SHIFT, mode);
	return ret;
}

/* PX low threshold */
static int px3215_get_plthres(struct i2c_client *client)
{
	int lsb, msb;
   
	lsb = __px3215_read_reg(client, PX3215C_PX_LTHL,
				PX3215C_PX_LTHL_MASK, PX3215C_PX_LTHL_SHIFT);
	msb = __px3215_read_reg(client, PX3215C_PX_LTHH,
				PX3215C_PX_LTHH_MASK, PX3215C_PX_LTHH_SHIFT);
	return ((msb << 2) | lsb);
}

static int px3215_set_plthres(struct i2c_client *client, int val)
{
	int lsb, msb, err;

	printk(KERN_INFO "[PX3215] %s called : val=%X\n",__func__, val);
	
	msb = val >> 2;
	lsb = val & PX3215C_PX_LTHL_MASK;
	err = __px3215_write_reg(client, PX3215C_PX_LTHL,
		PX3215C_PX_LTHL_MASK, PX3215C_PX_LTHL_SHIFT, lsb);
	if (err)
		return err;
	err = __px3215_write_reg(client, PX3215C_PX_LTHH,
		PX3215C_PX_LTHH_MASK, PX3215C_PX_LTHH_SHIFT, msb);
	return err;
}

/* PX high threshold */
static int px3215_get_phthres(struct i2c_client *client)
{
	int lsb, msb;
	lsb = __px3215_read_reg(client, PX3215C_PX_HTHL,
				PX3215C_PX_HTHL_MASK, PX3215C_PX_HTHL_SHIFT);
	msb = __px3215_read_reg(client, PX3215C_PX_HTHH,
				PX3215C_PX_HTHH_MASK, PX3215C_PX_HTHH_SHIFT);
	return ((msb << 2) | lsb);
}

static int px3215_set_phthres(struct i2c_client *client, int val)
{
	int lsb, msb, err;

	printk(KERN_INFO "[PX3215] %s called : val=%X\n",__func__, val);
	
	msb = val >> 2;
	lsb = val & PX3215C_PX_HTHL_MASK;
	err = __px3215_write_reg(client, PX3215C_PX_HTHL,
		PX3215C_PX_HTHL_MASK, PX3215C_PX_HTHL_SHIFT, lsb);
	if (err)
		return err;
	err = __px3215_write_reg(client, PX3215C_PX_HTHH,
		PX3215C_PX_HTHH_MASK, PX3215C_PX_HTHH_SHIFT, msb);

	return err;
}

static int px3215_get_object(struct i2c_client *client, int lock)
{
	int val;
	u8 value[2] = {0};
	/*if (!lock)	mutex_lock(&data->lock);*/
	value[0] = PX3215C_OBJ_COMMAND;
	__px3215_i2c_read(value, 2);
	val = (value[1] << 8) | value[0];
	val &= PX3215C_OBJ_MASK;
	/*if (!lock)	mutex_unlock(&data->lock);*/

	return val >> PX3215C_OBJ_SHIFT;
}

static int px3215_get_intstat(struct i2c_client *client)
{
	int val;
	u8 value[2] = {0};
	value[0] = PX3215C_INT_COMMAND;
	__px3215_i2c_read(value, 2);
	val = (value[1] << 8) | value[0];
	val &= PX3215C_INT_MASK;
	return val >> PX3215C_INT_SHIFT;
}


static int px3215_get_px_value(struct i2c_client *client)
{
	struct px3215_data *data = i2c_get_clientdata(client);
	int lsb, msb;
	u8 value[2] = {0};    
	mutex_lock(&data->lock);
	value[0] = AL3212_PX_LSB;
	__px3215_i2c_read(value, 2);
        lsb = (value[1] << 8) | value[0];
	if (lsb < 0) {
		mutex_unlock(&data->lock);
		return lsb;
	}
	value[0] = AL3212_PX_MSB;    
	__px3215_i2c_read(value, 2);	
        msb = (value[1] << 8) | value[0];
	mutex_unlock(&data->lock);
	if (msb < 0)
		return msb;

	return (u32)(((msb & AL3212_PX_MSB_MASK) << 4) | (lsb & AL3212_PX_LSB_MASK));
}

/*
 * sysfs layer
 */
static int px3215_input_init(struct px3215_data *data)
{
    struct input_dev *dev;
    int err;

    printk(KERN_INFO "[PX3215] %s called\n",__func__);  
    
    dev = input_allocate_device();
    if (!dev) {
        error("Not enough memory for dev");
        return -ENOMEM;
    }
    dev->name = "proximity_sensor";
    set_bit(EV_SYN,dev->evbit);
    set_bit(EV_ABS,dev->evbit);    
    input_set_capability(dev, EV_ABS, ABS_DISTANCE);
    input_set_abs_params(dev, ABS_DISTANCE, 0, 1, 0, 0);    
    input_set_drvdata(dev, data);
    err = input_register_device(dev);
    if (err < 0) {
        error("Failed to register input device");
        input_free_device(dev);
        return err;
    }
    data->input = dev;
    px3215_input_dev = data->input ;
    printk(KERN_INFO "[PX3215] Input device settings complete");
    
    return 0;
}

static void px3215_input_fini(struct px3215_data *data)
{
    struct input_dev *dev = data->input;
    input_unregister_device(dev);
    input_free_device(dev);
}

/* mode */
static ssize_t px3215_show_mode(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	return sprintf(buf, "%d\n", px3215_get_mode(data->client));
}

static ssize_t px3215_store_mode(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	unsigned long val;
	int ret;

	if ((strict_strtoul(buf, 10, &val) < 0) || (val > 7))
		return -EINVAL;

	ret = px3215_set_mode(data->client, val);
	
	if (ret < 0)
		return ret;
	return count;
}

static DEVICE_ATTR(mode,S_IRUGO | S_IWUSR | S_IWGRP,
		   px3215_show_mode, px3215_store_mode);


/* Px data */
static ssize_t px3215_show_pxvalue(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);

	/* No Px data if power down */
	if (px3215_get_mode(data->client) == 0x00)
		return -EBUSY;

	return sprintf(buf, "%d\n", px3215_get_px_value(data->client));
}

static DEVICE_ATTR(pxvalue,S_IRUGO | S_IWUSR | S_IWGRP, px3215_show_pxvalue, NULL);


/* proximity object detect */
static ssize_t px3215_show_object(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	return sprintf(buf, "%d\n", px3215_get_object(data->client,0));
}

static DEVICE_ATTR(object,S_IRUGO | S_IWUSR | S_IWGRP, px3215_show_object, NULL);


/* Px low threshold */
static ssize_t px3215_show_plthres(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	return sprintf(buf, "%d\n", px3215_get_plthres(data->client));
}

static ssize_t px3215_store_plthres(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	unsigned long val;
	int ret;

	if (strict_strtoul(buf, 10, &val) < 0)
		return -EINVAL;

	ret = px3215_set_plthres(data->client, val);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR(plthres, S_IRUGO | S_IWUSR | S_IWGRP,
		   px3215_show_plthres, px3215_store_plthres);

/* Px high threshold */
static ssize_t px3215_show_phthres(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	return sprintf(buf, "%d\n", px3215_get_phthres(data->client));
}

static ssize_t px3215_store_phthres(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	unsigned long val;
	int ret;

	if (strict_strtoul(buf, 10, &val) < 0)
		return -EINVAL;

	ret = px3215_set_phthres(data->client, val);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR(phthres, S_IRUGO | S_IWUSR | S_IWGRP,
		   px3215_show_phthres, px3215_store_phthres);

static ssize_t proximity_enable_show(struct device *dev, 
		struct device_attribute *attr, 
		char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
	return sprintf(buf, "%d\n", (px3215_get_mode(data->client)) ? 1 : 0);
}

static ssize_t proximity_enable_store(struct device *dev, 
		struct device_attribute *attr, 
		const char *buf, size_t size)
{
	struct input_dev *input = to_input_dev(dev);
	struct px3215_data *data = input_get_drvdata(input);
        bool new_value;

        if (sysfs_streq(buf, "1"))
            new_value = true;
        else if (sysfs_streq(buf, "0"))
            new_value = false;
        else {
            pr_err("%s: invalid value %d\n", __func__, *buf);
            return -EINVAL;
        }

        printk(KERN_INFO "[PX3215] proximity_enable_store: new_value=%d\n", new_value);   

	if(new_value)
	{
		px3215_set_mode(data->client, 2);
		enable_irq(data->irq);
	} else {
		disable_irq_nosync(data->irq);
		px3215_set_mode(data->client, 0);		
	}
	return size;
}


static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_enable_show, proximity_enable_store);

static ssize_t name_read(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", CHIP_NAME);
}

static DEVICE_ATTR(name, 0444, name_read, NULL);

static struct attribute *px3215_attributes[] = {
#if 0
	&dev_attr_mode.attr,
	&dev_attr_object.attr,
	&dev_attr_pxvalue.attr,
	&dev_attr_plthres.attr,
	&dev_attr_phthres.attr,
#endif	
	&dev_attr_enable.attr,
	NULL
};

static const struct attribute_group px3215_attr_group = {
	.attrs = px3215_attributes,
};

#if 0
static DEVICE_ATTR(adc, S_IRUGO | S_IWUSR | S_IWGRP,
	px3215_show_mode, px3215_store_mode);
static DEVICE_ATTR(raw_data, S_IRUGO | S_IWUSR | S_IWGRP,
	px3215_show_pxvalue, NULL);

static struct device_attribute *proximity_attrs[] = {
	&dev_attr_adc,
	&dev_attr_raw_data,
	NULL,
};
#endif

static int px3215_init_client(struct i2c_client *client)
{
	struct px3215_data *data = i2c_get_clientdata(client);
	int i;

	printk(KERN_INFO "[PX3215] %s called\n",__func__);  

	/* read all the registers once to fill the cache.
	 * if one of the reads fails, we consider the init failed */
	for (i = 0; i < ARRAY_SIZE(data->reg_cache); i++) {
		//int v = i2c_smbus_read_byte_data(client, px3215_reg[i]);
		u8 val;
        	u8 value[2] = {0};
        	value[0] = px3215_reg[i];
        	__px3215_i2c_read(value, 2);
                val = (value[1] << 8) | value[0];          
		if (val < 0)
			return -ENODEV;

		data->reg_cache[i] = val;
	}

	/* set defaults */
	px3215_set_mode(client, 0);
	msleep(20);
	px3215_set_plthres(client, 0x4B);
	px3215_set_phthres(client, 0x64);
	/*px3215_set_mode(client, 2);
	msleep(20);*/

	return 0;
}

/*
 * I2C layer
 */

static irqreturn_t px3215_irq(int irq, void *data_)
{
	struct px3215_data *data = data_;
	u8 int_stat;
	int Pval;

  	printk(KERN_INFO "[PX3215] px3215_irq called\n");
    
	mutex_lock(&data->lock);
	int_stat = px3215_get_intstat(data->client);
	// PX int
	if (int_stat & PX3215C_INT_PMASK)
	{
		Pval = px3215_get_object(data->client,1);
		printk("[PX3215] Pval=%d\n", Pval);
                input_report_abs(px3215_input_dev, ABS_DISTANCE, Pval);                
                input_sync(px3215_input_dev);		
	}
	mutex_unlock(&data->lock);

	return IRQ_HANDLED;
}

static int __devinit px3215_probe(struct i2c_client *client,
				    const struct i2c_device_id *id)
{
   
	struct px3215_platform_data *pdata = client->dev.platform_data;
	struct px3215_data *data;
	int err = 0;
	int ret = 0;
	this_client = client;

	printk(KERN_INFO "[PX3215] %s start \n", __func__);	  

	data = kzalloc(sizeof(struct px3215_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_name(&client->dev, client->name);
	data->irq_gpio = pdata->irq_gpio;
	data->irq = pdata->irq;
	data->client = client;
	i2c_set_clientdata(client, data);
	mutex_init(&data->lock);

	err = px3215_input_init(data);
	if (err)
		goto exit_kfree;

	/* initialize the PX3215C chip */
	err = px3215_init_client(this_client);
	if (err)
		goto exit_kfree;    

	/*Initialisation of GPIO_PS_OUT of proximity sensor*/
	if (gpio_request(data->irq_gpio, "Proximity Out")) {
		printk(KERN_ERR "Proximity Request GPIO_%d failed!\n", data->irq_gpio);
	}	
	gpio_direction_input(data->irq_gpio);

        irq_set_irq_type(data->irq, IRQ_TYPE_EDGE_FALLING);	    
	if( (ret = request_irq(data->irq, px3215_irq,  IRQF_DISABLED |IRQF_NO_SUSPEND , "PX3215_INT", data )) )
	{
		pr_err("PX3215 request_irq failed IRQ_NO:%d", data->irq);
		goto exit_kfree;
	} 
	else
	{
		printk(KERN_INFO "PX3215 request_irq success IRQ_NO:%d\n", data->irq);
	}
    
        disable_irq_nosync(data->irq);
        
    	/* register sysfs hooks */
	err = sysfs_create_group(&data->input->dev.kobj, &px3215_attr_group);
	if (err)
		goto exit_input;

	/* set initial proximity value as 1 */
	input_report_abs(px3215_input_dev, ABS_DISTANCE, 1);
	input_sync(px3215_input_dev);

	dev_info(&client->dev, "PX3215C driver version %s enabled\n", DRIVER_VERSION);
	return 0;

exit_input:
	px3215_input_fini(data);

exit_kfree:
	kfree(data);
	return err;
}

static int __devexit px3215_remove(struct i2c_client *client)
{
	struct px3215_data *data = i2c_get_clientdata(client);
	free_irq(data->irq, data);

	sysfs_remove_group(&data->input->dev.kobj, &px3215_attr_group);
	px3215_set_mode(client, 0);
	kfree(i2c_get_clientdata(client));
	return 0;
}


#define px3215_suspend	NULL
#define px3215_resume		NULL

static const struct i2c_device_id px3215_id[] = {
	//{ "px3215", 0 },
	{ "dyna", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, px3215_id);


static const struct dev_pm_ops px3215_pm_ops = {
	.suspend = px3215_suspend,
	.resume = px3215_resume,
};

static struct i2c_driver px3215_driver = {
	.driver = {
		.name	= PX3215C_DRV_NAME,
		.owner	= THIS_MODULE,
                .pm = &px3215_pm_ops,		
	},
	.probe	= px3215_probe,
	.remove	= __devexit_p(px3215_remove),
	.id_table = px3215_id,
};

static int __init px3215_init(void)
{
#if defined(CONFIG_SENSORS_CORE)
	struct device *dev_t;    
	dev_t = device_create( sensors_class, NULL, 0, NULL, "proximity_sensor");

	if (device_create_file(dev_t, &dev_attr_name) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_name.attr.name);

	if (IS_ERR(dev_t)) 
	{
            return PTR_ERR(dev_t);
	}
#endif  
    
	return i2c_add_driver(&px3215_driver);
}

static void __exit px3215_exit(void)
{
#if defined(CONFIG_SENSORS_CORE)    
    	device_destroy(sensors_class, 0);
#endif    	
	i2c_del_driver(&px3215_driver);
}

MODULE_AUTHOR("LiteOn-semi corporation.");
MODULE_DESCRIPTION("Test PX3215C driver on mini6410 with SS main chip.");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);

module_init(px3215_init);
module_exit(px3215_exit);

