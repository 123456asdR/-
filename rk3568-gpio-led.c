#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>

/* LED设备私有数据 */
struct rk3568_led {
	struct device *dev;          /* 绑定的设备 */
	struct gpio_desc *gpiod;     /* LED对应的GPIO描述符 */
	struct miscdevice miscdev;   /* 用户态节点 /dev/rk_led_xxx */
	struct mutex lock;           /* 保护state并发访问 */
	bool state;                  /* 当前LED状态: 0灭 1亮 */
};

/*
 * @description		: 通过file对象获取LED私有数据
 * @param - filp	: 文件对象
 * @return			: rk3568_led结构体指针
 */
static struct rk3568_led *rkled_from_file(struct file *filp)
{
	/* misc 设备打开后，private_data 中保存的是 miscdevice 指针 */
	struct miscdevice *mdev = filp->private_data;

	return container_of(mdev, struct rk3568_led, miscdev);
}

/*
 * @description		: 用户态读接口，返回LED当前状态
 * @param - filp	: 文件对象
 * @param - buf		: 用户态缓冲区
 * @param - count	: 读取长度
 * @param - ppos	: 文件偏移
 * @return			: 成功返回读取字节数; 其他负值，失败
 */
static ssize_t rkled_read(struct file *filp, char __user *buf,
			  size_t count, loff_t *ppos)
{
	/* 根据文件对象获取LED设备私有数据 */
	struct rk3568_led *led = rkled_from_file(filp);
	/* 保存返回给用户的数据："0\n" 或 "1\n" */
	char out[2];

	/* 只支持一次性读取，若偏移不为0则返回文件结束 */
	if (*ppos != 0)
		return 0;
	/* 用户缓冲区，最多容纳2字节数据 */
	if (count < sizeof(out))
		return -EINVAL;

	/* 加锁读取LED当前状态，防止并发访问 */
	mutex_lock(&led->lock);
	out[0] = led->state ? '1' : '0';
	out[1] = '\n';
	mutex_unlock(&led->lock);

	/* 将状态数据拷贝到用户空间 */
	if (copy_to_user(buf, out, sizeof(out)))
		return -EFAULT;

	/* 更新文件偏移，表示本次数据已读完 */
	*ppos = sizeof(out);

	return sizeof(out);
}

/*
 * @description		: 设备打开接口
 * @param - inode	: inode对象
 * @param - filp	: 文件对象
 * @return			: 0，成功; 其他负值，失败
 */
static int rkled_open(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * @description		: 用户态写接口，控制LED亮灭
 * @param - filp	: 文件对象
 * @param - buf		: 用户态传入缓冲区
 * @param - count	: 用户态写入长度
 * @param - ppos	: 文件偏移（本驱动未使用）
 * 命令说明			: '0'灭, '1'亮, '2'反转
 * @return			: 成功返回写入长度; 其他负值，失败
 */
static ssize_t rkled_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *ppos)
{
    /* 通过file对象找到驱动私有数据 */
	struct rk3568_led *led = rkled_from_file(filp);
	char kbuf[16];
	char cmd;
	int ret = 0;

    /* 空写入直接返回 */
	if (count == 0)
		return 0;

    /* 防止用户态写入过长导致越界，预留结尾'\0' */    
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;

    /* 从用户态拷贝数据到内核态 */
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

    /* 手动补齐字符串结束符，便于调试 */
	kbuf[count] = '\0';

	cmd = kbuf[0];

    /* 加锁，保护led->state并发访问 */
	mutex_lock(&led->lock);

	switch (cmd) {
	case '0':
		led->state = false;
		gpiod_set_value_cansleep(led->gpiod, 0);
		break;
	case '1':
		led->state = true;
		gpiod_set_value_cansleep(led->gpiod, 1);
		break;
	case '2':
		led->state = !led->state;
		gpiod_set_value_cansleep(led->gpiod, led->state);
		break;
	default:
		ret = -EINVAL;
		break;
	}
    /* 解锁 */
	mutex_unlock(&led->lock);

	if (ret)
		return ret;

	return count;
}

/* 字符设备操作集 */
static const struct file_operations rkled_fops = {
	.owner = THIS_MODULE,
    .open = rkled_open,
	.read = rkled_read,
	.write = rkled_write,
	.llseek = no_llseek,
};

/*
 * @description		: platform驱动probe函数，驱动与设备树匹配后执行
 * @param - pdev	: platform设备
 * @return			: 0，成功; 其他负值，失败
 */
static int rkled_probe(struct platform_device *pdev)
{
	struct rk3568_led *led;
	const char *name;
	int ret;

	pr_info("rkled_probe enter\n");
    /* 分配并清零LED设备私有数据 */
	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

    /* 保存设备对象并初始化互斥锁 */
	led->dev = &pdev->dev;
	mutex_init(&led->lock);

	/* 获取设备树中的 led-gpios，并初始化为输出低电平 */
	led->gpiod = devm_gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(led->gpiod))
		return dev_err_probe(&pdev->dev, PTR_ERR(led->gpiod),
				     "failed to get led-gpios\n");

    /* 初始化LED软件状态为灭 */
	led->state = false;

	/* 生成字符设备节点名，如 /dev/my_led_xxx */
	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "my_led_%s",
			      dev_name(&pdev->dev));
	if (!name)
		return -ENOMEM;

    /* 初始化 misc 设备 */
	led->miscdev.minor = MISC_DYNAMIC_MINOR;
	led->miscdev.name = name;
	led->miscdev.fops = &rkled_fops;
	led->miscdev.parent = &pdev->dev;

    /* 注册 misc 字符设备 */
	ret = misc_register(&led->miscdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "misc_register failed\n");

    /* 保存驱动私有数据，供 remove() 使用 */
	platform_set_drvdata(pdev, led);

	dev_info(&pdev->dev, "rk3568 led driver ready: /dev/%s\n", name);
	return 0;
}

/*
 * @description		: platform驱动remove函数，驱动卸载时清理misc设备
 * @param - pdev	: platform设备
 * @return			: 0，成功; 其他负值，失败
 */
static int rkled_remove(struct platform_device *pdev)
{
	struct rk3568_led *led = platform_get_drvdata(pdev);

	misc_deregister(&led->miscdev);
	return 0;
}

/* 设备树匹配表: 对应 compatible = "my,rk3568-gpio-led" */
static const struct of_device_id rkled_of_match[] = {
	{ .compatible = "my,rk3568-gpio-led" },
	{ }
};

MODULE_DEVICE_TABLE(of, rkled_of_match);

static struct platform_driver rkled_driver = {
	.probe = rkled_probe,
	.remove = rkled_remove,
	.driver = {
		.name = "rk3568-gpio-led",
		.of_match_table = rkled_of_match,
	},
};

module_platform_driver(rkled_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("RK3568 GPIO LED platform driver (DT + miscdev)");

