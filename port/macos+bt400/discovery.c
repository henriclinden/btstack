/*
 * Discover BLE Advertisements
 */

#define __BTSTACK_FILE__ "discovery.c"


#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"


static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            // BTstack activated, get started
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                printf("Start scaning!\n");
                gap_set_scan_parameters(0, 0x0030, 0x0030);
                gap_start_scan();
            }
            break;
            
        case GAP_EVENT_ADVERTISING_REPORT: {
            bd_addr_t address;
            gap_event_advertising_report_get_address(packet, address);
            uint8_t event_type = gap_event_advertising_report_get_advertising_event_type(packet);
            uint8_t address_type = gap_event_advertising_report_get_address_type(packet);
            int8_t rssi = gap_event_advertising_report_get_rssi(packet);
            uint8_t length = gap_event_advertising_report_get_data_length(packet);
            const uint8_t* data = gap_event_advertising_report_get_data(packet);

            printf("Advertisement event: evt-type %u, addr-type %u, addr %s, rssi %d, data[%u] ", event_type,
                   address_type, bd_addr_to_str(address), rssi, length);
            printf_hexdump(data, length);
            break;
        }
        default:
            break;
    }
}

int btstack_main(void);
int btstack_main(void)
{
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // Active scanning, 100% (scan interval = scan window)
    gap_set_scan_parameters(1, 48, 48);

    // Power on = start
    hci_power_control(HCI_POWER_ON);

    return 0;
}
