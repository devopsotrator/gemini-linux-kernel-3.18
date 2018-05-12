/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */


#include "step_counter.h"

static struct step_c_context *step_c_context_obj;
static DEFINE_SPINLOCK(step_irqsafe_lock);

static struct step_c_init_info *step_counter_init_list[MAX_CHOOSE_STEP_C_NUM] = { 0 };

static void step_c_work_func(struct work_struct *work)
{

	struct step_c_context *cxt = NULL;
	uint32_t counter;
	/* hwm_sensor_data sensor_data; */
	int status;
	int64_t nt;
	struct timespec time;
	int err;

	cxt = step_c_context_obj;

	if (NULL == cxt->step_c_data.get_data)
		STEP_C_LOG("step_c driver not register data path\n");


	time.tv_sec = time.tv_nsec = 0;
	time = get_monotonic_coarse();
	nt = time.tv_sec * 1000000000LL + time.tv_nsec;

	/* add wake lock to make sure data can be read before system suspend */
	err = cxt->step_c_data.get_data(&counter, &status);

	if (err) {
		STEP_C_ERR("get step_c data fails!!\n");
		goto step_c_loop;
	} else {
		{
			cxt->drv_data.counter = counter;
			cxt->drv_data.status = status;
		}
	}

	if (true == cxt->is_first_data_after_enable) {
		cxt->is_first_data_after_enable = false;
		/* filter -1 value */
		if (STEP_C_INVALID_VALUE == cxt->drv_data.counter) {
			STEP_C_LOG(" read invalid data\n");
			goto step_c_loop;

		}
	}
	/* report data to input device */
	/*STEP_C_LOG("step_c data[%d]\n", cxt->drv_data.counter);*/

	step_c_data_report(cxt->drv_data.counter, cxt->drv_data.status);

step_c_loop:
	if (true == cxt->is_polling_run) {
		{
			mod_timer(&cxt->timer, jiffies + atomic_read(&cxt->delay) / (1000 / HZ));
		}
	}
}

static void step_c_poll(unsigned long data)
{
	struct step_c_context *obj = (struct step_c_context *)data;

	if (obj != NULL)
		schedule_work(&obj->report);
}

static struct step_c_context *step_c_context_alloc_object(void)
{
	struct step_c_context *obj = kzalloc(sizeof(*obj), GFP_KERNEL);

	STEP_C_LOG("step_c_context_alloc_object++++\n");
	if (!obj) {
		STEP_C_ERR("Alloc step_c object error!\n");
		return NULL;
	}
	atomic_set(&obj->delay, 200);	/*5Hz */
	atomic_set(&obj->wake, 0);
	INIT_WORK(&obj->report, step_c_work_func);
	init_timer(&obj->timer);
	obj->timer.expires = jiffies + atomic_read(&obj->delay) / (1000 / HZ);
	obj->timer.function = step_c_poll;
	obj->timer.data = (unsigned long)obj;
	obj->is_first_data_after_enable = false;
	obj->is_polling_run = false;
	mutex_init(&obj->step_c_op_mutex);
	obj->is_step_c_batch_enable = false;	/* for batch mode init */
	obj->is_step_d_batch_enable = false;	/* for batch mode init */

	STEP_C_LOG("step_c_context_alloc_object----\n");
	return obj;
}
static int step_input_event_irqsafe(unsigned char handle,
			 const struct sensor_event *event)
{
	int err = 0;
	unsigned long flags = 0;

	spin_lock_irqsave(&step_irqsafe_lock, flags);
	err = sensor_input_event(handle, event);
	spin_unlock_irqrestore(&step_irqsafe_lock, flags);
	return err;
}
int step_notify(STEP_NOTIFY_TYPE type)
{
	int err = 0;
	struct step_c_context *cxt = NULL;
	struct sensor_event event;

	cxt = step_c_context_obj;

	if (type == TYPE_STEP_DETECTOR) {
		STEP_C_LOG("fwq TYPE_STEP_DETECTOR notify\n");
		/* cxt->step_c_data.get_data_step_d(&value); */
		/* step_c_data_report(cxt->idev,value,3); */
		event.flush_action = DATA_ACTION;
		event.handle = ID_STEP_DETECTOR;
		event.word[0] = 1;
		err = step_input_event_irqsafe(step_c_context_obj->mdev.minor, &event);

	}
	if (type == TYPE_SIGNIFICANT) {
		STEP_C_LOG("fwq TYPE_SIGNIFICANT notify\n");
		/* cxt->step_c_data.get_data_significant(&value); */
		event.flush_action = DATA_ACTION;
		event.handle = ID_SIGNIFICANT_MOTION;
		event.word[0] = 1;
		err = step_input_event_irqsafe(step_c_context_obj->mdev.minor, &event);
	}

	return err;
}

static int step_d_real_enable(int enable)
{
	int err = 0;
	struct step_c_context *cxt = NULL;

	cxt = step_c_context_obj;
	if (1 == enable) {
		err = cxt->step_c_ctl.enable_step_detect(1);
		if (err) {
			err = cxt->step_c_ctl.enable_step_detect(1);
			if (err) {
				err = cxt->step_c_ctl.enable_step_detect(1);
				if (err)
					STEP_C_ERR("step_d enable(%d) err 3 timers = %d\n", enable,
						   err);
			}
		}
		STEP_C_LOG("step_d real enable\n");
	}
	if (0 == enable) {
		err = cxt->step_c_ctl.enable_step_detect(0);
		if (err)
			STEP_C_ERR("step_d enable(%d) err = %d\n", enable, err);
		STEP_C_LOG("step_d real disable\n");

	}
	return err;
}

static int significant_real_enable(int enable)
{
	int err = 0;
	struct step_c_context *cxt = NULL;

	cxt = step_c_context_obj;
	if (1 == enable) {
		err = cxt->step_c_ctl.enable_significant(1);
		if (err) {
			err = cxt->step_c_ctl.enable_significant(1);
			if (err) {
				err = cxt->step_c_ctl.enable_significant(1);
				if (err)
					STEP_C_ERR
					    ("enable_significant enable(%d) err 3 timers = %d\n",
					     enable, err);
			}
		}
		STEP_C_LOG("enable_significant real enable\n");
	}
	if (0 == enable) {
		err = cxt->step_c_ctl.enable_significant(0);
		if (err)
			STEP_C_ERR("enable_significantenable(%d) err = %d\n", enable, err);
		STEP_C_LOG("enable_significant real disable\n");

	}
	return err;
}


static int step_c_real_enable(int enable)
{
	int err = 0;
	struct step_c_context *cxt = NULL;

	cxt = step_c_context_obj;
	if (1 == enable) {
		if (true == cxt->is_active_data || true == cxt->is_active_nodata) {
			err = cxt->step_c_ctl.enable_nodata(1);
			if (err) {
				err = cxt->step_c_ctl.enable_nodata(1);
				if (err) {
					err = cxt->step_c_ctl.enable_nodata(1);
					if (err)
						STEP_C_ERR("step_c enable(%d) err 3 timers = %d\n",
							   enable, err);
				}
			}
			STEP_C_LOG("step_c real enable\n");
		}
	}
	if (0 == enable) {
		if (false == cxt->is_active_data && false == cxt->is_active_nodata) {
			err = cxt->step_c_ctl.enable_nodata(0);
			if (err)
				STEP_C_ERR("step_c enable(%d) err = %d\n", enable, err);
			STEP_C_LOG("step_c real disable\n");
		}

	}

	return err;
}

static int step_c_enable_data(int enable)
{
	struct step_c_context *cxt = NULL;

	cxt = step_c_context_obj;
	if (NULL == cxt->step_c_ctl.open_report_data) {
		STEP_C_ERR("no step_c control path\n");
		return -1;
	}

	if (1 == enable) {
		STEP_C_LOG("STEP_C enable data\n");
		cxt->is_active_data = true;
		cxt->is_first_data_after_enable = true;
		cxt->step_c_ctl.open_report_data(1);
		if (false == cxt->is_polling_run && cxt->is_step_c_batch_enable == false) {
			if (false == cxt->step_c_ctl.is_report_input_direct) {
				mod_timer(&cxt->timer,
					  jiffies + atomic_read(&cxt->delay) / (1000 / HZ));
				cxt->is_polling_run = true;
			}
		}
	}
	if (0 == enable) {
		STEP_C_LOG("STEP_C disable\n");
		cxt->is_active_data = false;
		cxt->step_c_ctl.open_report_data(0);
		if (true == cxt->is_polling_run) {
			if (false == cxt->step_c_ctl.is_report_input_direct) {
				cxt->is_polling_run = false;
				del_timer_sync(&cxt->timer);
				cancel_work_sync(&cxt->report);
				cxt->drv_data.counter = STEP_C_INVALID_VALUE;
			}
		}

	}
	step_c_real_enable(enable);
	return 0;
}



int step_c_enable_nodata(int enable)
{
	struct step_c_context *cxt = NULL;

	cxt = step_c_context_obj;
	if (NULL == cxt->step_c_ctl.enable_nodata) {
		STEP_C_ERR("step_c_enable_nodata:step_c ctl path is NULL\n");
		return -1;
	}

	if (1 == enable)
		cxt->is_active_nodata = true;

	if (0 == enable)
		cxt->is_active_nodata = false;
	step_c_real_enable(enable);
	return 0;
}


static ssize_t step_c_show_enable_nodata(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	int len = 0;

	STEP_C_LOG(" not support now\n");
	return len;
}

static ssize_t step_c_store_enable_nodata(struct device *dev, struct device_attribute *attr,
					  const char *buf, size_t count)
{
	int err = 0;
	struct step_c_context *cxt = NULL;

	STEP_C_LOG("step_c_store_enable nodata buf=%s\n", buf);
	mutex_lock(&step_c_context_obj->step_c_op_mutex);
	cxt = step_c_context_obj;
	if (NULL == cxt->step_c_ctl.enable_nodata) {
		STEP_C_LOG("step_c_ctl enable nodata NULL\n");
		mutex_unlock(&step_c_context_obj->step_c_op_mutex);
		return count;
	}
	if (!strncmp(buf, "1", 1))
		err = step_c_enable_nodata(1);
	else if (!strncmp(buf, "0", 1))
		err = step_c_enable_nodata(0);
	else
		STEP_C_ERR(" step_c_store enable nodata cmd error !!\n");
	mutex_unlock(&step_c_context_obj->step_c_op_mutex);
	return err;
}

static ssize_t step_c_store_active(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct step_c_context *cxt = NULL;
	int res = 0;
	int handle = 0;
	int en = 0;

	STEP_C_LOG("step_c_store_active buf=%s\n", buf);
	mutex_lock(&step_c_context_obj->step_c_op_mutex);

	cxt = step_c_context_obj;
	if (NULL == cxt->step_c_ctl.open_report_data) {
		STEP_C_LOG("step_c_ctl enable NULL\n");
		mutex_unlock(&step_c_context_obj->step_c_op_mutex);
		return count;
	}
	res = sscanf(buf, "%d,%d", &handle, &en);
	if (res != 2)
		STEP_C_LOG(" step_store_active param error: res = %d\n", res);
	STEP_C_LOG(" step_store_active handle=%d ,en=%d\n", handle, en);
	switch (handle) {
	case ID_STEP_COUNTER:
		if (1 == en)
			step_c_enable_data(1);
		else if (0 == en)
			step_c_enable_data(0);
		else
			STEP_C_ERR(" step_c_store_active error !!\n");
		break;
	case ID_STEP_DETECTOR:
		if (1 == en)
			step_d_real_enable(1);
		else if (0 == en)
			step_d_real_enable(0);
		else
			STEP_C_ERR(" step_d_real_enable error !!\n");
		break;
	case ID_SIGNIFICANT_MOTION:
		if (1 == en)
			significant_real_enable(1);
		else if (0 == en)
			significant_real_enable(0);
		else
			STEP_C_ERR(" significant_real_enable error !!\n");
		break;

	}
	mutex_unlock(&step_c_context_obj->step_c_op_mutex);
	STEP_C_LOG(" step_c_store_active done\n");
	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t step_c_show_active(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct step_c_context *cxt = NULL;
	int div;

	cxt = step_c_context_obj;
	div = cxt->step_c_data.vender_div;
	STEP_C_LOG("step_c vender_div value: %d\n", div);
	return snprintf(buf, PAGE_SIZE, "%d\n", div);
}

static ssize_t step_c_store_delay(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int delay;
	int mdelay = 0;
	struct step_c_context *cxt = NULL;

	mutex_lock(&step_c_context_obj->step_c_op_mutex);
	cxt = step_c_context_obj;
	if (NULL == cxt->step_c_ctl.step_c_set_delay) {
		STEP_C_LOG("step_c_ctl step_c_set_delay NULL\n");
		mutex_unlock(&step_c_context_obj->step_c_op_mutex);
		return count;
	}

	if (0 != kstrtoint(buf, 10, &delay)) {
		STEP_C_ERR("invalid format!!\n");
		mutex_unlock(&step_c_context_obj->step_c_op_mutex);
		return count;
	}

	if (false == cxt->step_c_ctl.is_report_input_direct) {
		mdelay = (int)delay / 1000 / 1000;
		atomic_set(&step_c_context_obj->delay, mdelay);
	}
	cxt->step_c_ctl.step_c_set_delay(delay);
	STEP_C_LOG(" step_c_delay %d ns\n", delay);
	mutex_unlock(&step_c_context_obj->step_c_op_mutex);
	return count;

}

static ssize_t step_c_show_delay(struct device *dev, struct device_attribute *attr, char *buf)
{
	int len = 0;

	STEP_C_LOG(" not support now\n");
	return len;
}


static ssize_t step_c_store_batch(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct step_c_context *cxt = NULL;
	int handle = 0, flag = 0, res = 0;
	int64_t samplingPeriodNs = 0, maxBatchReportLatencyNs = 0;

	res = sscanf(buf, "%d,%d,%lld,%lld", &handle, &flag, &samplingPeriodNs, &maxBatchReportLatencyNs);
	if (res != 4)
		STEP_C_ERR("step_c_store_batch param error: err = %d\n", res);
	STEP_C_LOG("handle %d, flag:%d samplingPeriodNs:%lld, maxBatchReportLatencyNs: %lld\n",
			handle, flag, samplingPeriodNs, maxBatchReportLatencyNs);
	mutex_lock(&step_c_context_obj->step_c_op_mutex);
	cxt = step_c_context_obj;
	if (handle == ID_STEP_COUNTER) {
		if (!cxt->step_c_ctl.is_counter_support_batch)
			maxBatchReportLatencyNs = 0;
		if (NULL != cxt->step_c_ctl.step_c_batch)
			res = cxt->step_c_ctl.step_c_batch(flag, samplingPeriodNs, maxBatchReportLatencyNs);
		else
			STEP_C_ERR("SUPPORT STEP COUNTER COMMON VERSION BATCH\n");
		if (res < 0)
			STEP_C_ERR("step counter enable batch err %d\n", res);
	} else if (handle == ID_STEP_DETECTOR) {
		if (!cxt->step_c_ctl.is_detector_support_batch)
			maxBatchReportLatencyNs = 0;
		if (NULL != cxt->step_c_ctl.step_d_batch)
			res = cxt->step_c_ctl.step_d_batch(flag, samplingPeriodNs, maxBatchReportLatencyNs);
		else
			STEP_C_ERR("DON'T SUPPORT STEP DETECTOR COMMON VERSION BATCH\n");
		if (res < 0)
			STEP_C_ERR("step detector enable batch err %d\n", res);
	} else if (handle == ID_SIGNIFICANT_MOTION) {
		if (cxt->step_c_ctl.is_smd_support_batch == true)
			maxBatchReportLatencyNs = 0;
		else
			maxBatchReportLatencyNs = 0;
		if (NULL != cxt->step_c_ctl.smd_batch)
			res = cxt->step_c_ctl.smd_batch(flag, samplingPeriodNs, maxBatchReportLatencyNs);
		else
			STEP_C_ERR("STEP SMD DRIVER OLD ARCHITECTURE DON'T SUPPORT STEP SMD COMMON VERSION BATCH\n");
		if (res < 0)
			STEP_C_ERR("step smd enable batch err %d\n", res);
	}
	mutex_unlock(&step_c_context_obj->step_c_op_mutex);
	STEP_C_LOG(" step_c_store_batch done: %d\n", cxt->is_step_c_batch_enable);
	return count;
}

static ssize_t step_c_show_batch(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t step_c_store_flush(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct step_c_context *cxt = NULL;
	int handle = 0, err = 0;

	err = kstrtoint(buf, 10, &handle);
	if (err != 0)
		STEP_C_ERR("step_c_store_flush param error: err = %d\n", err);

	STEP_C_ERR("step_c_store_flush param: handle %d\n", handle);

	mutex_lock(&step_c_context_obj->step_c_op_mutex);
	cxt = step_c_context_obj;
	if (handle == ID_STEP_COUNTER) {
		if (NULL != cxt->step_c_ctl.step_c_flush)
			err = cxt->step_c_ctl.step_c_flush();
		else
			STEP_C_ERR("DON'T SUPPORT STEP COUNTER COMMON VERSION FLUSH\n");
		if (err < 0)
			STEP_C_ERR("step counter enable flush err %d\n", err);
	} else if (handle == ID_STEP_DETECTOR) {
		if (NULL != cxt->step_c_ctl.step_d_flush)
			err = cxt->step_c_ctl.step_d_flush();
		else
			STEP_C_ERR("DON'T SUPPORT STEP DETECTOR COMMON VERSION FLUSH\n");
		if (err < 0)
			STEP_C_ERR("step detector enable flush err %d\n", err);
	} else if (handle == ID_SIGNIFICANT_MOTION) {
		if (NULL != cxt->step_c_ctl.smd_flush)
			err = cxt->step_c_ctl.smd_flush();
		else
			STEP_C_ERR("DON'T SUPPORT SMD COMMON VERSION FLUSH\n");
		if (err < 0)
			STEP_C_ERR("smd enable flush err %d\n", err);
	}
	mutex_unlock(&step_c_context_obj->step_c_op_mutex);
	return count;
}

static ssize_t step_c_show_flush(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static ssize_t step_c_show_devnum(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 0);
}

static int step_counter_remove(struct platform_device *pdev)
{
	STEP_C_LOG("step_counter_remove\n");
	return 0;
}

static int step_counter_probe(struct platform_device *pdev)
{
	STEP_C_LOG("step_counter_probe\n");
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id step_counter_of_match[] = {
	{.compatible = "mediatek,step_counter",},
	{},
};
#endif

static struct platform_driver step_counter_driver = {
	.probe = step_counter_probe,
	.remove = step_counter_remove,
	.driver = {

		   .name = "step_counter",
#ifdef CONFIG_OF
		   .of_match_table = step_counter_of_match,
#endif
		   }
};

static int step_c_real_driver_init(void)
{
	int i = 0;
	int err = 0;

	STEP_C_LOG(" step_c_real_driver_init +\n");
	for (i = 0; i < MAX_CHOOSE_STEP_C_NUM; i++) {
		STEP_C_LOG(" i=%d\n", i);
		if (0 != step_counter_init_list[i]) {
			STEP_C_LOG(" step_c try to init driver %s\n",
				   step_counter_init_list[i]->name);
			err = step_counter_init_list[i]->init();
			if (0 == err) {
				STEP_C_LOG(" step_c real driver %s probe ok\n",
					   step_counter_init_list[i]->name);
				break;
			}
		}
	}

	if (i == MAX_CHOOSE_STEP_C_NUM) {
		STEP_C_LOG(" step_c_real_driver_init fail\n");
		err = -1;
	}
	return err;
}

int step_c_driver_add(struct step_c_init_info *obj)
{
	int err = 0;
	int i = 0;

	STEP_C_FUN();
	for (i = 0; i < MAX_CHOOSE_STEP_C_NUM; i++) {
		if (i == 0) {
			STEP_C_LOG("register step_counter driver for the first time\n");
			if (platform_driver_register(&step_counter_driver))
				STEP_C_ERR("failed to register gensor driver already exist\n");
		}

		if (NULL == step_counter_init_list[i]) {
			obj->platform_diver_addr = &step_counter_driver;
			step_counter_init_list[i] = obj;
			break;
		}
	}
	if (NULL == step_counter_init_list[i]) {
		STEP_C_ERR("STEP_C driver add err\n");
		err = -1;
	}

	return err;
}
static int step_open(struct inode *inode, struct file *file)
{
	nonseekable_open(inode, file);
	return 0;
}

static ssize_t step_read(struct file *file, char __user *buffer,
			  size_t count, loff_t *ppos)
{
	ssize_t read_cnt = 0;

	read_cnt = sensor_event_read(step_c_context_obj->mdev.minor, file, buffer, count, ppos);

	return read_cnt;
}

static unsigned int step_poll(struct file *file, poll_table *wait)
{
	return sensor_event_poll(step_c_context_obj->mdev.minor, file, wait);
}

static const struct file_operations step_fops = {
	.owner = THIS_MODULE,
	.open = step_open,
	.read = step_read,
	.poll = step_poll,
};

static int step_c_misc_init(struct step_c_context *cxt)
{

	int err = 0;
	/* kernel-3.10\include\linux\Miscdevice.h */
	/* use MISC_DYNAMIC_MINOR exceed 64 */
	cxt->mdev.minor = ID_STEP_COUNTER;
	cxt->mdev.name = STEP_C_MISC_DEV_NAME;
	cxt->mdev.fops = &step_fops;
	err = sensor_attr_register(&cxt->mdev);
	if (err)
		STEP_C_ERR("unable to register step_c misc device!!\n");
	return err;
}

DEVICE_ATTR(step_cenablenodata, S_IWUSR | S_IRUGO, step_c_show_enable_nodata,
	    step_c_store_enable_nodata);
DEVICE_ATTR(step_cactive, S_IWUSR | S_IRUGO, step_c_show_active, step_c_store_active);
DEVICE_ATTR(step_cdelay, S_IWUSR | S_IRUGO, step_c_show_delay, step_c_store_delay);
DEVICE_ATTR(step_cbatch, S_IWUSR | S_IRUGO, step_c_show_batch, step_c_store_batch);
DEVICE_ATTR(step_cflush, S_IWUSR | S_IRUGO, step_c_show_flush, step_c_store_flush);
DEVICE_ATTR(step_cdevnum, S_IWUSR | S_IRUGO, step_c_show_devnum, NULL);


static struct attribute *step_c_attributes[] = {
	&dev_attr_step_cenablenodata.attr,
	&dev_attr_step_cactive.attr,
	&dev_attr_step_cdelay.attr,
	&dev_attr_step_cbatch.attr,
	&dev_attr_step_cflush.attr,
	&dev_attr_step_cdevnum.attr,
	NULL
};

static struct attribute_group step_c_attribute_group = {
	.attrs = step_c_attributes
};

int step_c_register_data_path(struct step_c_data_path *data)
{
	struct step_c_context *cxt = NULL;

	cxt = step_c_context_obj;
	cxt->step_c_data.get_data = data->get_data;
	cxt->step_c_data.vender_div = data->vender_div;
	cxt->step_c_data.get_data_significant = data->get_data_significant;
	cxt->step_c_data.get_data_step_d = data->get_data_step_d;
	STEP_C_LOG("step_c register data path vender_div: %d\n", cxt->step_c_data.vender_div);
	if (NULL == cxt->step_c_data.get_data
	    || NULL == cxt->step_c_data.get_data_significant
	    || NULL == cxt->step_c_data.get_data_step_d) {
		STEP_C_LOG("step_c register data path fail\n");
		return -1;
	}
	return 0;
}

int step_c_register_control_path(struct step_c_control_path *ctl)
{
	struct step_c_context *cxt = NULL;
	int err = 0;

	cxt = step_c_context_obj;
	cxt->step_c_ctl.step_c_set_delay = ctl->step_c_set_delay;
	cxt->step_c_ctl.step_d_set_delay = ctl->step_d_set_delay;
	cxt->step_c_ctl.open_report_data = ctl->open_report_data;
	cxt->step_c_ctl.enable_nodata = ctl->enable_nodata;
	cxt->step_c_ctl.step_c_batch = ctl->step_c_batch;
	cxt->step_c_ctl.step_c_flush = ctl->step_c_flush;
	cxt->step_c_ctl.step_d_batch = ctl->step_d_batch;
	cxt->step_c_ctl.step_d_flush = ctl->step_d_flush;
	cxt->step_c_ctl.smd_batch = ctl->smd_batch;
	cxt->step_c_ctl.smd_flush = ctl->smd_flush;
	cxt->step_c_ctl.is_counter_support_batch = ctl->is_counter_support_batch;
	cxt->step_c_ctl.is_detector_support_batch = ctl->is_detector_support_batch;
	cxt->step_c_ctl.is_smd_support_batch = ctl->is_smd_support_batch;
	cxt->step_c_ctl.is_report_input_direct = ctl->is_report_input_direct;
	cxt->step_c_ctl.enable_significant = ctl->enable_significant;
	cxt->step_c_ctl.enable_step_detect = ctl->enable_step_detect;

	if (NULL == cxt->step_c_ctl.step_c_set_delay || NULL == cxt->step_c_ctl.open_report_data
	    || NULL == cxt->step_c_ctl.enable_nodata || NULL == cxt->step_c_ctl.step_d_set_delay
	    || NULL == cxt->step_c_ctl.enable_significant
	    || NULL == cxt->step_c_ctl.enable_step_detect) {
		STEP_C_LOG("step_c register control path fail\n");
		return -1;
	}

	/* add misc dev for sensor hal control cmd */
	err = step_c_misc_init(step_c_context_obj);
	if (err) {
		STEP_C_ERR("unable to register step_c misc device!!\n");
		return -2;
	}
	err = sysfs_create_group(&step_c_context_obj->mdev.this_device->kobj,
				 &step_c_attribute_group);
	if (err < 0) {
		STEP_C_ERR("unable to create step_c attribute file\n");
		return -3;
	}

	kobject_uevent(&step_c_context_obj->mdev.this_device->kobj, KOBJ_ADD);

	return 0;
}

int step_c_data_report(uint32_t new_counter, int status)
{
	int err = 0;
	struct sensor_event event;
	static uint32_t last_step_counter;

	if (last_step_counter != new_counter) {
		event.flush_action = DATA_ACTION;
		event.handle = ID_STEP_COUNTER;
		event.word[0] = new_counter;
		last_step_counter = new_counter;
		err = step_input_event_irqsafe(step_c_context_obj->mdev.minor, &event);
		if (err < 0)
			STEP_C_ERR("event buffer full, so drop this data\n");
	}
	return 0;
}

int step_c_flush_report(void)
{
	struct sensor_event event;
	int err = 0;

	event.handle = ID_STEP_COUNTER;
	event.flush_action = FLUSH_ACTION;
	err = step_input_event_irqsafe(step_c_context_obj->mdev.minor, &event);
	if (err < 0)
		STEP_C_ERR("event buffer full, so drop this data\n");
	else
		STEP_C_LOG("flush\n");
	return err;
}

int step_d_flush_report(void)
{
	struct sensor_event event;
	int err = 0;

	event.handle = ID_STEP_DETECTOR;
	event.flush_action = FLUSH_ACTION;
	err = step_input_event_irqsafe(step_c_context_obj->mdev.minor, &event);
	if (err < 0)
		STEP_C_ERR("event buffer full, so drop this data\n");
	else
		STEP_C_LOG("flush\n");
	return err;
}

int smd_flush_report(void)
{
	return 0;
}


static int step_c_probe(void)
{

	int err;

	STEP_C_LOG("+++++++++++++step_c_probe!!\n");

	step_c_context_obj = step_c_context_alloc_object();
	if (!step_c_context_obj) {
		err = -ENOMEM;
		STEP_C_ERR("unable to allocate devobj!\n");
		goto exit_alloc_data_failed;
	}

	/* init real step_c driver */
	err = step_c_real_driver_init();
	if (err) {
		STEP_C_ERR("step_c real driver init fail\n");
		goto real_driver_init_fail;
	}

	STEP_C_LOG("----step_c_probe OK !!\n");
	return 0;
real_driver_init_fail:
	kfree(step_c_context_obj);
exit_alloc_data_failed:
	STEP_C_LOG("----step_c_probe fail !!!\n");
	return err;
}



static int step_c_remove(void)
{

	int err = 0;

	STEP_C_FUN(f);
	sysfs_remove_group(&step_c_context_obj->mdev.this_device->kobj, &step_c_attribute_group);

	err = sensor_attr_deregister(&step_c_context_obj->mdev);
	if (err)
		STEP_C_ERR("misc_deregister fail: %d\n", err);
	kfree(step_c_context_obj);

	return 0;
}

static int __init step_c_init(void)
{
	STEP_C_FUN();

	if (step_c_probe()) {
		STEP_C_ERR("failed to register step_c driver\n");
		return -ENODEV;
	}

	return 0;
}

static void __exit step_c_exit(void)
{
	step_c_remove();
	platform_driver_unregister(&step_counter_driver);
}

late_initcall(step_c_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("STEP_CMETER device driver");
MODULE_AUTHOR("Mediatek");