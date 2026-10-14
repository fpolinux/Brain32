/*
  ESP32 Second Brain  (Telegram + Groq + Finnhub + SH1106 OLED)

  Libraries (Library Manager): ArduinoJson (v7.x), U8g2
  Board: ESP32 Dev Module (4MB), default partition scheme
  Wiring: OLED SDA=21 SCL=22, button GPIO4 -> GND (INPUT_PULLUP)
  Put your keys in secrets.h (same folder).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <initializer_list>
#include "secrets.h"
#if defined(USE_EDUROAM) && ESP_ARDUINO_VERSION_MAJOR < 3
#include "esp_wpa2.h"
#endif

// ===================== config =====================
#define BTN_PIN 4
static const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";  // Warsaw, auto DST

const int MAX_TODO = 50, MAX_LIST_ITEMS = 50, MAX_LISTS = 10, MAX_NOTES = 300;
const int MAX_AI = 20, AI_WARN_AT = 18, AI_RAM = 5;
const size_t MAX_ITEM = 200, MAX_NOTE_LEN = 1000, MAX_PROMPT = 1000;
const uint32_t FLOW_TIMEOUT_MS = 300000UL;  // 5 min

// ===================== types (keep above all functions) =====================
struct HwItem { String s; long long d = 0; String x; bool a72 = false, a24 = false; };
struct AiAns { uint32_t id = 0; String q, a; time_t t = 0; bool saved = false; };
struct NameCache { String sym, name, cur; };
struct AliasRow { const char* w; const char* c; };

enum FlowType { F_NONE, F_HW_ADD, F_HW_EDIT };
enum FlowStep { S_SUBJ, S_DATE, S_DESC, S_FIELD, S_VALUE };
struct Flow {
  FlowType type = F_NONE; FlowStep step = S_SUBJ;
  String subj; long long due = 0; int idx = -1; char field = 0;
  uint32_t last = 0; std::vector<String> subs;
};

// ===================== globals =====================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;
long long tgOffset = 0;
Flow flow;
AiAns aiRam[AI_RAM];
int aiHead = 0;
NameCache nameCache[8];
int ncHead = 0;

SemaphoreHandle_t uiMux;
char gBusy[24] = "";
std::vector<String> gTodoLines;
volatile int gTodoCount = 0;

// subcommand aliases -> canonical word
const AliasRow ALIASES[] = {
  {"add","add"},{"new","add"},{"create","add"},{"put","add"},{"+","add"},
  {"del","del"},{"delete","del"},{"remove","del"},{"rm","del"},{"done","del"},{"finish","del"},{"complete","del"},{"x","del"},
  {"edit","edit"},{"change","edit"},{"modify","edit"},{"update","edit"},{"rename","edit"},
  {"search","search"},{"find","search"},{"lookup","search"},{"query","search"},
  {"help","help"},{"?","help"},{"h","help"},{"info","help"},{"commands","help"},
  {"list","show"},{"ls","show"},{"view","show"},{"all","show"},{"show","show"},
  {"keep","keep"},{"save","keep"},{"pin","keep"},{"star","keep"},
  {"history","history"},{"hist","history"},{"saved","history"},{"log","history"},
  {"cancel","cancel"},{"stop","cancel"},{"quit","cancel"},{"exit","cancel"},{"abort","cancel"}
};

// ===================== help texts =====================
const char H_INDEX[] = R"HELP(🧠 SECOND BRAIN

/todo   to-do list
/note   quick notes (+ kept AI answers)
/hw     homework, 72h/24h reminders
/list   named lists (shopping etc.)
/ai     ask the AI
/stock  stock quote (US tickers)
/cancel abort a guided entry

Details: /todo help, /note help, /hw help, /list help, /ai help, /stock help
(or /help <command>)

OLED: button on GPIO4 flips between clock and todo screen.)HELP";

const char H_TODO[] = R"HELP(✅ TODO
Your task list.

Commands
/todo                   show the list
/todo add <text>        add an item
/todo del <n>           delete item n
/todo edit <n> <text>   replace item n
/todo help              this card

Tips
• The list is re-printed after every change, so check numbers before deleting.
• Also accepted: delete/remove/rm/done, change/update, new/create, ls/list.
• Max 50 items, 200 characters each.
• Items also show on the OLED (press the button).)HELP";

const char H_NOTE[] = R"HELP(📝 NOTES
Save quick text notes. Kept AI answers live here too.

Commands
/note <text>          add a note
/note add <text>      explicit add (see tips)
/note                 show the last 10 notes
/note ls              same as above
/note ls all          show every note
/note ai              show kept AI answers only
/note search <word>   search notes
/note del <n>         delete note n
/note rm <n>          same as del
/note help            this card

Tips
• Notes are timestamped automatically.
• Numbers are the same in every view, but they shift after a delete.
• To save a note that starts with search, del, rm, ls, ai, add or help, use /note add <text>.
• If you type "/note delete 2" the bot asks what you meant instead of saving it.
• Keep an AI answer for good with /ai history keep <n>.
• /notes and /n also work.)HELP";

const char H_HW[] = R"HELP(📚 HOMEWORK
Tracks assignments and warns you 72h and 24h before they're due.

Commands
/hw               list, sorted by due date
/hw add           guided: subject, due date, description
/hw edit <n>      change subject, due date or description
/hw del <n>       mark done (deletes it, stops reminders)
/hw help          this card

Typed dates
20-10-26, 20.10.2026, 20/10 (next 20 Oct), fri, tomorrow, today
Default time is 23:59. Add a time to change it: 20-10-26 08:00

Tips
• /cancel aborts a guided entry. It also times out after 5 minutes.
• Tap a previous subject button to skip typing.
• Due within 72h: no 72h warning. Due within 24h: no 24h warning.
• Overdue items stay in the list (flagged) until you delete them.
• Changing the due date resets the reminders for the new date.)HELP";

const char H_LIST[] = R"HELP(🛒 LISTS
Named lists, such as shopping or hardware. A list is created when you first add to it.

Commands
/list                        every list under a header
/list <name>                 just that list
/list <name> add <text>      add an item
/list <name> del <n>         delete item n
/list <name> edit <n> <text> replace item n
/list help                   this card

Tips
• /shopping add milk is a shortcut for /list shopping add milk.
• List names are one word. Words like add, del, edit, all, help can't be names.
• Numbers restart at 1 in each list.
• A list disappears when its last item is deleted.
• Max 10 lists, 50 items each.)HELP";

const char H_AI[] = R"HELP(🤖 AI (Groq / Qwen)
Ask a quick question. Replies are short and stateless (no memory between questions).

Commands
/ai <question>              ask the AI
/ai history                 saved answers, newest first (max 20)
/ai history <n>             full question and answer
/ai history search <word>   search saved answers
/ai history del <n>         delete an entry
/ai history keep <n>        copy to notes permanently
/ai help                    this card

Tips
• Tap 💾 Save under an answer to store it. Save soon: only the last 5 answers are held for saving, and a reboot clears them.
• The archive holds 20. At 18 you get a warning; after 20 the oldest rolls off.
• Kept answers stay in /note (tagged [AI]) even after they roll off the archive.
• Only /ai text is sent to Groq, never your notes or todos.
• Free-tier rate limits apply. If you hit one, wait a minute.
• To ask something starting with "history", reword it (e.g. "Explain history of...").)HELP";

const char H_STOCK[] = R"HELP(📈 STOCK
Price and daily change from Finnhub.

Commands
/stock <TICKER>   e.g. /stock AAPL
/stock help       this card

Tips
• Shows company name, price, change in currency and %.
• A wrong ticker returns suggestions.
• The free Finnhub plan covers US stocks. Warsaw (GPW) tickers won't work.
• Outside market hours you see the last close; the time shown is the last quote time.
• /quote and /price also work.)HELP";

// ===================== small utils =====================
String lower(String s) { s.toLowerCase(); return s; }
bool isNumber(const String& s) {
  if (!s.length()) return false;
  for (unsigned i = 0; i < s.length(); i++) if (!isDigit(s[i])) return false;
  return true;
}
bool parseIdx(const String& s, int n, int& out) {
  String t = s; t.trim();
  if (!isNumber(t)) return false;
  int v = t.toInt();
  if (v < 1 || v > n) return false;
  out = v; return true;
}
void splitFirst(const String& s, String& first, String& rest) {
  String t = s; t.trim();
  int i = -1;
  for (unsigned k = 0; k < t.length(); k++) if (isspace((unsigned char)t[k])) { i = k; break; }
  if (i < 0) { first = t; rest = ""; }
  else { first = t.substring(0, i); rest = t.substring(i + 1); rest.trim(); }
}
String clip(const String& s, int maxc) {  // UTF-8 safe
  int cnt = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    if ((((uint8_t)s[i]) & 0xC0) != 0x80) {
      if (cnt == maxc) return s.substring(0, i) + "…";
      cnt++;
    }
  }
  return s;
}
String oneLine(String s) { s.replace("\n", " "); s.replace("\r", " "); return s; }
String preview(const String& s, int n) { return clip(oneLine(s), n); }

String canon(String w) {
  w.toLowerCase();
  for (const auto& a : ALIASES) if (w == a.w) return String(a.c);
  return "";
}
int lev(const String& a, const String& b) {
  int n = a.length(), m = b.length();
  if (n > 24 || m > 24) return 99;
  int prev[25], cur[25];
  for (int j = 0; j <= m; j++) prev[j] = j;
  for (int i = 1; i <= n; i++) {
    cur[0] = i;
    for (int j = 1; j <= m; j++) {
      int c = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = min(min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + c);
    }
    memcpy(prev, cur, sizeof(int) * (m + 1));
  }
  return prev[m];
}
String suggestSub(String w, std::initializer_list<const char*> allowed) {
  w.toLowerCase();
  if (w.length() < 3) return "";
  for (const auto& a : ALIASES) {
    if (strlen(a.w) < 3) continue;
    if (lev(w, String(a.w)) <= 1)
      for (auto al : allowed) if (strcmp(al, a.c) == 0) return String(a.c);
  }
  return "";
}

// ===================== time =====================
bool timeOk() { return time(nullptr) > 1700000000; }
String fmtDT(time_t t) { struct tm tm; localtime_r(&t, &tm); char b[32]; strftime(b, sizeof b, "%a %d.%m.%Y %H:%M", &tm); return String(b); }
String fmtShort(time_t t) { struct tm tm; localtime_r(&t, &tm); char b[20]; strftime(b, sizeof b, "%d.%m %H:%M", &tm); return String(b); }

// ===================== storage (LittleFS + JSON) =====================
bool loadDoc(const char* path, JsonDocument& d) {
  String tmp = String(path) + ".tmp";
  if (!LittleFS.exists(path) && LittleFS.exists(tmp)) LittleFS.rename(tmp, path);  // recover from power loss
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  return !e;
}
bool saveDoc(const char* path, JsonDocument& d) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t n = serializeJson(d, f);
  f.close();
  if (n == 0) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}
JsonArray docArr(JsonDocument& d, const char* p) {
  loadDoc(p, d);
  if (d.is<JsonArray>()) return d.as<JsonArray>();
  return d.to<JsonArray>();
}

// ===================== UI shared state =====================
void setBusy(const String& s) {
  xSemaphoreTake(uiMux, portMAX_DELAY);
  strlcpy(gBusy, s.c_str(), sizeof(gBusy));
  xSemaphoreGive(uiMux);
}
void clearBusy() { setBusy(""); }

void refreshTodoCache() {
  JsonDocument d; JsonArray a = docArr(d, "/todo.json");
  xSemaphoreTake(uiMux, portMAX_DELAY);
  gTodoLines.clear();
  for (JsonVariant v : a) gTodoLines.push_back(v.as<String>());
  gTodoCount = (int)a.size();
  xSemaphoreGive(uiMux);
}

// ===================== OLED task =====================
enum Screen { SCR_CLOCK, SCR_TODO };

void drawBusy(const char* s) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawUTF8(0, 11, "ESP32 brain");
  u8g2.drawHLine(0, 14, 128);
  u8g2.drawUTF8(0, 38, s);
  u8g2.sendBuffer();
}
void drawClock() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawUTF8(0, 8, WiFi.status() == WL_CONNECTED ? "WiFi" : "No WiFi");
  char r[16]; snprintf(r, sizeof r, "T:%d", (int)gTodoCount);
  u8g2.drawUTF8(128 - u8g2.getStrWidth(r), 8, r);
  u8g2.drawHLine(0, 11, 128);
  char tb[8], db[28];
  if (timeOk()) {
    struct tm tm; time_t n = time(nullptr); localtime_r(&n, &tm);
    strftime(tb, sizeof tb, "%H:%M", &tm);
    strftime(db, sizeof db, "%a %d.%m.%Y", &tm);
  } else { strcpy(tb, "--:--"); strcpy(db, "syncing time..."); }
  u8g2.setFont(u8g2_font_logisoso28_tr);
  u8g2.drawUTF8((128 - u8g2.getStrWidth(tb)) / 2, 47, tb);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawUTF8((128 - u8g2.getStrWidth(db)) / 2, 62, db);
  u8g2.sendBuffer();
}
void drawTodo() {
  std::vector<String> L; int cnt;
  xSemaphoreTake(uiMux, portMAX_DELAY); L = gTodoLines; cnt = gTodoCount; xSemaphoreGive(uiMux);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  String h = "TODO (" + String(cnt) + " open)";
  u8g2.drawUTF8(0, 9, h.c_str());
  u8g2.drawHLine(0, 11, 128);
  int pages = max(1, ((int)L.size() + 4) / 5);
  int page = (millis() / 4000) % pages;
  if (pages > 1) { String p = String(page + 1) + "/" + String(pages); u8g2.drawUTF8(128 - u8g2.getStrWidth(p.c_str()), 9, p.c_str()); }
  if (L.empty()) u8g2.drawUTF8(0, 32, "Nothing to do!");
  for (int i = 0; i < 5; i++) {
    int k = page * 5 + i;
    if (k >= (int)L.size()) break;
    String s = clip(oneLine(String(k + 1) + "." + L[k]), 21);
    u8g2.drawUTF8(0, 22 + i * 10, s.c_str());
  }
  u8g2.sendBuffer();
}

void uiTask(void*) {
  pinMode(BTN_PIN, INPUT_PULLUP);
  Screen screen = SCR_CLOCK; uint32_t since = 0;
  bool raw = HIGH, stable = HIGH; uint32_t changed = 0;
  for (;;) {
    bool s = digitalRead(BTN_PIN);
    if (s != raw) { raw = s; changed = millis(); }
    if (millis() - changed > 40 && raw != stable) {
      stable = raw;
      if (stable == LOW) { screen = (screen == SCR_CLOCK) ? SCR_TODO : SCR_CLOCK; since = millis(); }
    }
    if (screen == SCR_TODO && millis() - since > 30000) screen = SCR_CLOCK;
    char busy[24];
    xSemaphoreTake(uiMux, portMAX_DELAY); strlcpy(busy, gBusy, sizeof busy); xSemaphoreGive(uiMux);
    if (busy[0]) drawBusy(busy);
    else if (screen == SCR_TODO) drawTodo();
    else drawClock();
    vTaskDelay(150 / portTICK_PERIOD_MS);
  }
}

// ===================== HTTPS + Telegram =====================
int httpsJson(const String& url, const String* body, JsonDocument& out, JsonDocument* filter,
              const char* hn, const char* hv, uint16_t to) {
  WiFiClientSecure c; c.setInsecure();  // home project; pin certs if you care
  HTTPClient h;
  h.setTimeout(to); h.setConnectTimeout(to);
  h.useHTTP10(true);  // avoids chunked encoding, lets ArduinoJson read the stream directly
  if (!h.begin(c, url)) return -1;
  if (hn) h.addHeader(hn, hv);
  int code;
  if (body) { h.addHeader("Content-Type", "application/json"); code = h.POST(*body); }
  else code = h.GET();
  if (code == 200) {
    DeserializationError e = filter ? deserializeJson(out, h.getStream(), DeserializationOption::Filter(*filter))
                                    : deserializeJson(out, h.getStream());
    if (e) code = -100;
  }
  h.end();
  return code;
}

String tgUrl(const char* m) { return String("https://api.telegram.org/bot") + BOT_TOKEN + "/" + m; }

long tgSendDoc(JsonDocument& b) {
  b["chat_id"] = (long long)ALLOWED_USER_ID;
  String p; serializeJson(b, p);
  JsonDocument f; f["result"]["message_id"] = true;
  JsonDocument r;
  int c = httpsJson(tgUrl("sendMessage"), &p, r, &f, nullptr, nullptr, 10000);
  return c == 200 ? r["result"]["message_id"].as<long>() : -1;
}
long tgSend(const String& t) { JsonDocument b; b["text"] = t; return tgSendDoc(b); }

void addBtn(JsonArray row, const String& text, const String& data) {
  JsonObject o = row.add<JsonObject>(); o["text"] = text; o["callback_data"] = data;
}
// Splits long text; optional button goes under the last chunk
long tgSendChunks(const String& text, const char* btnText, const String& btnData) {
  long mid = -1; int pos = 0, len = text.length();
  if (len == 0) return -1;
  while (pos < len) {
    int end = min(pos + 3800, len);
    if (end < len) {
      int nl = text.lastIndexOf('\n', end);
      if (nl > pos + 1000) end = nl;
      while (end > pos && (((uint8_t)text[end]) & 0xC0) == 0x80) end--;
    }
    String piece = text.substring(pos, end);
    pos = end;
    if (pos < len && text[pos] == '\n') pos++;
    JsonDocument b; b["text"] = piece;
    if (pos >= len && btnText) {
      JsonArray rows = b["reply_markup"]["inline_keyboard"].to<JsonArray>();
      addBtn(rows.add<JsonArray>(), String(btnText), btnData);
    }
    mid = tgSendDoc(b);
  }
  return mid;
}
void tgSendLong(const String& t) { tgSendChunks(t, nullptr, ""); }

void tgAnswerCb(const String& id, const String& text) {
  JsonDocument b; b["callback_query_id"] = id; if (text.length()) b["text"] = text;
  String p; serializeJson(b, p); JsonDocument r;
  httpsJson(tgUrl("answerCallbackQuery"), &p, r, nullptr, nullptr, nullptr, 8000);
}
void tgMarkSaved(long mid) {
  JsonDocument b; b["chat_id"] = (long long)ALLOWED_USER_ID; b["message_id"] = mid;
  JsonArray rows = b["reply_markup"]["inline_keyboard"].to<JsonArray>();
  addBtn(rows.add<JsonArray>(), "✔ Saved", "noop");
  String p; serializeJson(b, p); JsonDocument r;
  httpsJson(tgUrl("editMessageReplyMarkup"), &p, r, nullptr, nullptr, nullptr, 8000);
}
void tgSetCommands() {
  JsonDocument b; JsonArray c = b["commands"].to<JsonArray>();
  const char* L[][2] = {{"help","All commands"},{"todo","To-do list"},{"note","Notes"},{"hw","Homework"},
                        {"list","Shopping & other lists"},{"ai","Ask the AI"},{"stock","Stock quote"},{"cancel","Cancel current entry"}};
  for (auto& r : L) { JsonObject o = c.add<JsonObject>(); o["command"] = r[0]; o["description"] = r[1]; }
  String p; serializeJson(b, p); JsonDocument r;
  httpsJson(tgUrl("setMyCommands"), &p, r, nullptr, nullptr, nullptr, 8000);
}

void unknownOpt(const char* cmd, const String& w, const char* usage, std::initializer_list<const char*> allowed) {
  String s = "❓ Unknown option '" + w + "' for /" + cmd + ".";
  String sg = suggestSub(w, allowed);
  if (sg.length()) s += " Did you mean '" + sg + "'?";
  s += "\nUsage: " + String(usage) + "\nSee /" + cmd + " help";
  tgSend(s);
}

// ===================== TODO =====================
String todoText(JsonArray a) {
  if (a.size() == 0) return "✅ Todo list is empty. Add one: /todo add <text>";
  String s = "✅ TODO (" + String((int)a.size()) + ")\n"; int i = 1;
  for (JsonVariant v : a) s += String(i++) + ". " + v.as<String>() + "\n";
  return s;
}
void cmdTodo(const String& args) {
  const char* USAGE = "/todo [add <text> | del <n> | edit <n> <text>]";
  String w, r; splitFirst(args, w, r);
  String c = w.length() ? canon(w) : String("show");
  if (c == "help") { tgSendLong(H_TODO); return; }
  JsonDocument d; JsonArray a = docArr(d, "/todo.json");
  if (c == "show") { tgSendLong(todoText(a)); return; }
  if (c == "add") {
    if (!r.length()) { tgSend("Usage: /todo add <text>"); return; }
    if (r.length() > MAX_ITEM) { tgSend("Too long (max 200 characters)."); return; }
    if ((int)a.size() >= MAX_TODO) { tgSend("Todo list is full (50). Delete something first."); return; }
    a.add(r);
    if (!saveDoc("/todo.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
    refreshTodoCache(); tgSendLong("Added.\n\n" + todoText(a)); return;
  }
  if (c == "del") {
    int n; if (!parseIdx(r, a.size(), n)) { tgSend("Give an item number 1-" + String((int)a.size()) + ". Usage: /todo del <n>"); return; }
    String gone = a[n - 1].as<String>(); a.remove(n - 1);
    if (!saveDoc("/todo.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
    refreshTodoCache(); tgSendLong("Deleted: " + gone + "\n\n" + todoText(a)); return;
  }
  if (c == "edit") {
    String ns, txt; splitFirst(r, ns, txt); int n;
    if (!parseIdx(ns, a.size(), n) || !txt.length()) { tgSend("Usage: /todo edit <n> <new text>"); return; }
    if (txt.length() > MAX_ITEM) { tgSend("Too long (max 200 characters)."); return; }
    a[n - 1] = txt;
    if (!saveDoc("/todo.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
    refreshTodoCache(); tgSendLong("Updated.\n\n" + todoText(a)); return;
  }
  unknownOpt("todo", w, USAGE, {"add", "del", "edit", "show", "help"});
}

// ===================== LISTS =====================
String listBlock(const String& name, JsonArray la) {
  String t = name; if (t.length()) t.setCharAt(0, toupper(t[0]));
  String s = "🛒 " + t + " (" + String((int)la.size()) + ")\n"; int i = 1;
  for (JsonVariant v : la) s += String(i++) + ". " + v.as<String>() + "\n";
  return s;
}
void cmdList(const String& args) {
  const char* USAGE = "/list <name> [add <text> | del <n> | edit <n> <text>]";
  String first, rest; splitFirst(args, first, rest);
  String c1 = first.length() ? canon(first) : String("show");
  if (c1 == "help") { tgSendLong(H_LIST); return; }
  JsonDocument d; loadDoc("/lists.json", d);
  JsonObject o = d.is<JsonObject>() ? d.as<JsonObject>() : d.to<JsonObject>();
  if (c1 == "show") {
    if (o.size() == 0) { tgSend("No lists yet. Start one: /list shopping add milk"); return; }
    String s;
    for (JsonPair kv : o) s += listBlock(String(kv.key().c_str()), kv.value().as<JsonArray>()) + "\n";
    tgSendLong(s); return;
  }
  if (c1.length()) { tgSend("Which list? The name goes first, e.g. /list shopping add milk"); return; }
  String name = lower(first);
  if (name.length() > 20) { tgSend("List names are max 20 characters."); return; }
  String sub, r2; splitFirst(rest, sub, r2);
  String c = sub.length() ? canon(sub) : String("show");
  if (c == "help") { tgSendLong(H_LIST); return; }
  if (c == "show") {
    if (!o[name].is<JsonArray>()) {
      String s = "No list named '" + name + "'.";
      if (o.size()) { s += " Existing:"; for (JsonPair kv : o) s += " " + String(kv.key().c_str()); }
      tgSend(s + "\nCreate it: /list " + name + " add <item>"); return;
    }
    tgSendLong(listBlock(name, o[name].as<JsonArray>())); return;
  }
  if (c == "add") {
    if (!r2.length()) { tgSend("Usage: /list " + name + " add <item>"); return; }
    if (r2.length() > MAX_ITEM) { tgSend("Too long (max 200 characters)."); return; }
    if (!o[name].is<JsonArray>()) {
      if ((int)o.size() >= MAX_LISTS) { tgSend("Max 10 lists. Empty one out first."); return; }
      o[name].to<JsonArray>();
    }
    JsonArray la = o[name].as<JsonArray>();
    if ((int)la.size() >= MAX_LIST_ITEMS) { tgSend("That list is full (50 items)."); return; }
    la.add(r2);
    if (!saveDoc("/lists.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
    tgSendLong("Added.\n\n" + listBlock(name, la)); return;
  }
  if (c == "del" || c == "edit") {
    if (!o[name].is<JsonArray>()) { tgSend("No list named '" + name + "'."); return; }
    JsonArray la = o[name].as<JsonArray>(); int n;
    if (c == "del") {
      if (!parseIdx(r2, la.size(), n)) { tgSend("Give an item number 1-" + String((int)la.size()) + ". Usage: /list " + name + " del <n>"); return; }
      String gone = la[n - 1].as<String>(); la.remove(n - 1);
      String out = "Deleted: " + gone + "\n\n";
      if (la.size() == 0) { o.remove(name); out += "List '" + name + "' is now empty and was removed."; }
      else out += listBlock(name, la);
      if (!saveDoc("/lists.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
      tgSendLong(out); return;
    }
    String ns, txt; splitFirst(r2, ns, txt);
    if (!parseIdx(ns, la.size(), n) || !txt.length()) { tgSend("Usage: /list " + name + " edit <n> <new text>"); return; }
    if (txt.length() > MAX_ITEM) { tgSend("Too long (max 200 characters)."); return; }
    la[n - 1] = txt;
    if (!saveDoc("/lists.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
    tgSendLong("Updated.\n\n" + listBlock(name, la)); return;
  }
  unknownOpt("list", sub, USAGE, {"add", "del", "edit", "show", "help"});
}

// ===================== NOTES =====================
String noteLine(JsonObject n, int num) {
  time_t t = (time_t)n["t"].as<long long>();
  String s = "#" + String(num) + " ";
  if (t > 0) s += "[" + fmtShort(t) + "] ";
  if (n["ai"] | false) s += "[AI] ";
  return s + n["x"].as<String>();
}
void showNotes(int mode) {  // 0 recent, 1 all, 2 ai only
  JsonDocument d; JsonArray a = docArr(d, "/notes.json");
  std::vector<int> idx;
  for (int i = 0; i < (int)a.size(); i++) if (mode != 2 || (a[i]["ai"] | false)) idx.push_back(i);
  if (idx.empty()) { tgSend(mode == 2 ? "No kept AI answers yet. Use /ai history keep <n>." : "No notes yet. Add one: /note buy milk"); return; }
  int start = 0; if (mode == 0 && idx.size() > 10) start = idx.size() - 10;
  String s = mode == 2 ? "🤖 Kept AI answers (" + String((int)idx.size()) + ")\n\n"
                       : "📝 Notes (" + String((int)(idx.size() - start)) + " of " + String((int)idx.size()) + (start ? ", /note ls all for everything" : "") + ")\n\n";
  for (int k = start; k < (int)idx.size(); k++) s += noteLine(a[idx[k]].as<JsonObject>(), idx[k] + 1) + "\n\n";
  tgSendLong(s);
}
void noteAdd(const String& text) {
  if (!text.length()) { tgSend("Usage: /note add <text>"); return; }
  if (text.length() > MAX_NOTE_LEN) { tgSend("Note too long (max 1000 characters)."); return; }
  JsonDocument d; JsonArray a = docArr(d, "/notes.json");
  if ((int)a.size() >= MAX_NOTES) { tgSend("Notes are full (300). Delete some with /note del <n>."); return; }
  JsonObject o = a.add<JsonObject>(); o["t"] = timeOk() ? (long long)time(nullptr) : 0LL; o["x"] = text;
  if (!saveDoc("/notes.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
  tgSend("📝 Saved as #" + String((int)a.size()));
}
void noteSearch(const String& w) {
  if (!w.length()) { tgSend("Usage: /note search <word>"); return; }
  JsonDocument d; JsonArray a = docArr(d, "/notes.json");
  String lw = lower(w), s; int hits = 0;
  for (int i = 0; i < (int)a.size(); i++) {
    if (lower(a[i]["x"].as<String>()).indexOf(lw) >= 0) {
      if (hits < 15) s += noteLine(a[i].as<JsonObject>(), i + 1) + "\n\n";
      hits++;
    }
  }
  if (!hits) { tgSend("No notes contain '" + w + "'."); return; }
  tgSendLong("🔎 " + String(hits) + " match(es)" + (hits > 15 ? " (showing 15)" : "") + "\n\n" + s);
}
void noteDel(const String& r) {
  JsonDocument d; JsonArray a = docArr(d, "/notes.json"); int n;
  if (!parseIdx(r, a.size(), n)) { tgSend("Give a note number 1-" + String((int)a.size()) + ". Usage: /note del <n>"); return; }
  String gone = preview(a[n - 1]["x"].as<String>(), 50); a.remove(n - 1);
  if (!saveDoc("/notes.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
  tgSend("🗑 Deleted note #" + String(n) + ": " + gone + "\n(Numbers after it shifted down by one.)");
}
void cmdNote(const String& args) {
  if (!args.length()) { showNotes(0); return; }
  String first, rest; splitFirst(args, first, rest);
  String lw = lower(first);
  if (lw == "help") { tgSendLong(H_NOTE); return; }
  if (lw == "ls") { String r = lower(rest); showNotes(r == "all" ? 1 : (r == "ai" ? 2 : 0)); return; }
  if (lw == "ai") { showNotes(2); return; }
  if (lw == "search") { noteSearch(rest); return; }
  if (lw == "del" || lw == "rm") { noteDel(rest); return; }
  if (lw == "add") { noteAdd(rest); return; }
  // Not a reserved word: if it's a likely mix-up used like a command, ask instead of saving
  String c = canon(first); bool bare = !rest.length(), num = isNumber(rest);
  String hint;
  if (c == "del" && (bare || num)) hint = "/note del " + (bare ? String("<n>") : rest);
  else if (c == "show" && bare) hint = "/note ls";
  else if (c == "search" && bare) hint = "/note search <word>";
  else if (c == "help" && bare) hint = "/note help";
  else if (c == "add" && bare) hint = "/note add <text>";
  if (hint.length()) { tgSend("Did you mean " + hint + " ?\nTo save it as a note instead, send: /note add " + args); return; }
  noteAdd(args);
}

// ===================== HOMEWORK =====================
void loadHw(std::vector<HwItem>& v) {
  JsonDocument d; JsonArray a = docArr(d, "/hw.json");
  for (JsonObject o : a) {
    HwItem h; h.s = o["s"].as<String>(); h.d = o["d"].as<long long>(); h.x = o["x"].as<String>();
    h.a72 = o["a72"] | false; h.a24 = o["a24"] | false; v.push_back(h);
  }
}
bool saveHw(std::vector<HwItem>& v) {
  std::sort(v.begin(), v.end(), [](const HwItem& a, const HwItem& b) { return a.d < b.d; });
  JsonDocument d; JsonArray a = d.to<JsonArray>();
  for (auto& h : v) { JsonObject o = a.add<JsonObject>(); o["s"] = h.s; o["d"] = h.d; o["x"] = h.x; o["a72"] = h.a72; o["a24"] = h.a24; }
  return saveDoc("/hw.json", d);
}
void initFlags(HwItem& h) {  // skip warnings whose window has already started
  long long left = h.d - (long long)time(nullptr);
  h.a72 = left < 72LL * 3600; h.a24 = left < 24LL * 3600;
}
String hwText(std::vector<HwItem>& v) {
  if (v.empty()) return "📚 No homework saved. Add one with /hw add";
  String s = "📚 HOMEWORK (" + String((int)v.size()) + ")\n"; time_t now = time(nullptr);
  for (int i = 0; i < (int)v.size(); i++)
    s += String(i + 1) + ". " + (v[i].d < now ? "⚠ OVERDUE " : "") + v[i].s + ", due " + fmtDT((time_t)v[i].d) + "\n    " + v[i].x + "\n";
  return s;
}
void rememberSubject(const String& s) {
  JsonDocument d; JsonArray a = docArr(d, "/subjects.json");
  for (int i = a.size() - 1; i >= 0; i--) if (lower(a[i].as<String>()) == lower(s)) a.remove(i);
  JsonDocument d2; JsonArray b = d2.to<JsonArray>(); b.add(s);
  for (JsonVariant v : a) { if (b.size() >= 12) break; b.add(v.as<String>()); }
  saveDoc("/subjects.json", d2);
}

bool parseDue(String in, long long& out, String& err) {
  if (!timeOk()) { err = "Clock isn't synced yet, try again in a minute."; return false; }
  in.trim(); in.toLowerCase();
  String d, t; splitFirst(in, d, t);
  int hh = 23, mm = 59;
  if (t.length()) {
    int c = t.indexOf(':');
    String hs = c > 0 ? t.substring(0, c) : String(""), ms = c > 0 ? t.substring(c + 1) : String("");
    if (!isNumber(hs) || !isNumber(ms) || hs.toInt() > 23 || ms.toInt() > 59) { err = "Time must look like 08:30."; return false; }
    hh = hs.toInt(); mm = ms.toInt();
  }
  time_t now = time(nullptr); struct tm n; localtime_r(&now, &n);
  struct tm tm = n; tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = 0; tm.tm_isdst = -1;
  bool yearless = false; int day = 0, mon = 0;
  if (!d.length()) { err = "Please give a date."; return false; }
  if (d == "today") {}
  else if (d == "tomorrow" || d == "tmr") tm.tm_mday += 1;
  else if (isAlpha(d[0])) {
    const char* wd[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"}; int target = -1;
    for (int i = 0; i < 7; i++) if (d.startsWith(wd[i])) target = i;
    if (target < 0) { err = "I couldn't read that date."; return false; }
    int diff = (target - n.tm_wday + 7) % 7; if (diff == 0) diff = 7;
    tm.tm_mday += diff;
  } else {
    d.replace('.', '-'); d.replace('/', '-');
    int p1 = d.indexOf('-'), p2 = p1 >= 0 ? d.indexOf('-', p1 + 1) : -1;
    if (p1 < 0) { err = "I couldn't read that date."; return false; }
    String ds = d.substring(0, p1);
    String ms = p2 < 0 ? d.substring(p1 + 1) : d.substring(p1 + 1, p2);
    String ys = p2 < 0 ? String("") : d.substring(p2 + 1);
    if (!isNumber(ds) || !isNumber(ms) || (p2 >= 0 && !isNumber(ys))) { err = "I couldn't read that date."; return false; }
    day = ds.toInt(); mon = ms.toInt();
    int year = n.tm_year + 1900;
    if (p2 >= 0) { year = ys.toInt(); if (year < 100) year += 2000; } else yearless = true;
    if (mon < 1 || mon > 12 || day < 1 || day > 31) { err = "That isn't a valid date."; return false; }
    tm.tm_mday = day; tm.tm_mon = mon - 1; tm.tm_year = year - 1900;
  }
  struct tm chk = tm; time_t r = mktime(&chk);
  if (day) {
    if (yearless && r <= now) { tm.tm_year += 1; chk = tm; r = mktime(&chk); }
    if (chk.tm_mday != day || chk.tm_mon != mon - 1) { err = "That date doesn't exist."; return false; }
  }
  if (r <= now) { err = "That's in the past."; return false; }
  out = (long long)r; return true;
}

void askSubject() {
  JsonDocument sd; JsonArray sa = docArr(sd, "/subjects.json");
  flow.subs.clear(); for (JsonVariant v : sa) flow.subs.push_back(v.as<String>());
  JsonDocument b; b["text"] = "What subject is this for?" + String(flow.subs.size() ? "\n(Tap one or type a new subject)" : "");
  if (flow.subs.size()) {
    JsonArray rows = b["reply_markup"]["inline_keyboard"].to<JsonArray>(); JsonArray row;
    for (int i = 0; i < (int)flow.subs.size(); i++) {
      if (i % 2 == 0) row = rows.add<JsonArray>();
      addBtn(row, flow.subs[i], "s:" + String(i));
    }
  }
  tgSendDoc(b);
}
void askDate() { tgSend("When is it due? Type a date like 20-10-26, 20.10, fri or tomorrow. Add a time (e.g. 20-10-26 08:00) to override 23:59."); }

void startHwAdd() { flow = Flow(); flow.type = F_HW_ADD; flow.step = S_SUBJ; flow.last = millis(); askSubject(); }
void startHwEdit(int n) {
  std::vector<HwItem> v; loadHw(v);
  if (n < 1 || n > (int)v.size()) { tgSend("Give an item number 1-" + String((int)v.size()) + ". Usage: /hw edit <n>"); return; }
  flow = Flow(); flow.type = F_HW_EDIT; flow.step = S_FIELD; flow.idx = n - 1; flow.last = millis();
  JsonDocument b;
  b["text"] = "Editing #" + String(n) + ": " + v[n - 1].s + ", due " + fmtDT((time_t)v[n - 1].d) + "\n" + v[n - 1].x + "\n\nWhat do you want to change?";
  JsonArray row = b["reply_markup"]["inline_keyboard"].to<JsonArray>().add<JsonArray>();
  addBtn(row, "Subject", "f:s"); addBtn(row, "Due date", "f:d"); addBtn(row, "Description", "f:x");
  tgSendDoc(b);
}

void flowInput(String text) {
  text.trim();
  if (flow.type == F_HW_ADD) {
    if (flow.step == S_SUBJ) {
      if (!text.length() || text.length() > 40) { tgSend("Subject should be 1-40 characters. What subject is this for?"); return; }
      flow.subj = text; flow.step = S_DATE; askDate(); return;
    }
    if (flow.step == S_DATE) {
      String err; long long due;
      if (!parseDue(text, due, err)) { tgSend("⚠ " + err + " Try again, or /cancel."); return; }
      flow.due = due; flow.step = S_DESC;
      tgSend("You have a new HW item for " + flow.subj + " due " + fmtDT((time_t)due) + ". Please send a description."); return;
    }
    if (flow.step == S_DESC) {
      if (!text.length() || text.length() > MAX_ITEM) { tgSend("Description should be 1-200 characters. Send it again."); return; }
      std::vector<HwItem> v; loadHw(v);
      if (v.size() >= 50) { flow.type = F_NONE; tgSend("Homework list is full (50). Delete something first."); return; }
      HwItem h; h.s = flow.subj; h.d = flow.due; h.x = text; initFlags(h); v.push_back(h);
      if (!saveHw(v)) { flow.type = F_NONE; tgSend("⚠ Storage error, not saved."); return; }
      rememberSubject(flow.subj);
      String m = "✔ Saved: " + h.s + ", due " + fmtDT((time_t)h.d) + ", " + h.x + "\n";
      if (!h.a72) m += "Reminders at 72h and 24h before.";
      else if (!h.a24) m += "Due within 72h, so only the 24h reminder will fire.";
      else m += "Due within 24h, so no advance reminder will fire.";
      flow.type = F_NONE; tgSend(m); return;
    }
  }
  if (flow.type == F_HW_EDIT) {
    if (flow.step == S_FIELD) {
      String t = lower(text);
      if (t == "subject" || t == "s") flow.field = 's';
      else if (t == "date" || t == "due" || t == "due date" || t == "d") flow.field = 'd';
      else if (t == "description" || t == "desc" || t == "x") flow.field = 'x';
      else { tgSend("Pick one: subject, due date or description (or /cancel)."); return; }
      flow.step = S_VALUE;
      if (flow.field == 'd') askDate();
      else tgSend(flow.field == 's' ? "New subject?" : "New description?");
      return;
    }
    if (flow.step == S_VALUE) {
      std::vector<HwItem> v; loadHw(v);
      if (flow.idx < 0 || flow.idx >= (int)v.size()) { flow.type = F_NONE; tgSend("That item no longer exists."); return; }
      HwItem& h = v[flow.idx];
      if (flow.field == 's') { if (!text.length() || text.length() > 40) { tgSend("Subject should be 1-40 characters."); return; } h.s = text; rememberSubject(text); }
      else if (flow.field == 'x') { if (!text.length() || text.length() > MAX_ITEM) { tgSend("Description should be 1-200 characters."); return; } h.x = text; }
      else {
        String err; long long due;
        if (!parseDue(text, due, err)) { tgSend("⚠ " + err + " Try again, or /cancel."); return; }
        h.d = due; initFlags(h);
      }
      bool ok = saveHw(v); flow.type = F_NONE;
      if (!ok) { tgSend("⚠ Storage error, not saved."); return; }
      tgSendLong("✔ Updated.\n\n" + hwText(v)); return;
    }
  }
}

void cmdHw(const String& args) {
  const char* USAGE = "/hw [add | edit <n> | del <n>]";
  String w, r; splitFirst(args, w, r);
  String c = w.length() ? canon(w) : String("show");
  if (c == "help") { tgSendLong(H_HW); return; }
  if (c == "add") { if (!timeOk()) { tgSend("Clock isn't synced yet, try again in a minute."); return; } startHwAdd(); return; }
  std::vector<HwItem> v; loadHw(v);
  if (c == "show") { tgSendLong(hwText(v)); return; }
  if (c == "del") {
    int n; if (!parseIdx(r, v.size(), n)) { tgSend("Give an item number 1-" + String((int)v.size()) + ". Usage: /hw del <n>"); return; }
    HwItem gone = v[n - 1]; v.erase(v.begin() + (n - 1));
    if (!saveHw(v)) { tgSend("⚠ Storage error, not saved."); return; }
    tgSendLong("✔ Done: " + gone.s + ", " + gone.x + "\n\n" + hwText(v)); return;
  }
  if (c == "edit") {
    int n; if (!parseIdx(r, v.size(), n)) { tgSend("Give an item number 1-" + String((int)v.size()) + ". Usage: /hw edit <n>"); return; }
    startHwEdit(n); return;
  }
  unknownOpt("hw", w, USAGE, {"add", "del", "edit", "show", "help"});
}

void checkReminders() {
  if (!timeOk() || WiFi.status() != WL_CONNECTED) return;
  std::vector<HwItem> v; loadHw(v);
  if (v.empty()) return;
  long long now = (long long)time(nullptr); bool changed = false;
  for (int i = 0; i < (int)v.size(); i++) {
    HwItem& h = v[i]; long long left = h.d - now;
    if (left <= 0) { if (!h.a24 || !h.a72) { h.a24 = h.a72 = true; changed = true; } continue; }  // missed while offline
    int hrs = 0;
    if (!h.a24 && left <= 24LL * 3600) hrs = 24;
    else if (!h.a72 && left <= 72LL * 3600) hrs = 72;
    if (!hrs) continue;
    setBusy("Sending reminder");
    String m = "⏰ HW due in under " + String(hrs) + "h\n" + h.s + ", due " + fmtDT((time_t)h.d) + "\n" + h.x + "\nMark done: /hw del " + String(i + 1);
    if (tgSend(m) >= 0) { if (hrs == 24) h.a24 = h.a72 = true; else h.a72 = true; changed = true; }
    clearBusy();
  }
  if (changed) saveHw(v);
}
// ===================== AI (Groq) =======================
// Removes any <think>...</think> block (including an unfinished one)
void stripThink(String& s) {
  int a;
  while ((a = s.indexOf("<think>")) >= 0) {
    int b = s.indexOf("</think>", a);
    if (b < 0) { s.remove(a); break; }
    s.remove(a, b + 8 - a);
  }
  s.trim();
}
int groqAsk(const String& prompt, String& out) {
  // Build OpenAI-compatible chat completion payload
  JsonDocument req;
  req["model"] = GROQ_MODEL;
  req["max_tokens"] = 600;
  // qwen3-32b "thinks" by default and puts <think>...</think> in the answer; turn that off
  if (String(GROQ_MODEL).indexOf("qwen3") >= 0) { req["reasoning_effort"] = "none"; req["reasoning_format"] = "hidden"; }
  
  JsonArray messages = req["messages"].to<JsonArray>();
  JsonObject systemMsg = messages.add<JsonObject>();
  systemMsg["role"] = "system";
  systemMsg["content"] = "Answer concisely in under 150 words. Use plain text only, no markdown.";

  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = prompt;

  String body;
  serializeJson(req, body);

  // Filter output to extract only message text
  JsonDocument f;
  f["choices"][0]["message"]["content"] = true;

  JsonDocument res;
  String url = "https://api.groq.com/openai/v1/chat/completions";
  String authHeader = String("Bearer ") + GROQ_KEY;

  // Execute HTTPS call
  uint32_t t0 = millis();
  int code = httpsJson(url, &body, res, &f, "Authorization", authHeader.c_str(), 20000);
  Serial.printf("Groq: code %d after %lu ms\n", code, (unsigned long)(millis() - t0));

  if (code == 200) {
    out = res["choices"][0]["message"]["content"].as<String>();
    stripThink(out);
  }
  return code;
}
String groqErr(int code) {
  if (code == 429) return "⚠ Groq rate limit hit (free tier). Wait a minute and retry.";
  if (code == 401) return "⚠ Groq rejected the API key (401). Check GROQ_KEY in secrets.h.";
  if (code == 403) return "⚠ Groq refused access (403). Check the key and permissions.";
  if (code == 400) return "⚠ Groq rejected the request (400). Check GROQ_MODEL in secrets.h.";
  if (code == 404) return "⚠ Groq model not found (404). Check GROQ_MODEL in secrets.h.";
  if (code >= 500) return "⚠ Groq server error (" + String(code) + "). Try again shortly.";
  return "⚠ Couldn't reach Groq or it timed out (code " + String(code) + ").";
}
void askAi(const String& prompt) {
  if (prompt.length() > MAX_PROMPT) { 
    tgSend("Question too long (max 1000 characters)."); 
    return; 
  }
  
  tgSend("🤔 Thinking...");
  setBusy("Asking AI..."); 
  
  String ans; 
  int code = groqAsk(prompt, ans);
  if (code == 500 || code == 502 || code == 503) { setBusy("AI busy, retrying"); delay(3000); ans = ""; code = groqAsk(prompt, ans); }
  clearBusy();

  if (code != 200) { 
    tgSend(groqErr(code)); 
    return; 
  }
  
  if (!ans.length()) { 
    tgSend("⚠ AI returned no answer. Try rephrasing."); 
    return; 
  }

  // Save answer to temporary RAM ring buffer
  AiAns& s = aiRam[aiHead]; 
  aiHead = (aiHead + 1) % AI_RAM;
  s.id = esp_random() | 1; 
  s.q = prompt; 
  s.a = ans; 
  s.t = timeOk() ? time(nullptr) : 0; 
  s.saved = false;

  tgSendChunks(ans, "💾 Save", "sv:" + String(s.id));
}
int saveAi(uint32_t id, String& msg) {  // 1 saved, 2 already, 0 expired
  AiAns* s = nullptr;
  for (auto& x : aiRam) if (x.id == id) s = &x;
  if (!s) { msg = "Answer expired. Ask again, then tap Save soon after."; return 0; }
  if (s->saved) { msg = "Already saved."; return 2; }
  JsonDocument d; JsonArray a = docArr(d, "/aihist.json");
  String removed;
  while ((int)a.size() >= MAX_AI) { removed = preview(a[0]["q"].as<String>(), 40); a.remove(0); }
  JsonObject o = a.add<JsonObject>(); o["id"] = s->id; o["t"] = (long long)s->t; o["q"] = s->q; o["a"] = s->a;
  if (!saveDoc("/aihist.json", d)) { msg = "⚠ Storage error, not saved."; return 0; }
  s->saved = true;
  int n = a.size();
  msg = "✔ Saved (" + String(n) + "/" + String(MAX_AI) + ")";
  if (removed.length()) msg += ". Removed oldest entry: '" + removed + "'";
  if (n == AI_WARN_AT && !removed.length()) msg += "\nArchive is nearly full. Once it hits 20, the oldest answer is deleted automatically with each new save. Keep important ones with /ai history keep <n>.";
  return 1;
}
String histLine(JsonArray a, int idx, int n) {
  time_t t = (time_t)a[idx]["t"].as<long long>();
  return String(n) + ". " + (t > 0 ? "[" + fmtShort(t) + "] " : String("")) + preview(a[idx]["q"].as<String>(), 45);
}
void aiHistory(const String& args) {
  const char* USAGE = "/ai history [<n> | search <word> | del <n> | keep <n>]";
  String sub, r; splitFirst(args, sub, r);
  if (lower(sub) == "help") { tgSendLong(H_AI); return; }
  JsonDocument d; JsonArray a = docArr(d, "/aihist.json"); int size = a.size();
  String c = sub.length() ? canon(sub) : String("show"); int n;
  if (isNumber(sub)) {
    if (!parseIdx(sub, size, n)) { tgSend("No entry " + sub + ". You have " + String(size) + "."); return; }
    JsonObject o = a[size - n]; time_t t = (time_t)o["t"].as<long long>();
    tgSendLong("🤖 #" + String(n) + (t > 0 ? " [" + fmtDT(t) + "]" : String("")) + "\nQ: " + o["q"].as<String>() + "\n\nA: " + o["a"].as<String>()); return;
  }
  if (c == "show") {
    if (!size) { tgSend("No saved AI answers yet. Tap 💾 Save under an /ai reply."); return; }
    String s = "🤖 AI history (" + String(size) + "/" + String(MAX_AI) + ", newest first)\n";
    for (int k = 1; k <= size; k++) s += histLine(a, size - k, k) + "\n";
    tgSendLong(s + "\n/ai history <n> to read one."); return;
  }
  if (c == "search") {
    if (!r.length()) { tgSend("Usage: /ai history search <word>"); return; }
    String lw = lower(r), s; int hits = 0;
    for (int k = 1; k <= size; k++) {
      int i = size - k;
      if (lower(a[i]["q"].as<String>()).indexOf(lw) >= 0 || lower(a[i]["a"].as<String>()).indexOf(lw) >= 0) { s += histLine(a, i, k) + "\n"; hits++; }
    }
    tgSendLong(hits ? "🔎 " + String(hits) + " match(es)\n" + s : "No saved answers contain '" + r + "'."); return;
  }
  if (c == "del" || c == "keep") {
    if (!parseIdx(r, size, n)) { tgSend("Give an entry number 1-" + String(size) + ". Usage: /ai history " + String(c == "del" ? "del" : "keep") + " <n>"); return; }
    JsonObject o = a[size - n]; String pv = preview(o["q"].as<String>(), 50);
    if (c == "del") {
      a.remove(size - n);
      if (!saveDoc("/aihist.json", d)) { tgSend("⚠ Storage error, not saved."); return; }
      tgSend("🗑 Removed from history: " + pv); return;
    }
    uint32_t id = o["id"].as<uint32_t>();
    JsonDocument nd; JsonArray na = docArr(nd, "/notes.json");
    for (JsonObject x : na) if ((x["src"] | 0u) == id) { tgSend("Already in notes."); return; }
    if ((int)na.size() >= MAX_NOTES) { tgSend("Notes are full (300). Delete some first."); return; }
    JsonObject nn = na.add<JsonObject>();
    nn["t"] = o["t"].as<long long>(); nn["x"] = "Q: " + o["q"].as<String>() + "\n\nA: " + o["a"].as<String>(); nn["ai"] = true; nn["src"] = id;
    if (!saveDoc("/notes.json", nd)) { tgSend("⚠ Storage error, not saved."); return; }
    tgSend("✔ Kept in notes: '" + pv + "'\nFind it with /note ai"); return;
  }
  unknownOpt("ai history", sub, USAGE, {"search", "del", "keep", "show"});
}
void cmdAi(const String& args) {
  if (!args.length()) { tgSend("Usage: /ai <question>   (see /ai help)"); return; }
  String first, rest; splitFirst(args, first, rest);
  String lw = lower(first);
  if (lw == "help") { tgSendLong(H_AI); return; }
  String c = canon(first);
  if (c == "history") {
    String f2, r2; splitFirst(rest, f2, r2); String c2 = canon(f2);
    bool sure = lw == "history" || !rest.length() || isNumber(f2) || c2 == "search" || c2 == "del" || c2 == "keep";
    if (sure) { aiHistory(rest); return; }
  }
  if (WiFi.status() != WL_CONNECTED) { tgSend("No WiFi right now."); return; }
  askAi(args);
}

// ===================== STOCK (Finnhub) =====================
void cmdStock(const String& args) {
  String a = args; a.trim(); String sym, junk; splitFirst(a, sym, junk); sym.toUpperCase();
  if (!sym.length() || sym == "HELP" || sym == "?") { tgSendLong(H_STOCK); return; }
  for (unsigned i = 0; i < sym.length(); i++) if (!(isAlphaNumeric(sym[i]) || sym[i] == '.' || sym[i] == '-' || sym[i] == ':')) { tgSend("Ticker looks odd. Example: /stock AAPL"); return; }
  setBusy("Fetching " + sym);
  JsonDocument qf; qf["c"] = true; qf["d"] = true; qf["dp"] = true; qf["t"] = true;
  JsonDocument q;
  int code = httpsJson("https://finnhub.io/api/v1/quote?symbol=" + sym, nullptr, q, &qf, "X-Finnhub-Token", FINNHUB_KEY, 10000);
  if (code != 200) {
    clearBusy();
    if (code == 403) tgSend("⚠ Finnhub's free plan doesn't cover " + sym + " (non-US tickers such as Warsaw ones need a paid plan).");
    else if (code == 429) tgSend("⚠ Finnhub rate limit hit. Try again in a minute.");
    else if (code == 401) tgSend("⚠ Finnhub rejected the API key (401). Check secrets.h.");
    else tgSend("⚠ Couldn't fetch " + sym + " (code " + String(code) + ").");
    return;
  }
  float price = q["c"] | 0.0f; long long qt = q["t"].as<long long>();
  if (price == 0.0f && qt == 0) {  // unknown ticker: Finnhub returns zeros
    JsonDocument sf; sf["result"][0]["symbol"] = true; sf["result"][0]["description"] = true;
    JsonDocument sr;
    int sc = httpsJson("https://finnhub.io/api/v1/search?q=" + sym, nullptr, sr, &sf, "X-Finnhub-Token", FINNHUB_KEY, 10000);
    clearBusy();
    String s = "❓ No quote for '" + sym + "'.";
    if (sc == 200) {
      JsonArray res = sr["result"].as<JsonArray>(); int n = 0; String list;
      for (JsonObject o : res) { if (n++ >= 5) break; list += "\n" + o["symbol"].as<String>() + ": " + o["description"].as<String>(); }
      s += n ? " Did you mean:" + list : String(" No similar symbols found.");
    }
    tgSend(s); return;
  }
  String name = sym, cur = "";
  bool cached = false;
  for (auto& nc : nameCache) if (nc.sym == sym) { name = nc.name; cur = nc.cur; cached = true; }
  if (!cached) {
    JsonDocument pf; pf["name"] = true; pf["currency"] = true; JsonDocument p;
    if (httpsJson("https://finnhub.io/api/v1/stock/profile2?symbol=" + sym, nullptr, p, &pf, "X-Finnhub-Token", FINNHUB_KEY, 10000) == 200) {
      String n = p["name"] | ""; String c = p["currency"] | "";
      if (n.length()) { name = n; cur = c; }
    }
    nameCache[ncHead] = {sym, name, cur}; ncHead = (ncHead + 1) % 8;
  }
  clearBusy();
  char b1[16]; snprintf(b1, sizeof b1, "%.2f", price);
  String s = "📈 " + name + " (" + sym + ")\nPrice:  " + b1 + (cur.length() ? " " + cur : String("")) + "\n";
  if (q["d"].isNull() || q["dp"].isNull()) s += "Change: n/a\n";
  else {
    float d = q["d"] | 0.0f, dp = q["dp"] | 0.0f; char b2[40];
    snprintf(b2, sizeof b2, "%s %+.2f (%+.2f%%)", d >= 0 ? "▲" : "▼", d, dp);
    s += "Change: " + String(b2) + "\n";
  }
  if (qt > 0) s += "As of:  " + fmtDT((time_t)qt) + " (last quote)";
  tgSend(s);
}

// ===================== help routing =====================
void showHelp(String w) {
  w = lower(w);
  if (w == "notes" || w == "n") w = "note";
  if (w == "todos" || w == "tasks" || w == "task") w = "todo";
  if (w == "homework") w = "hw";
  if (w == "lists" || w == "shopping") w = "list";
  if (w == "ask") w = "ai";
  if (w == "quote" || w == "price") w = "stock";
  if (w == "todo") tgSendLong(H_TODO);
  else if (w == "note") tgSendLong(H_NOTE);
  else if (w == "hw") tgSendLong(H_HW);
  else if (w == "list") tgSendLong(H_LIST);
  else if (w == "ai") tgSendLong(H_AI);
  else if (w == "stock") tgSendLong(H_STOCK);
  else tgSendLong(H_INDEX);
}

// ===================== update handling =====================
void routeCommand(String cmd, String args) {
  if (cmd == "shopping") { cmd = "list"; args = "shopping " + args; args.trim(); }
  else if (cmd == "notes" || cmd == "n") cmd = "note";
  else if (cmd == "todos" || cmd == "task" || cmd == "tasks") cmd = "todo";
  else if (cmd == "homework") cmd = "hw";
  else if (cmd == "ask") cmd = "ai";
  else if (cmd == "quote" || cmd == "price") cmd = "stock";
  else if (cmd == "lists") cmd = "list";
  else if (cmd == "start") cmd = "help";
  if (cmd == "help") showHelp(args);
  else if (cmd == "todo") cmdTodo(args);
  else if (cmd == "note") cmdNote(args);
  else if (cmd == "hw") cmdHw(args);
  else if (cmd == "list") cmdList(args);
  else if (cmd == "ai") cmdAi(args);
  else if (cmd == "stock") cmdStock(args);
  else tgSend("Unknown command /" + cmd + ". Try /help.");
}

void handleText(String text) {
  text.trim();
  if (!text.length()) return;
  if (text[0] == '/') {
    String first, rest; splitFirst(text, first, rest);
    String cmd = first.substring(1); int at = cmd.indexOf('@'); if (at >= 0) cmd = cmd.substring(0, at);
    cmd.toLowerCase();
    if (cmd == "cancel") {
      if (flow.type != F_NONE) { flow.type = F_NONE; tgSend("✖ Cancelled."); } else tgSend("Nothing to cancel.");
      return;
    }
    if (flow.type != F_NONE) { flow.type = F_NONE; tgSend("↩ Unfinished entry cancelled."); }
    routeCommand(cmd, rest);
  } else if (flow.type != F_NONE) {
    flow.last = millis(); flowInput(text);
  } else tgSend("Send a command, e.g. /todo or /help.");
}

void handleCallback(const String& cbid, const String& data, long mid) {
  if (data == "noop") { tgAnswerCb(cbid, ""); return; }
  if (data.startsWith("sv:")) {
    uint32_t id = strtoul(data.c_str() + 3, nullptr, 10); String msg;
    int r = saveAi(id, msg);
    tgAnswerCb(cbid, r == 1 ? "Saved" : (r == 2 ? "Already saved" : "Not saved"));
    tgSend(msg);
    if (r == 1 || r == 2) tgMarkSaved(mid);
    return;
  }
  if (data.startsWith("s:") && flow.type == F_HW_ADD && flow.step == S_SUBJ) {
    int i = data.substring(2).toInt(); tgAnswerCb(cbid, "");
    if (i >= 0 && i < (int)flow.subs.size()) { flow.last = millis(); flowInput(flow.subs[i]); }
    return;
  }
  if (data.startsWith("f:") && flow.type == F_HW_EDIT && flow.step == S_FIELD) {
    tgAnswerCb(cbid, ""); flow.last = millis();
    char f = data[2]; flowInput(f == 's' ? "subject" : (f == 'd' ? "date" : "description"));
    return;
  }
  tgAnswerCb(cbid, "Expired");
}

void pollTelegram() {
  JsonDocument f;
  f["result"][0]["update_id"] = true;
  f["result"][0]["message"]["from"]["id"] = true;
  f["result"][0]["message"]["text"] = true;
  f["result"][0]["callback_query"]["id"] = true;
  f["result"][0]["callback_query"]["from"]["id"] = true;
  f["result"][0]["callback_query"]["data"] = true;
  f["result"][0]["callback_query"]["message"]["message_id"] = true;
  JsonDocument r;
  char off[24]; snprintf(off, sizeof off, "%lld", tgOffset);
  String url = tgUrl("getUpdates") + "?timeout=10&limit=5&offset=" + off + "&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D";
  int code = httpsJson(url, nullptr, r, &f, nullptr, nullptr, 15000);
  if (code != 200) { delay(3000); return; }
  for (JsonObject u : r["result"].as<JsonArray>()) {
    tgOffset = u["update_id"].as<long long>() + 1;
    prefs.putLong64("off", tgOffset);  // confirm before handling: at-most-once
    if (u["message"].is<JsonObject>()) {
      if (u["message"]["from"]["id"].as<long long>() != (long long)ALLOWED_USER_ID) { Serial.println("Ignored message from other user"); continue; }
      String text = u["message"]["text"] | "";
      handleText(text);
    } else if (u["callback_query"].is<JsonObject>()) {
      JsonObject cq = u["callback_query"];
      if (cq["from"]["id"].as<long long>() != (long long)ALLOWED_USER_ID) continue;
      String id = cq["id"] | "", data = cq["data"] | "";
      handleCallback(id, data, cq["message"]["message_id"].as<long>());
    }
  }
}

// ===================== setup / loop =====================
void wifiBegin() {
#ifdef USE_EDUROAM
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, EAP_IDENTITY, EAP_USERNAME, EAP_PASSWORD);
  #else
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
    esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(WIFI_SSID);
  #endif
#else
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif
}

void connectWifi() {
  setBusy("Connecting WiFi");
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  wifiBegin();
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) delay(250);
}
void syncTime() {
  setBusy("Syncing time");
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 60 && !timeOk(); i++) delay(250);
}

uint32_t lastRem = 0, lastWifiTry = 0;

void setup() {
  Serial.begin(115200);
  uiMux = xSemaphoreCreateMutex();
  u8g2.begin(); u8g2.setBusClock(400000);
  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 1, nullptr, 0);
  setBusy("Starting...");
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  prefs.begin("brain", false); tgOffset = prefs.getLong64("off", 0);
  refreshTodoCache();
  connectWifi();
  syncTime();
  tgSetCommands();
  tgSend("🧠 Online" + String(timeOk() ? ", " + fmtDT(time(nullptr)) : String("")) + ". Send /help.");
  clearBusy();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 15000) { lastWifiTry = millis(); WiFi.disconnect(); wifiBegin(); }
    delay(500); return;
  }
  if (!timeOk() && millis() % 20000 < 600) configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  pollTelegram();
  if (millis() - lastRem > 30000) { lastRem = millis(); checkReminders(); }
  if (flow.type != F_NONE && millis() - flow.last > FLOW_TIMEOUT_MS) { flow.type = F_NONE; tgSend("⌛ Timed out. Unfinished entry discarded."); }
  delay(20);
}
