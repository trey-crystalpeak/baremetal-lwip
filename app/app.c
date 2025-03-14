#include <stdio.h>
#include <stdint.h>
#include "lwip/netif.h"
#include "lwip/init.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/autoip.h"
#include "lwip/timeouts.h"
#include "eth_driver.h"

//versatilepb maps LAN91C111 registers here
void * const eth0_addr = (void *) 0x10010000;

// VersatilePB timer registers (SP804 Timer)
#define TIMER0_BASE       0x101E2000
#define TIMER_LOAD        0x00    // Load register
#define TIMER_VALUE       0x04    // Current value register
#define TIMER_CONTROL     0x08    // Control register
#define TIMER_INTCLR      0x0C    // Interrupt clear register

// Timer control register bits
#define TIMER_CTRL_ENABLE (1 << 7)
#define TIMER_CTRL_PERIODIC (1 << 6)
#define TIMER_CTRL_32BIT  (1 << 1)

// Timer frequency (1MHz on VersatilePB)
#define TIMER_FREQ_HZ     1000000

s_lan91c111_state sls = {.phy_address = 0,
                         .ever_sent_packet = 0, 
                         .tx_packet = 0, 
                         .irq_onoff = 0};

struct netif netif;

//feed frames from driver to LwIP
int process_frames(r16 * frame, int frame_len) {
  struct pbuf* p = pbuf_alloc(PBUF_RAW, frame_len, PBUF_POOL);
  if(p != NULL) {
    pbuf_take(p, frame, frame_len);
  }
  if(netif.input(p, &netif) != ERR_OK) {
    pbuf_free(p);
  }
}

//transmit frames from LwIP using driver
static err_t 
netif_output(struct netif *netif, struct pbuf *p)
{
  unsigned char mac_send_buffer[p->tot_len];
  pbuf_copy_partial(p, (void*)mac_send_buffer, p->tot_len, 0);
  nr_lan91c111_tx_frame(eth0_addr, &sls, mac_send_buffer, p->tot_len);
  return ERR_OK;
}

static err_t
netif_set_opts(struct netif *netif)
{
  netif->linkoutput = netif_output;
  netif->output     = etharp_output;
  netif->mtu        = 1500;
  netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  netif->hwaddr_len = 6;
  netif->hwaddr[0] = 0x00;
  netif->hwaddr[1] = 0x23;
  netif->hwaddr[2] = 0xC1;
  netif->hwaddr[3] = 0xDE;
  netif->hwaddr[4] = 0xD0;
  netif->hwaddr[5] = 0x0D;

  return ERR_OK;
}

// DHCP timeout handling
#define DHCP_FINE_TIMER_MSECS 500
#define DHCP_COARSE_TIMER_SECS 60
static uint32_t dhcp_fine_timer_ms;
static uint32_t dhcp_coarse_timer_ms;

// Timer management
static uint32_t overflow_count;
static uint32_t last_timer_value;

// Memory-mapped register access functions
static inline void mmio_write(uint32_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}

static inline uint32_t mmio_read(uint32_t addr) {
    return *(volatile uint32_t*)addr;
}

// Initialize the hardware timer - we use it in free-running mode
static void timer_init(void) {
    // Set timer to periodic mode, 32-bit counter, and enable it
    mmio_write(TIMER0_BASE + TIMER_CONTROL, TIMER_CTRL_32BIT | TIMER_CTRL_PERIODIC | TIMER_CTRL_ENABLE);
    
    // Load with max value (counts down from this value)
    mmio_write(TIMER0_BASE + TIMER_LOAD, 0xFFFFFFFF);
    
    // Initialize our timer tracking variables
    overflow_count = 0;
    last_timer_value = 0xFFFFFFFF;
}

// Get current time in milliseconds, handling timer overflow correctly
static uint32_t get_ms_time(void) {
    uint32_t current_value = mmio_read(TIMER0_BASE + TIMER_VALUE);
    
    // Detect overflow (current value is higher than last value since timer counts down)
    if (current_value > last_timer_value) {
        overflow_count++;
    }
    
    // Update last value for next call
    last_timer_value = current_value;
    
    // Calculate total microseconds: each overflow is 2^32 µs, plus the elapsed time from current cycle
    uint64_t total_us = ((uint64_t)overflow_count << 32) + (0xFFFFFFFF - current_value);
    
    // Convert to milliseconds
    return (uint32_t)(total_us / 1000);
}

// IP configuration fallback (used if DHCP fails)
void use_static_ip(void) {
  ip4_addr_t static_addr;
  ip4_addr_t static_netmask;
  ip4_addr_t static_gw;
  
  // Static IP configuration for tap0 interface
  IP4_ADDR(&static_addr, 10, 0, 2, 99);
  IP4_ADDR(&static_netmask, 255, 255, 255, 0);
  IP4_ADDR(&static_gw, 10, 0, 0, 1);
  
  // Set static IP address
  netif_set_addr(&netif, &static_addr, &static_netmask, &static_gw);
  printf("DHCP failed, using static IP: %s\n", ip4addr_ntoa(&static_addr));
}

void 
c_entry() {
  ip4_addr_t addr;
  ip4_addr_t netmask;
  ip4_addr_t gw;
  uint32_t current_time;
  
  // Initialize with zeros for DHCP
  IP4_ADDR(&addr, 0, 0, 0, 0);
  IP4_ADDR(&netmask, 0, 0, 0, 0);
  IP4_ADDR(&gw, 0, 0, 0, 0);

  // Initialize hardware timer
  timer_init();
  
  // Initialize DHCP timers
  current_time = get_ms_time();
  dhcp_fine_timer_ms = current_time;
  dhcp_coarse_timer_ms = current_time;

  lwip_init();
  
  // Add interface with empty IP addresses - will be configured by DHCP
  netif_add(&netif, &addr, &netmask, &gw, 
            NULL, netif_set_opts, netif_input);

  netif.name[0] = 'e';
  netif.name[1] = '0';
  netif_set_default(&netif);
  netif_set_up(&netif);

  // Start DHCP
  dhcp_start(&netif);
  
  // Initialize network hardware
  nr_lan91c111_reset(eth0_addr, &sls, &sls);
  nr_lan91c111_set_promiscuous(eth0_addr, &sls, 1);

  // Main loop with network processing
  while(1) {
    // Process incoming network frames
    nr_lan91c111_check_for_events(eth0_addr, &sls, process_frames);
    
    // Get accurate time
    current_time = get_ms_time();
    
    // Handle DHCP fine timer (500ms)
    if (current_time - dhcp_fine_timer_ms >= DHCP_FINE_TIMER_MSECS) {
      dhcp_fine_timer_ms = current_time;
      dhcp_fine_tmr();
      
      // Check if we have an address from DHCP
      if (dhcp_supplied_address(&netif)) {
        printf("DHCP configured: IP=%s\n", ip4addr_ntoa(&netif.ip_addr));
      }
    }
    
    // DHCP coarse timer (60s)
    if (current_time - dhcp_coarse_timer_ms >= DHCP_COARSE_TIMER_SECS * 1000) {
      dhcp_coarse_timer_ms = current_time;
      dhcp_coarse_tmr();
    }
    
    // If the interface is up but no address after a timeout, use static IP
    if ((current_time > 10000) && netif_is_up(&netif) && ip4_addr_isany_val(*netif_ip4_addr(&netif))) {
      use_static_ip();
    }
  }
}

