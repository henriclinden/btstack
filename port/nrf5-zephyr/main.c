/*
 * Copyright (c) 2016 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr.h>
#include <arch/cpu.h>
#include <misc/byteorder.h>
#include <logging/sys_log.h>
#include <misc/util.h>

#include <device.h>
#include <init.h>
#include <uart.h>

#include <net/buf.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/buf.h>
#include <bluetooth/hci_raw.h>

#include "ll.h"

// Nordic NDK
#include "nrf.h"

#include "common/log.h"

// BTstack
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_transport.h"

// static struct device *hci_uart_dev;
// static BT_STACK_NOINIT(tx_thread_stack, CONFIG_BT_HCI_TX_STACK_SIZE);
// static struct k_thread tx_thread_data;

/* HCI command buffers */
#define CMD_BUF_SIZE BT_BUF_RX_SIZE
NET_BUF_POOL_DEFINE(cmd_tx_pool, CONFIG_BT_HCI_CMD_COUNT, CMD_BUF_SIZE, BT_BUF_USER_DATA_MIN, NULL);

#if defined(CONFIG_BT_CTLR)
#define BT_L2CAP_MTU (CONFIG_BT_CTLR_TX_BUFFER_SIZE - BT_L2CAP_HDR_SIZE)
#else
#define BT_L2CAP_MTU 65 /* 64-byte public key + opcode */
#endif /* CONFIG_BT_CTLR */

/** Data size needed for ACL buffers */
#define BT_BUF_ACL_SIZE BT_L2CAP_BUF_SIZE(BT_L2CAP_MTU)

#if defined(CONFIG_BT_CTLR_TX_BUFFERS)
#define TX_BUF_COUNT CONFIG_BT_CTLR_TX_BUFFERS
#else
#define TX_BUF_COUNT 6
#endif

NET_BUF_POOL_DEFINE(acl_tx_pool, TX_BUF_COUNT, BT_BUF_ACL_SIZE, BT_BUF_USER_DATA_MIN, NULL);

static K_FIFO_DEFINE(tx_queue);
static K_FIFO_DEFINE(rx_queue);

//
// hci_transport_zephyr.c
//

static void (*transport_packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size);

/**
 * init transport
 * @param transport_config
 */
static void transport_init(const void *transport_config){
	/* startup Controller */
	bt_enable_raw(&rx_queue);
}

/**
 * open transport connection
 */
static int transport_open(void){
    return 0;
}

/**
 * close transport connection
 */
static int transport_close(void){
    return 0;
}

/**
 * register packet handler for HCI packets: ACL, SCO, and Events
 */
static void transport_register_packet_handler(void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)){
    transport_packet_handler = handler;
}

static void send_hardware_error(uint8_t error_code){
    // hci_outgoing_event[0] = HCI_EVENT_HARDWARE_ERROR;
    // hci_outgoing_event[1] = 1;
    // hci_outgoing_event[2] = error_code;
    // hci_outgoing_event_ready = 1;
}

static int transport_send_packet(uint8_t packet_type, uint8_t *packet, int size){
	struct net_buf *buf;
    switch (packet_type){
        case HCI_COMMAND_DATA_PACKET:
			buf = net_buf_alloc(&cmd_tx_pool, K_NO_WAIT);
			if (buf) {
				bt_buf_set_type(buf, BT_BUF_CMD);
				memcpy(net_buf_add(buf, size), packet, size);
				bt_send(buf);
			} else {
				log_error("No available command buffers!\n");
			}
            break;
        case HCI_ACL_DATA_PACKET:
			buf = net_buf_alloc(&acl_tx_pool, K_NO_WAIT);
			if (buf) {
				bt_buf_set_type(buf, BT_BUF_ACL_OUT);
				memcpy(net_buf_add(buf, size), packet, size);
				bt_send(buf);
			} else {
				log_error("No available ACL buffers!\n");
			}
            break;
        default:
            send_hardware_error(0x01);  // invalid HCI packet
            break;
    }

    return 0;   
}

static const hci_transport_t transport = {
    /* const char * name; */                                        "nRF5-Zephyr",
    /* void   (*init) (const void *transport_config); */            &transport_init,
    /* int    (*open)(void); */                                     &transport_open,
    /* int    (*close)(void); */                                    &transport_close,
    /* void   (*register_packet_handler)(void (*handler)(...); */   &transport_register_packet_handler,
    /* int    (*can_send_packet_now)(uint8_t packet_type); */       NULL,
    /* int    (*send_packet)(...); */                               &transport_send_packet,
    /* int    (*set_baudrate)(uint32_t baudrate); */                NULL,
    /* void   (*reset_link)(void); */                               NULL,
};

static const hci_transport_t * transport_get_instance(void){
	return &transport;
}

static void transport_deliver_controller_packet(struct net_buf * buf){
		uint16_t    size = buf->len;
		uint8_t * packet = buf->data;
		switch (bt_buf_get_type(buf)) {
			case BT_BUF_ACL_IN:
				transport_packet_handler(HCI_ACL_DATA_PACKET, packet, size);
				break;
			case BT_BUF_EVT:
				transport_packet_handler(HCI_EVENT_PACKET, packet, size);
				break;
			default:
				log_error("Unknown type %u\n", bt_buf_get_type(buf));
				net_buf_unref(buf);
				break;
		}
		net_buf_unref(buf);
}


// btstack_run_loop_zephry.c

// the run loop
static btstack_linked_list_t timers;

// TODO: handle 32 bit ms time overrun
static uint32_t btstack_run_loop_zephyr_get_time_ms(void){
	return  k_uptime_get_32();
}

static void btstack_run_loop_zephyr_set_timer(btstack_timer_source_t *ts, uint32_t timeout_in_ms){
    ts->timeout = k_uptime_get_32() + 1 + timeout_in_ms; 
}

/**
 * Add timer to run_loop (keep list sorted)
 */
static void btstack_run_loop_zephyr_add_timer(btstack_timer_source_t *ts){
    btstack_linked_item_t *it;
    for (it = (btstack_linked_item_t *) &timers; it->next ; it = it->next){
        // don't add timer that's already in there
        if ((btstack_timer_source_t *) it->next == ts){
            log_error( "btstack_run_loop_timer_add error: timer to add already in list!");
            return;
        }
        if (ts->timeout < ((btstack_timer_source_t *) it->next)->timeout) {
            break;
        }
    }
    ts->item.next = it->next;
    it->next = (btstack_linked_item_t *) ts;
}

/**
 * Remove timer from run loop
 */
static int btstack_run_loop_zephyr_remove_timer(btstack_timer_source_t *ts){
    return btstack_linked_list_remove(&timers, (btstack_linked_item_t *) ts);
}

static void btstack_run_loop_zephyr_dump_timer(void){
#ifdef ENABLE_LOG_INFO 
    btstack_linked_item_t *it;
    int i = 0;
    for (it = (btstack_linked_item_t *) timers; it ; it = it->next){
        btstack_timer_source_t *ts = (btstack_timer_source_t*) it;
        log_info("timer %u, timeout %u\n", i, (unsigned int) ts->timeout);
    }
#endif
}

/**
 * Execute run_loop
 */
static void btstack_run_loop_zephyr_execute(void) {
    while (1) {
        // get next timeout
        int32_t timeout_ticks = K_FOREVER;
        if (timers) {
            btstack_timer_source_t * ts = (btstack_timer_source_t *) timers;
            uint32_t now = k_uptime_get_32();
            if (ts->timeout < now){
                // remove timer before processing it to allow handler to re-register with run loop
                btstack_run_loop_remove_timer(ts);
                // printf("RL: timer %p\n", ts->process);
                ts->process(ts);
                continue;
            }
            timeout_ticks = ts->timeout - now;
        }
                
 	   	// process RX fifo only
    	struct net_buf *buf = net_buf_get(&rx_queue, timeout_ticks);
		if (buf){
			transport_deliver_controller_packet(buf);
		}
	}
}

static void btstack_run_loop_zephyr_btstack_run_loop_init(void){
    timers = NULL;
}

static const btstack_run_loop_t btstack_run_loop_wiced = {
    &btstack_run_loop_zephyr_btstack_run_loop_init,
    NULL,
    NULL,
    NULL,
    NULL,
    &btstack_run_loop_zephyr_set_timer,
    &btstack_run_loop_zephyr_add_timer,
    &btstack_run_loop_zephyr_remove_timer,
    &btstack_run_loop_zephyr_execute,
    &btstack_run_loop_zephyr_dump_timer,
    &btstack_run_loop_zephyr_get_time_ms,
};
/**
 * @brief Provide btstack_run_loop_posix instance for use with btstack_run_loop_init
 */
const btstack_run_loop_t * btstack_run_loop_zephyr_get_instance(void){
    return &btstack_run_loop_wiced;
}

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != BTSTACK_EVENT_STATE) return;
    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
    printf("BTstack up and running.\n");
}

int btstack_main(void);

#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER)
void bt_ctlr_assert_handle(char *file, u32_t line)
{
	printf("CONFIG_BT_CTLR_ASSERT_HANDLER: file %s, line %u\n", file, line);
	while (1) {
	}
}
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER */


void main(void)
{
	// configure console UART by replacing CONFIG_UART_NRF5_BAUD_RATE with 115200 in uart_console.c

	printf("BTstack booting up..\n");

	// start with BTstack init - especially configure HCI Transport
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_zephyr_get_instance());

    // enable full log output while porting
    hci_dump_open(NULL, HCI_DUMP_STDOUT);

    // init HCI
    hci_init(transport_get_instance(), NULL);

#if 1
    // nRF5 chipsets don't have an official public address
    // Instead, they use a Static Random Address set in the factory
    bd_addr_t addr;
#if 0
    // set random static address
    big_endian_store_16(addr, 0, NRF_FICR->DEVICEADDR[1] | 0xc000);
    big_endian_store_32(addr, 2, NRF_FICR->DEVICEADDR[0]);
    gap_random_address_set(addr);
    printf("Random Static Address: %s\n", bd_addr_to_str(addr));
#else
    // make Random Static Address available via HCI Read BD ADDR as fake public address
    little_endian_store_32(addr, 0, NRF_FICR->DEVICEADDR[0]);
    little_endian_store_16(addr, 4, NRF_FICR->DEVICEADDR[1] | 0xc000);
    ll_addr_set(0, addr);
#endif
#endif

    // inform about BTstack state
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    
    // hand over to btstack embedded code 
    btstack_main();

    // go
    btstack_run_loop_execute();

    while (1){};
}
