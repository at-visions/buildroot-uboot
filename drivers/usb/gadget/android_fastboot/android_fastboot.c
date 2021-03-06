/* This source based on android-fastboot source code.
 *
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (c) 2011 Amlogic (Shanghai), Inc.
 * Tianhui.Wang <tianhui.wang@amlogic.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */
//#include <usb/composite.c>
#include <common.h>
#include "android_fastboot.h"
#include <version_autogenerated.h>
//#define __DEBUG_FAST_BOOT__

#ifdef __DEBUG_FAST_BOOT__
#define dprintf(fmt,args...)\
		serial_printf(" %s: "fmt,\
				__FUNCTION__, ##args)
#else
#define dprintf(fmt,args...)
#endif

static struct usb_device_instance device_instance[1];
static struct usb_bus_instance bus_instance[1];
static struct usb_configuration_instance config_instance[1];
static struct usb_interface_instance interface_instance[MAX_INTERFACES];
static struct usb_alternate_instance alternate_instance[MAX_INTERFACES];
static struct usb_endpoint_instance endpoint_instance[NUM_ENDPOINTS+1];
static struct usb_endpoint_descriptor *ep_descriptor_ptrs[NUM_ENDPOINTS];
static struct usb_string_descriptor *fastboot_string_table[STR_COUNT];
extern struct usb_string_descriptor **usb_strings;
static char serialno[] = "20110615-thui";
static struct usb_configuration_descriptor	*configuration_descriptor = 0;
unsigned kernel_size = 0;
unsigned char fastboot_configured_flag = 0;
static struct udc_driver fastboot_drv = {0};
static void usb_rx_cmd_complete(struct urb *urb, unsigned actual, int status);
static void usb_rx_data_complete(struct urb *urb, unsigned actual, int status);
struct fastboot_dev fstb_dev;

static struct usb_endpoint_instance *ep1in, *ep1out;

static struct urb* rx_req;
static struct urb* tx_req;

static unsigned rx_addr;
static unsigned rx_length;

static char *cmdbuf;
static int loop_end = 0;

static void fastboot_event_handler (struct usb_device_instance *device,
				  usb_device_event_t event, int data)
{
	switch (event) {
	case DEVICE_RESET:
	case DEVICE_BUS_INACTIVE:
		fastboot_configured_flag = 0;
		break;
	case DEVICE_CONFIGURED:
		fastboot_configured_flag = 1;
		break;

	case DEVICE_ADDRESS_ASSIGNED:
		;//fastboot_init_endpoints ();

	default:
		break;
	}
}

static void usb_shutdown(void)
{
	udc_disconnect();
	loop_end = 1;
}

static void usb_poll(void)
{
	udc_irq();
}

/* itoa */
static void num_to_hex8(unsigned n, char *out)
{
    static char tohex[16] = "0123456789abcdef";
    int i;
    for(i = 7; i >= 0; i--) {
        out[i] = tohex[n & 15];
        n >>= 4;
    }
    out[8] = 0;
}

static void boot_linux(void)
{
	char uboot[32] = {0};
	char boot_addr[8] = {0};
	num_to_hex8 ((unsigned)kernel_addr + 2048,boot_addr);
	strcpy(uboot,"bootm 0x");
	strcat(uboot,boot_addr);
	serial_printf("%s\n",uboot);
	run_command(uboot, 0);
	for(;;);
}

static void usb_queue_req(struct urb* urb)
{
	if(urb == tx_req){
		if(urb->actual_length){
			if(udc_endpoint_write (urb->endpoint))
				return;
			urb->actual_length = 0;			
		}
	}else if(urb == rx_req)
		/* actually,the command length is no more than 64Bytes */		
		ep_out_start(0,ep1out->endpoint_address & USB_ENDPOINT_NUMBER_MASK);
}

static void rx_cmd(void)
{
    struct urb *req = rx_req;
    cmdbuf = req->buffer;
    //req->length = 64;
    fastboot_drv.complete = usb_rx_cmd_complete;
    usb_queue_req(req);
}

static void rx_data(void)
{
    struct urb *req = rx_req;
    req->buffer = (u8*) rx_addr;
    req->buffer_length = (rx_length > 4096) ? 4096 : rx_length;
    fastboot_drv.complete = usb_rx_data_complete;
    usb_queue_req(req);
}

static void tx_status(const char *status)
{
    struct urb *req = tx_req;
    int len = strlen(status);
    memcpy(req->buffer, status, len);
    req->actual_length = len;
    fastboot_drv.complete = 0;
    usb_queue_req(req);
}

static void usb_rx_data_complete(struct urb *urb, unsigned actual, int status)
{
    if(status != 0) return;

    if(actual > rx_length) {
        actual = rx_length;
    }

    rx_addr += actual;
    rx_length -= actual;
    
    if(rx_length > 0) {
        rx_data();
    } else {
	urb->buffer = (u8 *) urb->buffer_data;
	urb->buffer_length = sizeof (urb->buffer_data);

        tx_status("OKAY");
        rx_cmd();
    }
}

static unsigned hex2unsigned(char *x)
{
    unsigned n = 0;

    while(*x) {
        switch(*x) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            n = (n << 4) | (*x - '0');
            break;
        case 'a': case 'b': case 'c':
        case 'd': case 'e': case 'f':
            n = (n << 4) | (*x - 'a' + 10);
            break;
        case 'A': case 'B': case 'C':
        case 'D': case 'E': case 'F':
            n = (n << 4) | (*x - 'A' + 10);
            break;
        default:
            return n;
        }
        x++;
    }

    return n;
}

static void itoa(int num,char *str,int radix)
{  
	char index[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	unsigned unum;
	int i = 0,j,k;

	if(radix == 10 && num < 0){
		unum = (unsigned)-num;
		str[i++] = '-';
	}
	else unum = (unsigned)num;
 
	do {
		str[i++] = index[unum % (unsigned)radix];
		unum /= radix;
	} while(unum);

	str[i] = '\0';
	if(str[0] == '-')
		k = 1; 
	else	k = 0;

	for(j = k;j < (i-1)/2.0 + k;j++){
		num = str[j];
		str[j] = str[i-j-1+k];
		str[i-j-1+k] = num;
	}
}

ptentry *flash_find_ptn(const char *name)
{
	int n;
	int pcount = sizeof(inand_partition_info)/sizeof(ptentry);

	for(n = 0; n < pcount; n++) {
		if(!strcmp(inand_partition_info[n].name, name)) {
			return inand_partition_info + n;
		}
	}
	return 0;
}

static char signature[SIGNATURE_SIZE];

static void usb_rx_cmd_complete(struct urb *urb, unsigned actual, int status)
{
    if(status != 0) return;
    
    if(actual > 4095) actual = 4095;    
    cmdbuf[actual] = 0;
    char uboot[32] = {0};
    char addrlen[16] = {0};
    dprintf("\n> %s\n",cmdbuf);
    ptentry *ptn = NULL;
    int slen = 0;
//    dprintf("usb_rx_cmd_complete() '%s'\n", cmdbuf);  
    
    if((memcmp(cmdbuf, "reboot", 6) == 0) && (actual == 6)) {
        tx_status("OKAY");
        rx_cmd();
        udelay(100000);
        reset_cpu(0);
    }
#if 0
    if(memcmp(cmdbuf, "debug:", 6) == 0) {
        void debug(char *cmd, char *resp);
        memcpy(cmdbuf, "OKAY", 5);
        tx_status(cmdbuf);
        rx_cmd();
        mdelay(5000);
        dprintf("NOW!\n");
        debug(cmdbuf + 6, cmdbuf + 4);
        return;
    }
#endif
    if(memcmp(cmdbuf, "getvar:", 7) == 0) {
        char response[64];
        strcpy(response,"OKAY");
        
        if(!strcmp(cmdbuf + 7, "version")) {
            strcpy(response + 4, "fastboot for uboot 0.1");
        } else if(!strcmp(cmdbuf + 7, "product")) {
            strcpy(response + 4, "Amlogic.ltd");
        } else if(!strcmp(cmdbuf + 7, "serialno")) {
            strcpy(response + 4, serialno);
        } else {
            strcpy(response + 4, U_BOOT_VERSION);
        }
        tx_status(response);
        rx_cmd();
        return;
    }

    if(memcmp(cmdbuf, "download:", 9) == 0) {
        char status[16];
        rx_addr = kernel_addr;
        rx_length = hex2unsigned(cmdbuf + 9);
        if (rx_length > (64*1024*1024)) {
            tx_status("FAILdata too large");
            rx_cmd();
            return;
        }
        kernel_size = rx_length;
        dprintf("recv data addr=%x size=%x\n", rx_addr, rx_length); 
        strcpy(status,"DATA");
        num_to_hex8(rx_length, status + 4);
        tx_status(status);
        rx_data();
        return;
    }

    if(memcmp(cmdbuf, "erase:", 6) == 0){
	strcpy(uboot,"nand erase 0x");
 #if 0
	char *p = cmdbuf+6;
	char *q = strchr(p,'-');	
	memcpy(addrlen,p,q-p);
	addrlen[q-p] = ' ';
	addrlen[q-p+1] = '0';
	addrlen[q-p+2] = 'x';	
	
	strcat(uboot,addrlen);
	memcpy(addrlen,q+1,actual-6-(q-p)-1);
	addrlen[actual-6-(q-p)-1] = 0;
	strcat(uboot,addrlen);	
#else
	ptn = flash_find_ptn(cmdbuf + 6);
        if(ptn == 0) {
            tx_status("FAILpartition does not exist");
            rx_cmd();
            return;
        }
	itoa(ptn->offset, addrlen, 16);
	slen = strlen(addrlen);
	addrlen[slen] = ' ';
	addrlen[slen+1] = '0';
	addrlen[slen+2] = 'x';	
	strcat(uboot,addrlen);
	itoa(ptn->size, addrlen, 16);
	strcat(uboot,addrlen);
#endif
	serial_printf("%s\n",uboot);
	run_command(uboot, 0);
        tx_status("OKAY");
        rx_cmd();
        return;

    }

    if(memcmp(cmdbuf, "flash:", 6) == 0){
        ptentry *ptn;
        ptn = flash_find_ptn(cmdbuf + 6);
        if(kernel_size == 0) {
            tx_status("FAILno image downloaded");
            rx_cmd();
            return;
        }
        if(ptn == 0) {
            tx_status("FAILpartition does not exist");
            rx_cmd();
            return;
        }
        if(!strcmp(ptn->name,"boot") || !strcmp(ptn->name,"recovery")) {
            if(memcmp((void*) kernel_addr, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
                tx_status("FAILimage is not a boot image");
                rx_cmd();
                return;
            }
        }
	kernel_addr += 2048;

	strcpy(uboot,"nand write 0x");
	itoa(kernel_addr, addrlen, 16);
	slen = strlen(addrlen);
	addrlen[slen] = ' ';
	addrlen[slen+1] = '0';
	addrlen[slen+2] = 'x';	
	strcat(uboot,addrlen);
	itoa(ptn->offset, addrlen, 16);
	slen = strlen(addrlen);
	addrlen[slen] = ' ';
	addrlen[slen+1] = '0';
	addrlen[slen+2] = 'x';	
	addrlen[slen+3] = 0;
	strcat(uboot,addrlen);
	kernel_size = (kernel_size/(1024*1024) + 1)*1024*1024;//page aligned
	itoa(kernel_size, addrlen, 16);
	strcat(uboot,addrlen);

	serial_printf("%s\n",uboot);
	run_command(uboot, 0);

        tx_status("OKAY");
        rx_cmd();
        return;
    }

    if(memcmp(cmdbuf, "boot", 4) == 0) {
        dprintf("booting linux...\n");
        tx_status("OKAY");
        udelay(10000);
        usb_shutdown();
        boot_linux();
        return;
    }
    if(memcmp(cmdbuf, "signature", 9) == 0) {
        if (kernel_size != SIGNATURE_SIZE) {
            tx_status("FAILsignature not 256 bytes long");
            rx_cmd();
            return;
        }
        memcpy(signature, (void*)kernel_addr, SIGNATURE_SIZE);
        tx_status("OKAY");
        rx_cmd();
        return;
    }

    if(memcmp(cmdbuf, "reboot-bootloader", 17) == 0) {        
        tx_status("OKAY");
	loop_end = 1;
        return;
    }

    tx_status("FAILinvalid command");
    rx_cmd();
}

void usb_status(unsigned online, unsigned short highspeed)
{
    if(online) {
        dprintf("usb: online (%s)\n", highspeed ? "highspeed" : "fullspeed");
        rx_cmd();
    }
}

int fastboot_setup(struct urb *urb)
{
	unsigned short usb_highspeed = 0;
	struct usb_device_request request = urb->device_request;
	switch (request.bRequest){
		case USB_REQ_SET_CONFIGURATION:
			udc_ep_enable(ep1in);
			udc_ep_enable(ep1out);			
         		usb_status(request.wValue ? 1 : 0, usb_highspeed);
		default:
			break;
	}
	return 0;
}

static void fastboot_init_strings (void)
{
	struct usb_string_descriptor *string;
	static u8 wstrLang[4] = {4,USB_DT_STRING,0x9,0x4};

	fastboot_string_table[0] =
		(struct usb_string_descriptor*)wstrLang;

	string = (struct usb_string_descriptor *) fastboot_string_vid;
	fastboot_string_table[1]=string;

	string = (struct usb_string_descriptor *) fastboot_string_pid;
	fastboot_string_table[2]=string;

	string = (struct usb_string_descriptor *) fastboot_string_serial;
	fastboot_string_table[3]=string;

	/* Now, initialize the string table for ep0 handling */
	usb_strings = fastboot_string_table;
}
/* now we should setting the usb devices */
static void setup_usb_device(void)
{
	dprintf("setup fastboot to udc\n");
	int i;
	configuration_descriptor = (struct usb_configuration_descriptor*)
				&config_desc_fs;

	ep_descriptor_ptrs[0] =	&config_desc_fs.ep[0];
	ep_descriptor_ptrs[1] =	&config_desc_fs.ep[1];

	fastboot_init_strings();
	/* initialize device instance */
	memset (device_instance, 0, sizeof (struct usb_device_instance));
	device_instance->device_state = STATE_INIT;
	device_instance->device_descriptor = &device_descriptor;
	//device_instance->event = fastboot_event_handler;
	//device_instance->cdc_recv_setup = fastboot_setup;
	device_instance->bus = bus_instance;
	device_instance->configurations = NUM_CONFIGS;
	device_instance->configuration_instance_array = config_instance;

	/* initialize bus instance */
	memset (bus_instance, 0, sizeof (struct usb_bus_instance));
	bus_instance->device = device_instance;
	bus_instance->endpoint_array = endpoint_instance;
	bus_instance->max_endpoints = NUM_ENDPOINTS + 1;
	bus_instance->maxpacketsize = 64;
	bus_instance->serial_number_str = serialno;

	/* configuration instance */
	memset (config_instance, 0,sizeof (struct usb_configuration_instance));
	config_instance->interfaces = NUM_INTERFACES;
	config_instance->configuration_descriptor = configuration_descriptor;
	config_instance->interface_instance_array = interface_instance;

	/* interface instance */
	memset (interface_instance, 0,
		sizeof (struct usb_interface_instance));
	interface_instance->alternates = 1;
	interface_instance->alternates_instance_array = alternate_instance;
	/* alternates instance */
	memset (alternate_instance, 0,
		sizeof (struct usb_alternate_instance));
	alternate_instance->interface_descriptor = &(config_desc_fs.interface_desc);
	alternate_instance->endpoints = NUM_ENDPOINTS;
	alternate_instance->endpoints_descriptor_array = ep_descriptor_ptrs;

	memset (&endpoint_instance[0], 0, sizeof (struct usb_endpoint_instance));
	endpoint_instance[0].endpoint_address = 0;
	endpoint_instance[0].rcv_packetSize = EP0_MAX_PACKET_SIZE;
	endpoint_instance[0].rcv_attributes = USB_ENDPOINT_XFER_CONTROL;
	endpoint_instance[0].tx_packetSize = EP0_MAX_PACKET_SIZE;
	endpoint_instance[0].tx_attributes = USB_ENDPOINT_XFER_CONTROL;
	udc_setup_ep (device_instance, 0, &endpoint_instance[0]);

	udc_startup_events(device_instance);
	udc_connect();

	for (i = 1; i <= NUM_ENDPOINTS; i++) {
		memset (&endpoint_instance[i], 0, sizeof (struct usb_endpoint_instance));

		endpoint_instance[i].endpoint_address =	config_desc_fs.ep[i - 1].bEndpointAddress;
		endpoint_instance[i].rcv_attributes = config_desc_fs.ep[i - 1].bmAttributes;
		endpoint_instance[i].rcv_packetSize = le16_to_cpu(config_desc_fs.ep[i - 1].wMaxPacketSize);
		endpoint_instance[i].tx_attributes = config_desc_fs.ep[i - 1].bmAttributes;
		endpoint_instance[i].tx_packetSize = le16_to_cpu(config_desc_fs.ep[i - 1].wMaxPacketSize);
		endpoint_instance[i].tx_attributes = config_desc_fs.ep[i - 1].bmAttributes;

		urb_link_init (&endpoint_instance[i].rcv);
		urb_link_init (&endpoint_instance[i].rdy);
		urb_link_init (&endpoint_instance[i].tx);
		urb_link_init (&endpoint_instance[i].done);

		if (endpoint_instance[i].endpoint_address & USB_DIR_IN){
			endpoint_instance[i].tx_urb =
				usbd_alloc_urb (device_instance,
						&endpoint_instance[i]);
			ep1in = &endpoint_instance[i];
			tx_req = endpoint_instance[i].tx_urb;//IN
		}
		else{
			endpoint_instance[i].rcv_urb =
				usbd_alloc_urb (device_instance,
						&endpoint_instance[i]);
			ep1out = &endpoint_instance[i];
			rx_req = endpoint_instance[i].rcv_urb;//OUT
		}
		udc_setup_ep (device_instance, i, &endpoint_instance[i]);
	}
}

int do_fastboot(cmd_tbl_t *cmdtp, int flag, int argc, char *argv[])
{	
	dprintf("init the fastboot\n");
	/* init the dev and drv */
	fstb_dev.device = device_instance;
	fstb_dev.udc_driver = &fastboot_drv;
	fstb_dev.dev_state = FSTB_INIT_PHASE;

	fastboot_drv.driver_name = "fastboot";
	fastboot_drv.private_data = &fstb_dev;
	fastboot_drv.usb_dev = device_instance;
	fastboot_drv.setup = fastboot_setup;
	/* register it to udc drv */
	udc_register_driver(&fastboot_drv);
	udc_init();
	setup_usb_device();
	loop_end = 0;

	while(!loop_end)
		usb_poll();

	return 0;
	
}

U_BOOT_CMD(
	fastboot,CONFIG_SYS_MAXARGS, 1, do_fastboot,
	"please run cmd: fastboot",
	" - This cmd set the uboot goto fastboot mode.\n"
	" if you want to come back,run 'reboot-bootloader' cmd in fastboot console,\n"
	" then you should enter ctrl+c to cancel pre_cmd sending to target."
);
