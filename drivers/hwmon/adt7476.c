/*
 * A hwmon driver for the Analog Devices ADT7476
 * Copyright (C) 2007 IBM
 *
 * Author: Darrick J. Wong <djwong@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/log2.h>

/* Addresses to scan */
static const unsigned short normal_i2c[] = { 0x2C, 0x2D, 0x2E, I2C_CLIENT_END };

/* Insmod parameters */
I2C_CLIENT_INSMOD_1(adt7476);

/* ADT7476 registers */
#define ADT7476_REG_CFG6			0x10
#define ADT7476_REG_CFG7			0x11

#define ADT7476_REG_EXTRES_BASE_ADDR		0x76

#define ADT7476_REG_VOLT_BASE_ADDR		0x20
#define ADT7476_REG_VOLT_MIN_BASE_ADDR		0x44

#define ADT7476_REG_TEMP_BASE_ADDR		0x25
#define ADT7476_REG_TEMP_LIMITS_BASE_ADDR	0x4E
#define ADT7476_REG_TEMP_TMIN_BASE_ADDR		0x67
#define ADT7476_REG_TEMP_TMAX_BASE_ADDR		0x6A

#define ADT7476_REG_FAN_BASE_ADDR		0x28
#define ADT7476_REG_FAN_MIN_BASE_ADDR		0x54

#define ADT7476_REG_PWM_BASE_ADDR		0x30
#define	ADT7476_REG_PWM_MIN_BASE_ADDR		0x64
#define ADT7476_REG_PWM_MAX_BASE_ADDR		0x38
#define ADT7476_REG_PWM_BHVR_BASE_ADDR		0x5C
#define		ADT7476_PWM_BHVR_MASK		0xE0
#define		ADT7476_PWM_BHVR_SHIFT		5

#define ADT7476_REG_CFG1			0x40
#define 	ADT7476_CFG1_START		0x01
#define		ADT7476_CFG1_READY		0x04
#define ADT7476_REG_CFG2			0x73
#define ADT7476_REG_CFG3			0x78
#define ADT7476_REG_CFG4			0x7D
#define		ADT7476_CFG4_MAX_DUTY_AT_OVT	0x08
#define ADT7476_REG_CFG5			0x7C
#define		ADT7476_CFG5_TEMP_TWOS		0x01
#define		ADT7476_CFG5_TEMP_OFFSET	0x02

#define ADT7476_REG_DEVICE			0x3D
#define 	ADT7476_DEVICE			0x76
#define ADT7476_REG_VENDOR			0x3E
#define 	ADT7476_VENDOR			0x41

#define ADT7476_REG_ALARM1			0x41
#define		ADT7476_2V5_ALARM		0x01
#define		ADT7476_VCCP_ALARM		0x02
#define		ADT7476_VCC_ALARM		0x04
#define		ADT7476_5V_ALARM		0x08
#define		ADT7476_R1T_ALARM		0x10
#define		ADT7476_LT_ALARM		0x20
#define		ADT7476_R2T_ALARM		0x40
#define		ADT7476_OOL			0x80
#define ADT7476_REG_ALARM2			0x42
#define		ADT7476_12V_ALARM		0x01
#define		ADT7476_OVT_ALARM		0x02
#define		ADT7476_FAN1_ALARM		0x04
#define		ADT7476_FAN2_ALARM		0x08
#define		ADT7476_FAN3_ALARM		0x10
#define		ADT7476_FAN4_ALARM		0x20
#define		ADT7476_R1T_SHORT		0x40
#define		ADT7476_R2T_SHORT		0x80

#define ALARM2(x)	((x) << 8)

#define ADT7476_EXTRES_COUNT	2
#define ADT7476_REG_EXTRES(x)	(ADT7476_REG_EXTRES_BASE_ADDR + (x))

#define ADT7476_VOLT_COUNT	5
#define ADT7476_REG_VOLT(x)	(ADT7476_REG_VOLT_BASE_ADDR + (x))
#define ADT7476_REG_VOLT_MIN(x)	(ADT7476_REG_VOLT_MIN_BASE_ADDR + ((x) * 2))
#define ADT7476_REG_VOLT_MAX(x)	(ADT7476_REG_VOLT_MIN_BASE_ADDR + \
				((x) * 2) + 1)

#define ADT7476_TEMP_COUNT	3
#define ADT7476_REG_TEMP(x)	(ADT7476_REG_TEMP_BASE_ADDR + (x))
#define ADT7476_REG_TEMP_MIN(x) (ADT7476_REG_TEMP_LIMITS_BASE_ADDR + ((x) * 2))
#define ADT7476_REG_TEMP_MAX(x) (ADT7476_REG_TEMP_LIMITS_BASE_ADDR + \
				((x) * 2) + 1)
#define ADT7476_REG_TEMP_TMIN(x)	(ADT7476_REG_TEMP_TMIN_BASE_ADDR + (x))
#define ADT7476_REG_TEMP_TMAX(x)	(ADT7476_REG_TEMP_TMAX_BASE_ADDR + (x))

#define ADT7476_FAN_COUNT	4
#define ADT7476_REG_FAN(x)	(ADT7476_REG_FAN_BASE_ADDR + ((x) * 2))
#define ADT7476_REG_FAN_MIN(x)	(ADT7476_REG_FAN_MIN_BASE_ADDR + ((x) * 2))

#define ADT7476_PWM_COUNT	3
#define ADT7476_REG_PWM(x)	(ADT7476_REG_PWM_BASE_ADDR + (x))
#define ADT7476_REG_PWM_MAX(x)	(ADT7476_REG_PWM_MAX_BASE_ADDR + (x))
#define ADT7476_REG_PWM_MIN(x)	(ADT7476_REG_PWM_MIN_BASE_ADDR + (x))
#define ADT7476_REG_PWM_BHVR(x)	(ADT7476_REG_PWM_BHVR_BASE_ADDR + (x))

/* How often do we reread sensors values? (In jiffies) */
#define SENSOR_REFRESH_INTERVAL	(2 * HZ)

/* How often do we reread sensor limit values? (In jiffies) */
#define LIMIT_REFRESH_INTERVAL	(60 * HZ)

/* datasheet says to divide this number by the fan reading to get fan rpm */
#define FAN_PERIOD_TO_RPM(x)	((90000 * 60) / (x))
#define FAN_RPM_TO_PERIOD	FAN_PERIOD_TO_RPM
#define FAN_PERIOD_INVALID	65535
#define FAN_DATA_VALID(x)	((x) && (x) != FAN_PERIOD_INVALID)

struct adt7476_data {
	struct device		*hwmon_dev;
	struct attribute_group	attrs;
	struct mutex		lock;
	char			sensors_valid;
	char			limits_valid;
	unsigned long		sensors_last_updated;	/* In jiffies */
	unsigned long		limits_last_updated;	/* In jiffies */

	u8			extres[ADT7476_EXTRES_COUNT];

	u8			volt[ADT7476_VOLT_COUNT];
	u8			volt_min[ADT7476_VOLT_COUNT];
	u8			volt_max[ADT7476_VOLT_COUNT];

	s8			temp[ADT7476_TEMP_COUNT];
	s8			temp_min[ADT7476_TEMP_COUNT];
	s8			temp_max[ADT7476_TEMP_COUNT];
	s8			temp_tmin[ADT7476_TEMP_COUNT];
	/* This is called the !THERM limit in the datasheet */
	s8			temp_tmax[ADT7476_TEMP_COUNT];

	u16			fan[ADT7476_FAN_COUNT];
	u16			fan_min[ADT7476_FAN_COUNT];

	u8			pwm[ADT7476_PWM_COUNT];
	u8			pwm_max[ADT7476_PWM_COUNT];
	u8			pwm_min[ADT7476_PWM_COUNT];
	u8			pwm_behavior[ADT7476_PWM_COUNT];

	u8			temp_twos_complement;
	u8			temp_offset;

	u16			alarm;
	u8			max_duty_at_overheat;
};

static int adt7476_probe(struct i2c_client *client,
			 const struct i2c_device_id *id);
static int adt7476_detect(struct i2c_client *client, int kind,
			  struct i2c_board_info *info);
static int adt7476_remove(struct i2c_client *client);

static const struct i2c_device_id adt7476_id[] = {
	{ "adt7476", adt7476 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, adt7476_id);

static struct i2c_driver adt7476_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "adt7476",
	},
	.probe		= adt7476_probe,
	.remove		= adt7476_remove,
	.id_table	= adt7476_id,
	.detect		= adt7476_detect,
	.address_data	= &addr_data,
};

/*
 * 16-bit registers on the ADT7476 are low-byte first.  The data sheet says
 * that the low byte must be read before the high byte.
 */
static inline int adt7476_read_word_data(struct i2c_client *client, u8 reg)
{
	u16 foo;
	foo = i2c_smbus_read_byte_data(client, reg);
	foo |= ((u16)i2c_smbus_read_byte_data(client, reg + 1) << 8);
	return foo;
}

static inline int adt7476_write_word_data(struct i2c_client *client, u8 reg,
					  u16 value)
{
	return i2c_smbus_write_byte_data(client, reg, value & 0xFF)
	       && i2c_smbus_write_byte_data(client, reg + 1, value >> 8);
}

static void adt7476_init_client(struct i2c_client *client)
{
	int reg = i2c_smbus_read_byte_data(client, ADT7476_REG_CFG1);

	if (!(reg & ADT7476_CFG1_READY)) {
		dev_err(&client->dev, "Chip not ready.\n");
	} else {
		/* start monitoring */
		i2c_smbus_write_byte_data(client, ADT7476_REG_CFG1,
					  reg | ADT7476_CFG1_START);
	}
}

static struct adt7476_data *adt7476_update_device(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	unsigned long local_jiffies = jiffies;
	u8 cfg;
	int i;

	mutex_lock(&data->lock);
	if (time_before(local_jiffies, data->sensors_last_updated +
		SENSOR_REFRESH_INTERVAL)
		&& data->sensors_valid)
		goto no_sensor_update;

	/*
	 * Read extended resolution bits first
	 * This locks the voltage/temeprature values until they
	 * have all been read
	 */
	for (i = 0; i < ADT7476_EXTRES_COUNT; i++)
		data->extres[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_EXTRES(i));

	for (i = 0; i < ADT7476_VOLT_COUNT; i++)
		data->volt[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_VOLT(i));

	/* Determine temperature encoding */
	cfg = i2c_smbus_read_byte_data(client, ADT7476_REG_CFG5);
	data->temp_twos_complement = (cfg & ADT7476_CFG5_TEMP_TWOS);

	/*
	 * What does this do? it implies a variable temperature sensor
	 * offset, but the datasheet doesn't say anything about this bit
	 * and other parts of the datasheet imply that "offset64" mode
	 * means that you shift temp values by -64 if the above bit was set.
	 */
	data->temp_offset = (cfg & ADT7476_CFG5_TEMP_OFFSET);

	for (i = 0; i < ADT7476_TEMP_COUNT; i++)
		data->temp[i] = i2c_smbus_read_byte_data(client,
							 ADT7476_REG_TEMP(i));

	for (i = 0; i < ADT7476_FAN_COUNT; i++)
		data->fan[i] = adt7476_read_word_data(client,
						ADT7476_REG_FAN(i));

	for (i = 0; i < ADT7476_PWM_COUNT; i++)
		data->pwm[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_PWM(i));

	data->alarm = i2c_smbus_read_byte_data(client, ADT7476_REG_ALARM1);
	if (data->alarm & ADT7476_OOL)
		data->alarm |= ALARM2(i2c_smbus_read_byte_data(client,
							 ADT7476_REG_ALARM2));

	data->sensors_last_updated = local_jiffies;
	data->sensors_valid = 1;

no_sensor_update:
	if (time_before(local_jiffies, data->limits_last_updated +
		LIMIT_REFRESH_INTERVAL)
		&& data->limits_valid)
		goto out;

	for (i = 0; i < ADT7476_VOLT_COUNT; i++) {
		data->volt_min[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_VOLT_MIN(i));
		data->volt_max[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_VOLT_MAX(i));
	}

	for (i = 0; i < ADT7476_TEMP_COUNT; i++) {
		data->temp_min[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_TEMP_MIN(i));
		data->temp_max[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_TEMP_MAX(i));
		data->temp_tmin[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_TEMP_TMIN(i));
		data->temp_tmax[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_TEMP_TMAX(i));
	}

	for (i = 0; i < ADT7476_FAN_COUNT; i++)
		data->fan_min[i] = adt7476_read_word_data(client,
						ADT7476_REG_FAN_MIN(i));

	for (i = 0; i < ADT7476_PWM_COUNT; i++) {
		data->pwm_max[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_PWM_MAX(i));
		data->pwm_min[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_PWM_MIN(i));
		data->pwm_behavior[i] = i2c_smbus_read_byte_data(client,
						ADT7476_REG_PWM_BHVR(i));
	}

	i = i2c_smbus_read_byte_data(client, ADT7476_REG_CFG4);
	data->max_duty_at_overheat = !!(i & ADT7476_CFG4_MAX_DUTY_AT_OVT);

	data->limits_last_updated = local_jiffies;
	data->limits_valid = 1;

out:
	mutex_unlock(&data->lock);
	return data;
}

/*
 * On this chip, voltages are given as a count of steps between a minimum
 * and maximum voltage, not a direct voltage.
 */
static const struct {
	int cmin;
	int cmax;
} volt_scale[] = {
	{ 32,  33267},		/* 2.5V		0.0032 - 3.3267 */
	{ 29,  29970},		/* VCCP		0.0029 - 2.9970 */
	{ 42,  43957},		/* VCC		0.0042 - 4.3957 */
	{ 65,  66634},		/* 5V		0.0065 - 6.6634 */
	{156, 159843},		/* 12V		0.0156 -15.9843 */
};

static int decode_volt(int volt_index, u16 raw)
{
	int cmin = volt_scale[volt_index].cmin;
	int cmax = volt_scale[volt_index].cmax;
	return ((raw * (cmax - cmin)) / 1024) + cmin;
}

static u16 encode_volt(int volt_index, int cooked)
{
	int cmin = volt_scale[volt_index].cmin;
	int cmax = volt_scale[volt_index].cmax;
	u16 x;

	if (cooked > cmax)
		cooked = cmax;
	else if (cooked < cmin)
		cooked = cmin;

	x = ((cooked - cmin) * 1024) / (cmax - cmin);

	return x;
}

static ssize_t show_volt_min(struct device *dev,
			     struct device_attribute *devattr,
			     char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n",
		       decode_volt(attr->index, data->volt_min[attr->index] << 2));
}

static ssize_t set_volt_min(struct device *dev,
			    struct device_attribute *devattr,
			    const char *buf,
			    size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int volt = encode_volt(attr->index, simple_strtol(buf, NULL, 10));

	mutex_lock(&data->lock);
	data->volt_min[attr->index] = volt >> 2;
	i2c_smbus_write_byte_data(client, ADT7476_REG_VOLT_MIN(attr->index),
				  data->volt_min[attr->index]);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_volt_max(struct device *dev,
			     struct device_attribute *devattr,
			     char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n",
		       decode_volt(attr->index, data->volt_max[attr->index] << 2));
}

static ssize_t set_volt_max(struct device *dev,
			    struct device_attribute *devattr,
			    const char *buf,
			    size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int volt = encode_volt(attr->index, simple_strtol(buf, NULL, 10));

	mutex_lock(&data->lock);
	data->volt_max[attr->index] = volt >> 2;
	i2c_smbus_write_byte_data(client, ADT7476_REG_VOLT_MAX(attr->index),
				  data->volt_max[attr->index]);
	mutex_unlock(&data->lock);

	return count;
}

/* extraction information ofr extended resolution voltage */
static const struct {
	u8	extreg;
	u8	shift;
} extvolt[ADT7476_VOLT_COUNT] = {
	{0, 0},
	{0, 2},
	{0, 4},
	{0, 6},
	{1, 0},
};

static ssize_t show_volt(struct device *dev, struct device_attribute *devattr,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	int i = attr->index;
	u16 volt = (data->volt[i] << 2) | ((data->extres[extvolt[i].extreg] >> extvolt[i].shift) & 3);

	return sprintf(buf, "%d\n", decode_volt(i, volt));
}

/*
 * This chip can report temperature data either as a two's complement
 * number in the range -128 to 127, or as an unsigned number that must
 * be offset by 64.
 */
static int decode_temp(u8 twos_complement, u8 raw)
{
	return twos_complement ? (s8)raw:raw - 64;
}

static u8 encode_temp(u8 twos_complement, int cooked)
{
	return twos_complement ? cooked & 0xFF : cooked + 64;
}

static ssize_t show_temp_min(struct device *dev,
			     struct device_attribute *devattr,
			     char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", 1000 * decode_temp(
						data->temp_twos_complement,
						data->temp_min[attr->index]));
}

static ssize_t set_temp_min(struct device *dev,
			    struct device_attribute *devattr,
			    const char *buf,
			    size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10) / 1000;
	temp = encode_temp(data->temp_twos_complement, temp);

	mutex_lock(&data->lock);
	data->temp_min[attr->index] = temp;
	i2c_smbus_write_byte_data(client, ADT7476_REG_TEMP_MIN(attr->index),
				  temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_temp_max(struct device *dev,
			     struct device_attribute *devattr,
			     char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", 1000 * decode_temp(
						data->temp_twos_complement,
						data->temp_max[attr->index]));
}

static ssize_t set_temp_max(struct device *dev,
			    struct device_attribute *devattr,
			    const char *buf,
			    size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10) / 1000;
	temp = encode_temp(data->temp_twos_complement, temp);

	mutex_lock(&data->lock);
	data->temp_max[attr->index] = temp;
	i2c_smbus_write_byte_data(client, ADT7476_REG_TEMP_MAX(attr->index),
				  temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_temp(struct device *dev, struct device_attribute *devattr,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", 1000 * decode_temp(
						data->temp_twos_complement,
						data->temp[attr->index]));
}

static ssize_t show_fan_min(struct device *dev,
			    struct device_attribute *devattr,
			    char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);

	if (FAN_DATA_VALID(data->fan_min[attr->index]))
		return sprintf(buf, "%d\n",
			       FAN_PERIOD_TO_RPM(data->fan_min[attr->index]));
	else
		return sprintf(buf, "0\n");
}

static ssize_t set_fan_min(struct device *dev,
			   struct device_attribute *devattr,
			   const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10);

	if (!temp)
		return -EINVAL;
	temp = FAN_RPM_TO_PERIOD(temp);

	mutex_lock(&data->lock);
	data->fan_min[attr->index] = temp;
	adt7476_write_word_data(client, ADT7476_REG_FAN_MIN(attr->index), temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_fan(struct device *dev, struct device_attribute *devattr,
			char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);

	if (FAN_DATA_VALID(data->fan[attr->index]))
		return sprintf(buf, "%d\n",
			       FAN_PERIOD_TO_RPM(data->fan[attr->index]));
	else
		return sprintf(buf, "0\n");
}

static ssize_t show_max_duty_at_crit(struct device *dev,
				     struct device_attribute *devattr,
				     char *buf)
{
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", data->max_duty_at_overheat);
}

static ssize_t set_max_duty_at_crit(struct device *dev,
				    struct device_attribute *devattr,
				    const char *buf,
				    size_t count)
{
	u8 reg;
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10);

	mutex_lock(&data->lock);
	data->max_duty_at_overheat = !!temp;
	reg = i2c_smbus_read_byte_data(client, ADT7476_REG_CFG4);
	if (temp)
		reg |= ADT7476_CFG4_MAX_DUTY_AT_OVT;
	else
		reg &= ~ADT7476_CFG4_MAX_DUTY_AT_OVT;
	i2c_smbus_write_byte_data(client, ADT7476_REG_CFG4, reg);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_pwm(struct device *dev, struct device_attribute *devattr,
			char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", data->pwm[attr->index]);
}

static ssize_t set_pwm(struct device *dev, struct device_attribute *devattr,
			const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10);

	mutex_lock(&data->lock);
	data->pwm[attr->index] = temp;
	i2c_smbus_write_byte_data(client, ADT7476_REG_PWM(attr->index), temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_pwm_max(struct device *dev,
			    struct device_attribute *devattr,
			    char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", data->pwm_max[attr->index]);
}

static ssize_t set_pwm_max(struct device *dev,
			   struct device_attribute *devattr,
			   const char *buf,
			   size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10);

	mutex_lock(&data->lock);
	data->pwm_max[attr->index] = temp;
	i2c_smbus_write_byte_data(client, ADT7476_REG_PWM_MAX(attr->index),
				  temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_pwm_min(struct device *dev,
			    struct device_attribute *devattr,
			    char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", data->pwm_min[attr->index]);
}

static ssize_t set_pwm_min(struct device *dev,
			   struct device_attribute *devattr,
			   const char *buf,
			   size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10);

	mutex_lock(&data->lock);
	data->pwm_min[attr->index] = temp;
	i2c_smbus_write_byte_data(client, ADT7476_REG_PWM_MIN(attr->index),
				  temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_temp_tmax(struct device *dev,
			      struct device_attribute *devattr,
			      char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", 1000 * decode_temp(
						data->temp_twos_complement,
						data->temp_tmax[attr->index]));
}

static ssize_t set_temp_tmax(struct device *dev,
			     struct device_attribute *devattr,
			     const char *buf,
			     size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10) / 1000;
	temp = encode_temp(data->temp_twos_complement, temp);

	mutex_lock(&data->lock);
	data->temp_tmax[attr->index] = temp;
	i2c_smbus_write_byte_data(client, ADT7476_REG_TEMP_TMAX(attr->index),
				  temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_temp_tmin(struct device *dev,
			      struct device_attribute *devattr,
			      char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	return sprintf(buf, "%d\n", 1000 * decode_temp(
						data->temp_twos_complement,
						data->temp_tmin[attr->index]));
}

static ssize_t set_temp_tmin(struct device *dev,
			     struct device_attribute *devattr,
			     const char *buf,
			     size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10) / 1000;
	temp = encode_temp(data->temp_twos_complement, temp);

	mutex_lock(&data->lock);
	data->temp_tmin[attr->index] = temp;
	i2c_smbus_write_byte_data(client, ADT7476_REG_TEMP_TMIN(attr->index),
				  temp);
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_pwm_enable(struct device *dev,
			       struct device_attribute *devattr,
			       char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);

	switch (data->pwm_behavior[attr->index] >> ADT7476_PWM_BHVR_SHIFT) {
	case 3:
		return sprintf(buf, "0\n");
	case 7:
		return sprintf(buf, "1\n");
	default:
		return sprintf(buf, "2\n");
	}
}

static ssize_t set_pwm_enable(struct device *dev,
			      struct device_attribute *devattr,
			      const char *buf,
			      size_t count)
{
	u8 reg;
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10);

	switch (temp) {
	case 0:
		temp = 3;
		break;
	case 1:
		temp = 7;
		break;
	case 2:
		/* Enter automatic mode with fans off */
		temp = 4;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&data->lock);
	reg = i2c_smbus_read_byte_data(client,
				       ADT7476_REG_PWM_BHVR(attr->index));
	reg = (temp << ADT7476_PWM_BHVR_SHIFT) |
	      (reg & ~ADT7476_PWM_BHVR_MASK);
	i2c_smbus_write_byte_data(client, ADT7476_REG_PWM_BHVR(attr->index),
				  reg);
	data->pwm_behavior[attr->index] = reg;
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_pwm_auto_temp(struct device *dev,
				  struct device_attribute *devattr,
				  char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);
	int bhvr = data->pwm_behavior[attr->index] >> ADT7476_PWM_BHVR_SHIFT;

	switch (bhvr) {
	case 3:
	case 4:
	case 7:
		return sprintf(buf, "0\n");
	case 0:
	case 1:
	case 5:
	case 6:
		return sprintf(buf, "%d\n", bhvr + 1);
	case 2:
		return sprintf(buf, "4\n");
	}
	/* shouldn't ever get here */
	BUG();
}

static ssize_t set_pwm_auto_temp(struct device *dev,
				 struct device_attribute *devattr,
				 const char *buf,
				 size_t count)
{
	u8 reg;
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev);
	struct adt7476_data *data = i2c_get_clientdata(client);
	int temp = simple_strtol(buf, NULL, 10);

	switch (temp) {
	case 1:
	case 2:
	case 6:
	case 7:
		temp--;
		break;
	case 0:
		temp = 4;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&data->lock);
	reg = i2c_smbus_read_byte_data(client,
				       ADT7476_REG_PWM_BHVR(attr->index));
	reg = (temp << ADT7476_PWM_BHVR_SHIFT) |
	      (reg & ~ADT7476_PWM_BHVR_MASK);
	i2c_smbus_write_byte_data(client, ADT7476_REG_PWM_BHVR(attr->index),
				  reg);
	data->pwm_behavior[attr->index] = reg;
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t show_alarm(struct device *dev,
			  struct device_attribute *devattr,
			  char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct adt7476_data *data = adt7476_update_device(dev);

	if (data->alarm & attr->index)
		return sprintf(buf, "1\n");
	else
		return sprintf(buf, "0\n");
}


static SENSOR_DEVICE_ATTR(in1_max, S_IWUSR | S_IRUGO, show_volt_max,
			  set_volt_max, 0);
static SENSOR_DEVICE_ATTR(in2_max, S_IWUSR | S_IRUGO, show_volt_max,
			  set_volt_max, 1);
static SENSOR_DEVICE_ATTR(in3_max, S_IWUSR | S_IRUGO, show_volt_max,
			  set_volt_max, 2);
static SENSOR_DEVICE_ATTR(in4_max, S_IWUSR | S_IRUGO, show_volt_max,
			  set_volt_max, 3);
static SENSOR_DEVICE_ATTR(in5_max, S_IWUSR | S_IRUGO, show_volt_max,
			  set_volt_max, 4);

static SENSOR_DEVICE_ATTR(in1_min, S_IWUSR | S_IRUGO, show_volt_min,
			  set_volt_min, 0);
static SENSOR_DEVICE_ATTR(in2_min, S_IWUSR | S_IRUGO, show_volt_min,
			  set_volt_min, 1);
static SENSOR_DEVICE_ATTR(in3_min, S_IWUSR | S_IRUGO, show_volt_min,
			  set_volt_min, 2);
static SENSOR_DEVICE_ATTR(in4_min, S_IWUSR | S_IRUGO, show_volt_min,
			  set_volt_min, 3);
static SENSOR_DEVICE_ATTR(in5_min, S_IWUSR | S_IRUGO, show_volt_min,
			  set_volt_min, 4);

static SENSOR_DEVICE_ATTR(in1_input, S_IRUGO, show_volt, NULL, 0);
static SENSOR_DEVICE_ATTR(in2_input, S_IRUGO, show_volt, NULL, 1);
static SENSOR_DEVICE_ATTR(in3_input, S_IRUGO, show_volt, NULL, 2);
static SENSOR_DEVICE_ATTR(in4_input, S_IRUGO, show_volt, NULL, 3);
static SENSOR_DEVICE_ATTR(in5_input, S_IRUGO, show_volt, NULL, 4);

static SENSOR_DEVICE_ATTR(in1_alarm, S_IRUGO, show_alarm, NULL,
			  ADT7476_2V5_ALARM);
static SENSOR_DEVICE_ATTR(in2_alarm, S_IRUGO, show_alarm, NULL,
			  ADT7476_VCCP_ALARM);
static SENSOR_DEVICE_ATTR(in3_alarm, S_IRUGO, show_alarm, NULL,
			  ADT7476_VCC_ALARM);
static SENSOR_DEVICE_ATTR(in4_alarm, S_IRUGO, show_alarm, NULL,
			  ADT7476_5V_ALARM);
static SENSOR_DEVICE_ATTR(in5_alarm, S_IRUGO, show_alarm, NULL,
			  ALARM2(ADT7476_12V_ALARM));

static SENSOR_DEVICE_ATTR(temp1_max, S_IWUSR | S_IRUGO, show_temp_max,
			  set_temp_max, 0);
static SENSOR_DEVICE_ATTR(temp2_max, S_IWUSR | S_IRUGO, show_temp_max,
			  set_temp_max, 1);
static SENSOR_DEVICE_ATTR(temp3_max, S_IWUSR | S_IRUGO, show_temp_max,
			  set_temp_max, 2);

static SENSOR_DEVICE_ATTR(temp1_min, S_IWUSR | S_IRUGO, show_temp_min,
			  set_temp_min, 0);
static SENSOR_DEVICE_ATTR(temp2_min, S_IWUSR | S_IRUGO, show_temp_min,
			  set_temp_min, 1);
static SENSOR_DEVICE_ATTR(temp3_min, S_IWUSR | S_IRUGO, show_temp_min,
			  set_temp_min, 2);

static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, show_temp, NULL, 0);

static SENSOR_DEVICE_ATTR(temp2_input, S_IRUGO, show_temp, NULL, 1);
static SENSOR_DEVICE_ATTR(temp3_input, S_IRUGO, show_temp, NULL, 2);

static SENSOR_DEVICE_ATTR(temp1_alarm, S_IRUGO, show_alarm, NULL,
			  ADT7476_R1T_ALARM | ALARM2(ADT7476_R1T_SHORT));
static SENSOR_DEVICE_ATTR(temp2_alarm, S_IRUGO, show_alarm, NULL,
			  ADT7476_LT_ALARM);
static SENSOR_DEVICE_ATTR(temp3_alarm, S_IRUGO, show_alarm, NULL,
			  ADT7476_R2T_ALARM | ALARM2(ADT7476_R2T_SHORT));

static SENSOR_DEVICE_ATTR(fan1_min, S_IWUSR | S_IRUGO, show_fan_min,
			  set_fan_min, 0);
static SENSOR_DEVICE_ATTR(fan2_min, S_IWUSR | S_IRUGO, show_fan_min,
			  set_fan_min, 1);
static SENSOR_DEVICE_ATTR(fan3_min, S_IWUSR | S_IRUGO, show_fan_min,
			  set_fan_min, 2);
static SENSOR_DEVICE_ATTR(fan4_min, S_IWUSR | S_IRUGO, show_fan_min,
			  set_fan_min, 3);

static SENSOR_DEVICE_ATTR(fan1_input, S_IRUGO, show_fan, NULL, 0);
static SENSOR_DEVICE_ATTR(fan2_input, S_IRUGO, show_fan, NULL, 1);
static SENSOR_DEVICE_ATTR(fan3_input, S_IRUGO, show_fan, NULL, 2);
static SENSOR_DEVICE_ATTR(fan4_input, S_IRUGO, show_fan, NULL, 3);

static SENSOR_DEVICE_ATTR(fan1_alarm, S_IRUGO, show_alarm, NULL,
			  ALARM2(ADT7476_FAN1_ALARM));
static SENSOR_DEVICE_ATTR(fan2_alarm, S_IRUGO, show_alarm, NULL,
			  ALARM2(ADT7476_FAN2_ALARM));
static SENSOR_DEVICE_ATTR(fan3_alarm, S_IRUGO, show_alarm, NULL,
			  ALARM2(ADT7476_FAN3_ALARM));
static SENSOR_DEVICE_ATTR(fan4_alarm, S_IRUGO, show_alarm, NULL,
			  ALARM2(ADT7476_FAN4_ALARM));

static SENSOR_DEVICE_ATTR(pwm_use_point2_pwm_at_crit, S_IWUSR | S_IRUGO,
			  show_max_duty_at_crit, set_max_duty_at_crit, 0);

static SENSOR_DEVICE_ATTR(pwm1, S_IWUSR | S_IRUGO, show_pwm, set_pwm, 0);
static SENSOR_DEVICE_ATTR(pwm2, S_IWUSR | S_IRUGO, show_pwm, set_pwm, 1);
static SENSOR_DEVICE_ATTR(pwm3, S_IWUSR | S_IRUGO, show_pwm, set_pwm, 2);

static SENSOR_DEVICE_ATTR(pwm1_auto_point1_pwm, S_IWUSR | S_IRUGO,
			  show_pwm_min, set_pwm_min, 0);
static SENSOR_DEVICE_ATTR(pwm2_auto_point1_pwm, S_IWUSR | S_IRUGO,
			  show_pwm_min, set_pwm_min, 1);
static SENSOR_DEVICE_ATTR(pwm3_auto_point1_pwm, S_IWUSR | S_IRUGO,
			  show_pwm_min, set_pwm_min, 2);

static SENSOR_DEVICE_ATTR(pwm1_auto_point2_pwm, S_IWUSR | S_IRUGO,
			  show_pwm_max, set_pwm_max, 0);
static SENSOR_DEVICE_ATTR(pwm2_auto_point2_pwm, S_IWUSR | S_IRUGO,
			  show_pwm_max, set_pwm_max, 1);
static SENSOR_DEVICE_ATTR(pwm3_auto_point2_pwm, S_IWUSR | S_IRUGO,
			  show_pwm_max, set_pwm_max, 2);

static SENSOR_DEVICE_ATTR(temp1_auto_point1_temp, S_IWUSR | S_IRUGO,
			  show_temp_tmin, set_temp_tmin, 0);
static SENSOR_DEVICE_ATTR(temp2_auto_point1_temp, S_IWUSR | S_IRUGO,
			  show_temp_tmin, set_temp_tmin, 1);
static SENSOR_DEVICE_ATTR(temp3_auto_point1_temp, S_IWUSR | S_IRUGO,
			  show_temp_tmin, set_temp_tmin, 2);

static SENSOR_DEVICE_ATTR(temp1_auto_point2_temp, S_IWUSR | S_IRUGO,
			  show_temp_tmax, set_temp_tmax, 0);
static SENSOR_DEVICE_ATTR(temp2_auto_point2_temp, S_IWUSR | S_IRUGO,
			  show_temp_tmax, set_temp_tmax, 1);
static SENSOR_DEVICE_ATTR(temp3_auto_point2_temp, S_IWUSR | S_IRUGO,
			  show_temp_tmax, set_temp_tmax, 2);

static SENSOR_DEVICE_ATTR(pwm1_enable, S_IWUSR | S_IRUGO, show_pwm_enable,
			  set_pwm_enable, 0);
static SENSOR_DEVICE_ATTR(pwm2_enable, S_IWUSR | S_IRUGO, show_pwm_enable,
			  set_pwm_enable, 1);
static SENSOR_DEVICE_ATTR(pwm3_enable, S_IWUSR | S_IRUGO, show_pwm_enable,
			  set_pwm_enable, 2);

static SENSOR_DEVICE_ATTR(pwm1_auto_channels_temp, S_IWUSR | S_IRUGO,
			  show_pwm_auto_temp, set_pwm_auto_temp, 0);
static SENSOR_DEVICE_ATTR(pwm2_auto_channels_temp, S_IWUSR | S_IRUGO,
			  show_pwm_auto_temp, set_pwm_auto_temp, 1);
static SENSOR_DEVICE_ATTR(pwm3_auto_channels_temp, S_IWUSR | S_IRUGO,
			  show_pwm_auto_temp, set_pwm_auto_temp, 2);

static struct attribute *adt7476_attr[] =
{
	&sensor_dev_attr_in1_max.dev_attr.attr,
	&sensor_dev_attr_in2_max.dev_attr.attr,
	&sensor_dev_attr_in3_max.dev_attr.attr,
	&sensor_dev_attr_in4_max.dev_attr.attr,
	&sensor_dev_attr_in5_max.dev_attr.attr,

	&sensor_dev_attr_in1_min.dev_attr.attr,
	&sensor_dev_attr_in2_min.dev_attr.attr,
	&sensor_dev_attr_in3_min.dev_attr.attr,
	&sensor_dev_attr_in4_min.dev_attr.attr,
	&sensor_dev_attr_in5_min.dev_attr.attr,

	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,
	&sensor_dev_attr_in5_input.dev_attr.attr,

	&sensor_dev_attr_in1_alarm.dev_attr.attr,
	&sensor_dev_attr_in2_alarm.dev_attr.attr,
	&sensor_dev_attr_in3_alarm.dev_attr.attr,
	&sensor_dev_attr_in4_alarm.dev_attr.attr,
	&sensor_dev_attr_in5_alarm.dev_attr.attr,

	&sensor_dev_attr_temp1_max.dev_attr.attr,
	&sensor_dev_attr_temp2_max.dev_attr.attr,
	&sensor_dev_attr_temp3_max.dev_attr.attr,
	&sensor_dev_attr_temp1_min.dev_attr.attr,
	&sensor_dev_attr_temp2_min.dev_attr.attr,
	&sensor_dev_attr_temp3_min.dev_attr.attr,
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	&sensor_dev_attr_temp1_alarm.dev_attr.attr,
	&sensor_dev_attr_temp2_alarm.dev_attr.attr,
	&sensor_dev_attr_temp3_alarm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_temp3_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_temp3_auto_point2_temp.dev_attr.attr,

	&sensor_dev_attr_fan1_min.dev_attr.attr,
	&sensor_dev_attr_fan2_min.dev_attr.attr,
	&sensor_dev_attr_fan3_min.dev_attr.attr,
	&sensor_dev_attr_fan4_min.dev_attr.attr,
	&sensor_dev_attr_fan1_input.dev_attr.attr,
	&sensor_dev_attr_fan2_input.dev_attr.attr,
	&sensor_dev_attr_fan3_input.dev_attr.attr,
	&sensor_dev_attr_fan4_input.dev_attr.attr,
	&sensor_dev_attr_fan1_alarm.dev_attr.attr,
	&sensor_dev_attr_fan2_alarm.dev_attr.attr,
	&sensor_dev_attr_fan3_alarm.dev_attr.attr,
	&sensor_dev_attr_fan4_alarm.dev_attr.attr,

	&sensor_dev_attr_pwm_use_point2_pwm_at_crit.dev_attr.attr,

	&sensor_dev_attr_pwm1.dev_attr.attr,
	&sensor_dev_attr_pwm2.dev_attr.attr,
	&sensor_dev_attr_pwm3.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm3_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm3_auto_point2_pwm.dev_attr.attr,

	&sensor_dev_attr_pwm1_enable.dev_attr.attr,
	&sensor_dev_attr_pwm2_enable.dev_attr.attr,
	&sensor_dev_attr_pwm3_enable.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_channels_temp.dev_attr.attr,
	&sensor_dev_attr_pwm2_auto_channels_temp.dev_attr.attr,
	&sensor_dev_attr_pwm3_auto_channels_temp.dev_attr.attr,

	NULL
};

/* Return 0 if detection is successful, -ENODEV otherwise */
static int adt7476_detect(struct i2c_client *client, int kind,
			  struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	if (kind <= 0) {
		int vendor, device;

		vendor = i2c_smbus_read_byte_data(client, ADT7476_REG_VENDOR);
		if (vendor != ADT7476_VENDOR)
			return -ENODEV;

		device = i2c_smbus_read_byte_data(client, ADT7476_REG_DEVICE);
		if (device != ADT7476_DEVICE)
			return -ENODEV;

	} else
		dev_dbg(&adapter->dev, "detection forced\n");

	strlcpy(info->type, "adt7476", I2C_NAME_SIZE);

	return 0;
}

static int adt7476_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct adt7476_data *data;
	int err;

	data = kzalloc(sizeof(struct adt7476_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto exit;
	}

	i2c_set_clientdata(client, data);
	mutex_init(&data->lock);

	dev_info(&client->dev, "%s chip found\n", client->name);

	/* Initialize the ADT7476 chip */
	adt7476_init_client(client);

	/* Register sysfs hooks */
	data->attrs.attrs = adt7476_attr;
	err = sysfs_create_group(&client->dev.kobj, &data->attrs);
	if (err)
		goto exit_free;

	data->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(data->hwmon_dev)) {
		err = PTR_ERR(data->hwmon_dev);
		goto exit_remove;
	}

	return 0;

exit_remove:
	sysfs_remove_group(&client->dev.kobj, &data->attrs);
exit_free:
	kfree(data);
exit:
	return err;
}

static int adt7476_remove(struct i2c_client *client)
{
	struct adt7476_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);
	sysfs_remove_group(&client->dev.kobj, &data->attrs);
	kfree(data);
	return 0;
}

static int __init adt7476_init(void)
{
	return i2c_add_driver(&adt7476_driver);
}

static void __exit adt7476_exit(void)
{
	i2c_del_driver(&adt7476_driver);
}

MODULE_AUTHOR("Darrick J. Wong <djwong@us.ibm.com>");
MODULE_DESCRIPTION("ADT7476 driver");
MODULE_LICENSE("GPL");

module_init(adt7476_init);
module_exit(adt7476_exit);
