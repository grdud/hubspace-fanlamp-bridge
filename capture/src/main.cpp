// USB BLE advertisement transport. No Wi-Fi, GPIO control, pairing or auto-TX.
#include <Arduino.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

struct Packet { uint32_t ms; int rssi; uint8_t addr[6], len, data[31]; };
QueueHandle_t packets;
EventGroupHandle_t events;
volatile uint32_t dropped = 0;
volatile int lastStatus = 0;
constexpr EventBits_t DONE = 1;
bool scanning = false;

void gap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
  if (event == ESP_GAP_BLE_SCAN_RESULT_EVT && p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
    Packet x{};
    x.ms = millis(); x.rssi = p->scan_rst.rssi;
    memcpy(x.addr, p->scan_rst.bda, 6);
    x.len = min((int)p->scan_rst.adv_data_len, 31);
    memcpy(x.data, p->scan_rst.ble_adv, x.len);
    if (xQueueSend(packets, &x, 0) != pdTRUE) dropped++;
    return;
  }
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: lastStatus = p->scan_param_cmpl.status; break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: lastStatus = p->scan_start_cmpl.status; break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: lastStatus = p->scan_stop_cmpl.status; break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: lastStatus = p->adv_data_raw_cmpl.status; break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT: lastStatus = p->adv_start_cmpl.status; break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT: lastStatus = p->adv_stop_cmpl.status; break;
    default: return;
  }
  xEventGroupSetBits(events, DONE);
}
void prepare() { xEventGroupClearBits(events, DONE); lastStatus = -1; }
bool completed(esp_err_t result) {
  if (result != ESP_OK) { Serial.printf("ERR API %d\n", result); return false; }
  if (!(xEventGroupWaitBits(events, DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000)) & DONE)) {
    Serial.println("ERR BLE timeout; restarting passive firmware"); Serial.flush(); ESP.restart(); return false;
  }
  if (lastStatus != ESP_BT_STATUS_SUCCESS) { Serial.printf("ERR BLE %d\n", lastStatus); return false; }
  return true;
}
bool scan(bool enabled) {
  if (enabled == scanning) return true;
  prepare();
  if (!completed(enabled ? esp_ble_gap_start_scanning(0) : esp_ble_gap_stop_scanning())) return false;
  scanning = enabled;
  return true;
}
int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
void command(const String &s) {
  if (s == "PING") { Serial.printf("OK fan-bridge 0.1 dropped=%lu scan=%d\n", (unsigned long)dropped, scanning); return; }
  if (s == "SCAN" || s == "STOP") { if (scan(s == "SCAN")) Serial.println("OK"); return; }
  if (!s.startsWith("TX ")) { Serial.println("ERR use PING SCAN STOP TX <hex>"); return; }
  String hex = s.substring(3);
  if (!hex.length() || hex.length() % 2 || hex.length() > 62) { Serial.println("ERR payload length"); return; }
  uint8_t raw[31]; int len = hex.length()/2;
  for (int i=0; i<len; i++) {
    int hi=nibble(hex[i*2]), lo=nibble(hex[i*2+1]);
    if (hi<0 || lo<0) { Serial.println("ERR payload hex"); return; }
    raw[i] = (hi<<4)|lo;
  }
  bool resume = scanning;
  if (!scan(false)) return;
  prepare();
  if (!completed(esp_ble_gap_config_adv_data_raw(raw, len))) { if(resume) scan(true); return; }
  esp_ble_adv_params_t params{};
  params.adv_int_min=0x20; params.adv_int_max=0x20;
  params.adv_type=ADV_TYPE_NONCONN_IND;
  params.own_addr_type=BLE_ADDR_TYPE_PUBLIC;
  params.channel_map=ADV_CHNL_ALL;
  params.adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  prepare();
  if (!completed(esp_ble_gap_start_advertising(&params))) { if(resume) scan(true); return; }
  delay(350);
  prepare();
  bool stopped = completed(esp_ble_gap_stop_advertising());
  if (!stopped) { Serial.flush(); ESP.restart(); return; }
  bool ready = !resume || scan(true);
  if (ready) Serial.println("OK TX radio-only; fan response unverified");
}
void setup() {
  Serial.begin(115200);
  packets=xQueueCreate(96, sizeof(Packet)); events=xEventGroupCreate();
  if (!packets || !events) { Serial.println("ERR memory"); while(true) delay(1000); }
  esp_bt_controller_config_t cfg=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_init(&cfg)!=ESP_OK || esp_bt_controller_enable(ESP_BT_MODE_BLE)!=ESP_OK ||
      esp_bluedroid_init()!=ESP_OK || esp_bluedroid_enable()!=ESP_OK || esp_ble_gap_register_callback(gap)!=ESP_OK) {
    Serial.println("ERR BLE init"); while(true) delay(1000);
  }
  esp_ble_scan_params_t params{};
  params.scan_type=BLE_SCAN_TYPE_PASSIVE;
  params.own_addr_type=BLE_ADDR_TYPE_PUBLIC;
  params.scan_filter_policy=BLE_SCAN_FILTER_ALLOW_ALL;
  params.scan_interval=0x50; params.scan_window=0x50;
  params.scan_duplicate=BLE_SCAN_DUPLICATE_DISABLE;
  prepare();
  if(completed(esp_ble_gap_set_scan_params(&params))) scan(true);
  Serial.println("READY fan-bridge 0.1 passive BLE USB 115200");
}
void loop() {
  static String line;
  static bool overflow=false;
  while(Serial.available()) {
    char c=Serial.read();
    if(c=='\n') { if(!overflow) { line.trim(); if(line.length()) command(line); } else Serial.println("ERR line too long"); line=""; overflow=false; }
    else if(line.length()<100) line+=c;
    else overflow=true;
  }
  Packet p;
  for(int n=0;n<8 && xQueueReceive(packets,&p,0)==pdTRUE;n++) {
    Serial.printf("ADV %lu %d ",(unsigned long)p.ms,p.rssi);
    for(auto b:p.addr) Serial.printf("%02X",b);
    Serial.print(' ');
    for(int i=0;i<p.len;i++) Serial.printf("%02X",p.data[i]);
    Serial.println();
  }
  delay(1);
}
