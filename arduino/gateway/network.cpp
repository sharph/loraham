#include <RH_RF95.h>  //See http://www.airspayce.com/mikem/arduino/RadioHead/

#include "config.h"
#include "platforms.h"

#include "utilities.h"
#include "sleep.h"
#include "network.h"

#define NETWORK_TIMERSLOT 1

// initialization status of radio
bool radioisinit = false;

typedef struct {
  uint8_t data[RH_RF95_MAX_MESSAGE_LEN + 1];
  int rssi;
  bool delay; // do we need to delay before sending this packet; is this packet digipeated?
} Packet;


// Singleton instance of the radio driver
RH_RF95 rf95(RFM95_CS, RFM95_INT);

// recv/xmit buffers
Packet recvbuf[BUFFER_PACKETS];
Packet xmitbuf[BUFFER_PACKETS];

uint8_t recvbufi = 0;
int8_t xmitbufi = -1;


// queue a packet for transmission
void queuepkt(uint8_t *buf, bool delay) {
  if (xmitbufi < BUFFER_PACKETS - 1) {
    xmitbufi++;
  }
  strcpy((char*) xmitbuf[xmitbufi].data, (char*) buf);
  xmitbuf[xmitbufi].data[RH_RF95_MAX_MESSAGE_LEN] = 0; // just in case!
  xmitbuf[xmitbufi].delay = delay;
}

void radiosetup()
{
  // clear buffers
  memset(recvbuf, 0, (BUFFER_PACKETS*sizeof (Packet)));
  memset(xmitbuf, 0, (BUFFER_PACKETS*sizeof (Packet)));

  // configure radio pins
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
}


void radioon() {
  if(radioisinit == true) {
    rf95.setModeRx();
    return;
  }
  // manual reset
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  while (!rf95.init()) {
    Serial.println("# LoRa radio init failed");
    delay(10000);
  }

  RH_RF95::ModemConfig modemconfig = {
    // see pg 106 http://www.hoperf.com/upload/rf/RFM95_96_97_98W.pdf
    0x76, // reg 1D - 125kHz, 4/7
    0xc4, // reg 1E - SF=12, CRC on
    0x0c  // reg 26 - low data rate on, AGC on
  };

  rf95.setModemRegisters(&modemconfig);

  // Defaults after init are 434.0MHz, modulation GFSK_Rb250Fd250, +13dbM
  if (!rf95.setFrequency(RF95_FREQ)) {
    Serial.println("# setFrequency failed");
    //while (1);
  } else {
    Serial.print("# Set Freq to: "); Serial.println(RF95_FREQ);
  }

  // The default transmitter power is 13dBm, using PA_BOOST.
  // If you are using RFM95/96/97/98 modules which uses the PA_BOOST transmitter pin, then
  // you can set transmitter powers from 5 to 23 dBm:
  rf95.setTxPower(23, false);
  Serial.println("# Set power to 23.");
  Serial.print("# Max packet length: "); Serial.println(RH_RF95_MAX_MESSAGE_LEN);
  rf95.setModeRx();
  radioisinit = true;
}

void radiooff() {
  if(radioisinit == false) {
    return;
  }
  rf95.sleep();
}

//! put a beacon packet in the queue
void beacon(const char* msg) {
  static int packetnum = 0;
  float vcc=voltage();

  char radiopacket[RH_RF95_MAX_MESSAGE_LEN+1];
  snprintf(radiopacket,
           RH_RF95_MAX_MESSAGE_LEN,
           "BEACON %s %s VCC=%d.%03d count=%d uptime=%ld%s%s",
           CALLSIGN,
           BANNER,
           (int) vcc, (int) (vcc*1000)%1000,
           packetnum,
           uptime(),
           msg[0] == 0 ? "" : " ",
           msg);

  radiopacket[RH_RF95_MAX_MESSAGE_LEN] = 0;

  queuepkt((uint8_t*) radiopacket, false);
  packetnum++;
}

//! if packet available, place it in the recv buffer
bool recvpkt() {
  uint8_t len = RH_RF95_MAX_MESSAGE_LEN;
  bool packetrecieved = false;
  while (rf95.available()) {
    recvbufi = (recvbufi + 1) % BUFFER_PACKETS;
    if (rf95.recv(recvbuf[recvbufi].data, &len)) {
      recvbuf[recvbufi].data[len] = 0;
      recvbuf[recvbufi].rssi = rf95.lastRssi();
      Serial.println((char*) recvbuf[recvbufi].data);
      Serial.print("RX ");
      Serial.print(CALLSIGN);
      Serial.print(" rssi");
      Serial.println(recvbuf[recvbufi].rssi);
      Serial.println();
      packetrecieved = true;
    }
  }
  return packetrecieved;
}

// looks at all the packets in the recv buffer and takes appropriate action
bool handlepackets() {
  bool packethandled = false;
  for (int i = 0; i < BUFFER_PACKETS; i++) {
    if (strlen((char*) recvbuf[i].data) > 0) {
      if (shouldrt(recvbuf[i].data)) {
        digipeat(recvbuf[i].data, recvbuf[i].rssi);
      }

      memset(&recvbuf[i], 0, sizeof (Packet)); // handled
      packethandled = true;
    }
  }
  return packethandled;
}

// Add RT lines to recieved packet and queue it for transmission
bool digipeat(uint8_t *pkt, int rssi) {
  uint8_t data[RH_RF95_MAX_MESSAGE_LEN + 2];
  snprintf((char*) data,
           RH_RF95_MAX_MESSAGE_LEN + 1,
           "%s\n" //First line is the original packet.
           "RT %s rssi=%d", //Then we append our call and strength as a repeater.
           (char*) pkt,
           CALLSIGN,  //Repeater's callsign.
           rssi //Signal strength, for routing.
          );
  if (strlen((char*) data) > RH_RF95_MAX_MESSAGE_LEN) {
    Serial.println("# Packet too long.\n");
    return false; // packet too long
  }
  
  queuepkt(data, true);
  return true;
}

// transmits all the packets in the xmit stack, while receiving any that come in
void xmitstack() {
  int delaytime = random(MAX_XMIT_WAIT) + 2000; // add 2 seconds in case another packet is on its way
  bool delayed = false;
  while (xmitbufi > -1) {
    if (!delayed && xmitbuf[xmitbufi].delay) {
      sleepreset(NETWORK_TIMERSLOT);
      while (!sleep(delaytime / 1000, NETWORK_TIMERSLOT)) { recvpkt(); }
      delay(delaytime % 1000);
      delayed = true;
    }
    while (recvpkt()) {}
    
    //Serial.print("TX: ");
    Serial.println((char*) xmitbuf[xmitbufi].data);
    Serial.println();
#ifdef DEBUG_LED_XMIT
    digitalWrite(LED, HIGH);
#endif
    rf95.send(xmitbuf[xmitbufi].data, strlen((char*) xmitbuf[xmitbufi].data));
    rf95.waitPacketSent();
#ifdef DEBUG_LED_XMIT
    digitalWrite(LED, LOW);
#endif

    xmitbufi--;
  }
  rf95.setModeRx();
}


