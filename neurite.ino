/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Linkgo LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <Arduino.h>

#include "neurite_utils.h"

#include <ESP8266WiFi.h>
#ifdef WIFI_CONFIG_MULTI
#include <ESP8266WiFiMulti.h>
#endif
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <ArduinoJson.h>
#include "FS.h"

extern "C" {
#include "osapi.h"
#include "ets_sys.h"
#include "user_interface.h"
}

#define NEURITE_CFG_PATH	"/config.json"
#define NEURITE_CFG_SIZE	1024
#define NEURITE_CMD_BUF_SIZE	256
#define NEURITE_UID_LEN		32
#define NEURITE_SSID_LEN	32
#define NEURITE_PSK_LEN		32

#define MQTT_TOPIC_LEN		64
#define MQTT_MSG_LEN		256

#define NEURITE_LED		13
#define NEURITE_BUTTON		0

const char STR_READY[] PROGMEM = "neurite ready";

struct cmd_parser_s;
typedef void (*cmd_parser_cb_fp)(struct cmd_parser_s *cp);

struct cmd_parser_s {
	char *buf;
	uint16_t buf_size;
	uint16_t data_len;
	bool cmd_begin;
	cmd_parser_cb_fp callback;
};

struct neurite_cfg_s {
	char uid[NEURITE_UID_LEN];
	char topic_to[MQTT_TOPIC_LEN];
	char topic_from[MQTT_TOPIC_LEN];
	char ssid[NEURITE_SSID_LEN];
	char psk[NEURITE_PSK_LEN];
	char ota_url[MQTT_MSG_LEN];
};

struct neurite_data_s {
	bool wifi_connected;
	bool mqtt_connected;
	struct neurite_cfg_s cfg;
	struct cmd_parser_s *cp;
};

static struct neurite_data_s g_nd;
static struct cmd_parser_s g_cp;
static char cmd_buf[NEURITE_CMD_BUF_SIZE];
static bool b_cfg_ready;

#ifdef WIFI_CONFIG_MULTI
ESP8266WiFiMulti WiFiMulti;
#endif
Ticker ticker_worker;
Ticker ticker_led;
Ticker ticker_cmd;
Ticker ticker_mon;
Ticker ticker_but;
WiFiClient wifi_cli;
PubSubClient mqtt_cli(wifi_cli);
StaticJsonBuffer<NEURITE_CFG_SIZE> json_buf;
static char cfg_buf[NEURITE_CFG_SIZE];

static void reboot(struct neurite_data_s *nd);

enum {
	WORKER_ST_0 = 0,
	WORKER_ST_1,
	WORKER_ST_2,
	WORKER_ST_3,
	WORKER_ST_4
};
static int worker_st = WORKER_ST_0;

static inline void update_worker_state(int st)
{
	log_dbg("-> WORKER_ST_%d\n\r", st);
	worker_st = st;
}

static void ticker_led_breath(void)
{
	static int val = 700;
	static int state = 0;
	if (val >= 700)
		state = 1;
	else if (val <= 200)
		state = 0;
	else
		;
	if (state)
		val -= 10;
	else
		val += 10;
	analogWrite(NEURITE_LED, val);
}

static void ticker_led_blink(void)
{
	static int val = 1000;
	analogWrite(NEURITE_LED, val);
	val = (val == 1000) ? 200 : 1000;
}

static void cfg_run_dump(struct neurite_data_s *nd)
{
	log_dbg("ssid: %s\n\r", nd->cfg.ssid);
	log_dbg("psk: %s\n\r", nd->cfg.psk);
	log_dbg("uid: %s\n\r", nd->cfg.uid);
	log_dbg("topic_to: %s\n\r", nd->cfg.topic_to);
	log_dbg("topic_from: %s\n\r", nd->cfg.topic_from);
}

static void cfg_file_dump(struct neurite_data_s *nd)
{
	File configFile = SPIFFS.open(NEURITE_CFG_PATH, "r");
	if (!configFile) {
		log_err("open failed\n\r");
		return;
	}
	log_dbg("%s:\n\r", NEURITE_CFG_PATH);
	while (configFile.available())
		LOG_SERIAL.write(configFile.read());
	LOG_SERIAL.println();
	configFile.close();
}

static bool cfg_load(struct neurite_data_s *nd)
{
	File configFile = SPIFFS.open(NEURITE_CFG_PATH, "r");
	if (!configFile) {
		log_err("open failed\n\r");
		return false;
	}
	size_t size = configFile.size();
	if (size > NEURITE_CFG_SIZE) {
		log_err("file too large\n\r");
		configFile.close();
		return false;
	}
	size = configFile.readBytes(cfg_buf, size);
	log_info("config file size: %d\n\r", size);
	configFile.close();
	cfg_file_dump(nd);
	return true;
}

static int cfg_load_sync(struct neurite_data_s *nd)
{
	log_dbg("in\n\r");
	if (!cfg_load(nd)) {
		log_err("load cfg failed\n\r");
		return false;
	}
	JsonObject& json = json_buf.parseObject(cfg_buf);
	if (!json.success()) {
		log_err("parse cfg failed\n\r");
		return false;
	}
	const char *ssid = json["ssid"];
	const char *psk = json["psk"];
	const char *uid = json["uid"];
	const char *topic_to = json["topic_to"];
	const char *topic_from = json["topic_from"];
	log_dbg("loaded ssid: %s\n\r", ssid);
	log_dbg("loaded psk: %s\n\r", psk);
	log_dbg("loaded uid: %s\n\r", uid);
	log_dbg("loaded topic_to: %s\n\r", topic_to);
	log_dbg("loaded topic_from: %s\n\r", topic_from);
#if 1
	if (ssid)
		strncpy(nd->cfg.ssid, ssid, NEURITE_SSID_LEN);
	if (psk)
		strncpy(nd->cfg.psk, psk, NEURITE_PSK_LEN);
	if (uid)
		strncpy(nd->cfg.uid, uid, NEURITE_UID_LEN);
	if (topic_to)
		strncpy(nd->cfg.topic_to, topic_to, MQTT_TOPIC_LEN);
	if (topic_from)
		strncpy(nd->cfg.topic_from, topic_from, MQTT_TOPIC_LEN);
#endif
	cfg_run_dump(nd);
}

static bool cfg_save_sync(struct neurite_data_s *nd)
{
	JsonObject& json = json_buf.createObject();

	cfg_run_dump(nd);

	json["ssid"] = nd->cfg.ssid;
	json["psk"] = nd->cfg.psk;
	json["uid"] = nd->cfg.uid;
	json["topic_to"] = nd->cfg.topic_to;
	json["topic_from"] = nd->cfg.topic_from;
	const char *ssid = json["ssid"];
	const char *psk = json["psk"];
	const char *uid = json["uid"];
	const char *topic_to = json["topic_to"];
	const char *topic_from = json["topic_from"];
	log_dbg("ssid: %s\n\r", ssid);
	log_dbg("psk: %s\n\r", psk);
	log_dbg("uid: %s\n\r", uid);
	log_dbg("topic_to: %s\n\r", topic_to);
	log_dbg("topic_from: %s\n\r", topic_from);

	File configFile = SPIFFS.open(NEURITE_CFG_PATH, "w");
	if (!configFile) {
		log_err("open failed\n\r");
		return false;
	} else {
		log_dbg("open successfully\n\r");
	}
	size_t size = json.printTo(configFile);
	log_dbg("wrote %d bytes\n\r", size);
	configFile.close();
	cfg_file_dump(nd);
	return true;
}

static bool cfg_validate(struct neurite_data_s *nd)
{
	if (!cfg_load(nd)) {
		log_err("load cfg failed\n\r");
		return false;
	}
	JsonObject& json = json_buf.parseObject(cfg_buf);
	if (!json.success()) {
		log_err("parse cfg failed\n\r");
		return false;
	}
	const char *ssid = json["ssid"];
	const char *psk = json["psk"];
	log_dbg("loaded ssid: %s\n\r", ssid);
	log_dbg("loaded psk: %s\n\r", psk);
	if (!ssid || (strcmp(ssid, SSID1) == 0)) {
		log_warn("no ssid cfg\n\r");
		return false;
	}

	log_dbg("out\n\r");
	return true;
}

static void wifi_connect(struct neurite_data_s *nd)
{
	log_dbg("Connecting to ");
	Serial.println(nd->cfg.ssid);
	WiFi.mode(WIFI_STA);
#ifdef WIFI_CONFIG_MULTI
	WiFiMulti.addAP(nd->cfg.ssid, nd->cfg.psk);
	WiFiMulti.addAP(SSID2, PSK2);
#else
	WiFi.begin(nd->cfg.ssid, nd->cfg.psk);
#endif
}

static inline bool wifi_check_status(struct neurite_data_s *nd)
{
	return (WiFi.status() == WL_CONNECTED);
}

static int ota_over_http(char *url)
{
	if (!url) {
		log_err("invalid url\n\r");
		return -1;
	}
	log_info("ota firmware: %s\n\r", url);

	t_httpUpdate_return ret;
	ret = ESPhttpUpdate.update(url);

	switch (ret) {
		case HTTP_UPDATE_FAILED:
			log_err("failed\n\r");
			break;

		case HTTP_UPDATE_NO_UPDATES:
			log_warn("no updates\n\r");
			break;

		case HTTP_UPDATE_OK:
			log_info("ok\n\r");
			break;
	}
	return ret;
}

static void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
	log_dbg("topic (%d): %s\n\r", strlen(topic), topic);
	log_dbg("data (%d): ", length);
	for (int i = 0; i < length; i++)
		LOG_SERIAL.print((char)payload[i]);
	LOG_SERIAL.println();
}

static inline bool mqtt_check_status(struct neurite_data_s *nd)
{
	return mqtt_cli.connected();
}

static void mqtt_config(struct neurite_data_s *nd)
{
	mqtt_cli.setServer(MQTT_SERVER, 1883);
	mqtt_cli.setCallback(mqtt_callback);
}

static void mqtt_connect(struct neurite_data_s *nd)
{
	mqtt_cli.connect(nd->cfg.uid);
}

static void ticker_monitor_task(struct neurite_data_s *nd)
{
	nd->wifi_connected = wifi_check_status(nd);
	nd->mqtt_connected = mqtt_check_status(nd);
	if (!nd->wifi_connected && worker_st >= WORKER_ST_2) {
		log_warn("WiFi disconnected\n\r");
		update_worker_state(WORKER_ST_0);
	}
	if (!nd->mqtt_connected && worker_st >= WORKER_ST_4) {
		log_warn("MQTT disconnected\n\r");
		update_worker_state(WORKER_ST_3);
	}
}

static void ticker_worker_task(void)
{
#if 0
	struct neurite_data_s *nd = &g_nd;
	char tmp[64];
	sprintf(tmp, "%s time: %d ms", nd->cfg.uid, millis());
	mqtt_cli.publish(nd->cfg.topic_to, tmp);
#endif
}

static void button_release_handler(struct neurite_data_s *nd, int dts)
{
	log_dbg("button pressed for %d ms\n\r", dts);
}

static void button_hold_handler(struct neurite_data_s *nd, int dts)
{
	if (dts > 5000) {
		if (SPIFFS.remove(NEURITE_CFG_PATH)) {
			log_info("%s removed\n\r", NEURITE_CFG_PATH);
			reboot(nd);
		} else {
			log_err("%s failed to remove\n\r", NEURITE_CFG_PATH);
		}
	}
}

static void ticker_button_task(struct neurite_data_s *nd)
{
	static int ts = 0;
	static int r_curr = HIGH;
	static int r_prev = HIGH;
	int dts;
	r_curr = digitalRead(NEURITE_BUTTON);
	if ((r_curr == LOW) && (r_prev == HIGH)) {
		ts = millis();
		r_prev = r_curr;
	} else {
		if (r_prev == LOW) {
			dts = millis() - ts;
			r_prev = r_curr;
			if (r_curr == HIGH)
				button_release_handler(nd, dts);
			else
				button_hold_handler(nd, dts);
		}
	}
}

static inline void stop_ticker_led(struct neurite_data_s *nd)
{
	ticker_led.detach();
}
static inline void start_ticker_led_breath(struct neurite_data_s *nd)
{
	stop_ticker_led(nd);
	ticker_led.attach_ms(50, ticker_led_breath);
}

static inline void start_ticker_led_blink(struct neurite_data_s *nd)
{
	stop_ticker_led(nd);
	ticker_led.attach_ms(50, ticker_led_blink);
}

static inline void stop_ticker_mon(struct neurite_data_s *nd)
{
	ticker_mon.detach();
}

static inline void start_ticker_mon(struct neurite_data_s *nd)
{
	stop_ticker_mon(nd);
	ticker_mon.attach_ms(100, ticker_monitor_task, nd);
}

static inline void stop_ticker_worker(struct neurite_data_s *nd)
{
	ticker_worker.detach();
}

static inline void start_ticker_worker(struct neurite_data_s *nd)
{
	stop_ticker_worker(nd);
	ticker_worker.attach(1, ticker_worker_task);
}

static inline void stop_ticker_but(struct neurite_data_s *nd)
{
	ticker_but.detach();
}

static inline void start_ticker_but(struct neurite_data_s *nd)
{
	stop_ticker_but(nd);
	ticker_but.attach_ms(50, ticker_button_task, nd);
}

static void reboot(struct neurite_data_s *nd)
{
	log_info("rebooting...\n\r");
	stop_ticker_but(nd);
	stop_ticker_mon(nd);
	stop_ticker_worker(nd);
	stop_ticker_led(nd);
	stop_ticker_cmd(nd);
	ESP.restart();
}

static void cmd_completed_cb(struct cmd_parser_s *cp)
{
	struct neurite_data_s *nd = &g_nd;
	dbg_assert(cp);
	if (cp->data_len > 0) {
		if (nd->mqtt_connected)
			mqtt_cli.publish(nd->cfg.topic_to, cp->buf);
		log_dbg("msg %slaunched(len %d): %s\n\r",
			nd->mqtt_connected?"":"not ", cp->data_len, cp->buf);
	}
	__bzero(cp->buf, cp->buf_size);
	cp->data_len = 0;
}

static int cmd_parser_init(struct cmd_parser_s *cp, char *buf, uint16_t buf_size)
{
	dbg_assert(cp);
	cp->buf = buf;
	cp->buf_size = buf_size;
	cp->data_len = 0;
	cp->callback = cmd_completed_cb;
	return 0;
}

static void cmd_parse_byte(struct cmd_parser_s *cp, char value)
{
	dbg_assert(cp);
	switch (value) {
		case '\r':
			if (cp->callback != NULL)
				cp->callback(cp);
			break;
		default:
			if (cp->data_len < cp->buf_size)
				cp->buf[cp->data_len++] = value;
			break;
	}
}

static void ticker_cmd_task(struct neurite_data_s *nd)
{
	char c;
	if (CMD_SERIAL.available() <= 0)
		return;
	c = CMD_SERIAL.read();
	LOG_SERIAL.printf("%c", c);
	cmd_parse_byte(nd->cp, c);
}

static inline void stop_ticker_cmd(struct neurite_data_s *nd)
{
	ticker_cmd.detach();
}

static void start_ticker_cmd(struct neurite_data_s *nd)
{
	stop_ticker_cmd(nd);
	ticker_cmd.attach_ms(1, ticker_cmd_task, nd);
}

static void fs_init(struct neurite_data_s *nd)
{
	if (!SPIFFS.begin()) {
		log_err("failed to mount fs\n\r");
		dbg_assert(0);
	}
	FSInfo info;
	if (!SPIFFS.info(info)) {
		log_err("failed to mount fs\n\r");
		dbg_assert(0);
	}
	log_dbg("Total: %u\n\r", info.totalBytes);
	log_dbg("Used: %u\n\r", info.usedBytes);
	log_dbg("Block: %u\n\r", info.blockSize);
	log_dbg("Page: %u\n\r", info.pageSize);
	log_dbg("Max open files: %u\n\r", info.maxOpenFiles);
	log_dbg("Max path len: %u\n\r", info.maxPathLength);

	Dir dir = SPIFFS.openDir("/");
	while (dir.next()) {
		String fileName = dir.fileName();
		size_t fileSize = dir.fileSize();
		log_dbg("file: %s, size: %s\n\r",
			fileName.c_str(), formatBytes(fileSize).c_str());
	}
}

static void cfg_init(struct neurite_data_s *nd)
{
	__bzero(&nd->cfg, sizeof(struct neurite_cfg_s));
	sprintf(nd->cfg.uid, "neurite-%08x", ESP.getChipId());
	sprintf(nd->cfg.topic_to, "/neuro/chatroom", nd->cfg.uid);
	sprintf(nd->cfg.topic_from, "/neuro/chatroom", nd->cfg.uid);
	sprintf(nd->cfg.ssid, "%s", SSID1);
	sprintf(nd->cfg.psk, "%s", PSK1);
	cfg_save_sync(nd);
}
#if 0
bool loadConfig() {
  File configFile = SPIFFS.open(NEURITE_CFG_PATH, "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(cfg_buf, size);

  JsonObject& json = json_buf.parseObject(cfg_buf);

  if (!json.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }

  const char* ssid = json["ssid"];
  const char* psk = json["psk"];

  // Real world application would store these values in some variables for
  // later use.

  Serial.print("Loaded ssid: ");
  Serial.println(ssid);
  Serial.print("Loaded psk: ");
  Serial.println(psk);
  return true;
}

bool saveConfig() {
  JsonObject& json = json_buf.createObject();
  json["ssid"] = "ssidisshort";
  json["psk"] = "passwordislongenough";

  File configFile = SPIFFS.open(NEURITE_CFG_PATH, "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  json.printTo(configFile);
  return true;
}
#endif
/* TODO server test */

ESP8266WebServer *server;
File fsUploadFile;

static void handleNotFound() {
	if (!handleFileRead(server->uri())) {
		String message = "File Not Found\n\n";
		message += "URI: ";
		message += server->uri();
		message += "\nMethod: ";
		message += (server->method() == HTTP_GET)?"GET":"POST";
		message += "\nArguments: ";
		message += server->args();
		message += "\n";
		for (uint8_t i=0; i<server->args(); i++) {
			message += " " + server->argName(i) + ": " + server->arg(i) + "\n";
		}
		server->send(404, "text/plain", message);
	}
}

static String formatBytes(size_t bytes)
{
	if (bytes < 1024) {
		return String(bytes)+"B";
	} else if (bytes < (1024 * 1024)) {
		return String(bytes/1024.0)+"KB";
	} else if (bytes < (1024 * 1024 * 1024)) {
		return String(bytes/1024.0/1024.0)+"MB";
	} else {
		return String(bytes/1024.0/1024.0/1024.0)+"GB";
	}
}

String getContentType(String filename)
{
	if (server->hasArg("download")) return "application/octet-stream";
	else if (filename.endsWith(".htm")) return "text/html";
	else if (filename.endsWith(".html")) return "text/html";
	else if (filename.endsWith(".css")) return "text/css";
	else if (filename.endsWith(".js")) return "application/javascript";
	else if (filename.endsWith(".png")) return "image/png";
	else if (filename.endsWith(".gif")) return "image/gif";
	else if (filename.endsWith(".jpg")) return "image/jpeg";
	else if (filename.endsWith(".ico")) return "image/x-icon";
	else if (filename.endsWith(".xml")) return "text/xml";
	else if (filename.endsWith(".pdf")) return "application/x-pdf";
	else if (filename.endsWith(".zip")) return "application/x-zip";
	else if (filename.endsWith(".gz")) return "application/x-gzip";
	return "text/plain";
}

bool handleFileRead(String path)
{
	LOG_SERIAL.println("handleFileRead: " + path);
	if (path.endsWith("/"))
		path += "index.htm";
	String contentType = getContentType(path);
	String pathWithGz = path + ".gz";
	if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
		if (SPIFFS.exists(pathWithGz))
			path += ".gz";
		File file = SPIFFS.open(path, "r");
		size_t sent = server->streamFile(file, contentType);
		file.close();
		return true;
	}
	return false;
}

void handleFileUpload()
{
	if (server->uri() != "/edit")
		return;
	HTTPUpload& upload = server->upload();
	if (upload.status == UPLOAD_FILE_START) {
		String filename = upload.filename;
		if (!filename.startsWith("/"))
			filename = "/"+filename;
		log_info(": ");
		LOG_SERIAL.println(filename);
		fsUploadFile = SPIFFS.open(filename, "w");
		filename = String();
	} else if (upload.status == UPLOAD_FILE_WRITE) {
		//LOG_SERIAL.print("handleFileUpload Data: "); LOG_SERIAL.println(upload.currentSize);
		if (fsUploadFile)
			fsUploadFile.write(upload.buf, upload.currentSize);
	} else if (upload.status == UPLOAD_FILE_END) {
		if (fsUploadFile)
			fsUploadFile.close();
		log_info("done, size: ");
		LOG_SERIAL.println(upload.totalSize);
	}
}

void handleFileDelete()
{
	if (server->args() == 0) return server->send(500, "text/plain", "BAD ARGS");
	String path = server->arg(0);
	log_info(": ");
	LOG_SERIAL.println(path);
	if (path == "/")
		return server->send(500, "text/plain", "BAD PATH");
	if (!SPIFFS.exists(path))
		return server->send(404, "text/plain", "FileNotFound");
	SPIFFS.remove(path);
	server->send(200, "text/plain", "");
	path = String();
}

void handleFileCreate()
{
	if (server->args() == 0)
		return server->send(500, "text/plain", "BAD ARGS");
	String path = server->arg(0);
	log_info(": ");
	LOG_SERIAL.println(path);
	if (path == "/")
		return server->send(500, "text/plain", "BAD PATH");
	if (SPIFFS.exists(path))
		return server->send(500, "text/plain", "FILE EXISTS");
	File file = SPIFFS.open(path, "w");
	if (file)
		file.close();
	else
		return server->send(500, "text/plain", "CREATE FAILED");
	server->send(200, "text/plain", "");
	path = String();
}

void handleFileList()
{
	if (!server->hasArg("dir"))
		server->send(500, "text/plain", "BAD ARGS"); return;
	String path = server->arg("dir");
	log_info(": ");
	LOG_SERIAL.println(path);
	Dir dir = SPIFFS.openDir(path);
	path = String();

	String output = "[";
	while (dir.next()) {
		File entry = dir.openFile("r");
		if (output != "[") output += ',';
		bool isDir = false;
		output += "{\"type\":\"";
		output += (isDir)?"dir":"file";
		output += "\",\"name\":\"";
		output += String(entry.name()).substring(1);
		output += "\"}";
		entry.close();
	}
	output += "]";
	server->send(200, "text/json", output);
}

void handleRoot()
{
	if (!handleFileRead("/index.html"))
		server->send(404, "text/plain", "FileNotFound");
}

void handleSave()
{
	struct neurite_data_s *nd = &g_nd;
	String message;
	log_dbg("in\n\r");
	message += "URI: ";
	message += server->uri();
	message += "\nMethod: ";
	message += (server->method() == HTTP_GET)?"GET":"POST";
	message += "\nArguments: ";
	message += server->args();
	message += "\n\n";
	for (uint8_t i=0; i<server->args(); i++) {
		message += " " + server->argName(i) + ": " + server->arg(i) + "\n";
		if (server->argName(i).equals("ssid")) {
			if (server->arg(i).length() > NEURITE_SSID_LEN) {
				log_warn("ssid too long %d (>%d)\n\r",
					 server->arg(i).length(), NEURITE_SSID_LEN);
				goto err_handle_save;
			} else {
				strncpy(nd->cfg.ssid, server->arg(i).c_str(), NEURITE_SSID_LEN);
			}
		} else if (server->argName(i).equals("password")) {
			if (server->arg(i).length() > NEURITE_PSK_LEN) {
				log_warn("password too long %d (>%d)\n\r",
					 server->arg(i).length(), NEURITE_PSK_LEN);
				goto err_handle_save;
			} else {
				strncpy(nd->cfg.psk, server->arg(i).c_str(), NEURITE_PSK_LEN);
			}
		} else {
			log_warn("%s not handled\n\r", server->arg(i).c_str());
		}
	}
	message += "Jolly good config!\n";
	cfg_save_sync(nd);
	server->send(200, "text/plain", message);
	log_dbg("out ok\n\r");
	return;
err_handle_save:
	message += "Bad request\n";
	server->send(400, "text/plain", message);
	log_dbg("out bad\n\r");
	return;
}

/* server test end */

inline void neurite_worker(void)
{
	struct neurite_data_s *nd = &g_nd;
	switch (worker_st) {
		case WORKER_ST_0:
			start_ticker_led_blink(nd);
			wifi_connect(nd);
			update_worker_state(WORKER_ST_1);
			break;
		case WORKER_ST_1:
			if (!wifi_check_status(nd))
				break;
			nd->wifi_connected = true;
			log_dbg("WiFi connected\n\r");
			log_info("STA IP address: ");
			LOG_SERIAL.println(WiFi.localIP());
			update_worker_state(WORKER_ST_2);
			break;
		case WORKER_ST_2:
			if (digitalRead(NEURITE_BUTTON) == LOW) {
				stop_ticker_led(nd);
				stop_ticker_but(nd);
				ota_over_http(OTA_URL_DEFAULT);
				start_ticker_but(nd);
				start_ticker_led_blink(nd);
			}
			mqtt_config(nd);
			update_worker_state(WORKER_ST_3);
			break;
		case WORKER_ST_3:
			if (!mqtt_check_status(nd)) {
				mqtt_connect(nd);
				break;
			}

			nd->mqtt_connected = true;
			mqtt_cli.subscribe(nd->cfg.topic_from);
			char payload_buf[32];
			dbg_assert(payload_buf);
			sprintf(payload_buf, "checkin: %s", nd->cfg.uid);
			mqtt_cli.publish(nd->cfg.topic_to, (const char *)payload_buf);

			start_ticker_led_breath(nd);
			start_ticker_worker(nd);
			start_ticker_mon(nd);
			CMD_SERIAL.println(FPSTR(STR_READY));
			log_dbg("heap free: %d\n\r", ESP.getFreeHeap());
			update_worker_state(WORKER_ST_4);
			break;
		case WORKER_ST_4:
			mqtt_cli.loop();
			break;
		default:
			log_err("unknown worker state: %d\n\r", worker_st);
			break;
	}
}

enum {
	CFG_ST_0 = 0,
	CFG_ST_1,
	CFG_ST_2
};
static int cfg_st = CFG_ST_0;

static inline void update_cfg_state(int st)
{
	log_dbg("-> CFG_ST_%d\n\r", st);
	cfg_st = st;
}

#ifdef NEURITE_ENABLE_MDNS
const char *host = "neurite";
#endif
inline void neurite_cfg_worker(void)
{
	struct neurite_data_s *nd = &g_nd;
	IPAddress myIP;
	switch (cfg_st) {
		case CFG_ST_0:
			analogWrite(NEURITE_LED, 300);
			WiFi.softAP(nd->cfg.uid);
			myIP = WiFi.softAPIP();
			CMD_SERIAL.print("AP IP address: ");
			CMD_SERIAL.println(myIP);
			update_cfg_state(CFG_ST_1);
			break;
		case CFG_ST_1:
#ifdef NEURITE_ENABLE_MDNS
			MDNS.begin(host);
			CMD_SERIAL.print("Open http://");
			CMD_SERIAL.print(host);
			CMD_SERIAL.println(".local to get started");
#endif
			server = new ESP8266WebServer(80);
			server->on("/list", HTTP_GET, handleFileList);
			server->on("/edit", HTTP_PUT, handleFileCreate);
			server->on("/edit", HTTP_DELETE, handleFileDelete);
			server->on("/edit", HTTP_POST, []() {
				server->send(200, "text/plain", "");
			}, handleFileUpload);
			server->on("/save", HTTP_POST, handleSave);

			server->onNotFound(handleNotFound);

			server->on("/all", HTTP_GET, []() {
				String json = "{";
				json += "\"heap\":"+String(ESP.getFreeHeap());
				json += ", \"analog\":"+String(analogRead(A0));
				json += ", \"gpio\":"+String((uint32_t)(((GPI | GPO) & 0xFFFF) | ((GP16I & 0x01) << 16)));
				json += "}";
				server->send(200, "text/json", json);
				json = String();
			});

			server->on("/", handleRoot);
			server->begin();
			log_dbg("HTTP server started\n\r");
			log_dbg("heap free: %d\n\r", ESP.getFreeHeap());
			update_cfg_state(CFG_ST_2);
			break;
		case CFG_ST_2:
			server->handleClient();
			break;
		default:
			log_err("unknown cfg state: %d\n\r", worker_st);
			break;
	}
}

void neurite_init(void)
{
	struct neurite_data_s *nd = &g_nd;
	log_dbg("in\n\r");

	__bzero(cfg_buf, NEURITE_CFG_SIZE);
	__bzero(nd, sizeof(struct neurite_data_s));
	__bzero(&g_cp, sizeof(struct cmd_parser_s));
	nd->cp = &g_cp;
	__bzero(cmd_buf, NEURITE_CMD_BUF_SIZE);
	cmd_parser_init(nd->cp, cmd_buf, NEURITE_CMD_BUF_SIZE);

	fs_init(nd);

	if (cfg_validate(nd)) {
		cfg_load_sync(nd);
		b_cfg_ready = true;
		log_dbg("cfg ready\n\r");
	} else {
		cfg_init(nd);
		b_cfg_ready = false;
		log_dbg("cfg not ready\n\r");
	}

	log_info("chip id: %08x\n\r", system_get_chip_id());
	log_info("uid: %s\n\r", nd->cfg.uid);
	log_info("topic_to: %s\n\r", nd->cfg.topic_to);
	log_info("topic_from: %s\n\r", nd->cfg.topic_from);

	start_ticker_but(nd);
	start_ticker_cmd(nd);
	log_dbg("out\n\r");
}

void setup()
{
	LOG_SERIAL.begin(115200);
	LOG_SERIAL.setDebugOutput(false);
	LOG_SERIAL.printf("\n\r");
	LOG_SERIAL.flush();
	pinMode(NEURITE_LED, OUTPUT);
	digitalWrite(NEURITE_LED, HIGH);
	pinMode(NEURITE_BUTTON, INPUT);
	log_dbg("heap free: %d\n\r", ESP.getFreeHeap());
	log_dbg("flash size: %d\n\r", ESP.getFlashChipRealSize());
	log_dbg("sketch size: %d\n\r", ESP.getSketchSize());
	log_dbg("sketch free: %d\n\r", ESP.getFreeSketchSpace());

	neurite_init();
}

void loop()
{
	if (b_cfg_ready)
		neurite_worker();
	else
		neurite_cfg_worker();
}
