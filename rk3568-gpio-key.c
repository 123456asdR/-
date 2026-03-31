#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>

/*
 * @description		: 按键事件数据
 */
struct rkkey_event {
	int value;      /* 1:按下, 0:释放 */
	u64 ts_ns;      /* 事件时间戳 */
};

/*
 * @description		: 按键设备私有数据
 */
struct rk3568_key {
	struct device *dev;					/* 指向设备对象 */
	struct gpio_desc *gpiod_key;		/* GPIO 描述符 */
	int irq;							/* GPIO 对应的中断号 */
	u32 debounce_ms;					/* 防抖时间，单位 ms */

	struct delayed_work debounce_work;	/* 延时工作，用于软件防抖 */
	wait_queue_head_t wq;				/* 等待队列，给 read 阻塞等待事件 */
	struct mutex lock;					/* 互斥锁 */

	bool event_pending;					/* 是否存在未读取事件 */
	struct rkkey_event event;			/* 最近一次按键事件 */
	int last_stable; 					/* 最近一次稳定状态 */

	struct miscdevice miscdev;
};

/*
 * @description		: 从file对象获取私有数据
 * @param - filp	: 文件对象
 * @return			: rk3568_key结构体指针
 */
static struct rk3568_key *rkkey_from_file(struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;
	return container_of(mdev, struct rk3568_key, miscdev);
}

/*
 * @description		: 防抖工作函数，延时后读取GPIO确认稳定状态
 * @param - work	: work结构体
 * @return			: 无
 */
static void rkkey_debounce_work(struct work_struct *work)
{
	/* 通过 work 反推出当前按键设备的私有数据 */
	struct rk3568_key *key = container_of(to_delayed_work(work), struct rk3568_key, debounce_work);
	int raw, pressed;

	/* 读取当前 GPIO 电平 */
	raw = gpiod_get_value_cansleep(key->gpiod_key);
	if (raw < 0)
		return;

	/* key-gpios 配置为 GPIO_ACTIVE_LOW：raw=0 表示按下 */
	pressed = (raw == 0) ? 1 : 0;

	/* 加锁 */
	mutex_lock(&key->lock);
	/* 只有状态发生变化时才上报新事件 */
	if (pressed != key->last_stable) {
		key->last_stable = pressed;			/* 更新当前稳定状态 */
		key->event.value = pressed;			/* 记录本次按键值 */
		key->event.ts_ns = ktime_get_ns();	/* 记录事件时间戳 */
		key->event_pending = true;			/* 标记有新事件可读 */
		wake_up_interruptible(&key->wq);	/* 唤醒阻塞等待按键事件的读进程 */
	}	
	/* 解锁 */
	mutex_unlock(&key->lock);
}

/*
 * @description		: 按键中断处理函数，只触发防抖延时处理
 * @param - irq		: 中断号
 * @param - dev_id	: 私有数据指针
 * @return			: IRQ_HANDLED
 */
static irqreturn_t rkkey_irq_handler(int irq, void *dev_id)
{
	struct rk3568_key *key = dev_id;

	mod_delayed_work(system_wq, &key->debounce_work,
			 msecs_to_jiffies(key->debounce_ms));
	return IRQ_HANDLED;
}

/*
 * @description		: open接口
 * @param - inode	: inode对象
 * @param - filp	: 文件对象
 * @return			: 0，成功; 其他负值，失败
 */
static int rkkey_open(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * @description		: read接口，阻塞等待按键事件
 * @param - filp	: 文件对象
 * @param - buf		: 用户态缓冲区
 * @param - count	: 读取长度
 * @param - ppos	: 文件偏移（未使用）
 * @return			: 成功返回拷贝字节数; 其他负值，失败
 */
static ssize_t rkkey_read(struct file *filp, char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct rk3568_key *key = rkkey_from_file(filp);
	struct rkkey_event ev;
	int ret;

	/* 用户缓冲区长度不足，无法存放一个完整事件 */
	if (count < sizeof(ev))
		return -EINVAL;

	/* 若当前没有按键事件，则阻塞等待，直到事件到来或被信号打断 */
	ret = wait_event_interruptible(key->wq, key->event_pending);
	if (ret)
		return ret;

	/* 加锁读取事件数据，并清除事件待处理标志 */
	mutex_lock(&key->lock);
	ev = key->event;
	key->event_pending = false;
	mutex_unlock(&key->lock);

	/* 将事件数据拷贝到用户空间 */
	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;

	/* 返回实际读取的字节数 */
	return sizeof(ev);
}

/*
 * @description		: write接口
 * @param - filp	: 文件对象
 * @param - buf		: 用户态缓冲区
 * @param - count	: 写入长度
 * @param - ppos	: 文件偏移（未使用）
 * @return			: 0，成功; 其他负值，失败
 */
static ssize_t rkkey_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	return count;
}

/*
 * @description		: file_operations
 */
static const struct file_operations rkkey_fops = {
	.owner = THIS_MODULE,
	.open = rkkey_open,
	.read = rkkey_read,
	.write = rkkey_write,
	.llseek = no_llseek,
};

/*
 * @description		: probe函数，完成GPIO/IRQ/waitqueue/字符设备初始化
 * @param - pdev	: platform设备
 * @return			: 0，成功; 其他负值，失败
 */
static int rkkey_probe(struct platform_device *pdev)
{
	struct rk3568_key *key;
	const char *name;
	int ret, raw;

	/* 为按键设备私有数据分配内存，并自动清零 */
	key = devm_kzalloc(&pdev->dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	/* 保存device对象，并初始化锁、等待队列和延时工作 */
	key->dev = &pdev->dev;
	mutex_init(&key->lock);
	init_waitqueue_head(&key->wq);
	INIT_DELAYED_WORK(&key->debounce_work, rkkey_debounce_work);

	/* 读取设备树 key-gpios , 并配置为输入模式 */
	key->gpiod_key = devm_gpiod_get(&pdev->dev, "key", GPIOD_IN);
	if (IS_ERR(key->gpiod_key))
		return dev_err_probe(&pdev->dev, PTR_ERR(key->gpiod_key),
				     "failed to get key-gpios\n");

	/* 读取防抖时间，默认20ms */
	ret = of_property_read_u32(pdev->dev.of_node, "debounce-ms", &key->debounce_ms);
	if (ret)
		key->debounce_ms = 20;

	/* 将GPIO转换为中断号 */
	key->irq = gpiod_to_irq(key->gpiod_key);
	if (key->irq < 0)
		return dev_err_probe(&pdev->dev, key->irq, "gpiod_to_irq failed\n");

	/* 注册中断处理函数，按键按下和释放都可触发中断 */
	ret = devm_request_irq(&pdev->dev, key->irq, rkkey_irq_handler,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       dev_name(&pdev->dev), key);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "request_irq failed\n");

	/* 读取当前GPIO电平,初始化稳定状态 */
	raw = gpiod_get_value_cansleep(key->gpiod_key);
	if (raw < 0)
		raw = 1;
	key->last_stable = (raw == 0) ? 1 : 0;
	key->event_pending = false;

	/* 生成字符设备节点名称，如 /dev/my_key_xxx */
	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "my_key_%s", dev_name(&pdev->dev));
	if (!name)
		return -ENOMEM;

	/* 注册misc字符设备 */
	key->miscdev.minor = MISC_DYNAMIC_MINOR;
	key->miscdev.name = name;
	key->miscdev.fops = &rkkey_fops;
	key->miscdev.parent = &pdev->dev;

	ret = misc_register(&key->miscdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "misc_register failed\n");

	/* 保存驱动私有数据,remove可以直接使用pdev */
	platform_set_drvdata(pdev, key);

	/* 打印驱动初始化成功信息 */
	dev_info(&pdev->dev, "key driver ready: /dev/%s, irq=%d, debounce=%ums\n",
		 name, key->irq, key->debounce_ms);
	return 0;
}

/*
 * @description		: remove函数，清理资源
 * @param - pdev	: platform设备
 * @return			: 0，成功; 其他负值，失败
 */
static int rkkey_remove(struct platform_device *pdev)
{
	/* 获取probe中保存的按键设备私有数据 */
	struct rk3568_key *key = platform_get_drvdata(pdev);

	/* 取消延时防抖工作，若正在执行则等待其完成 */
	cancel_delayed_work_sync(&key->debounce_work);
	/* 注销misc字符设备，删除对应的/dev节点 */
	misc_deregister(&key->miscdev);
	return 0;
}

/*
 * @description		: 设备树匹配表
 */
static const struct of_device_id rkkey_of_match[] = {
	{ .compatible = "my,rk3568-gpio-key" },
	{ }
};
MODULE_DEVICE_TABLE(of, rkkey_of_match);

/*
 * @description		: platform驱动描述结构体
 */
static struct platform_driver rkkey_driver = {
	.probe = rkkey_probe,
	.remove = rkkey_remove,
	.driver = {
		.name = "rk3568-gpio-key",
		.of_match_table = rkkey_of_match,
	},
};

module_platform_driver(rkkey_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("RK3568 GPIO key driver (IRQ + waitqueue + debounce)");