#include "web_ui.h"
#include "storage_backend.h"
#include <Update.h>

static volatile bool otaTrafficLock = false;
static unsigned long otaTrafficLockUntilMs = 0;

static void setOtaTrafficLock(unsigned long msFromNow) {
  otaTrafficLock = true;
  otaTrafficLockUntilMs = millis() + msFromNow;
}

static void clearOtaTrafficLock() {
  otaTrafficLock = false;
  otaTrafficLockUntilMs = 0;
}

static bool isOtaTrafficLocked() {
  if (!otaTrafficLock) return false;
  long remain = (long)(otaTrafficLockUntilMs - millis());
  if (remain <= 0) {
    clearOtaTrafficLock();
    return false;
  }
  return true;
}


static String jsEsc(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '\'' || c == '"') out += '\\';
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

static String jsonEsc(const String& s) {
  return jsEsc(s);
}

static bool isSafeBinFileName(const String& name);
static String fileNameToPath(const String& name);
static String buildFwUrl(const String& name);

static bool assignedNodeMatchesFwVersion(int idx, const char* fwVersion) {
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES || !fwVersion || !fwVersion[0]) return false;
  return nodes[idx].fwValid && strncmp(nodes[idx].fwVersion, fwVersion, sizeof(nodes[idx].fwVersion) - 1) == 0;
}

static bool unknownNodeMatchesFwVersion(int idx, const char* fwVersion) {
  if (idx < 0 || idx >= MAX_UNKNOWN_NODES || !fwVersion || !fwVersion[0]) return false;
  return unknownNodes[idx].fwValid && strncmp(unknownNodes[idx].fwVersion, fwVersion, sizeof(unknownNodes[idx].fwVersion) - 1) == 0;
}

static bool batteryTooLow(bool batteryValid, float batterySoc) {
  return batteryValid && (batterySoc < systemOptions.minBatterySoc);
}

static bool validateAssignedNodeOtaPolicy(int idx, const OtaStartPacket& pkt, String& err) {
  if (systemOptions.skipAlreadyUpdated && assignedNodeMatchesFwVersion(idx, pkt.fwVersion)) {
    err = "Nodo gia aggiornato";
    return false;
  }
  if (!nodes[idx].batteryValid && !systemOptions.allowUnknownBattery) {
    err = "Batteria sconosciuta";
    return false;
  }
  if (systemOptions.blockLowBattery && batteryTooLow(nodes[idx].batteryValid, nodes[idx].batterySoc)) {
    err = "Batteria troppo bassa";
    return false;
  }
  return true;
}

static bool validateUnknownNodeOtaPolicy(int idx, const OtaStartPacket& pkt, String& err) {
  if (systemOptions.skipAlreadyUpdated && unknownNodeMatchesFwVersion(idx, pkt.fwVersion)) {
    err = "Nodo gia aggiornato";
    return false;
  }
  if (!unknownNodes[idx].batteryValid && !systemOptions.allowUnknownBattery) {
    err = "Batteria sconosciuta";
    return false;
  }
  if (systemOptions.blockLowBattery && batteryTooLow(unknownNodes[idx].batteryValid, unknownNodes[idx].batterySoc)) {
    err = "Batteria troppo bassa";
    return false;
  }
  return true;
}


struct ParsedFirmwareInfo {
  bool fileExists = false;
  bool fileNameValid = false;
  bool embeddedFound = false;
  bool embeddedValid = false;
  bool embeddedMatchesFile = false;
  bool canUpdateMaster = false;
  bool canUpdateSlave = false;
  size_t size = 0;
  String name;
  String url;
  String fileTarget;
  String fileChip;
  String fileVersion;
  String fileChannel;
  String metaTarget;
  String metaChip;
  String metaVersion;
  String metaChannel;
  String metaProto;
  String target;
  String chip;
  String version;
  String channel;
  String proto;
  String effectiveFwVersion;
  String validation;
};

static String normalizeUpper(const String& s) {
  String out = s;
  out.trim();
  out.toUpperCase();
  return out;
}

static String normalizeLower(const String& s) {
  String out = s;
  out.trim();
  out.toLowerCase();
  return out;
}

static bool parseFirmwareFileName(const String& fileName, ParsedFirmwareInfo& info) {
  String name = fileName;
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  if (!name.endsWith(".bin")) return false;
  if (!name.startsWith("RF73_")) return false;

  String base = name.substring(0, name.length() - 4);
  String rest = base.substring(5);

  int first = rest.indexOf('_');
  if (first <= 0) return false;
  int second = rest.indexOf('_', first + 1);
  if (second <= first + 1) return false;
  int last = rest.lastIndexOf('_');
  if (last <= second + 1 || last >= (int)rest.length() - 1) return false;

  info.fileTarget = normalizeUpper(rest.substring(0, first));
  info.fileChip = normalizeUpper(rest.substring(first + 1, second));
  info.fileVersion = normalizeUpper(rest.substring(second + 1, last));
  info.fileChannel = normalizeLower(rest.substring(last + 1));

  if (!(info.fileTarget == "MASTER" || info.fileTarget == "SLAVE")) return false;
  if (!(info.fileChip == "ESP32S3" || info.fileChip == "ESP32C3")) return false;
  if (info.fileVersion.length() == 0 || info.fileChannel.length() == 0) return false;

  info.fileNameValid = true;
  return true;
}

static bool readEmbeddedMetaString(File& fw, String& metaOut) {
  static const char marker[] = "RF73META|";
  constexpr size_t markerLen = sizeof(marker) - 1;
  constexpr size_t chunkSize = 256;
  uint8_t chunk[chunkSize];
  uint8_t overlap[markerLen - 1] = {};
  size_t overlapLen = 0;

  fw.seek(0);

  while (true) {
    int n = fw.read(chunk, sizeof(chunk));
    if (n <= 0) break;

    uint8_t combined[chunkSize + markerLen];
    size_t total = 0;
    if (overlapLen) {
      memcpy(combined, overlap, overlapLen);
      total += overlapLen;
    }
    memcpy(combined + total, chunk, n);
    total += (size_t)n;

    for (size_t i = 0; i + markerLen <= total; i++) {
      if (memcmp(combined + i, marker, markerLen) != 0) continue;

      char meta[192];
      size_t m = 0;
      bool done = false;

      for (size_t j = i; j < total && m < sizeof(meta) - 1; j++) {
        uint8_t c = combined[j];
        if (c == 0) {
          done = true;
          break;
        }
        if (c < 32 || c > 126) break;
        meta[m++] = (char)c;
      }

      while (!done && fw.available() && m < sizeof(meta) - 1) {
        int c = fw.read();
        if (c < 0) break;
        if (c == 0) {
          done = true;
          break;
        }
        if (c < 32 || c > 126) break;
        meta[m++] = (char)c;
      }

      meta[m] = '\0';
      metaOut = String(meta);
      return metaOut.startsWith("RF73META|");
    }

    overlapLen = min((size_t)(markerLen - 1), total);
    if (overlapLen) memcpy(overlap, combined + total - overlapLen, overlapLen);
  }

  metaOut = "";
  return false;
}

static void parseEmbeddedMetaFields(const String& metaLine, ParsedFirmwareInfo& info) {
  if (!metaLine.startsWith("RF73META|")) return;

  int pos = 9;
  while (pos < (int)metaLine.length()) {
    int sep = metaLine.indexOf('|', pos);
    String token = (sep >= 0) ? metaLine.substring(pos, sep) : metaLine.substring(pos);
    int eq = token.indexOf('=');
    if (eq > 0) {
      String key = normalizeLower(token.substring(0, eq));
      String val = token.substring(eq + 1);
      val.trim();
      if (key == "type") info.metaTarget = normalizeUpper(val);
      else if (key == "chip") info.metaChip = normalizeUpper(val);
      else if (key == "version") info.metaVersion = normalizeUpper(val);
      else if (key == "channel") info.metaChannel = normalizeLower(val);
      else if (key == "proto") info.metaProto = normalizeUpper(val);
    }
    if (sep < 0) break;
    pos = sep + 1;
  }

  info.embeddedValid =
    (info.metaTarget == "MASTER" || info.metaTarget == "SLAVE") &&
    (info.metaChip == "ESP32S3" || info.metaChip == "ESP32C3") &&
    info.metaVersion.length() > 0 &&
    info.metaChannel.length() > 0;
}

static String buildEffectiveFwVersion(const String& version, const String& channel) {
  if (!version.length()) return "";
  if (!channel.length()) return version;
  String suffix = String("_") + channel;
  if (version.endsWith(suffix)) return version;
  return version + suffix;
}

static void finalizeFirmwareInfo(ParsedFirmwareInfo& info) {
  info.target = info.fileTarget;
  info.chip = info.fileChip;
  info.version = info.fileVersion;
  info.channel = info.fileChannel;
  info.proto = String("");
  info.effectiveFwVersion = buildEffectiveFwVersion(info.version, info.channel);

  if (!info.fileNameValid) {
    info.validation = "Nome file non conforme";
    return;
  }

  if (info.effectiveFwVersion.length() == 0) {
    info.validation = "Versione firmware mancante";
    return;
  }

  if (info.effectiveFwVersion.length() > 11) {
    info.validation = "FW target troppo lunga";
    return;
  }

  info.canUpdateMaster = (info.target == "MASTER" && info.chip == "ESP32S3");
  info.canUpdateSlave = (info.target == "SLAVE" && info.chip == "ESP32C3");
  info.validation = "OK nome file";
}


static bool inspectFirmwareFile(const String& fileName, ParsedFirmwareInfo& info) {
  info = ParsedFirmwareInfo();
  info.name = fileName;
  info.url = buildFwUrl(fileName);

  if (!isSafeBinFileName(fileName)) {
    info.validation = "Nome file non valido";
    return false;
  }

  File fw = rf73Storage.open(fileNameToPath(fileName), FILE_READ);
  if (!fw) {
    info.validation = "File non trovato";
    return false;
  }

  info.fileExists = true;
  info.size = fw.size();
  parseFirmwareFileName(fileName, info);

  // Filename-only validation mode: do not scan embedded metadata here.
  // This keeps firmware inspection light and avoids slowing down the Web UI
  // during page load and firmware list refresh.
  fw.close();

  finalizeFirmwareInfo(info);
  return true;
}

static bool buildSlaveOtaPacketFromFile(const String& fileName, OtaStartPacket& pkt, String& err) {
  ParsedFirmwareInfo info;
  if (!inspectFirmwareFile(fileName, info) || !info.fileExists) {
    err = info.validation.length() ? info.validation : String("File firmware non trovato");
    return false;
  }
  if (!info.canUpdateSlave) {
    err = info.validation.length() ? info.validation : String("Firmware non valido per moduli");
    return false;
  }

  memset(&pkt, 0, sizeof(pkt));
  pkt.magic[0] = 'R';
  pkt.magic[1] = 'F';
  pkt.version = 1;
  pkt.type = PKT_OTA_START;
  strncpy(pkt.ssid, AP_SSID, sizeof(pkt.ssid) - 1);
  strncpy(pkt.password, AP_PASS, sizeof(pkt.password) - 1);
  String url = buildFwUrl(fileName);
  url.toCharArray(pkt.url, sizeof(pkt.url));
  pkt.fwSize = (uint32_t)info.size;
  pkt.fwCrc = 0;
  info.effectiveFwVersion.toCharArray(pkt.fwVersion, sizeof(pkt.fwVersion));
  return true;
}

static bool validateMasterFirmwareFile(const String& fileName, ParsedFirmwareInfo& info, String& err) {
  if (!inspectFirmwareFile(fileName, info) || !info.fileExists) {
    err = info.validation.length() ? info.validation : String("File firmware non trovato");
    return false;
  }
  if (!info.canUpdateMaster) {
    err = info.validation.length() ? info.validation : String("Firmware non valido per master");
    return false;
  }
  return true;
}

static bool isSafeBinFileName(const String& name) {
  if (name.length() == 0) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (name.indexOf("..") >= 0) return false;
  if (!name.endsWith(".bin")) return false;
  return true;
}

static String fileNameToPath(const String& name) {
  return String("/") + name;
}

static String buildFwUrl(const String& name) {
  return String("http://192.168.4.1/fwfile?name=") + name;
}

static bool getFirmwareFromSd(const String& fileName, size_t& sizeOut) {
  sizeOut = 0;
  if (!isSafeBinFileName(fileName)) return false;
  File fw = rf73Storage.open(fileNameToPath(fileName), FILE_READ);
  if (!fw) return false;
  sizeOut = fw.size();
  fw.close();
  return true;
}

static String getDefaultFirmwareName() {
  File root = rf73Storage.open("/");
  if (!root) return "";

  while (true) {
    File f = root.openNextFile();
    if (!f) break;

    String name = String(f.name());
    bool isDir = f.isDirectory();
    f.close();

    if (isDir) continue;

    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);

    if (name.endsWith(".bin")) {
      root.close();
      return name;
    }
  }

  root.close();
  return "";
}

static String guessVersionFromFileName(const String& fileName) {
  String v = fileName;
  if (v.endsWith(".bin")) v.remove(v.length() - 4);

  int firstDigit = -1;
  for (int i = 0; i < (int)v.length(); i++) {
    if (isDigit(v[i])) {
      firstDigit = i;
      break;
    }
  }

  if (firstDigit < 0) return "";

  v = v.substring(firstDigit);
  v.replace('_', '.');
  v.replace('-', '.');
  while (v.indexOf("..") >= 0) v.replace("..", ".");
  if (!v.startsWith("v") && !v.startsWith("V")) v = "v" + v;
  return v;
}


static String getFirmwareListJson() {
  String json = "[";
  bool first = true;

  File root = rf73Storage.open("/");
  if (!root) return "[]";

  while (true) {
    File f = root.openNextFile();
    if (!f) break;

    String name = String(f.name());
    bool isDir = f.isDirectory();
    f.close();

    if (isDir) continue;

    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".bin")) continue;

    ParsedFirmwareInfo info;
    inspectFirmwareFile(name, info);

    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"name\":\"" + jsonEsc(name) + "\",";
    json += "\"size\":" + String((unsigned long)info.size) + ",";
    json += "\"url\":\"" + jsonEsc(info.url) + "\",";
    json += "\"target\":\"" + jsonEsc(info.target) + "\",";
    json += "\"chip\":\"" + jsonEsc(info.chip) + "\",";
    json += "\"version\":\"" + jsonEsc(info.version) + "\",";
    json += "\"channel\":\"" + jsonEsc(info.channel) + "\",";
    json += "\"effectiveFwVersion\":\"" + jsonEsc(info.effectiveFwVersion) + "\",";
    json += "\"validation\":\"" + jsonEsc(info.validation) + "\",";
    json += "\"embeddedFound\":" + String(info.embeddedFound ? "true" : "false") + ",";
    json += "\"embeddedMatchesFile\":" + String(info.embeddedMatchesFile ? "true" : "false") + ",";
    json += "\"canSlave\":" + String(info.canUpdateSlave ? "true" : "false") + ",";
    json += "\"canMaster\":" + String(info.canUpdateMaster ? "true" : "false");
    json += "}";
  }

  root.close();
  json += "]";
  return json;
}

String makeHtmlPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>SmartSetUP Master</title>
<style>
:root{
  --bg:#1f2328;
  --panel:#2a2f36;
  --panel2:#313843;
  --text:#f5f7fa;
  --muted:#aeb8c4;
  --green:#22c55e;
  --blue:#2d8cff;
  --blue2:#60a5fa;
  --red:#ef4444;
  --amber:#f59e0b;
  --border:#3b4452;
  --shadow:0 8px 24px rgba(0,0,0,.28);
}
*{box-sizing:border-box}
body{
  margin:0;
  font-family:Arial,Helvetica,sans-serif;
  background:linear-gradient(180deg,#191d21 0%,#1f2328 100%);
  color:var(--text);
}
.wrap{
  max-width:1380px;
  margin:0 auto;
  padding:20px 16px 30px;
}
.topbar{
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:12px;
  flex-wrap:wrap;
  margin-bottom:18px;
}
.title{font-size:30px;font-weight:800}
.subtitle{font-size:14px;color:var(--muted)}
.badge{
  display:inline-flex;align-items:center;gap:8px;
  background:var(--panel);border:1px solid var(--border);
  padding:10px 14px;border-radius:14px;box-shadow:var(--shadow);color:var(--muted)
}
.dot{
  width:10px;height:10px;border-radius:50%;
  background:var(--green);box-shadow:0 0 10px rgba(34,197,94,.7)
}

.nav-tabs{
  display:flex;
  gap:10px;
  flex-wrap:wrap;
  margin-bottom:18px;
}
.nav-btn{
  appearance:none;
  border:1px solid var(--border);
  background:#20252b;
  color:#fff;
  border-radius:12px;
  padding:10px 14px;
  font-size:14px;
  font-weight:700;
  cursor:pointer;
  box-shadow:var(--shadow);
}
.nav-btn.active{
  background:linear-gradient(135deg,var(--blue),var(--blue2));
  border-color:transparent;
}
.nav-btn:disabled{
  opacity:.45;
  cursor:not-allowed;
}

.page{display:none}
.page.active{display:block}

.actions{
  display:flex;
  gap:10px;
  flex-wrap:wrap;
  margin-bottom:16px
}
button{
  appearance:none;border:none;border-radius:12px;
  padding:10px 14px;font-size:14px;font-weight:700;cursor:pointer;
  transition:transform .08s ease,opacity .2s ease;box-shadow:var(--shadow)
}
button:hover{opacity:.95}
button:active{transform:scale(.985)}
button:disabled{opacity:.45;cursor:not-allowed}
.btn-primary{background:linear-gradient(135deg,var(--blue),var(--blue2));color:#fff}
.btn-green{background:linear-gradient(135deg,#16a34a,var(--green));color:#fff}
.btn-dark{background:#20252b;color:#fff;border:1px solid var(--border)}
.btn-amber{background:linear-gradient(135deg,#d97706,var(--amber));color:#fff}
.btn-red{background:linear-gradient(135deg,#dc2626,var(--red));color:#fff}

.section-title{
  margin:8px 0 12px 0;
  font-size:22px;
  font-weight:800;
}

.grid{
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(320px,1fr));
  gap:16px;
}
.card{
  background:linear-gradient(180deg,var(--panel) 0%, var(--panel2) 100%);
  border:1px solid var(--border);
  border-radius:18px;
  padding:16px;
  box-shadow:var(--shadow);
}
.head{
  display:flex;justify-content:space-between;align-items:flex-start;gap:12px;margin-bottom:12px
}
.node-id{font-size:24px;font-weight:800}
.meta-small{font-size:12px;color:var(--muted)}
.live{
  display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px
}
.livebox{
  background:rgba(0,0,0,.12);border:1px solid var(--border);
  border-radius:14px;padding:12px
}
.label{font-size:12px;color:var(--muted);margin-bottom:6px}
.value{font-size:30px;font-weight:800;line-height:1.1}
.status-row{
  display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px
}
.tele-row{
  font-size:13px;
  color:var(--muted);
  margin-bottom:12px;
}
.pill{
  padding:6px 10px;border-radius:999px;font-size:12px;font-weight:800;
  border:1px solid transparent
}
.ok{color:#dcfce7;background:rgba(34,197,94,.16);border-color:rgba(34,197,94,.35)}
.offline{color:#fee2e2;background:rgba(239,68,68,.14);border-color:rgba(239,68,68,.35)}
.busy{color:#fff7ed;background:rgba(245,158,11,.16);border-color:rgba(245,158,11,.35)}
.idle{color:#dbeafe;background:rgba(45,140,255,.14);border-color:rgba(45,140,255,.35)}

.cfg{
  display:grid;grid-template-columns:1fr 1fr;gap:10px
}
.field{
  display:flex;flex-direction:column;gap:6px
}
.field.full{grid-column:1 / -1}
.field label{font-size:12px;color:var(--muted)}
.field input,.field select, textarea{
  width:100%;
  border-radius:10px;border:1px solid var(--border);
  background:#1b2025;color:#fff;padding:10px 10px;font-size:14px
}
.field input:disabled,
.field select:disabled,
textarea:disabled{
  opacity:.55;
  cursor:not-allowed;
}
.info-box{
  width:100%;
  border-radius:10px;
  border:1px solid var(--border);
  background:#1b2025;
  color:#fff;
  padding:10px 12px;
  font-size:14px;
}
.card-actions{
  display:flex;flex-wrap:wrap;gap:8px;margin-top:14px
}

.ota-message{
  font-size:13px;
  color:var(--text);
  background:rgba(0,0,0,.12);
  border:1px solid var(--border);
  border-radius:10px;
  padding:10px 12px;
  margin-top:10px;
  min-height:40px;
}

.progress-wrap{
  width:100%;
  height:10px;
  border-radius:999px;
  background:rgba(0,0,0,.18);
  border:1px solid var(--border);
  overflow:hidden;
  margin-top:10px;
}
.progress-bar{
  height:100%;
  width:0%;
  border-radius:999px;
  background:linear-gradient(135deg,var(--blue),var(--blue2));
  transition:width .12s linear;
}
.progress-bar.done{
  background:linear-gradient(135deg,#16a34a,var(--green));
}
.progress-bar.fail{
  background:linear-gradient(135deg,#dc2626,var(--red));
}

.small-list{
  font-size:13px;
  line-height:1.5;
  white-space:pre-wrap;
  background:#1b2025;
  border:1px solid var(--border);
  border-radius:10px;
  padding:12px;
}

.safe-lock{
  border:1px solid rgba(245,158,11,.35);
  background:rgba(245,158,11,.10);
  color:#fff7ed;
  border-radius:12px;
  padding:12px 14px;
  margin-bottom:14px;
  display:none;
}
.safe-lock.active{
  display:block;
}

.mode-switch{
  display:flex;
  gap:10px;
  flex-wrap:wrap;
  margin-bottom:14px;
}
.mode-btn{
  appearance:none;
  border:1px solid var(--border);
  background:#20252b;
  color:#fff;
  border-radius:12px;
  padding:10px 14px;
  font-size:14px;
  font-weight:700;
  cursor:pointer;
  box-shadow:var(--shadow);
}
.mode-btn.active{
  background:linear-gradient(135deg,var(--blue),var(--blue2));
  border-color:transparent;
}
.mode-btn:disabled{
  opacity:.45;
  cursor:not-allowed;
}

.manual-panel{display:none}
.manual-panel.active{display:block}

.live-board{
  position:relative;
  width:100%;
  max-width:980px;
  margin:0 auto;
  height:760px;
}
.measure-tile{
  position:absolute;
  width:220px;
  height:220px;
  border-radius:22px;
  padding:18px;
  box-shadow:var(--shadow);
  border:1px solid rgba(255,255,255,.08);
  display:flex;
  flex-direction:column;
  justify-content:space-between;
}
.measure-label{
  font-size:28px;
  font-weight:800;
  letter-spacing:.5px;
}
.measure-value{
  font-size:64px;
  font-weight:900;
  line-height:1;
}
.measure-sub{
  font-size:14px;
  font-weight:700;
  opacity:.92;
}
.tile-green{
  background:linear-gradient(180deg, rgba(34,197,94,.95) 0%, rgba(22,163,74,.95) 100%);
  color:#ffffff;
}
.tile-yellow{
  background:linear-gradient(180deg, rgba(245,158,11,.95) 0%, rgba(217,119,6,.95) 100%);
  color:#ffffff;
}
.tile-red{
  background:linear-gradient(180deg, rgba(239,68,68,.95) 0%, rgba(220,38,38,.95) 100%);
  color:#ffffff;
}
.tile-fl{ top:0; left:0; }
.tile-fr{ top:0; right:0; }
.tile-st{ top:270px; left:50%; transform:translateX(-50%); }
.tile-rl{ bottom:0; left:0; }
.tile-rr{ bottom:0; right:0; }

.footer{
  margin-top:18px;
  font-size:13px;
  color:var(--muted);
  text-align:center
}
.toast{
  position:fixed;right:16px;bottom:16px;
  background:#101317;border:1px solid var(--border);border-radius:12px;
  padding:12px 14px;color:var(--text);box-shadow:var(--shadow);
  opacity:0;transform:translateY(12px);transition:.2s ease;pointer-events:none
}
.toast.show{opacity:1;transform:translateY(0)}

@media (max-width: 1100px){
  .live-board{
    max-width:760px;
    height:620px;
  }
  .measure-tile{
    width:180px;
    height:180px;
  }
  .tile-st{
    top:220px;
  }
  .measure-value{
    font-size:52px;
  }
}

@media (max-width: 760px){
  .live-board{
    position:static;
    max-width:none;
    height:auto;
    display:grid;
    grid-template-columns:1fr 1fr;
    gap:14px;
    justify-items:center;
    align-items:center;
  }

  .measure-tile{
    position:static;
    width:min(40vw, 180px);
    height:min(40vw, 180px);
    margin:0;
    transform:none !important;
  }

  .tile-fl{ grid-column:1; grid-row:1; }
  .tile-fr{ grid-column:2; grid-row:1; }
  .tile-st{ grid-column:1 / span 2; grid-row:2; }
  .tile-rl{ grid-column:1; grid-row:3; }
  .tile-rr{ grid-column:2; grid-row:3; }

  .measure-value{ font-size:42px; }
  .measure-label{ font-size:24px; }

  .cfg{
    grid-template-columns:1fr;
  }
}
</style>
</head>
<body>
<div class="wrap">
  <div class="topbar">
    <div>
      <div class="title">RaceFab73 SmartSetUP</div>
      <div class="subtitle">Master FW: __MASTER_FW_VERSION__</div>
    </div>
    <div class="badge">
      <span class="dot"></span>
      <span>AP 192.168.4.1</span>
    </div>
  </div>

  <div id="safeLockBanner" class="safe-lock">
    OTA in corso: alcune azioni sono temporaneamente bloccate per sicurezza.
  </div>

  <div class="nav-tabs">
    <button id="btnLive" class="nav-btn active" onclick="showPage('live')">Live Measure</button>
    <button id="btnConfig" class="nav-btn" onclick="showPage('config')">Sensors Config</button>
    <button id="btnOta" class="nav-btn" onclick="showPage('ota')">Firmware Update</button>
    <button id="btnOptions" class="nav-btn" onclick="showPage('options')">Opzioni</button>
  </div>

  <div id="page-live" class="page active">
    <div class="live-board">
      <div id="tile-FL" class="measure-tile tile-fl tile-red"></div>
      <div id="tile-FR" class="measure-tile tile-fr tile-red"></div>
      <div id="tile-ST" class="measure-tile tile-st tile-red"></div>
      <div id="tile-RL" class="measure-tile tile-rl tile-red"></div>
      <div id="tile-RR" class="measure-tile tile-rr tile-red"></div>
    </div>
  </div>

  <div id="page-config" class="page">
    <div class="actions">
      <button id="btnZeroAll" class="btn-green" onclick="zeroAll()">ZERO ALL</button>
      <button class="btn-primary" onclick="refreshNow()">AGGIORNA</button>
    </div>

    <div class="section-title">Nodi assegnati</div>
    <div id="grid" class="grid"></div>

    <div style="height:18px"></div>
    <div class="section-title">Nodi sconosciuti</div>
    <div id="unknownGrid" class="grid"></div>
  </div>

  <div id="page-ota" class="page">
    <div class="actions">
      <button class="btn-primary" onclick="refreshNow()">AGGIORNA</button>
    </div>

    <div class="section-title">Parametri OTA</div>
    <div class="card">
      <div class="cfg">
        <div class="field">
          <label>Rete OTA</label>
          <div class="info-box" id="ota-net">__AP_SSID__</div>
        </div>
        <div class="field">
          <label>Stato SD</label>
          <div class="info-box" id="fw-status">checking...</div>
        </div>
        <div class="field full">
          <label>Firmware file su SD</label>
          <select id="fw-file-select" onchange="onFirmwareFileChanged()">
            <option value="">-- nessun file --</option>
          </select>
        </div>
        <div class="field full">
          <label>Firmware URL</label>
          <input id="ota-url" type="text" readonly value="">
        </div>
        <div class="field">
          <label>Firmware Version target</label>
          <input id="ota-fwVersion" type="text" readonly value="">
        </div>
        <div class="field">
          <label>Target file</label>
          <div class="info-box" id="ota-target">--</div>
        </div>
        <div class="field full">
          <label>Coerenza file / destinazione</label>
          <div class="info-box" id="ota-compat">Nessun file</div>
        </div>
        <div class="field full">
          <label>File selezionato</label>
          <div class="info-box" id="ota-file">Nessun file</div>
        </div>
      </div>
    </div>

    <div style="height:18px"></div>
    <div class="section-title">Aggiornamento moduli</div>
    <div class="card">
      <div class="mode-switch">
        <button id="btnAutoMode" class="mode-btn active" onclick="setUpdateMode('auto')">AGGIORNAMENTO AUTOMATICO</button>
        <button id="btnManualMode" class="mode-btn" onclick="setUpdateMode('manual')">AGGIORNAMENTO MANUALE</button>
      </div>

      <div id="autoModePanel">
        <div class="card-actions" style="margin-top:0;margin-bottom:12px;">
          <button id="btnReloadFwList" class="btn-dark" onclick="loadFwList(true)">RICARICA FILE SD</button>
          <button id="btnBatchOta" class="btn-amber" onclick="startBatchOta()">AVVIA AGGIORNAMENTO</button>
        </div>

        <div class="ota-message" id="batch-ota-msg">Nessuna attività batch</div>
        <div class="small-list" id="batch-ota-log">In attesa...</div>
      </div>

      <div id="manualModePanel" class="manual-panel">
        <div class="section-title" style="margin-top:4px;">Nodi assegnati</div>
        <div id="otaGrid" class="grid"></div>

        <div style="height:18px"></div>
        <div class="section-title">Nodi sconosciuti</div>
        <div id="otaUnknownGrid" class="grid"></div>
      </div>
    </div>

    <div style="height:18px"></div>
    <div class="section-title">Aggiornamento master</div>
    <div class="card">
      <div class="tele-row" id="master-ota-file-info">Seleziona un firmware .bin dalla SD per aggiornare il master.</div>
      <div class="ota-message" id="master-ota-msg">Nessuna attività OTA master</div>
      <div class="card-actions">
        <button id="btnMasterOta" class="btn-red" onclick="startMasterOta()">AGGIORNA MASTER</button>
        <button class="btn-dark" onclick="openFirmwareUrl()">APRI FILE SELEZIONATO</button>
      </div>
    </div>
  </div>


  <div id="page-options" class="page">
    <div class="actions">
      <button id="btnSaveOptions" class="btn-primary" onclick="saveSystemOptions()">SALVA OPZIONI</button>
      <button class="btn-dark" onclick="fetchData();showToast('Opzioni ricaricate')">RICARICA</button>
    </div>

    <div class="section-title">Opzioni sistema</div>
    <div class="card">
      <div class="cfg">
        <div class="field full">
          <label><input data-option-input="1" id="opt-skip-same" type="checkbox" onchange="updateOptionsUi()"> Salta moduli già aggiornati</label>
        </div>
        <div class="field full">
          <label><input data-option-input="1" id="opt-allow-unknown-batt" type="checkbox" onchange="updateOptionsUi()"> Consenti update con batteria sconosciuta</label>
        </div>
        <div class="field full">
          <label><input data-option-input="1" id="opt-block-low-batt" type="checkbox" onchange="updateOptionsUi()"> Blocca update con batteria bassa</label>
        </div>
        <div class="field">
          <label>Soglia batteria minima [%]</label>
          <input data-option-input="1" id="opt-min-batt-soc" type="number" min="0" max="100" step="1" value="30" onchange="updateOptionsUi()">
        </div>
        <div class="field full">
          <label><input data-option-input="1" id="opt-verbose-ota" type="checkbox" onchange="updateOptionsUi()"> Log OTA dettagliato</label>
        </div>
        <div class="field full">
          <label>Comportamento attuale</label>
          <div class="info-box" id="options-summary">In attesa dati...</div>
        </div>
      </div>
    </div>
  </div>

  <div class="footer">
    Aggiornamento automatico preferenziale · aggiornamento manuale separato · lock sicurezza durante OTA
  </div>
</div>

<div id="toast" class="toast">OK</div>

<script>
const OTA_SSID = '__AP_SSID__';
const OTA_PASS = '__AP_PASS__';

let nodeData = [];
let rendered = false;
const unknownRoleSelections = {};
let unknownUiLocked = false;
let unknownUiUnlockAt = 0;
let currentPage = 'live';
let updateMode = 'auto';
let fwListCache = [];
let masterOtaBusy = false;
let batchOtaBusy = false;
let batchStartPending = false;
let batchLogLines = [];
let batchOtaState = null;
let systemOptionsData = null;

let pollingPausedUntil = 0;
let currentPollTimer = null;

const POLL_LIVE_MS = 220;
const POLL_NORMAL_MS = 1000;
const POLL_BATCH_MS = 500;
const POLL_MASTER_OTA_MS = 2500;

function showToast(msg){
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(()=>t.classList.remove('show'), 1300);
}

function pausePolling(ms){
  pollingPausedUntil = Math.max(pollingPausedUntil, Date.now() + ms);
}

function getPollInterval(){
  if (masterOtaBusy) return POLL_MASTER_OTA_MS;
  if (batchOtaBusy || batchStartPending || anySlaveOtaActive()) return POLL_BATCH_MS;
  if (currentPage === 'live') return POLL_LIVE_MS;
  return POLL_NORMAL_MS;
}

function restartPolling(){
  if (currentPollTimer) clearInterval(currentPollTimer);
  currentPollTimer = setInterval(async () => {
    if (Date.now() < pollingPausedUntil) return;
    if (masterOtaBusy) return;
    await fetchData();
  }, getPollInterval());
}

function addBatchLog(line){
  batchLogLines.push(line);
  if(batchLogLines.length > 120) batchLogLines.shift();
  const el = document.getElementById('batch-ota-log');
  if(el) el.textContent = batchLogLines.join('\n');
}

function showPage(page){
  currentPage = page;
  document.getElementById('page-live').classList.toggle('active', page === 'live');
  document.getElementById('page-config').classList.toggle('active', page === 'config');
  document.getElementById('page-ota').classList.toggle('active', page === 'ota');
  document.getElementById('page-options').classList.toggle('active', page === 'options');
  document.getElementById('btnLive').classList.toggle('active', page === 'live');
  document.getElementById('btnConfig').classList.toggle('active', page === 'config');
  document.getElementById('btnOta').classList.toggle('active', page === 'ota');
  document.getElementById('btnOptions').classList.toggle('active', page === 'options');

  if(page === 'ota' && !fwListCache.length){
    loadFwList(false);
  }

  restartPolling();
}

function setUpdateMode(mode){
  updateMode = mode;
  document.getElementById('btnAutoMode').classList.toggle('active', mode === 'auto');
  document.getElementById('btnManualMode').classList.toggle('active', mode === 'manual');
  document.getElementById('autoModePanel').style.display = mode === 'auto' ? 'block' : 'none';
  document.getElementById('manualModePanel').classList.toggle('active', mode === 'manual');
}

function fmtNumber(v, digits=1){
  return Number(v).toFixed(digits);
}

function anySlaveOtaActive(){
  return (nodeData || []).some(n => n.ota && n.ota.active);
}

function isUiLockedForOta(){
  return batchOtaBusy || anySlaveOtaActive() || masterOtaBusy;
}

function getSystemOptions(){
  return systemOptionsData || {
    skipAlreadyUpdated:false,
    allowUnknownBattery:true,
    blockLowBattery:false,
    verboseOtaLog:false,
    minBatterySoc:30
  };
}

function otaPolicyBlockReason(n){
  const fw = selectedFw();
  if(!fw || !fw.canSlave) return '';
  const opt = getSystemOptions();
  if(opt.skipAlreadyUpdated && n && n.fwValid && fw.effectiveFwVersion && String(n.fwVersion) === String(fw.effectiveFwVersion)) return 'GIÀ OK';
  if((!n || !n.batteryValid) && !opt.allowUnknownBattery) return 'BAT ?';
  if(opt.blockLowBattery && n && n.batteryValid && Number(n.batterySoc) < Number(opt.minBatterySoc || 0)) return 'BAT LOW';
  return '';
}

function optionsSummaryText(o){
  if(!o) return 'In attesa dati...';
  const parts = [];
  parts.push(o.skipAlreadyUpdated ? 'Skip stessi FW ON' : 'Skip stessi FW OFF');
  parts.push(o.allowUnknownBattery ? 'Batteria sconosciuta consentita' : 'Batteria sconosciuta bloccante');
  parts.push(o.blockLowBattery ? `Batteria bassa bloccante sotto ${Number(o.minBatterySoc).toFixed(0)}%` : 'Batteria bassa non bloccante');
  parts.push(o.verboseOtaLog ? 'Log OTA dettagliato' : 'Log OTA standard');
  return parts.join(' · ');
}

function updateOptionsUi(){
  const blockLow = document.getElementById('opt-block-low-batt')?.checked;
  const minSoc = document.getElementById('opt-min-batt-soc');
  if(minSoc) minSoc.disabled = !blockLow;

  const summary = document.getElementById('options-summary');
  if(summary){
    summary.textContent = optionsSummaryText({
      skipAlreadyUpdated: document.getElementById('opt-skip-same')?.checked,
      allowUnknownBattery: document.getElementById('opt-allow-unknown-batt')?.checked,
      blockLowBattery: !!blockLow,
      verboseOtaLog: document.getElementById('opt-verbose-ota')?.checked,
      minBatterySoc: Number(minSoc?.value || 0)
    });
  }
}

function renderSystemOptions(data){
  const o = data.options || null;
  if(!o) return;
  systemOptionsData = o;

  const skipSame = document.getElementById('opt-skip-same');
  const allowUnknown = document.getElementById('opt-allow-unknown-batt');
  const blockLow = document.getElementById('opt-block-low-batt');
  const verbose = document.getElementById('opt-verbose-ota');
  const minSoc = document.getElementById('opt-min-batt-soc');

  if(skipSame) skipSame.checked = !!o.skipAlreadyUpdated;
  if(allowUnknown) allowUnknown.checked = !!o.allowUnknownBattery;
  if(blockLow) blockLow.checked = !!o.blockLowBattery;
  if(verbose) verbose.checked = !!o.verboseOtaLog;
  if(minSoc) minSoc.value = Number(o.minBatterySoc || 0).toFixed(0);

  updateOptionsUi();
}

async function saveSystemOptions(){
  if (!requireUnlockedUi('salva opzioni')) return;

  const form = new URLSearchParams();
  form.set('skipAlreadyUpdated', document.getElementById('opt-skip-same')?.checked ? '1' : '0');
  form.set('allowUnknownBattery', document.getElementById('opt-allow-unknown-batt')?.checked ? '1' : '0');
  form.set('blockLowBattery', document.getElementById('opt-block-low-batt')?.checked ? '1' : '0');
  form.set('verboseOtaLog', document.getElementById('opt-verbose-ota')?.checked ? '1' : '0');
  form.set('minBatterySoc', document.getElementById('opt-min-batt-soc')?.value || '0');

  try{
    const res = await fetch('/setOptions', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });
    const txt = await res.text();
    if(!res.ok){
      showToast(txt || 'Errore salvataggio opzioni');
      return;
    }
    showToast(txt || 'Opzioni salvate');
    await fetchData();
  }catch(e){
    showToast('Errore salvataggio opzioni');
  }
}


function fwCanSlave(fw){
  return !!(fw && fw.canSlave);
}

function fwCanMaster(fw){
  return !!(fw && fw.canMaster);
}

function updateOtaSafeUiState(){
  const locked = isUiLockedForOta();
  const banner = document.getElementById('safeLockBanner');
  if (banner) banner.classList.toggle('active', locked);

  const fw = selectedFw();
  const fwSel = document.getElementById('fw-file-select');
  const fwVer = document.getElementById('ota-fwVersion');
  const btnReloadFw = document.getElementById('btnReloadFwList');
  const btnBatch = document.getElementById('btnBatchOta');
  const btnMaster = document.getElementById('btnMasterOta');
  const btnZeroAll = document.getElementById('btnZeroAll');
  const btnAutoMode = document.getElementById('btnAutoMode');
  const btnManualMode = document.getElementById('btnManualMode');

  if (fwSel) fwSel.disabled = locked;
  if (fwVer) fwVer.disabled = true;
  if (btnReloadFw) btnReloadFw.disabled = locked;
  if (btnBatch) btnBatch.disabled = locked || !fwCanSlave(fw) || updateMode !== 'auto';
  if (btnMaster) btnMaster.disabled = locked || !fwCanMaster(fw);
  if (btnZeroAll) btnZeroAll.disabled = locked;
  if (btnAutoMode) btnAutoMode.disabled = locked;
  if (btnManualMode) btnManualMode.disabled = locked;

  document.querySelectorAll('[data-cfg-action="1"]').forEach(el => el.disabled = locked);
  document.querySelectorAll('[data-zero-action="1"]').forEach(el => el.disabled = locked);
  document.querySelectorAll('[data-assign-action="1"]').forEach(el => el.disabled = locked);
  document.querySelectorAll('[data-single-ota-action="1"]').forEach(el => el.disabled = locked || !fwCanSlave(fw));
  document.querySelectorAll('[data-option-input="1"]').forEach(el => {
    const disableForThreshold = (el.id === 'opt-min-batt-soc') && !document.getElementById('opt-block-low-batt')?.checked;
    el.disabled = locked || disableForThreshold;
  });
  const btnSaveOptions = document.getElementById('btnSaveOptions');
  if (btnSaveOptions) btnSaveOptions.disabled = locked;
}

function requireUnlockedUi(actionName){
  if (isUiLockedForOta()){
    showToast(`Azione bloccata durante OTA: ${actionName}`);
    return false;
  }
  return true;
}

function statusClass(n){
  if(!n.online) return 'offline';
  if(n.pendingAck) return 'busy';
  if(n.lastAckOk) return 'ok';
  return 'idle';
}

function statusText(n){
  if(!n.online) return 'OFFLINE';
  if(n.pendingAck) return 'WAIT ACK';
  return n.txStatus || 'LIVE';
}

function tileClassForNode(n){
  if(!n || !n.online) return 'tile-red';
  return n.stable ? 'tile-green' : 'tile-yellow';
}

function tileStatusText(n){
  if(!n || !n.online) return 'OFFLINE';
  return n.stable ? 'STABLE' : 'MOVING';
}

function renderMeasureTile(elementId, label, node){
  const el = document.getElementById(elementId);
  if(!el) return;

  el.className =
    'measure-tile ' +
    (elementId === 'tile-FL' ? 'tile-fl ' : '') +
    (elementId === 'tile-FR' ? 'tile-fr ' : '') +
    (elementId === 'tile-ST' ? 'tile-st ' : '') +
    (elementId === 'tile-RL' ? 'tile-rl ' : '') +
    (elementId === 'tile-RR' ? 'tile-rr ' : '') +
    tileClassForNode(node);

  const valueText = (!node || !node.online)
    ? '--.-°'
    : `${fmtNumber(node.camber,1)}°`;

  el.innerHTML = `
    <div class="measure-label">${label}</div>
    <div class="measure-value">${valueText}</div>
    <div class="measure-sub">${tileStatusText(node)}</div>
  `;
}

function renderLiveMeasure(data){
  const getNode = (id) => (data.nodes || []).find(x => x.id === id) || null;
  renderMeasureTile('tile-FL', 'FL', getNode('FL'));
  renderMeasureTile('tile-FR', 'FR', getNode('FR'));
  renderMeasureTile('tile-ST', 'ST', getNode('ST'));
  renderMeasureTile('tile-RL', 'RL', getNode('RL'));
  renderMeasureTile('tile-RR', 'RR', getNode('RR'));
}

function lockUnknownUi(ms = 2500){
  unknownUiLocked = true;
  unknownUiUnlockAt = Date.now() + ms;
}

function unlockUnknownUi(){
  unknownUiLocked = false;
  unknownUiUnlockAt = 0;
}

function refreshUnknownUiLock(){
  if(unknownUiLocked && Date.now() > unknownUiUnlockAt){
    unlockUnknownUi();
  }
}

function batteryText(n){
  if(!n || !n.batteryValid) return 'BAT --';
  return `BAT ${Number(n.batterySoc).toFixed(0)}% · ${Number(n.batteryVoltage).toFixed(2)}V`;
}

function toeText(n){
  if(!n || !n.toeValid) return 'TOE --';
  return `TOE ${Number(n.toe).toFixed(2)}°`;
}

function fwText(n){
  if(!n || !n.fwValid) return 'FW --';
  return `FW ${n.fwVersion}`;
}

function otaStageText(stage){
  switch(Number(stage || 0)){
    case 0: return 'READY';
    case 1: return 'WIFI';
    case 2: return 'HTTP';
    case 3: return 'FLASH';
    case 4: return 'FINALIZE';
    default: return `STAGE ${stage}`;
  }
}

function otaIsBusy(n){
  return !!(n && n.ota && n.ota.active);
}

function otaIsSuccess(n){
  if(!n || !n.ota) return false;
  return !n.ota.active && !!n.ota.lastOk;
}

function otaIsFail(n){
  if(!n || !n.ota) return false;
  if(n.ota.active) return false;
  const msg = String(n.ota.message || '').toLowerCase();
  return (!n.ota.lastOk && msg.length > 0);
}

function otaResultClass(n){
  if(otaIsBusy(n)) return 'busy';
  if(otaIsSuccess(n)) return 'ok';
  if(otaIsFail(n)) return 'offline';
  return 'idle';
}

function otaResultText(n){
  if(!n || !n.ota) return 'IDLE';
  if(n.ota.active) return `${otaStageText(n.ota.stage)} · ${Number(n.ota.progress || 0)}%`;
  if(n.ota.lastOk) return 'SUCCESS';
  if(n.ota.message && n.ota.message.length) return 'FAILED';
  return 'IDLE';
}

function otaMessageText(n){
  if(!n || !n.ota) return 'Nessuna attività OTA';
  if(n.ota.active){
    return `Aggiornamento in corso: ${otaStageText(n.ota.stage)} (${Number(n.ota.progress || 0)}%)`;
  }
  if(n.ota.message && n.ota.message.length) return n.ota.message;
  if(n.ota.lastOk) return 'Aggiornamento completato';
  return 'Nessuna attività OTA';
}

function otaProgressValue(n){
  if(!n || !n.ota) return 0;
  let p = Number(n.ota.progress || 0);
  if (n.ota.lastOk && !n.ota.active) p = 100;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

function otaProgressClass(n){
  if(otaIsSuccess(n)) return 'progress-bar done';
  if(otaIsFail(n)) return 'progress-bar fail';
  return 'progress-bar';
}


function selectedFw(){
  const sel = document.getElementById('fw-file-select');
  const name = sel ? sel.value : '';
  return fwListCache.find(x => x.name === name) || null;
}

function firmwareTargetTextJs(fw){
  if(!fw) return '--';
  const parts = [];
  if(fw.target) parts.push(fw.target);
  if(fw.chip) parts.push(fw.chip);
  if(fw.channel) parts.push(fw.channel);
  return parts.length ? parts.join(' · ') : '--';
}

function firmwareCompatibilityTextJs(fw){
  if(!fw) return 'Nessun file';
  const parts = [];
  if(fw.validation) parts.push(fw.validation);
  if(fw.canSlave) parts.push('MODULI OK');
  if(fw.canMaster) parts.push('MASTER OK');
  return parts.join(' · ');
}


function updateSelectedFirmwareUi(){
  const fw = selectedFw();
  const fileBox = document.getElementById('ota-file');
  const urlEl = document.getElementById('ota-url');
  const verEl = document.getElementById('ota-fwVersion');
  const targetEl = document.getElementById('ota-target');
  const compatEl = document.getElementById('ota-compat');
  const masterInfo = document.getElementById('master-ota-file-info');

  if(!fw){
    if(fileBox) fileBox.textContent = 'Nessun file';
    if(urlEl) urlEl.value = '';
    if(verEl) verEl.value = '';
    if(targetEl) targetEl.textContent = '--';
    if(compatEl) compatEl.textContent = 'Nessun file';
    if(masterInfo) masterInfo.textContent = 'Seleziona un firmware .bin dalla SD per aggiornare il master.';
    updateOtaSafeUiState();
    return;
  }

  if(fileBox) fileBox.textContent = `${fw.name} · ${fw.size} bytes`;
  if(urlEl) urlEl.value = fw.url || '';
  if(verEl) verEl.value = fw.effectiveFwVersion || '';
  if(targetEl) targetEl.textContent = firmwareTargetTextJs(fw);
  if(compatEl) compatEl.textContent = firmwareCompatibilityTextJs(fw);
  if(masterInfo) masterInfo.textContent = `File selezionato: ${fw.name} · ${fw.size} bytes · ${firmwareCompatibilityTextJs(fw)}`;
  updateOtaSafeUiState();
}


function onFirmwareFileChanged(){
  if (!requireUnlockedUi('selezione firmware')) return;
  updateSelectedFirmwareUi();
}

async function loadFwList(showMsg=false){
  if (!requireUnlockedUi('ricarica file SD')) return;

  try{
    const res = await fetch('/fwlist', { cache: 'no-store' });
    if (!res.ok) return;
    const arr = await res.json();
    fwListCache = Array.isArray(arr) ? arr : [];

    const sel = document.getElementById('fw-file-select');
    const current = sel ? sel.value : '';
    if(sel){
      sel.innerHTML = '';
      const empty = document.createElement('option');
      empty.value = '';
      empty.textContent = fwListCache.length ? '-- seleziona file --' : '-- nessun file .bin --';
      sel.appendChild(empty);

      fwListCache.forEach(fw=>{
        const opt = document.createElement('option');
        opt.value = fw.name;
        opt.textContent = `${fw.name} (${fw.size} B)`;
        sel.appendChild(opt);
      });

      if(fwListCache.some(x => x.name === current)) sel.value = current;
      else if(fwListCache.length === 1) sel.value = fwListCache[0].name;
    }

    updateSelectedFirmwareUi();
    if(showMsg) showToast('Lista firmware aggiornata');
  }catch(e){
    if(showMsg) showToast('Errore lettura file SD');
  }
}


function updateFirmwareInfo(data){
  const box = document.getElementById('fw-status');
  if(!box) return;

  if(data.firmware && data.firmware.exists){
    box.textContent = `PRESENTE · ${data.firmware.path} · ${data.firmware.size} bytes`;
  }else{
    box.textContent = 'Nessun .bin selezionato / nessun file valido su SD';
  }
}


function openFirmwareUrl(){
  const url = document.getElementById('ota-url')?.value?.trim() || '';
  if(!url){
    showToast('Nessun file selezionato');
    return;
  }
  window.open(url, '_blank');
}

function setMasterOtaBusy(busy, msg=''){
  masterOtaBusy = busy;
  const box = document.getElementById('master-ota-msg');
  if(box && msg) box.textContent = msg;
  updateOtaSafeUiState();
  restartPolling();
}

function setBatchOtaBusy(busy, msg=''){
  batchOtaBusy = busy;
  const box = document.getElementById('batch-ota-msg');
  if(box && msg) box.textContent = msg;
  updateOtaSafeUiState();
  restartPolling();
}

async function waitForMasterBackOnline() {
  const box = document.getElementById('master-ota-msg');
  const started = Date.now();
  let sawOffline = false;

  if (box) box.textContent = 'Attendo il riavvio del master...';

  while (Date.now() - started < 60000) {
    await new Promise(r => setTimeout(r, 2000));

    try {
      const res = await fetch('/firmwareInfo', { cache: 'no-store' });
      if (res.ok) {
        if (box) {
          box.textContent = sawOffline
            ? 'Master tornato online.'
            : 'Master raggiungibile. Aggiornamento concluso.';
        }
        setMasterOtaBusy(false, sawOffline
          ? 'Master aggiornato e tornato online.'
          : 'Master raggiungibile. Aggiornamento concluso.');
        await fetchData();
        showToast('Master online');
        return true;
      }
    } catch (e) {
      sawOffline = true;
      if (box) box.textContent = 'Master in riavvio... attendo riconnessione';
    }
  }

  setMasterOtaBusy(false, 'Timeout attesa master. Se necessario ricarica la pagina.');
  showToast('Timeout attesa master');
  return false;
}

function deriveBatchResultStatus(n){
  if(!n || !n.ota) return 'NO STATE';

  if(n.ota.active) return 'IN PROGRESS';
  if(n.ota.lastOk) return 'SUCCESS';

  const msg = String(n.ota.message || '').trim();
  if (!msg.length) return 'NO RESULT';

  const up = msg.toUpperCase();
  if (up.includes('HTTP')) return 'HTTP FAIL';
  if (up.includes('TIMEOUT')) return 'TIMEOUT';
  if (up.includes('FAIL')) return 'RESULT FAIL';
  if (up.includes('ERR')) return 'RESULT ERR';
  if (up.includes('ABORT')) return 'ABORTED';
  return msg;
}

async function waitForNodeOtaCompletion(nodeId, targetFw, timeoutMs=90000){
  const started = Date.now();
  let sawActive = false;
  let sawOfflineAfterActive = false;

  while(Date.now() - started < timeoutMs){
    await new Promise(r => setTimeout(r, 350));
    await fetchData();

    const n = nodeData.find(x => x.id === nodeId);
    if(!n) continue;

    if (n.ota && n.ota.active) {
      sawActive = true;
      continue;
    }

    if (sawActive && !n.online) {
      sawOfflineAfterActive = true;
      continue;
    }

    const fwMatches = !!targetFw && n.fwValid && String(n.fwVersion).trim() === String(targetFw).trim();

    if (fwMatches && (sawActive || sawOfflineAfterActive || n.online)) {
      return { ok:true, status:'SUCCESS' };
    }

    if (n.ota) {
      if (n.ota.lastOk) {
        return { ok:true, status:'SUCCESS' };
      }

      const derived = deriveBatchResultStatus(n);
      if (sawActive && derived !== 'NO RESULT' && derived !== 'NO STATE' && derived !== 'IDLE') {
        return { ok:false, status:derived };
      }
    }

    if (sawOfflineAfterActive && n.online) {
      if (fwMatches) {
        return { ok:true, status:'SUCCESS' };
      }
    }
  }

  return { ok:false, status:'TIMEOUT' };
}

function renderCards(data){
  const grid = document.getElementById('grid');
  grid.innerHTML = '';
  const locked = isUiLockedForOta();

  (data.nodes || []).forEach((n, idx)=>{
    const card = document.createElement('div');
    card.className = 'card';
    card.id = `card-${idx}`;
    card.innerHTML = `
      <div class="head">
        <div>
          <div class="node-id" id="nodeTitle-${idx}">${n.id}</div>
          <div class="meta-small" id="nodeMac-${idx}">${n.macKnown ? n.mac : 'MAC sconosciuto'}</div>
        </div>
        <div class="meta-small" id="nodeSeen-${idx}">${n.online ? 'ONLINE' : 'OFFLINE'}</div>
      </div>

      <div class="live">
        <div class="livebox">
          <div class="label">Camber</div>
          <div class="value" id="camber-${idx}">${n.online ? fmtNumber(n.camber,1)+'°' : '--.-°'}</div>
        </div>
        <div class="livebox">
          <div class="label">Z</div>
          <div class="value" id="z-${idx}">${n.online ? fmtNumber(n.z,1)+'°' : '--.-°'}</div>
        </div>
      </div>

      <div class="status-row">
        <span class="pill ${n.stable ? 'ok':'idle'}" id="stable-${idx}">${n.online ? (n.stable ? 'STABLE':'MOVING') : 'NO DATA'}</span>
        <span class="pill ${statusClass(n)}" id="tx-${idx}">${statusText(n)}</span>
      </div>

      <div class="tele-row" id="tele-${idx}">
        ${batteryText(n)} · ${toeText(n)} · ${fwText(n)}
      </div>

      <div class="cfg">
        <div class="field">
          <label>alpha</label>
          <input id="alpha-${idx}" type="number" step="0.01" min="0" max="1" value="${n.cfg.alpha}" ${locked ? 'disabled' : ''}>
        </div>
        <div class="field">
          <label>sampleCount</label>
          <input id="sampleCount-${idx}" type="number" step="1" min="1" max="200" value="${n.cfg.sampleCount}" ${locked ? 'disabled' : ''}>
        </div>
        <div class="field">
          <label>stabilityThreshold</label>
          <input id="stabilityThreshold-${idx}" type="number" step="0.01" min="0.001" max="20" value="${n.cfg.stabilityThreshold}" ${locked ? 'disabled' : ''}>
        </div>
        <div class="field">
          <label>stabilityTime [ms]</label>
          <input id="stabilityTime-${idx}" type="number" step="1" min="10" max="10000" value="${n.cfg.stabilityTimeMs}" ${locked ? 'disabled' : ''}>
        </div>
        <div class="field">
          <label>invertSign</label>
          <select id="invertSign-${idx}" ${locked ? 'disabled' : ''}>
            <option value="0" ${n.cfg.invertSign ? '' : 'selected'}>false</option>
            <option value="1" ${n.cfg.invertSign ? 'selected' : ''}>true</option>
          </select>
        </div>
        <div class="field">
          <label>autoBeepStable</label>
          <select id="autoBeepStable-${idx}" ${locked ? 'disabled' : ''}>
            <option value="0" ${n.cfg.autoBeepStable ? '' : 'selected'}>false</option>
            <option value="1" ${n.cfg.autoBeepStable ? 'selected' : ''}>true</option>
          </select>
        </div>
        <div class="field full">
          <label>nodeId</label>
          <input id="nodeId-${idx}" type="text" maxlength="3" value="${n.id}" ${locked ? 'disabled' : ''}>
        </div>
      </div>

      <div class="card-actions">
        <button data-cfg-action="1" class="btn-primary" onclick="sendCfg(${idx})" ${locked ? 'disabled' : ''}>INVIA CFG</button>
        <button data-cfg-action="1" class="btn-dark" onclick="readCfg(${idx})" ${locked ? 'disabled' : ''}>LEGGI CFG</button>
        <button data-zero-action="1" class="btn-amber" onclick="zeroNode(${idx})" ${locked ? 'disabled' : ''}>ZERO NODO</button>
      </div>
    `;
    grid.appendChild(card);
  });
}

function syncInputIfNotFocused(id, value){
  const el = document.getElementById(id);
  if(!el) return;
  if(document.activeElement === el) return;
  if(el.tagName === 'SELECT'){
    el.value = value ? '1' : '0';
  }else{
    el.value = value;
  }
}

function updateCards(data){
  (data.nodes || []).forEach((n, idx)=>{
    const title = document.getElementById(`nodeTitle-${idx}`);
    const mac = document.getElementById(`nodeMac-${idx}`);
    const seen = document.getElementById(`nodeSeen-${idx}`);
    const camber = document.getElementById(`camber-${idx}`);
    const z = document.getElementById(`z-${idx}`);
    const stable = document.getElementById(`stable-${idx}`);
    const tx = document.getElementById(`tx-${idx}`);
    const tele = document.getElementById(`tele-${idx}`);

    if(title) title.textContent = n.id;
    if(mac) mac.textContent = n.macKnown ? n.mac : 'MAC sconosciuto';
    if(seen) seen.textContent = n.online ? 'ONLINE' : 'OFFLINE';
    if(camber) camber.textContent = n.online ? `${fmtNumber(n.camber,1)}°` : '--.-°';
    if(z) z.textContent = n.online ? `${fmtNumber(n.z,1)}°` : '--.-°';

    if(stable){
      stable.textContent = n.online ? (n.stable ? 'STABLE':'MOVING') : 'NO DATA';
      stable.className = `pill ${n.online ? (n.stable ? 'ok':'idle') : 'offline'}`;
    }

    if(tx){
      tx.textContent = statusText(n);
      tx.className = `pill ${statusClass(n)}`;
    }

    if(tele){
      tele.textContent = `${batteryText(n)} · ${toeText(n)} · ${fwText(n)}`;
    }

    syncInputIfNotFocused(`alpha-${idx}`, n.cfg.alpha);
    syncInputIfNotFocused(`sampleCount-${idx}`, n.cfg.sampleCount);
    syncInputIfNotFocused(`stabilityThreshold-${idx}`, n.cfg.stabilityThreshold);
    syncInputIfNotFocused(`stabilityTime-${idx}`, n.cfg.stabilityTimeMs);
    syncInputIfNotFocused(`invertSign-${idx}`, n.cfg.invertSign);
    syncInputIfNotFocused(`autoBeepStable-${idx}`, n.cfg.autoBeepStable);
    syncInputIfNotFocused(`nodeId-${idx}`, n.id);
  });
}

function rememberUnknownRole(idx, value){
  unknownRoleSelections[idx] = value;
}

function renderUnknown(data){
  refreshUnknownUiLock();
  if(unknownUiLocked) return;

  const grid = document.getElementById("unknownGrid");
  grid.innerHTML = "";

  const arr = data.unknown || [];
  if(arr.length === 0){
    const card = document.createElement("div");
    card.className = "card";
    card.innerHTML = `<div class="node-id">Nessun nodo sconosciuto</div>`;
    grid.appendChild(card);
    return;
  }

  const locked = isUiLockedForOta();

  arr.forEach(n => {
    const selectedRole = unknownRoleSelections[n.idx] || "FL";

    const card = document.createElement("div");
    card.className = "card";

    card.innerHTML = `
      <div class="head">
        <div>
          <div class="node-id">UNKNOWN</div>
          <div class="meta-small">${n.mac}</div>
        </div>
        <div class="meta-small">${n.online ? "ONLINE" : "OFFLINE"}</div>
      </div>

      <div class="live">
        <div class="livebox">
          <div class="label">ID attuale</div>
          <div class="value" style="font-size:24px">${n.id}</div>
        </div>
        <div class="livebox">
          <div class="label">Camber</div>
          <div class="value" style="font-size:24px">${n.online ? Number(n.camber).toFixed(1)+'°' : '--.-°'}</div>
        </div>
      </div>

      <div class="tele-row">
        ${batteryText(n)} · ${toeText(n)} · ${fwText(n)}
      </div>

      <div class="status-row">
        <span class="pill ${n.online ? 'ok':'offline'}">${n.online ? 'LIVE':'OFFLINE'}</span>
        <span class="pill ${n.pendingAck ? 'busy' : (n.lastAckOk ? 'ok':'idle')}">${n.txStatus || 'UNKNOWN'}</span>
      </div>

      <div class="cfg">
        <div class="field full">
          <label>Assegna ruolo</label>
          <select
            id="unk-role-${n.idx}"
            onclick="lockUnknownUi()"
            onfocus="lockUnknownUi()"
            onchange="rememberUnknownRole(${n.idx}, this.value); lockUnknownUi();"
            onblur="setTimeout(unlockUnknownUi, 300)"
            ${locked ? 'disabled' : ''}>
            <option value="FL" ${selectedRole === "FL" ? "selected" : ""}>FL</option>
            <option value="FR" ${selectedRole === "FR" ? "selected" : ""}>FR</option>
            <option value="RL" ${selectedRole === "RL" ? "selected" : ""}>RL</option>
            <option value="RR" ${selectedRole === "RR" ? "selected" : ""}>RR</option>
            <option value="ST" ${selectedRole === "ST" ? "selected" : ""}>ST</option>
          </select>
        </div>
      </div>

      <div class="card-actions">
        <button data-assign-action="1" class="btn-primary" onclick="assignUnknown(${n.idx})" ${locked ? 'disabled' : ''}>ASSEGNA</button>
      </div>
    `;
    grid.appendChild(card);
  });
}

function renderOtaCards(data){
  const grid = document.getElementById('otaGrid');
  if(!grid) return;
  grid.innerHTML = '';

  const targetFw = selectedFw()?.effectiveFwVersion || '--';
  const locked = isUiLockedForOta();

  (data.nodes || []).forEach((n, idx)=>{
    const policyBlock = otaPolicyBlockReason(n);
    const canStartBase = !!selectedFw() && !!n.macKnown;
    const busy = otaIsBusy(n);
    const canStart = canStartBase && !locked && !busy && !policyBlock;
    const progress = otaProgressValue(n);
    const pClass = otaProgressClass(n);

    const card = document.createElement('div');
    card.className = 'card';

    card.innerHTML = `
      <div class="head">
        <div>
          <div class="node-id">${n.id}</div>
          <div class="meta-small">${n.macKnown ? n.mac : 'MAC sconosciuto'}</div>
        </div>
        <div class="meta-small">${n.online ? 'ONLINE' : 'OFFLINE'}</div>
      </div>

      <div class="live">
        <div class="livebox">
          <div class="label">Stato OTA</div>
          <div class="value" style="font-size:24px">${otaIsBusy(n) ? otaStageText(n.ota.stage) : (otaIsSuccess(n) ? 'DONE' : (otaIsFail(n) ? 'FAIL' : 'READY'))}</div>
        </div>
        <div class="livebox">
          <div class="label">Progress</div>
          <div class="value" style="font-size:24px">${progress}%</div>
          <div class="progress-wrap"><div class="${pClass}" style="width:${progress}%"></div></div>
        </div>
      </div>

      <div class="status-row">
        <span class="pill ${n.online ? 'ok':'offline'}">${n.online ? 'LIVE' : 'OFFLINE'}</span>
        <span class="pill ${otaResultClass(n)}">${otaResultText(n)}</span>
      </div>

      <div class="tele-row">
        ${batteryText(n)} · Current ${n.fwValid ? n.fwVersion : '--'} · Target ${targetFw}${policyBlock ? ` · ${policyBlock}` : ''}
      </div>

      <div class="ota-message">${otaMessageText(n)}</div>

      <div class="card-actions">
        <button data-single-ota-action="1" class="btn-primary" onclick="sendOtaAssigned(${idx})" ${canStart ? '' : 'disabled'}>${busy ? 'OTA IN CORSO' : (policyBlock || 'START OTA')}</button>
      </div>
    `;

    grid.appendChild(card);
  });
}

function renderOtaUnknown(data){
  const grid = document.getElementById('otaUnknownGrid');
  if(!grid) return;
  grid.innerHTML = '';

  const arr = data.unknown || [];
  if(arr.length === 0){
    const card = document.createElement('div');
    card.className = 'card';
    card.innerHTML = `<div class="node-id">Nessun nodo sconosciuto</div>`;
    grid.appendChild(card);
    return;
  }

  const locked = isUiLockedForOta();

  arr.forEach(n=>{
    const policyBlock = otaPolicyBlockReason(n);
    const canStartBase = !!selectedFw();
    const busy = otaIsBusy(n);
    const canStart = canStartBase && !locked && !busy && !policyBlock;
    const progress = otaProgressValue(n);
    const pClass = otaProgressClass(n);

    const card = document.createElement('div');
    card.className = 'card';

    card.innerHTML = `
      <div class="head">
        <div>
          <div class="node-id">UNKNOWN</div>
          <div class="meta-small">${n.mac || 'MAC sconosciuto'}</div>
        </div>
        <div class="meta-small">${n.online ? 'ONLINE' : 'OFFLINE'}</div>
      </div>

      <div class="live">
        <div class="livebox">
          <div class="label">ID attuale</div>
          <div class="value" style="font-size:24px">${n.id || '---'}</div>
        </div>
        <div class="livebox">
          <div class="label">Progress</div>
          <div class="value" style="font-size:24px">${progress}%</div>
          <div class="progress-wrap"><div class="${pClass}" style="width:${progress}%"></div></div>
        </div>
      </div>

      <div class="status-row">
        <span class="pill ${n.online ? 'ok':'offline'}">${n.online ? 'LIVE' : 'OFFLINE'}</span>
        <span class="pill ${otaResultClass(n)}">${otaResultText(n)}</span>
      </div>

      <div class="tele-row">
        ${batteryText(n)} · Current ${n.fwValid ? n.fwVersion : '--'}${policyBlock ? ` · ${policyBlock}` : ''}
      </div>

      <div class="ota-message">${otaMessageText(n)}</div>

      <div class="card-actions">
        <button data-single-ota-action="1" class="btn-primary" onclick="sendOtaUnknown(${n.idx})" ${canStart ? '' : 'disabled'}>${busy ? 'OTA IN CORSO' : (policyBlock || 'START OTA')}</button>
      </div>
    `;

    grid.appendChild(card);
  });
}


async function sendOtaAssignedInternal(idx){
  try{
    pausePolling(250);

    const fw = selectedFw();
    if(!fw) return { ok:false, status:'Seleziona un file firmware' };
    if(!fw.canSlave) return { ok:false, status:fw.validation || 'Firmware non valido per moduli' };

    const form = new URLSearchParams();
    form.set('idx', idx);
    form.set('name', fw.name);

    const res = await fetch('/otaAssigned', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });

    if (res.ok) return { ok:true, status:'START SENT' };

    let txt = '';
    try { txt = await res.text(); } catch(e) {}
    return { ok:false, status:txt || 'SEND ERR' };
  }catch(e){
    return { ok:false, status:'SEND ERR' };
  }
}


async function sendOtaUnknownInternal(idx){
  try{
    pausePolling(250);

    const fw = selectedFw();
    if(!fw) return { ok:false, status:'Seleziona un file firmware' };
    if(!fw.canSlave) return { ok:false, status:fw.validation || 'Firmware non valido per moduli' };

    const form = new URLSearchParams();
    form.set('idx', idx);
    form.set('name', fw.name);

    const res = await fetch('/otaUnknown', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });

    if (res.ok) return { ok:true, status:'START SENT' };

    let txt = '';
    try { txt = await res.text(); } catch(e) {}
    return { ok:false, status:txt || 'SEND ERR' };
  }catch(e){
    return { ok:false, status:'SEND ERR' };
  }
}

async function sendOtaAssigned(idx){
  if (!requireUnlockedUi('OTA singolo')) return;

  const result = await sendOtaAssignedInternal(idx);
  if (result.ok) {
    showToast('OTA inviato');
    setTimeout(fetchData, 250);
  } else {
    showToast(result.status || 'Errore invio OTA');
  }
}

async function sendOtaUnknown(idx){
  if (!requireUnlockedUi('OTA singolo')) return;

  const result = await sendOtaUnknownInternal(idx);
  if (result.ok) {
    showToast('OTA inviato');
    setTimeout(fetchData, 250);
  } else {
    showToast(result.status || 'Errore invio OTA');
  }
}

function renderBatchOtaState(data){
  const b = data.batchOta || null;
  batchOtaState = b;

  if (!b) {
    batchOtaBusy = false;
    const msgEl = document.getElementById('batch-ota-msg');
    if (msgEl) msgEl.textContent = 'Nessuna attività batch';
    batchLogLines = ['In attesa...'];
    const logEl = document.getElementById('batch-ota-log');
    if (logEl) logEl.textContent = batchLogLines.join('\n');
    updateOtaSafeUiState();
    return;
  }

  batchOtaBusy = !!b.active;

  const msgEl = document.getElementById('batch-ota-msg');
  if (msgEl) {
    let msg = b.status || 'Nessuna attività batch';
    if (b.total > 0) {
      const current = b.current >= 0 ? (b.current + 1) : 0;
      msg += ` · ${current}/${b.total}`;
    }
    msgEl.textContent = msg;
  }

  if (b.log && String(b.log).trim().length) {
    batchLogLines = String(b.log).split('\n');
  } else if (b.active) {
    batchLogLines = ['Batch OTA attivo...'];
  } else {
    batchLogLines = ['In attesa...'];
  }

  const logEl = document.getElementById('batch-ota-log');
  if (logEl) logEl.textContent = batchLogLines.join('\n');

  updateOtaSafeUiState();
}

async function startBatchOta(){
  if (!requireUnlockedUi('batch OTA')) return;
  if(batchOtaBusy || batchStartPending) return;
  batchStartPending = true;

  const fw = selectedFw();
  if(!fw){
    batchStartPending = false;
    showToast('Seleziona un file firmware');
    return;
  }
  if(!fw.canSlave){
    batchStartPending = false;
    showToast(fw.validation || 'Firmware non valido per moduli');
    return;
  }

  await fetchData();

  const eligible = (nodeData || []).filter(n => n.macKnown);
  if(!eligible.length){
    batchStartPending = false;
    showToast('Nessun nodo assegnato aggiornabile');
    return;
  }

  const targetFw = selectedFw()?.effectiveFwVersion || '--';
  const ok = confirm(`Aggiornare ${eligible.length} moduli con:
${fw.name}
Versione target: ${targetFw}

Procedura sequenziale locale sul master.\n\nPolicy attiva: ${optionsSummaryText(getSystemOptions())}`);
  if(!ok){ batchStartPending = false; return; }

  try{
    pausePolling(250);
    batchOtaBusy = true;
    updateOtaSafeUiState();

    const form = new URLSearchParams();
    form.set('name', fw.name);
    
    const res = await fetch('/batchOtaStart', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });

    let txt = '';
    try { txt = await res.text(); } catch(e) {}

    if(!res.ok){
      batchOtaBusy = false;
      updateOtaSafeUiState();
      showToast(txt || 'Errore avvio batch');
      batchStartPending = false;
      return;
    }

    batchStartPending = false;
    showToast(txt || 'Batch OTA avviato');
    await fetchData();
  }catch(e){
    batchStartPending = false;
    batchOtaBusy = false;
    updateOtaSafeUiState();
    showToast('Errore avvio batch');
  }
}

async function startMasterOta(){
  if (!requireUnlockedUi('OTA master')) return;

  const fw = selectedFw();
  if(!fw){
    showToast('Seleziona un file firmware');
    return;
  }
  if(!fw.canMaster){
    showToast(fw.validation || 'Firmware non valido per master');
    return;
  }

  const ok = confirm(`Confermi aggiornamento MASTER con:\n${fw.name}\n\nIl master si riavvierà al termine.`);
  if(!ok){ batchStartPending = false; return; }

  try{
    setMasterOtaBusy(true, `Aggiornamento master in corso con ${fw.name}...`);
    pausePolling(5000);

    const form = new URLSearchParams();
    form.set('name', fw.name);

    const res = await fetch('/otaMaster', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body: form.toString()
    });

    let txt = '';
    try { txt = await res.text(); } catch(e) {}

    if(res.ok){
      document.getElementById('master-ota-msg').textContent =
        txt || 'Aggiornamento avviato. Attendo riavvio master...';
      showToast(txt || 'Aggiornamento master avviato');
    } else {
      setMasterOtaBusy(false, txt || 'Errore aggiornamento master');
      showToast(txt || 'Errore aggiornamento master');
      return;
    }
  } catch(e){
    document.getElementById('master-ota-msg').textContent =
      'Connessione interrotta: il master potrebbe essere in riavvio...';
  }

  await waitForMasterBackOnline();
}

async function fetchData(){
  try{
    const res = await fetch('/data', { cache: 'no-store' });
    if (!res.ok) return;

    const data = await res.json();
    nodeData = data.nodes || [];

    renderLiveMeasure(data);

    if(!rendered) renderCards(data);
    updateCards(data);
    renderUnknown(data);

    renderOtaCards(data);
    renderOtaUnknown(data);
    renderBatchOtaState(data);
    renderSystemOptions(data);
    updateFirmwareInfo(data);
    updateSelectedFirmwareUi();
    updateOtaSafeUiState();
  }catch(e){
    console.log('fetch error', e);
  }
}

async function zeroAll(){
  if (!requireUnlockedUi('ZERO ALL')) return;

  try{
    pausePolling(150);
    const res = await fetch('/zero', {method:'POST'});
    if (res.ok) showToast(await res.text());
  }catch(e){
    showToast('Errore ZERO ALL');
  }
}

async function zeroNode(idx){
  if (!requireUnlockedUi('ZERO nodo')) return;

  try{
    pausePolling(150);
    const form = new URLSearchParams();
    form.set('idx', idx);
    const res = await fetch('/zeroNode', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });
    if (res.ok) showToast(await res.text());
  }catch(e){
    showToast('Errore ZERO nodo');
  }
}

async function readCfg(idx){
  if (!requireUnlockedUi('LEGGI CFG')) return;

  try{
    pausePolling(150);
    const form = new URLSearchParams();
    form.set('idx', idx);
    const res = await fetch('/getCfg', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });
    if (res.ok) showToast(await res.text());
  }catch(e){
    showToast('Errore richiesta cfg');
  }
}

async function sendCfg(idx){
  if (!requireUnlockedUi('INVIA CFG')) return;

  try{
    pausePolling(150);
    const form = new URLSearchParams();
    form.set('idx', idx);
    form.set('alpha', document.getElementById(`alpha-${idx}`).value);
    form.set('sampleCount', document.getElementById(`sampleCount-${idx}`).value);
    form.set('stabilityThreshold', document.getElementById(`stabilityThreshold-${idx}`).value);
    form.set('stabilityTime', document.getElementById(`stabilityTime-${idx}`).value);
    form.set('invertSign', document.getElementById(`invertSign-${idx}`).value);
    form.set('autoBeepStable', document.getElementById(`autoBeepStable-${idx}`).value);
    form.set('nodeId', document.getElementById(`nodeId-${idx}`).value.toUpperCase());

    const res = await fetch('/setCfg', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });

    if (res.ok) showToast(await res.text());
  }catch(e){
    showToast('Errore invio cfg');
  }
}

async function assignUnknown(idx){
  if (!requireUnlockedUi('ASSEGNA')) return;

  try{
    pausePolling(250);
    lockUnknownUi(2000);

    const role = unknownRoleSelections[idx] || document.getElementById(`unk-role-${idx}`).value;

    const form = new URLSearchParams();
    form.set('idx', idx);
    form.set('role', role);

    const res = await fetch('/assignUnknown', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:form.toString()
    });

    if (res.ok) showToast(await res.text());
    setTimeout(unlockUnknownUi, 500);
  }catch(e){
    unlockUnknownUi();
    showToast('Errore assegnazione');
  }
}

function refreshNow(){
  fetchData();
  showToast('Aggiornato');
}

fetchData();
setUpdateMode('auto');
restartPolling();
</script>
</body>
</html>
)rawliteral";

  html.replace("__AP_SSID__", jsEsc(String(AP_SSID)));
  html.replace("__AP_PASS__", jsEsc(String(AP_PASS)));
  html.replace("__MASTER_FW_VERSION__", jsEsc(String(MASTER_FW_DISPLAY_VERSION)));

  return html;
}

String makeJsonData() {
  NodeState nodesSnap[MAX_ASSIGNED_NODES];
  UnknownNodeState unknownSnap[MAX_UNKNOWN_NODES];
  BatchOtaState batchSnap;

  portENTER_CRITICAL(&mux);
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) nodesSnap[i] = nodes[i];
  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) unknownSnap[i] = unknownNodes[i];
  batchSnap = batchOta;
  portEXIT_CRITICAL(&mux);

  String json;
  json.reserve(8192);
  json = "{";

  json += "\"masterFwVersion\":\"" + jsonEsc(String(MASTER_FW_DISPLAY_VERSION)) + "\",";

  json += "\"nodes\":[";
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    json += "{";
    json += "\"id\":\"" + String(nodesSnap[i].id) + "\",";
    json += "\"camber\":" + String(nodesSnap[i].camber, 3) + ",";
    json += "\"z\":" + String(nodesSnap[i].z, 3) + ",";
    json += "\"stable\":" + String(nodesSnap[i].stable ? "true" : "false") + ",";
    json += "\"online\":" + String(nodesSnap[i].online ? "true" : "false") + ",";
    json += "\"macKnown\":" + String(nodesSnap[i].macKnown ? "true" : "false") + ",";
    json += "\"mac\":\"" + String(nodesSnap[i].macKnown ? macToString(nodesSnap[i].mac) : "") + "\",";
    json += "\"pendingAck\":" + String(nodesSnap[i].tx.pendingAck ? "true" : "false") + ",";
    json += "\"lastAckOk\":" + String(nodesSnap[i].tx.lastAckOk ? "true" : "false") + ",";
    json += "\"txStatus\":\"" + jsonEsc(String(nodesSnap[i].tx.lastStatus)) + "\",";
    json += "\"batteryVoltage\":" + String(nodesSnap[i].batteryVoltage, 3) + ",";
    json += "\"batterySoc\":" + String(nodesSnap[i].batterySoc, 1) + ",";
    json += "\"batteryValid\":" + String(nodesSnap[i].batteryValid ? "true" : "false") + ",";
    json += "\"toe\":" + String(nodesSnap[i].toe, 3) + ",";
    json += "\"toeValid\":" + String(nodesSnap[i].toeValid ? "true" : "false") + ",";
    json += "\"fwVersion\":\"" + jsonEsc(String(nodesSnap[i].fwVersion)) + "\",";
    json += "\"fwValid\":" + String(nodesSnap[i].fwValid ? "true" : "false") + ",";
    json += "\"ota\":{";
    json += "\"active\":" + String(nodesSnap[i].ota.active ? "true" : "false") + ",";
    json += "\"lastOk\":" + String(nodesSnap[i].ota.lastOk ? "true" : "false") + ",";
    json += "\"stage\":" + String(nodesSnap[i].ota.stage) + ",";
    json += "\"progress\":" + String(nodesSnap[i].ota.progress) + ",";
    json += "\"message\":\"" + jsonEsc(String(nodesSnap[i].ota.message)) + "\"";
    json += "},";
    json += "\"cfg\":{";
    json += "\"alpha\":" + String(nodesSnap[i].cfg.alpha, 3) + ",";
    json += "\"sampleCount\":" + String(nodesSnap[i].cfg.sampleCount) + ",";
    json += "\"stabilityThreshold\":" + String(nodesSnap[i].cfg.stabilityThreshold, 3) + ",";
    json += "\"stabilityTimeMs\":" + String(nodesSnap[i].cfg.stabilityTimeMs) + ",";
    json += "\"invertSign\":" + String(nodesSnap[i].cfg.invertSign ? "true" : "false") + ",";
    json += "\"autoBeepStable\":" + String(nodesSnap[i].cfg.autoBeepStable ? "true" : "false");
    json += "}";
    json += "}";
    if (i < MAX_ASSIGNED_NODES - 1) json += ",";
  }
  json += "],";

  json += "\"unknown\":[";
  bool firstUnknown = true;
  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) {
    if (!unknownSnap[i].used) continue;

    if (!firstUnknown) json += ",";
    firstUnknown = false;

    json += "{";
    json += "\"idx\":" + String(i) + ",";
    json += "\"id\":\"" + String(unknownSnap[i].id) + "\",";
    json += "\"camber\":" + String(unknownSnap[i].camber, 3) + ",";
    json += "\"online\":" + String(unknownSnap[i].online ? "true" : "false") + ",";
    json += "\"mac\":\"" + String(unknownSnap[i].macKnown ? macToString(unknownSnap[i].mac) : "") + "\",";
    json += "\"pendingAck\":" + String(unknownSnap[i].pendingAck ? "true" : "false") + ",";
    json += "\"lastAckOk\":" + String(unknownSnap[i].lastAckOk ? "true" : "false") + ",";
    json += "\"txStatus\":\"" + jsonEsc(String(unknownSnap[i].lastStatus)) + "\",";
    json += "\"batteryVoltage\":" + String(unknownSnap[i].batteryVoltage, 3) + ",";
    json += "\"batterySoc\":" + String(unknownSnap[i].batterySoc, 1) + ",";
    json += "\"batteryValid\":" + String(unknownSnap[i].batteryValid ? "true" : "false") + ",";
    json += "\"toe\":" + String(unknownSnap[i].toe, 3) + ",";
    json += "\"toeValid\":" + String(unknownSnap[i].toeValid ? "true" : "false") + ",";
    json += "\"fwVersion\":\"" + jsonEsc(String(unknownSnap[i].fwVersion)) + "\",";
    json += "\"fwValid\":" + String(unknownSnap[i].fwValid ? "true" : "false") + ",";
    json += "\"ota\":{";
    json += "\"active\":" + String(unknownSnap[i].ota.active ? "true" : "false") + ",";
    json += "\"lastOk\":" + String(unknownSnap[i].ota.lastOk ? "true" : "false") + ",";
    json += "\"stage\":" + String(unknownSnap[i].ota.stage) + ",";
    json += "\"progress\":" + String(unknownSnap[i].ota.progress) + ",";
    json += "\"message\":\"" + jsonEsc(String(unknownSnap[i].ota.message)) + "\"";
    json += "}";
    json += "}";
  }
  json += "]";

  String defName = getDefaultFirmwareName();
  size_t fwSize = 0;
  bool fwExists = false;
  if (defName.length()) fwExists = getFirmwareFromSd(defName, fwSize);

  json += ",";
  json += "\"batchOta\":{";
  json += "\"active\":" + String(batchSnap.active ? "true" : "false") + ",";
  json += "\"finished\":" + String(batchSnap.finished ? "true" : "false") + ",";
  json += "\"phase\":" + String(batchSnap.phase) + ",";
  json += "\"phaseText\":\"" + jsonEsc(String(batchPhaseToText(batchSnap.phase))) + "\",";
  json += "\"total\":" + String(batchSnap.total) + ",";
  json += "\"current\":" + String(batchSnap.current) + ",";
  json += "\"status\":\"" + jsonEsc(String(batchSnap.status)) + "\",";
  json += "\"fwFile\":\"" + jsonEsc(String(batchSnap.fwFile)) + "\",";
  json += "\"fwVersion\":\"" + jsonEsc(String(batchSnap.startPkt.fwVersion)) + "\",";
  json += "\"log\":\"" + jsonEsc(String(batchSnap.log)) + "\",";
  json += "\"items\":[";
  bool firstBatchItem = true;
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    if (!batchSnap.items[i].used) continue;
    if (!firstBatchItem) json += ",";
    firstBatchItem = false;
    json += "{";
    json += "\"nodeId\":\"" + jsonEsc(String(batchSnap.items[i].nodeId)) + "\",";
    json += "\"nodeIndex\":" + String(batchSnap.items[i].nodeIndex) + ",";
    json += "\"started\":" + String(batchSnap.items[i].started ? "true" : "false") + ",";
    json += "\"finished\":" + String(batchSnap.items[i].finished ? "true" : "false") + ",";
    json += "\"success\":" + String(batchSnap.items[i].success ? "true" : "false") + ",";
    json += "\"result\":\"" + jsonEsc(String(batchSnap.items[i].result)) + "\",";
    json += "\"lastStage\":" + String(batchSnap.items[i].lastStage) + ",";
    json += "\"lastProgress\":" + String(batchSnap.items[i].lastProgress);
    json += "}";
  }
  json += "]";
  json += "}";

  json += ",";
  json += "\"options\":{";
  json += "\"skipAlreadyUpdated\":" + String(systemOptions.skipAlreadyUpdated ? "true" : "false") + ",";
  json += "\"allowUnknownBattery\":" + String(systemOptions.allowUnknownBattery ? "true" : "false") + ",";
  json += "\"blockLowBattery\":" + String(systemOptions.blockLowBattery ? "true" : "false") + ",";
  json += "\"verboseOtaLog\":" + String(systemOptions.verboseOtaLog ? "true" : "false") + ",";
  json += "\"minBatterySoc\":" + String(systemOptions.minBatterySoc, 1);
  json += "}";

  json += "}";
  return json;
}

void handleRoot() {
  server.send(200, "text/html", makeHtmlPage());
}

void handleData() {
  server.send(200, "application/json", makeJsonData());
}

void handleZero() {
  sendZeroAll();
  server.send(200, "text/plain", "ZERO ALL inviato");
}

void handleZeroNode() {

  if (!server.hasArg("idx")) {
    server.send(400, "text/plain", "idx mancante");
    return;
  }

  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES) {
    server.send(400, "text/plain", "idx non valido");
    return;
  }

  if (!nodes[idx].macKnown) {
    server.send(409, "text/plain", "Nodo non associato");
    return;
  }

  bool ok = sendLegacyCommandToNode(idx, "zero");
  server.send(ok ? 200 : 500, "text/plain", ok ? "ZERO nodo inviato" : "Errore invio ZERO nodo");
}

void handleSetCfg() {

  if (!server.hasArg("idx")) {
    server.send(400, "text/plain", "idx mancante");
    return;
  }

  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES) {
    server.send(400, "text/plain", "idx non valido");
    return;
  }

  if (!nodes[idx].macKnown) {
    server.send(409, "text/plain", "Nodo non associato");
    return;
  }

  ConfigPacketV1 pkt = {};
  String err;
  if (!getNodeConfigFromRequest(idx, pkt, err)) {
    server.send(400, "text/plain", err);
    return;
  }

  bool ok = sendConfigToNode(idx, pkt);
  server.send(ok ? 200 : 500, "text/plain", ok ? "Config inviata" : "Errore invio config");
}

void handleGetCfg() {

  if (!server.hasArg("idx")) {
    server.send(400, "text/plain", "idx mancante");
    return;
  }

  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES) {
    server.send(400, "text/plain", "idx non valido");
    return;
  }

  if (!nodes[idx].macKnown) {
    server.send(409, "text/plain", "Nodo non associato");
    return;
  }

  bool ok = requestConfigFromNode(idx);
  server.send(ok ? 200 : 500, "text/plain", ok ? "Richiesta config inviata" : "Errore richiesta config");
}

void handleAssignUnknown() {

  if (!server.hasArg("idx") || !server.hasArg("role")) {
    server.send(400, "text/plain", "Parametri mancanti");
    return;
  }

  int idx = server.arg("idx").toInt();
  String role = server.arg("role");
  role.trim();
  role.toUpperCase();

  if (!isOfficialRole(role.c_str())) {
    server.send(400, "text/plain", "Ruolo non valido");
    return;
  }

  bool ok = assignUnknownNodeRole(idx, role.c_str());
  server.send(ok ? 200 : 409, "text/plain", ok ? "Assegnazione inviata" : "Assegnazione fallita");
}


void handleOtaAssigned() {
  if (!server.hasArg("idx") || !server.hasArg("name")) {
    server.send(400, "text/plain", "Parametri mancanti");
    return;
  }

  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES) {
    server.send(400, "text/plain", "idx non valido");
    return;
  }

  OtaStartPacket pkt = {};
  String err;
  if (!buildSlaveOtaPacketFromFile(server.arg("name"), pkt, err)) {
    server.send(400, "text/plain", err);
    return;
  }

  if (!validateAssignedNodeOtaPolicy(idx, pkt, err)) {
    server.send(409, "text/plain", err);
    return;
  }

  bool ok = sendOtaToAssignedNode(idx, pkt);
  server.send(ok ? 200 : 500, "text/plain", ok ? "OTA inviato" : "Errore invio OTA");
}


void handleOtaUnknown() {
  if (!server.hasArg("idx") || !server.hasArg("name")) {
    server.send(400, "text/plain", "Parametri mancanti");
    return;
  }

  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_UNKNOWN_NODES) {
    server.send(400, "text/plain", "idx non valido");
    return;
  }

  OtaStartPacket pkt = {};
  String err;
  if (!buildSlaveOtaPacketFromFile(server.arg("name"), pkt, err)) {
    server.send(400, "text/plain", err);
    return;
  }

  if (!validateUnknownNodeOtaPolicy(idx, pkt, err)) {
    server.send(409, "text/plain", err);
    return;
  }

  bool ok = sendOtaToUnknownNode(idx, pkt);
  server.send(ok ? 200 : 500, "text/plain", ok ? "OTA inviato" : "Errore invio OTA");
}


void handleBatchOtaStart() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "name mancante");
    return;
  }

  String name = server.arg("name");
  OtaStartPacket pkt = {};
  String err;
  if (!buildSlaveOtaPacketFromFile(name, pkt, err)) {
    server.send(400, "text/plain", err);
    return;
  }

  String startErr;
  bool ok = startBatchOta(name.c_str(), pkt, startErr);
  if (!ok) {
    server.send(409, "text/plain", startErr);
    return;
  }

  server.send(200, "text/plain", "Batch OTA avviato");
}


void handleSetOptions() {
  SystemOptions next = systemOptions;

  if (server.hasArg("skipAlreadyUpdated")) next.skipAlreadyUpdated = parseBoolArg(server.arg("skipAlreadyUpdated"));
  if (server.hasArg("allowUnknownBattery")) next.allowUnknownBattery = parseBoolArg(server.arg("allowUnknownBattery"));
  if (server.hasArg("blockLowBattery")) next.blockLowBattery = parseBoolArg(server.arg("blockLowBattery"));
  if (server.hasArg("verboseOtaLog")) next.verboseOtaLog = parseBoolArg(server.arg("verboseOtaLog"));
  if (server.hasArg("minBatterySoc")) {
    float v = server.arg("minBatterySoc").toFloat();
    if (v < 0.0f || v > 100.0f) {
      server.send(400, "text/plain", "Soglia batteria non valida");
      return;
    }
    next.minBatterySoc = v;
  }

  systemOptions = next;
  if (!saveSystemOptions()) {
    server.send(500, "text/plain", "Errore salvataggio opzioni");
    return;
  }

  server.send(200, "text/plain", "Opzioni salvate");
}

void handleFirmwareInfo() {
  String defName = getDefaultFirmwareName();
  ParsedFirmwareInfo info;
  bool exists = defName.length() && inspectFirmwareFile(defName, info) && info.fileExists;

  String json = "{";
  json += "\"exists\":" + String(exists ? "true" : "false") + ",";
  json += "\"path\":\"" + jsonEsc(defName) + "\",";
  json += "\"url\":\"" + jsonEsc(defName.length() ? buildFwUrl(defName) : "") + "\",";
  json += "\"size\":" + String((unsigned long)(exists ? info.size : 0)) + ",";
  json += "\"target\":\"" + jsonEsc(exists ? info.target : String("")) + "\",";
  json += "\"chip\":\"" + jsonEsc(exists ? info.chip : String("")) + "\",";
  json += "\"version\":\"" + jsonEsc(exists ? info.effectiveFwVersion : String("")) + "\",";
  json += "\"validation\":\"" + jsonEsc(exists ? info.validation : String("")) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleFwList() {
  server.send(200, "application/json", getFirmwareListJson());
}

void handleFwFile() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "name mancante");
    return;
  }

  String name = server.arg("name");
  if (!isSafeBinFileName(name)) {
    server.send(400, "text/plain", "nome file non valido");
    return;
  }

  File fw = rf73Storage.open(fileNameToPath(name), FILE_READ);
  if (!fw) {
    server.send(404, "text/plain", "Firmware file not found on SD");
    return;
  }

  setOtaTrafficLock(30000);

  server.setContentLength(fw.size());
  server.sendHeader("Cache-Control", "no-cache");
  server.streamFile(fw, "application/octet-stream");
  fw.close();

  clearOtaTrafficLock();
}


void handleOtaMaster() {
  Serial.println();
  Serial.println("========== MASTER SELF OTA ==========");

  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "name mancante");
    return;
  }

  String name = server.arg("name");
  ParsedFirmwareInfo info;
  String err;
  if (!validateMasterFirmwareFile(name, info, err)) {
    server.send(400, "text/plain", err);
    return;
  }

  File fw = rf73Storage.open(fileNameToPath(name), FILE_READ);
  if (!fw) {
    server.send(404, "text/plain", "File firmware non trovato");
    return;
  }

  size_t fwSize = fw.size();
  if (fwSize == 0) {
    fw.close();
    server.send(400, "text/plain", "File firmware vuoto");
    return;
  }

  if (!Update.begin(fwSize)) {
    fw.close();
    server.send(500, "text/plain", "Update.begin fallita");
    return;
  }

  size_t written = Update.writeStream(fw);
  fw.close();

  if (written != fwSize) {
    Update.abort();
    server.send(500, "text/plain", "Scrittura firmware incompleta");
    return;
  }

  if (!Update.end(true)) {
    server.send(500, "text/plain", "Update.end fallita");
    return;
  }

  if (!Update.isFinished()) {
    server.send(500, "text/plain", "Update non completato");
    return;
  }

  server.send(200, "text/plain", "Aggiornamento master completato. Riavvio...");
  WiFiClient client = server.client();
  delay(200);
  client.flush();
  delay(250);
  ESP.restart();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/zero", HTTP_POST, handleZero);
  server.on("/zeroNode", HTTP_POST, handleZeroNode);
  server.on("/setCfg", HTTP_POST, handleSetCfg);
  server.on("/getCfg", HTTP_POST, handleGetCfg);
  server.on("/assignUnknown", HTTP_POST, handleAssignUnknown);
  server.on("/otaAssigned", HTTP_POST, handleOtaAssigned);
  server.on("/otaUnknown", HTTP_POST, handleOtaUnknown);
  server.on("/batchOtaStart", HTTP_POST, handleBatchOtaStart);
  server.on("/firmwareInfo", HTTP_GET, handleFirmwareInfo);
  server.on("/fwlist", HTTP_GET, handleFwList);
  server.on("/fwfile", HTTP_GET, handleFwFile);
  server.on("/otaMaster", HTTP_POST, handleOtaMaster);
  server.on("/setOptions", HTTP_POST, handleSetOptions);
  server.begin();

  Serial.println("Webserver ready");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}