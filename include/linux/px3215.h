#ifndef _PX3215_h_
#define	_PX3215_h_


#define	PX3215C_DRV_NAME	"dyna"

struct px3215_platform_data {
	unsigned int irq_gpio;
	int irq;
};

#endif
