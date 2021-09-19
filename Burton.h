#ifndef BURTON_H
#define BURTON_H

#include <stdarg.h>
#define BURTON_BASIC_PRINTF()   pr_debug("BURTON %s//%s//%d\n", __FILE__ ,__func__ ,__LINE__)

#define BURTON_BASIC_INF(fmt, ...)   pr_debug("BURTON %s//%s//%d", __FILE__ ,__func__ ,__LINE__); printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)

#endif