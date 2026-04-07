
#include "local_ui.h"
#include "storage_backend.h"
#include "timekeeper.h"
#include "i2c.h"
#include "io_extension.h"
#include "rgb_lcd_port.h"
#include "lvgl_port.h"
#include <Update.h>
#include <lvgl.h>
#include <Arduino.h>

#define RF73_HAS_LVGL 1

extern "C" float read_battery_voltage(void) __attribute__((weak));

#if RF73_HAS_LVGL
namespace {

struct ParsedFw {
  String name;
  String target;
  String chip;
  String version;
  String channel;
  bool valid = false;
  bool canSlave = false;
  bool canMaster = false;
};

enum UiPageId : uint8_t { PAGE_HOME=0, PAGE_LIVE, PAGE_SENSORS, PAGE_OTA, PAGE_OPTIONS };

static bool g_ready = false;
static bool g_boardReady = false;
static unsigned long g_lastUiRefreshMs = 0;
static UiPageId g_page = PAGE_HOME;
static ParsedFw g_selectedSlaveFw;
static ParsedFw g_selectedMasterFw;

static lv_obj_t* screenRoot = nullptr;
static lv_obj_t* header = nullptr;
static lv_obj_t* footer = nullptr;
static lv_obj_t* content = nullptr;
static lv_obj_t* pageHome = nullptr;
static lv_obj_t* pageLive = nullptr;
static lv_obj_t* pageSensors = nullptr;
static lv_obj_t* pageOta = nullptr;
static lv_obj_t* pageOptions = nullptr;

static lv_obj_t* lblHeaderBattery = nullptr;
static lv_obj_t* lblHeaderWifi = nullptr;
static lv_obj_t* lblHeaderTime = nullptr;
static lv_obj_t* lblHeaderDevice = nullptr;

static lv_obj_t* lblFooterEspNow = nullptr;
static lv_obj_t* lblFooterOta = nullptr;
static lv_obj_t* lblFooterEvent = nullptr;
static lv_obj_t* lblFooterDiag = nullptr;
static lv_obj_t* sdTapZone = nullptr;
static lv_obj_t* sdListModal = nullptr;
static lv_obj_t* sdListScroll = nullptr;
static lv_obj_t* sdListText = nullptr;

static lv_obj_t* liveValue[MAX_ASSIGNED_NODES] = {};
static lv_obj_t* liveMeta[MAX_ASSIGNED_NODES] = {};
static lv_obj_t* liveTile[MAX_ASSIGNED_NODES] = {};

static lv_obj_t* sensorsCards[MAX_ASSIGNED_NODES] = {};
static lv_obj_t* sensorsCardId[MAX_ASSIGNED_NODES] = {};
static lv_obj_t* sensorsCardStatus[MAX_ASSIGNED_NODES] = {};
static lv_obj_t* sensorsCardValue[MAX_ASSIGNED_NODES] = {};
static lv_obj_t* sensorsCardMeta[MAX_ASSIGNED_NODES] = {};
static lv_obj_t* sensorsUnknownCount = nullptr;
static lv_obj_t* sensorsUnknownScroll = nullptr;
static lv_obj_t* sensorsUnknownText = nullptr;

static lv_obj_t* otaSlaveFile = nullptr;
static lv_obj_t* otaMasterFile = nullptr;
static lv_obj_t* otaBatchStatus = nullptr;
static lv_obj_t* otaManualBtns[MAX_ASSIGNED_NODES] = {};

static lv_obj_t* swSkipSame = nullptr;
static lv_obj_t* swAllowUnknownBatt = nullptr;
static lv_obj_t* swBlockLowBatt = nullptr;
static lv_obj_t* swVerbose = nullptr;
static lv_obj_t* sliderMinSoc = nullptr;
static lv_obj_t* lblMinSoc = nullptr;
static lv_obj_t* touchCursor = nullptr;
static lv_obj_t* touchCursorDot = nullptr;

static const lv_color_t C_BG     = lv_color_hex(0x0F172A);
static const lv_color_t C_PANEL   = lv_color_hex(0x111827);
static const lv_color_t C_PANEL2  = lv_color_hex(0x1F2937);
static const lv_color_t C_TEXT    = lv_color_hex(0xFFFFFF);
static const lv_color_t C_MUTED   = lv_color_hex(0xFFFFFF);
static const lv_color_t C_GREEN   = lv_color_hex(0x22C55E);
static const lv_color_t C_BLUE    = lv_color_hex(0x2563EB);
static const lv_color_t C_RED     = lv_color_hex(0xEF4444);
static const lv_color_t C_AMBER   = lv_color_hex(0xF59E0B);
static const lv_color_t C_BORDER  = lv_color_hex(0x334155);
static const lv_font_t* FONT_TITLE = &lv_font_montserrat_26;
static const lv_font_t* FONT_BODY  = &lv_font_montserrat_26;
static const lv_font_t* FONT_VALUE = &lv_font_montserrat_44;


static void syncOptionsWidgets();

static bool parseFwName(const String& fileName, ParsedFw& out) {
  String name = fileName;
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  if (!name.startsWith("RF73_") || !name.endsWith(".bin")) return false;
  String base = name.substring(0, name.length() - 4);
  String rest = base.substring(5);
  int p1 = rest.indexOf('_');
  int p2 = rest.indexOf('_', p1 + 1);
  int p3 = rest.lastIndexOf('_');
  if (p1 <= 0 || p2 <= p1 + 1 || p3 <= p2 + 1) return false;
  out.name = name;
  out.target = rest.substring(0, p1); out.target.toUpperCase();
  out.chip = rest.substring(p1 + 1, p2); out.chip.toUpperCase();
  out.version = rest.substring(p2 + 1, p3); out.version.toUpperCase();
  out.channel = rest.substring(p3 + 1); out.channel.toLowerCase();
  out.valid = true;
  out.canMaster = (out.target == "MASTER" && out.chip == "ESP32S3");
  out.canSlave = (out.target == "SLAVE" && out.chip == "ESP32C3");
  return true;
}

static void refreshFirmwareSelections() {
  g_selectedSlaveFw = ParsedFw();
  g_selectedMasterFw = ParsedFw();
  if (!rf73Storage.isReady()) return;

  File root = rf73Storage.open(RF73_FW_DIR, FILE_READ);
  if (!root) return;

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      ParsedFw fw;
      if (parseFwName(String(file.name()), fw)) {
        if (!g_selectedSlaveFw.valid && fw.canSlave) g_selectedSlaveFw = fw;
        if (!g_selectedMasterFw.valid && fw.canMaster) g_selectedMasterFw = fw;
      }
    }
    file = root.openNextFile();
  }
}

static void updateLabel(lv_obj_t* obj, const String& s) {
  if (obj) lv_label_set_text(obj, s.c_str());
}

static String macToString(const uint8_t* mac) {
  if (!mac) return "--:--:--:--:--:--";
  char b[24];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}
static String listSdFilesText() {
  if (!rf73Storage.isReady()) return "SD non pronta";

  String out;
  File root = rf73Storage.open("/", FILE_READ);
  if (!root) return "Impossibile aprire root";
  if (!root.isDirectory()) {
    root.close();
    return "Root non valida";
  }

  File f = root.openNextFile();
  int count = 0;
  while (f) {
    out += String(f.name());
    if (f.isDirectory()) out += "  [DIR]";
    else out += "  (" + String((unsigned)f.size()) + " B)";
    out += "\n";
    ++count;
    f.close();
    f = root.openNextFile();
  }
  root.close();

  if (count == 0) return "SD vuota";
  return out;
}

static void closeSdListPopup() {
  if (sdListModal) {
    lv_obj_del(sdListModal);
    sdListModal = nullptr;
    sdListScroll = nullptr;
    sdListText = nullptr;
  }
}

static void sdListCloseEvent(lv_event_t* e) {
  LV_UNUSED(e);
  closeSdListPopup();
}

static void showSdListPopup() {
  if (sdListModal) {
    if (sdListText) updateLabel(sdListText, listSdFilesText());
    return;
  }

  sdListModal = lv_obj_create(lv_scr_act());
  lv_obj_set_size(sdListModal, 900, 450);
  lv_obj_center(sdListModal);
  lv_obj_set_style_bg_color(sdListModal, C_PANEL, 0);
  lv_obj_set_style_bg_opa(sdListModal, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(sdListModal, C_BORDER, 0);
  lv_obj_set_style_border_width(sdListModal, 2, 0);
  lv_obj_set_style_radius(sdListModal, 12, 0);
  lv_obj_set_style_pad_all(sdListModal, 12, 0);
  lv_obj_set_scrollbar_mode(sdListModal, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(sdListModal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(sdListModal);
  lv_label_set_text(title, "File presenti su SD");
  lv_obj_set_style_text_color(title, C_TEXT, 0);
  lv_obj_set_style_text_font(title, FONT_TITLE, 0);
  lv_obj_set_pos(title, 12, 8);

  lv_obj_t* btnClose = lv_btn_create(sdListModal);
  lv_obj_set_size(btnClose, 130, 42);
  lv_obj_set_pos(btnClose, 750, 8);
  lv_obj_add_event_cb(btnClose, sdListCloseEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(btnClose, C_PANEL2, 0);
  lv_obj_set_style_border_color(btnClose, C_BORDER, 0);

  lv_obj_t* btnLbl = lv_label_create(btnClose);
  lv_label_set_text(btnLbl, "CHIUDI");
  lv_obj_set_style_text_color(btnLbl, C_TEXT, 0);
  lv_obj_center(btnLbl);

  sdListScroll = lv_obj_create(sdListModal);
  lv_obj_set_pos(sdListScroll, 12, 58);
  lv_obj_set_size(sdListScroll, 874, 378);
  lv_obj_set_style_bg_color(sdListScroll, C_PANEL2, 0);
  lv_obj_set_style_border_width(sdListScroll, 1, 0);
  lv_obj_set_style_border_color(sdListScroll, C_BORDER, 0);
  lv_obj_set_style_radius(sdListScroll, 10, 0);
  lv_obj_set_style_pad_all(sdListScroll, 12, 0);
  lv_obj_set_scroll_dir(sdListScroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sdListScroll, LV_SCROLLBAR_MODE_ACTIVE);

  sdListText = lv_label_create(sdListScroll);
  lv_obj_set_width(sdListText, 840);
  lv_obj_set_style_text_color(sdListText, C_TEXT, 0);
  lv_obj_set_style_text_font(sdListText, FONT_BODY, 0);
  lv_label_set_long_mode(sdListText, LV_LABEL_LONG_WRAP);
  lv_label_set_text(sdListText, "");

  updateLabel(sdListText, listSdFilesText());
}

static void sdTapEvent(lv_event_t* e) {
  LV_UNUSED(e);
  showSdListPopup();
}

static bool policyAllowsNodeOta(int idx, const String& targetVersion, String& err) {
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES) {
    err = "Nodo non valido";
    return false;
  }
  if (systemOptions.skipAlreadyUpdated && nodes[idx].fwValid && nodes[idx].fwVersion[0] && targetVersion == String(nodes[idx].fwVersion)) {
    err = "Gia aggiornato";
    return false;
  }
  if (!nodes[idx].batteryValid && !systemOptions.allowUnknownBattery) {
    err = "Batteria sconosciuta";
    return false;
  }
  if (systemOptions.blockLowBattery && nodes[idx].batteryValid && nodes[idx].batterySoc < systemOptions.minBatterySoc) {
    err = "Batteria bassa";
    return false;
  }
  return true;
}

static bool buildOtaPacketFromFw(const ParsedFw& fw, OtaStartPacket& pkt) {
  if (!fw.valid) return false;
  memset(&pkt, 0, sizeof(pkt));
  pkt.magic[0] = 'R';
  pkt.magic[1] = 'F';
  pkt.version = 1;
  pkt.type = PKT_OTA_START;
  strlcpy(pkt.ssid, AP_SSID, sizeof(pkt.ssid));
  strlcpy(pkt.password, AP_PASS, sizeof(pkt.password));
  String url = String("http://192.168.4.1/") + fw.name;
  strlcpy(pkt.url, url.c_str(), sizeof(pkt.url));
  String fwVer = fw.version + "_" + fw.channel;
  strlcpy(pkt.fwVersion, fwVer.c_str(), sizeof(pkt.fwVersion));
  pkt.fwSize = 0;
  pkt.fwCrc = 0;
  return true;
}

static void showPage(UiPageId page) {
  g_page = page;
  lv_obj_add_flag(pageHome, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(pageLive, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(pageSensors, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(pageOta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(pageOptions, LV_OBJ_FLAG_HIDDEN);
  switch (page) {
    case PAGE_HOME: lv_obj_clear_flag(pageHome, LV_OBJ_FLAG_HIDDEN); break;
    case PAGE_LIVE: lv_obj_clear_flag(pageLive, LV_OBJ_FLAG_HIDDEN); break;
    case PAGE_SENSORS: lv_obj_clear_flag(pageSensors, LV_OBJ_FLAG_HIDDEN); break;
    case PAGE_OTA: lv_obj_clear_flag(pageOta, LV_OBJ_FLAG_HIDDEN); refreshFirmwareSelections(); break;
    case PAGE_OPTIONS: lv_obj_clear_flag(pageOptions, LV_OBJ_FLAG_HIDDEN); syncOptionsWidgets(); break;
  }
}

static void btnPageEvent(lv_event_t* e) {
  UiPageId page = (UiPageId)(intptr_t)lv_event_get_user_data(e);
  showPage(page);
}

static void actionZeroAll(lv_event_t* e) {
  LV_UNUSED(e);
  sendZeroAll();
}

static void actionReloadFw(lv_event_t* e) {
  LV_UNUSED(e);
  refreshFirmwareSelections();
  setUiLastEvent("SD aggiornata");
}

static void actionBatchSlave(lv_event_t* e) {
  LV_UNUSED(e);
  if (!g_selectedSlaveFw.valid) {
    setUiLastEvent("Nessun FW slave valido");
    return;
  }
  OtaStartPacket pkt;
  if (!buildOtaPacketFromFw(g_selectedSlaveFw, pkt)) {
    setUiLastEvent("Errore packet OTA");
    return;
  }
  String err;
  if (!startBatchOta(g_selectedSlaveFw.name.c_str(), pkt, err)) {
    setUiLastEvent(err.c_str());
  } else {
    setUiLastEvent("Batch OTA avviato");
  }
}

static void actionManualSlave(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (!g_selectedSlaveFw.valid) {
    setUiLastEvent("Nessun FW slave valido");
    return;
  }
  OtaStartPacket pkt;
  if (!buildOtaPacketFromFw(g_selectedSlaveFw, pkt)) {
    setUiLastEvent("Errore packet OTA");
    return;
  }
  String err;
  if (!policyAllowsNodeOta(idx, String(pkt.fwVersion), err)) {
    setUiLastEvent((String(nodes[idx].id) + " " + err).c_str());
    return;
  }
  if (sendOtaToAssignedNode(idx, pkt)) {
    setUiLastEvent((String("OTA ") + nodes[idx].id + " inviato").c_str());
  } else {
    setUiLastEvent((String("OTA ") + nodes[idx].id + " invio fallito").c_str());
  }
}

static void actionMasterUpdate(lv_event_t* e) {
  LV_UNUSED(e);
  if (!g_selectedMasterFw.valid) {
    setUiLastEvent("Nessun FW master valido");
    return;
  }
  File fw = rf73Storage.open(String("/") + g_selectedMasterFw.name, FILE_READ);
  if (!fw) {
    setUiLastEvent("Apertura FW master fallita");
    return;
  }
  size_t size = fw.size();
  if (!Update.begin(size)) {
    setUiLastEvent("Update.begin fallita");
    return;
  }
  uint8_t buf[2048];
  size_t writtenTotal = 0;
  while (fw.available()) {
    int n = fw.read(buf, sizeof(buf));
    if (n <= 0) break;
    size_t w = Update.write(buf, (size_t)n);
    if (w != (size_t)n) {
      Update.abort();
      setUiLastEvent("Scrittura FW master fallita");
      return;
    }
    writtenTotal += w;
    lv_timer_handler();
  }
  if (!Update.end(true) || writtenTotal != size) {
    setUiLastEvent("Finalize FW master fallita");
    return;
  }
  setUiLastEvent("FW master pronto, riavvio...");
  delay(500);
  ESP.restart();
}

static void actionSaveOptions(lv_event_t* e) {
  LV_UNUSED(e);
  systemOptions.skipAlreadyUpdated = lv_obj_has_state(swSkipSame, LV_STATE_CHECKED);
  systemOptions.allowUnknownBattery = lv_obj_has_state(swAllowUnknownBatt, LV_STATE_CHECKED);
  systemOptions.blockLowBattery = lv_obj_has_state(swBlockLowBatt, LV_STATE_CHECKED);
  systemOptions.verboseOtaLog = lv_obj_has_state(swVerbose, LV_STATE_CHECKED);
  systemOptions.minBatterySoc = (float)lv_slider_get_value(sliderMinSoc);
  if (saveSystemOptions()) setUiLastEvent("Opzioni salvate");
  else setUiLastEvent("Errore salvataggio opzioni");
  syncOptionsWidgets();
}

static void optionSliderEvent(lv_event_t* e) {
  LV_UNUSED(e);
  if (!sliderMinSoc || !lblMinSoc) return;
  String txt = String("Soglia minima: ") + lv_slider_get_value(sliderMinSoc) + "%";
  lv_label_set_text(lblMinSoc, txt.c_str());
}

static lv_obj_t* makePanel(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_bg_color(obj, C_PANEL, 0);
  lv_obj_set_style_border_color(obj, C_BORDER, 0);
  lv_obj_set_style_radius(obj, 14, 0);
  lv_obj_set_style_pad_all(obj, 10, 0);
  return obj;
}

static lv_obj_t* makeActionButton(lv_obj_t* parent, const char* txt, int x, int y, int w, int h, lv_event_cb_t cb, void* user = nullptr) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, C_BLUE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4ED8), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x60A5FA), 0);
  lv_obj_set_style_radius(btn, 14, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
  lv_obj_t* label = lv_label_create(btn);
  lv_label_set_text(label, txt);
  lv_obj_set_style_text_color(label, C_TEXT, 0);
  lv_obj_center(label);
  return btn;
}

static void buildHeader() {
  header = lv_obj_create(screenRoot);
  lv_obj_set_size(header, RF73_LOCAL_LCD_WIDTH, RF73_LOCAL_HEADER_HEIGHT);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_bg_color(header, C_PANEL, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_pad_all(header, 8, 0);

  lblHeaderBattery = lv_label_create(header);
  lv_obj_set_style_text_color(lblHeaderBattery, C_TEXT, 0);
  lv_obj_set_pos(lblHeaderBattery, 16, 8);

  lblHeaderWifi = lv_label_create(header);
  lv_obj_set_style_text_color(lblHeaderWifi, C_TEXT, 0);
  lv_obj_set_pos(lblHeaderWifi, 230, 8);

  lblHeaderTime = lv_label_create(header);
  lv_obj_set_style_text_color(lblHeaderTime, C_TEXT, 0);
  lv_obj_align(lblHeaderTime, LV_ALIGN_TOP_MID, 0, 8);

  lblHeaderDevice = lv_label_create(header);
  lv_obj_set_style_text_color(lblHeaderDevice, C_TEXT, 0);
  lv_obj_align(lblHeaderDevice, LV_ALIGN_TOP_RIGHT, -16, 8);
  lv_label_set_text(lblHeaderDevice, DEVICE_NAME);
}

static void buildFooter() {
  footer = lv_obj_create(screenRoot);
  lv_obj_set_size(footer, RF73_LOCAL_LCD_WIDTH, RF73_LOCAL_FOOTER_HEIGHT);
  lv_obj_set_pos(footer, 0, RF73_LOCAL_LCD_HEIGHT - RF73_LOCAL_FOOTER_HEIGHT);
  lv_obj_set_style_bg_color(footer, C_PANEL, 0);
  lv_obj_set_style_border_width(footer, 0, 0);
  lv_obj_set_style_pad_all(footer, 6, 0);
  lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

  lblFooterEspNow = lv_label_create(footer);
  lv_obj_set_style_text_color(lblFooterEspNow, C_TEXT, 0);
  lv_obj_set_pos(lblFooterEspNow, 12, 10);

  lblFooterOta = lv_label_create(footer);
  lv_obj_set_style_text_color(lblFooterOta, C_TEXT, 0);
  lv_obj_set_pos(lblFooterOta, 220, 10);

  lblFooterEvent = lv_label_create(footer);
  lv_obj_set_style_text_color(lblFooterEvent, C_TEXT, 0);
  lv_obj_set_pos(lblFooterEvent, 420, 10);
  lv_obj_set_width(lblFooterEvent, 420);
  lv_label_set_long_mode(lblFooterEvent, LV_LABEL_LONG_DOT);

  lblFooterDiag = lv_label_create(footer);
  lv_obj_set_style_text_color(lblFooterDiag, C_TEXT, 0);
  lv_obj_align(lblFooterDiag, LV_ALIGN_TOP_RIGHT, -12, 10);

  sdTapZone = lv_btn_create(footer);
  lv_obj_remove_style_all(sdTapZone);
  lv_obj_set_size(sdTapZone, 150, RF73_LOCAL_FOOTER_HEIGHT - 8);
  lv_obj_set_pos(sdTapZone, RF73_LOCAL_LCD_WIDTH - 170, 4);
  lv_obj_set_style_bg_opa(sdTapZone, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(sdTapZone, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(sdTapZone, sdTapEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_move_foreground(sdTapZone);
}

static void buildHomePage() {
  pageHome = lv_obj_create(content);
  lv_obj_set_size(pageHome, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(pageHome, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pageHome, 0, 0);

  lv_obj_t* title = lv_label_create(pageHome);
  lv_label_set_text(title, "SmartSetUP");
  lv_obj_set_style_text_font(title, FONT_TITLE, 0);
  lv_obj_set_pos(title, 24, 18);

  lv_obj_t* sub = lv_label_create(pageHome);
  lv_label_set_text(sub, "HMI locale primaria · paddock mode");
  lv_obj_set_style_text_color(sub, C_MUTED, 0);
  lv_obj_set_pos(sub, 28, 58);

  makeActionButton(pageHome, "LIVE MEASURE", 48, 110, 220, 120, btnPageEvent, (void*)PAGE_LIVE);
  makeActionButton(pageHome, "SENSORS CONFIG", 292, 110, 220, 120, btnPageEvent, (void*)PAGE_SENSORS);
  makeActionButton(pageHome, "FIRMWARE UPDATE", 536, 110, 220, 120, btnPageEvent, (void*)PAGE_OTA);
  makeActionButton(pageHome, "OPZIONI", 780, 110, 190, 120, btnPageEvent, (void*)PAGE_OPTIONS);

  makeActionButton(pageHome, "ZERO ALL", 48, 262, 190, 72, actionZeroAll);
  makeActionButton(pageHome, "RICARICA SD", 254, 262, 190, 72, actionReloadFw);
  makeActionButton(pageHome, "BATCH OTA", 460, 262, 190, 72, actionBatchSlave);
  makeActionButton(pageHome, "MASTER OTA", 666, 262, 190, 72, actionMasterUpdate);
}

static void buildLivePage() {
  pageLive = lv_obj_create(content);
  lv_obj_set_size(pageLive, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(pageLive, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pageLive, 0, 0);

  makeActionButton(pageLive, "HOME", 18, 12, 120, 44, btnPageEvent, (void*)PAGE_HOME);
  makeActionButton(pageLive, "ZERO ALL", 858, 12, 140, 44, actionZeroAll);

  const int tileW = 300;
  const int tileH = 120;
  struct P { int x; int y; const char* id; } pos[5] = {
    {  56,  82, "FL"}, { 668,  82, "FR"}, { 362, 210, "ST"}, {  56, 338, "RL"}, { 668, 338, "RR"}
  };

  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    liveTile[i] = makePanel(pageLive, pos[i].x, pos[i].y, tileW, tileH);
    lv_obj_t* id = lv_label_create(liveTile[i]);
    lv_label_set_text(id, pos[i].id);
    lv_obj_set_style_text_font(id, FONT_BODY, 0);
    lv_obj_set_pos(id, 10, 6);

    liveValue[i] = lv_label_create(liveTile[i]);
    lv_label_set_text(liveValue[i], "--.-°");
    lv_obj_set_style_text_font(liveValue[i], FONT_VALUE, 0);
    lv_obj_set_pos(liveValue[i], 10, 34);

    liveMeta[i] = lv_label_create(liveTile[i]);
    lv_label_set_text(liveMeta[i], "ATTESA DATI");
    lv_obj_set_style_text_color(liveMeta[i], C_MUTED, 0);
    lv_obj_set_pos(liveMeta[i], 12, 88);
  }
}

static void buildSensorsPage() {
  pageSensors = lv_obj_create(content);
  lv_obj_set_size(pageSensors, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(pageSensors, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pageSensors, 0, 0);

  makeActionButton(pageSensors, "HOME", 18, 12, 120, 44, btnPageEvent, (void*)PAGE_HOME);
  makeActionButton(pageSensors, "ZERO ALL", 824, 12, 170, 44, actionZeroAll);
  makeActionButton(pageSensors, "AGGIORNA", 640, 12, 170, 44, actionReloadFw);

  lv_obj_t* title = lv_label_create(pageSensors);
  lv_label_set_text(title, "Sensors Config");
  lv_obj_set_style_text_font(title, FONT_TITLE, 0);
  lv_obj_set_style_text_color(title, C_TEXT, 0);
  lv_obj_set_pos(title, 190, 16);

  lv_obj_t* subtitle = lv_label_create(pageSensors);
  lv_label_set_text(subtitle, "Tile nodi assegnati + sezione sconosciuti");
  lv_obj_set_style_text_color(subtitle, C_MUTED, 0);
  lv_obj_set_pos(subtitle, 190, 50);

  lv_obj_t* scroll = lv_obj_create(pageSensors);
  lv_obj_set_pos(scroll, 0, 74);
  lv_obj_set_size(scroll, RF73_LOCAL_LCD_WIDTH, 446);
  lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scroll, 0, 0);
  lv_obj_set_style_pad_all(scroll, 0, 0);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);

  struct CardPos { int x; int y; };
  const CardPos pos[MAX_ASSIGNED_NODES] = {
    {24, 8}, {362, 8}, {700, 8}, {193, 196}, {531, 196}
  };

  for (int i = 0; i < MAX_ASSIGNED_NODES; ++i) {
    sensorsCards[i] = makePanel(scroll, pos[i].x, pos[i].y, 300, 170);
    lv_obj_clear_flag(sensorsCards[i], LV_OBJ_FLAG_SCROLLABLE);

    sensorsCardId[i] = lv_label_create(sensorsCards[i]);
    lv_obj_set_style_text_font(sensorsCardId[i], FONT_TITLE, 0);
    lv_obj_set_style_text_color(sensorsCardId[i], C_TEXT, 0);
    lv_obj_set_pos(sensorsCardId[i], 10, 8);

    sensorsCardStatus[i] = lv_label_create(sensorsCards[i]);
    lv_obj_set_style_text_color(sensorsCardStatus[i], C_TEXT, 0);
    lv_obj_align(sensorsCardStatus[i], LV_ALIGN_TOP_RIGHT, -10, 12);

    lv_obj_t* cap = lv_label_create(sensorsCards[i]);
    lv_label_set_text(cap, "Camber");
    lv_obj_set_style_text_color(cap, C_MUTED, 0);
    lv_obj_set_pos(cap, 10, 44);

    sensorsCardValue[i] = lv_label_create(sensorsCards[i]);
    lv_obj_set_style_text_font(sensorsCardValue[i], FONT_VALUE, 0);
    lv_obj_set_style_text_color(sensorsCardValue[i], C_TEXT, 0);
    lv_obj_set_pos(sensorsCardValue[i], 10, 66);

    sensorsCardMeta[i] = lv_label_create(sensorsCards[i]);
    lv_obj_set_width(sensorsCardMeta[i], 276);
    lv_label_set_long_mode(sensorsCardMeta[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(sensorsCardMeta[i], C_TEXT, 0);
    lv_obj_set_pos(sensorsCardMeta[i], 10, 122);
  }

  lv_obj_t* unknownPanel = makePanel(scroll, 24, 388, 976, 184);
  lv_obj_clear_flag(unknownPanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* unkTitle = lv_label_create(unknownPanel);
  lv_label_set_text(unkTitle, "Nodi sconosciuti");
  lv_obj_set_style_text_font(unkTitle, FONT_BODY, 0);
  lv_obj_set_style_text_color(unkTitle, C_TEXT, 0);
  lv_obj_set_pos(unkTitle, 12, 10);

  sensorsUnknownCount = lv_label_create(unknownPanel);
  lv_label_set_text(sensorsUnknownCount, "0 online");
  lv_obj_set_style_text_color(sensorsUnknownCount, C_MUTED, 0);
  lv_obj_align(sensorsUnknownCount, LV_ALIGN_TOP_RIGHT, -12, 12);

  sensorsUnknownScroll = lv_obj_create(unknownPanel);
  lv_obj_set_pos(sensorsUnknownScroll, 10, 44);
  lv_obj_set_size(sensorsUnknownScroll, 952, 126);
  lv_obj_set_style_bg_color(sensorsUnknownScroll, C_PANEL2, 0);
  lv_obj_set_style_border_color(sensorsUnknownScroll, C_BORDER, 0);
  lv_obj_set_style_border_width(sensorsUnknownScroll, 1, 0);
  lv_obj_set_style_radius(sensorsUnknownScroll, 10, 0);
  lv_obj_set_style_pad_all(sensorsUnknownScroll, 10, 0);
  lv_obj_set_scroll_dir(sensorsUnknownScroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sensorsUnknownScroll, LV_SCROLLBAR_MODE_AUTO);

  sensorsUnknownText = lv_label_create(sensorsUnknownScroll);
  lv_obj_set_width(sensorsUnknownText, 920);
  lv_label_set_long_mode(sensorsUnknownText, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(sensorsUnknownText, C_TEXT, 0);
  lv_label_set_text(sensorsUnknownText, "Nessun nodo sconosciuto online");

  lv_obj_set_height(scroll, 590);
}

static void buildOtaPage() {
  pageOta = lv_obj_create(content);
  lv_obj_set_size(pageOta, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(pageOta, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pageOta, 0, 0);

  makeActionButton(pageOta, "HOME", 18, 12, 120, 44, btnPageEvent, (void*)PAGE_HOME);
  makeActionButton(pageOta, "RICARICA SD", 150, 12, 150, 44, actionReloadFw);
  makeActionButton(pageOta, "AUTO MODULI", 314, 12, 170, 44, actionBatchSlave);
  makeActionButton(pageOta, "AGGIORNA MASTER", 498, 12, 190, 44, actionMasterUpdate);

  lv_obj_t* p1 = makePanel(pageOta, 30, 84, 964, 120);
  lv_obj_t* l1 = lv_label_create(p1);
  lv_label_set_text(l1, "FW slave selezionato");
  lv_obj_set_style_text_color(l1, C_MUTED, 0);
  lv_obj_set_pos(l1, 8, 8);
  otaSlaveFile = lv_label_create(p1);
  lv_obj_set_pos(otaSlaveFile, 8, 38);
  lv_obj_set_style_text_font(otaSlaveFile, FONT_TITLE, 0);

  lv_obj_t* l2 = lv_label_create(p1);
  lv_label_set_text(l2, "FW master selezionato");
  lv_obj_set_style_text_color(l2, C_MUTED, 0);
  lv_obj_set_pos(l2, 500, 8);
  otaMasterFile = lv_label_create(p1);
  lv_obj_set_pos(otaMasterFile, 500, 38);
  lv_obj_set_style_text_font(otaMasterFile, FONT_TITLE, 0);

  lv_obj_t* p2 = makePanel(pageOta, 30, 220, 964, 112);
  lv_obj_t* lbl = lv_label_create(p2);
  lv_label_set_text(lbl, "Stato OTA");
  lv_obj_set_style_text_color(lbl, C_MUTED, 0);
  lv_obj_set_pos(lbl, 8, 8);
  otaBatchStatus = lv_label_create(p2);
  lv_obj_set_pos(otaBatchStatus, 8, 36);
  lv_obj_set_width(otaBatchStatus, 930);
  lv_label_set_long_mode(otaBatchStatus, LV_LABEL_LONG_WRAP);

  lv_obj_t* p3 = makePanel(pageOta, 30, 350, 964, 164);
  lv_obj_t* hdr = lv_label_create(p3);
  lv_label_set_text(hdr, "Aggiornamento manuale moduli");
  lv_obj_set_style_text_color(hdr, C_MUTED, 0);
  lv_obj_set_pos(hdr, 8, 8);
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    otaManualBtns[i] = makeActionButton(p3, nodes[i].id, 18 + i * 188, 56, 170, 74, actionManualSlave, (void*)(intptr_t)i);
  }
}

static lv_obj_t* makeOptionRow(lv_obj_t* parent, int y, const char* title, const char* subtitle, lv_obj_t** outSwitch) {
  lv_obj_t* row = makePanel(parent, 30, y, 964, 74);
  lv_obj_t* t = lv_label_create(row);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_font(t, FONT_BODY, 0);
  lv_obj_set_pos(t, 8, 8);
  lv_obj_t* s = lv_label_create(row);
  lv_label_set_text(s, subtitle);
  lv_obj_set_style_text_color(s, C_MUTED, 0);
  lv_obj_set_pos(s, 8, 42);
  *outSwitch = lv_switch_create(row);
  lv_obj_align(*outSwitch, LV_ALIGN_RIGHT_MID, -16, 0);
  return row;
}

static void buildOptionsPage() {
  pageOptions = lv_obj_create(content);
  lv_obj_set_size(pageOptions, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(pageOptions, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pageOptions, 0, 0);

  makeActionButton(pageOptions, "HOME", 18, 12, 120, 44, btnPageEvent, (void*)PAGE_HOME);
  makeActionButton(pageOptions, "SALVA", 858, 12, 140, 44, actionSaveOptions);

  makeOptionRow(pageOptions, 80,  "Salta moduli gia aggiornati", "Skip automatico se la versione coincide", &swSkipSame);
  makeOptionRow(pageOptions, 164, "Consenti update con batteria sconosciuta", "Per nodi senza fuel gauge ancora installato", &swAllowUnknownBatt);
  makeOptionRow(pageOptions, 248, "Blocca update con batteria bassa", "Usa la soglia minima qui sotto", &swBlockLowBatt);
  makeOptionRow(pageOptions, 332, "Log OTA dettagliato", "Piu verboso in batch e footer eventi", &swVerbose);

  lv_obj_t* row = makePanel(pageOptions, 416, 30, 964, 88);
  lblMinSoc = lv_label_create(row);
  lv_label_set_text(lblMinSoc, "Soglia minima: 30%");
  lv_obj_set_style_text_font(lblMinSoc, FONT_BODY, 0);
  lv_obj_set_pos(lblMinSoc, 8, 10);
  sliderMinSoc = lv_slider_create(row);
  lv_obj_set_width(sliderMinSoc, 640);
  lv_obj_set_pos(sliderMinSoc, 8, 48);
  lv_slider_set_range(sliderMinSoc, 5, 95);
  lv_obj_add_event_cb(sliderMinSoc, optionSliderEvent, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void createUi() {
  screenRoot = lv_obj_create(NULL);
  lv_obj_set_size(screenRoot, RF73_LOCAL_LCD_WIDTH, RF73_LOCAL_LCD_HEIGHT);
  lv_obj_set_style_bg_color(screenRoot, C_BG, 0);
  lv_obj_set_style_border_width(screenRoot, 0, 0);

  buildHeader();
  buildFooter();

  content = lv_obj_create(screenRoot);
  lv_obj_set_pos(content, 0, RF73_LOCAL_HEADER_HEIGHT);
  lv_obj_set_size(content, RF73_LOCAL_LCD_WIDTH, RF73_LOCAL_LCD_HEIGHT - RF73_LOCAL_HEADER_HEIGHT - RF73_LOCAL_FOOTER_HEIGHT);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0);

  buildHomePage();
  buildLivePage();
  buildSensorsPage();
  buildOtaPage();
  buildOptionsPage();

  touchCursor = lv_obj_create(screenRoot);
  lv_obj_remove_style_all(touchCursor);
  lv_obj_set_size(touchCursor, 26, 26);
  lv_obj_set_style_radius(touchCursor, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(touchCursor, 2, 0);
  lv_obj_set_style_border_color(touchCursor, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(touchCursor, lv_color_hex(0x2563EB), 0);
  lv_obj_set_style_bg_opa(touchCursor, LV_OPA_70, 0);
  lv_obj_add_flag(touchCursor, LV_OBJ_FLAG_HIDDEN);

  touchCursorDot = lv_obj_create(touchCursor);
  lv_obj_remove_style_all(touchCursorDot);
  lv_obj_set_size(touchCursorDot, 8, 8);
  lv_obj_set_style_radius(touchCursorDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(touchCursorDot, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(touchCursorDot);

  lv_scr_load(screenRoot);
  showPage(PAGE_HOME);
}

static void syncOptionsWidgets() {
  auto setSw = [](lv_obj_t* sw, bool v){
    if (!sw) return;
    if (v) lv_obj_add_state(sw, LV_STATE_CHECKED); else lv_obj_clear_state(sw, LV_STATE_CHECKED);
  };
  setSw(swSkipSame, systemOptions.skipAlreadyUpdated);
  setSw(swAllowUnknownBatt, systemOptions.allowUnknownBattery);
  setSw(swBlockLowBatt, systemOptions.blockLowBattery);
  setSw(swVerbose, systemOptions.verboseOtaLog);
  if (sliderMinSoc) lv_slider_set_value(sliderMinSoc, (int)systemOptions.minBatterySoc, LV_ANIM_OFF);
  optionSliderEvent(nullptr);
}

static void updateHeaderFooter() {
  float battV = 0.0f;
  bool battValid = false;
  const bool hasBatteryReader = (read_battery_voltage != nullptr);
  if (hasBatteryReader) {
    battV = read_battery_voltage();
    battValid = (battV > 2.5f && battV < 5.5f);
  }

  String battText;
  if (!hasBatteryReader) battText = "BAT ADC ?";
  else battText = battValid ? (String("BAT ") + String(battV, 2) + "V") : "BAT --";

  wifi_mode_t mode = WiFi.getMode();
  bool apOn = (mode == WIFI_AP || mode == WIFI_AP_STA);
  bool staOn = (WiFi.status() == WL_CONNECTED);
  String wifiText = String("AP ") + (apOn ? "ON" : "OFF") + " (" + WiFi.softAPgetStationNum() + ") | STA " + (staOn ? "ON" : "OFF");

  String diagText = rf73Storage.isReady() ? (String("SD ") + rf73Storage.backendName()) : "SD --";
  if (staOn) diagText += String(" | RSSI ") + WiFi.RSSI() + " dBm";
  String nowText = rf73DateTimeText();

  updateLabel(lblHeaderBattery, battText);
  updateLabel(lblHeaderWifi, wifiText);
  updateLabel(lblHeaderTime, nowText);
  updateLabel(lblHeaderDevice, DEVICE_NAME);

  String espNowText = String("ESP-NOW ") + (isEspNowLinkHealthy() ? "OK" : "ATTESA") + "  A:" + getAssignedOnlineCount() + " U:" + getUnknownOnlineCount();
  String otaText = String("OTA ") + (batchOta.active ? getCurrentBatchNodeId() : "--");
  String eventText = String(uiLastEvent[0] ? uiLastEvent : "Pronto");
  updateLabel(lblFooterEspNow, espNowText);
  updateLabel(lblFooterOta, otaText);
  updateLabel(lblFooterEvent, eventText);
  updateLabel(lblFooterDiag, diagText);
}

static void updateLivePage() {
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    String value = String(nodes[i].camber, 2) + "°";
    if (!nodes[i].online && !nodes[i].lastSeenMs) value = "--.-°";
    String meta;
    if (nodes[i].online) meta = String(nodes[i].stable ? "STABILE" : "MOVING");
    else meta = nodes[i].lastSeenMs ? "OFFLINE" : "ATTESA DATI";
    if (nodes[i].batteryValid) meta += String(" · ") + String(nodes[i].batterySoc, 0) + "%";
    if (nodes[i].fwValid) meta += String(" · ") + nodes[i].fwVersion;
    updateLabel(liveValue[i], value);
    updateLabel(liveMeta[i], meta);
    lv_color_t c = !nodes[i].online ? C_RED : (nodes[i].stable ? C_GREEN : C_AMBER);
    lv_obj_set_style_bg_color(liveTile[i], c, 0);
    lv_obj_set_style_bg_opa(liveTile[i], LV_OPA_20, 0);
    lv_obj_set_style_border_color(liveTile[i], c, 0);
  }
}

static void updateSensorsPage() {
  for (int i = 0; i < MAX_ASSIGNED_NODES; ++i) {
    String idText = nodes[i].id[0] ? String(nodes[i].id) : String("--");
    String statusText = nodes[i].online ? "ONLINE" : "OFFLINE";
    String valueText = nodes[i].online || nodes[i].lastSeenMs ? (String(nodes[i].camber, 1) + "°") : "--.-°";

    String meta = String("Toe ");
    meta += nodes[i].toeValid ? (String(nodes[i].toe, 1) + "°") : "--";
    meta += " | Batt ";
    meta += nodes[i].batteryValid ? (String(nodes[i].batterySoc, 0) + "%") : "--%";
    meta += "\nFW ";
    meta += (nodes[i].fwValid && nodes[i].fwVersion[0]) ? String(nodes[i].fwVersion) : String("--");
    meta += "\nMAC ";
    meta += macToString(nodes[i].mac);

    updateLabel(sensorsCardId[i], idText);
    updateLabel(sensorsCardStatus[i], statusText);
    updateLabel(sensorsCardValue[i], valueText);
    updateLabel(sensorsCardMeta[i], meta);

    lv_color_t c = !nodes[i].online ? C_RED : (nodes[i].stable ? C_GREEN : C_AMBER);
    lv_obj_set_style_border_color(sensorsCards[i], c, 0);
    lv_obj_set_style_bg_color(sensorsCards[i], c, 0);
    lv_obj_set_style_bg_opa(sensorsCards[i], LV_OPA_10, 0);
    lv_obj_set_style_text_color(sensorsCardStatus[i], c, 0);
  }

  String unknown;
  unknown.reserve(1400);
  int unknownCount = 0;
  for (int i = 0; i < MAX_UNKNOWN_NODES; ++i) {
    if (!unknownNodes[i].used || !unknownNodes[i].online) continue;
    ++unknownCount;

    unknown += String(unknownNodes[i].id[0] ? unknownNodes[i].id : "UNK");
    unknown += " | ";
    unknown += macToString(unknownNodes[i].mac);
    unknown += " | camber ";
    unknown += String(unknownNodes[i].camber, 1);
    unknown += "° | toe ";
    unknown += unknownNodes[i].toeValid ? (String(unknownNodes[i].toe, 1) + "°") : "--";
    unknown += " | batt ";
    unknown += unknownNodes[i].batteryValid ? (String(unknownNodes[i].batterySoc, 0) + "%") : "--%";
    unknown += " | fw ";
    unknown += (unknownNodes[i].fwValid && unknownNodes[i].fwVersion[0]) ? String(unknownNodes[i].fwVersion) : String("--");
    unknown += "\n";
  }

  if (unknown.length() == 0) {
    unknown = "Nessun nodo sconosciuto online";
  }

  updateLabel(sensorsUnknownText, unknown);
  if (sensorsUnknownCount) {
    lv_label_set_text_fmt(sensorsUnknownCount, "%d online", unknownCount);
  }
}

static void updateOtaPage() {
  updateLabel(otaSlaveFile, g_selectedSlaveFw.valid ? g_selectedSlaveFw.name : String("Nessun file slave valido"));
  updateLabel(otaMasterFile, g_selectedMasterFw.valid ? g_selectedMasterFw.name : String("Nessun file master valido"));
  String status = String(batchPhaseToText(batchOta.phase)) + " | " + batchOta.status + "\n" + getLastBatchLogLine();
  updateLabel(otaBatchStatus, status);
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    String label = String(nodes[i].id) + "\n" + (nodes[i].online ? "ON" : "OFF");
    if (nodes[i].fwValid) label += String("\n") + nodes[i].fwVersion;
    lv_obj_t* child = lv_obj_get_child(otaManualBtns[i], 0);
    if (child) lv_label_set_text(child, label.c_str());
  }
}

static bool bringUpBoard() {
  if (!RF73_ENABLE_LOCAL_HMI || !RF73_USE_WAVESHARE_7B) return false;

  Serial.println("[LOCAL UI] Bring-up Waveshare 7B...");

  // IMPORTANT:
  // touch_gt911_init() from the official Waveshare demo already performs
  // DEV_I2C_Init() and IO_EXTENSION_Init() internally.
  // Calling them again here causes:
  //   I2C bus acquire failed / ESP_ERR_INVALID_STATE
  // and the board reboots during local HMI bring-up.
  esp_lcd_touch_handle_t tp = touch_gt911_init();
  if (!tp) {
    Serial.println("[LOCAL UI] GT911 init failed");
    return false;
  }

  esp_lcd_panel_handle_t panel = waveshare_esp32_s3_rgb_lcd_init();
  if (!panel) {
    Serial.println("[LOCAL UI] LCD init failed");
    return false;
  }

  if (lvgl_port_init(panel, tp) != ESP_OK) {
    Serial.println("[LOCAL UI] LVGL port init failed");
    return false;
  }

  // Use the official demo symbol name as-is.
  wavesahre_rgb_lcd_bl_on();
  Serial.println("[LOCAL UI] Waveshare 7B ready");
  return true;
}

} // namespace
#endif

bool rf73LocalUiBegin() {
#if RF73_HAS_LVGL
  Serial.println("[LOCAL UI] BUILD FORZATA 7B");
  g_boardReady = bringUpBoard();
  if (!g_boardReady) return false;

  Serial.println("[LOCAL UI] createUi...");
  createUi();

  Serial.println("[LOCAL UI] sync widgets...");
  syncOptionsWidgets();
  refreshFirmwareSelections();
  updateHeaderFooter();
  updateLivePage();
  updateSensorsPage();
  updateOtaPage();

  Serial.println("[LOCAL UI] BEGIN DONE");
  return true;
#else
  Serial.println("[LOCAL UI] LVGL non trovato: HMI locale disabilitata.");
  return false;
#endif
}

bool rf73LocalUiStartRuntime() {
#if RF73_HAS_LVGL
  if (!g_boardReady) return false;

  g_ready = true;
  setUiLastEvent("HMI locale pronta");
  return true;
#else
  return false;
#endif
}

void rf73LocalUiTick() {
#if RF73_HAS_LVGL
  if (!g_ready) return;

  unsigned long now = millis();
  if (now - g_lastUiRefreshMs < 150) return;
  g_lastUiRefreshMs = now;

  if (!lvgl_port_lock(5)) return;

  updateHeaderFooter();
  switch (g_page) {
    case PAGE_LIVE: updateLivePage(); break;
    case PAGE_SENSORS: updateSensorsPage(); break;
    case PAGE_OTA: updateOtaPage(); break;
    case PAGE_OPTIONS: syncOptionsWidgets(); break;
    default: break;
  }

  lvgl_port_unlock();
#endif
}

bool rf73LocalUiIsReady() {
#if RF73_HAS_LVGL
  return g_ready;
#else
  return false;
#endif
}
