#include "AmsWebServer.h"
#include "AmsWebHeaders.h"
#include "base64.h"
#include "hexutils.h"

#include <ArduinoJson.h>

#include "html/index_html.h"
#include "html/index_css.h"
#include "html/index_js.h"
#include "html/github_svg.h"
#include "html/data_json.h"
#include "html/dayplot_json.h"
#include "html/monthplot_json.h"
#include "html/energyprice_json.h"
#include "html/tempsensor_json.h"
#include "html/response_json.h"

#include "version.h"

#if defined(ESP32)
#include <esp_task_wdt.h>
#endif


AmsWebServer::AmsWebServer(uint8_t* buf, RemoteDebug* Debug, HwTools* hw) {
	this->debugger = Debug;
	this->hw = hw;
	this->buf = (char*) buf;
}

void AmsWebServer::setup(AmsConfiguration* config, GpioConfig* gpioConfig, MeterConfig* meterConfig, AmsData* meterState, AmsDataStorage* ds, EnergyAccounting* ea) {
    this->config = config;
	this->gpioConfig = gpioConfig;
	this->meterConfig = meterConfig;
	this->meterState = meterState;
	this->ds = ds;
	this->ea = ea;

	// TODO
	server.on(F("/"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/configuration"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/status"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/consent"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/vendor"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/setup"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/mqtt-ca"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/mqtt-cert"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	server.on(F("/mqtt-key"), HTTP_GET, std::bind(&AmsWebServer::indexHtml, this));
	
	server.on(F("/index.css"), HTTP_GET, std::bind(&AmsWebServer::indexCss, this));
	server.on(F("/index.js"), HTTP_GET, std::bind(&AmsWebServer::indexJs, this));
	server.on(F("/github.svg"), HTTP_GET, std::bind(&AmsWebServer::githubSvg, this)); 
	server.on(F("/favicon.ico"), HTTP_GET, std::bind(&AmsWebServer::faviconIco, this)); 
	server.on(F("/sysinfo.json"), HTTP_GET, std::bind(&AmsWebServer::sysinfoJson, this));
	server.on(F("/data.json"), HTTP_GET, std::bind(&AmsWebServer::dataJson, this));
	server.on(F("/dayplot.json"), HTTP_GET, std::bind(&AmsWebServer::dayplotJson, this));
	server.on(F("/monthplot.json"), HTTP_GET, std::bind(&AmsWebServer::monthplotJson, this));
	server.on(F("/energyprice.json"), HTTP_GET, std::bind(&AmsWebServer::energyPriceJson, this));
	server.on(F("/temperature.json"), HTTP_GET, std::bind(&AmsWebServer::temperatureJson, this));
	server.on(F("/tariff.json"), HTTP_GET, std::bind(&AmsWebServer::tariffJson, this));

	server.on(F("/wifiscan.json"), HTTP_GET, std::bind(&AmsWebServer::wifiScanJson, this));

	server.on(F("/configuration.json"), HTTP_GET, std::bind(&AmsWebServer::configurationJson, this));
	server.on(F("/save"), HTTP_POST, std::bind(&AmsWebServer::handleSave, this));
	server.on(F("/reboot"), HTTP_POST, std::bind(&AmsWebServer::reboot, this));
	server.on(F("/upgrade"), HTTP_POST, std::bind(&AmsWebServer::upgrade, this));
	server.on(F("/firmware"), HTTP_POST, std::bind(&AmsWebServer::firmwarePost, this), std::bind(&AmsWebServer::firmwareUpload, this));
	server.on(F("/is-alive"), HTTP_GET, std::bind(&AmsWebServer::isAliveCheck, this));

	server.on(F("/reset"), HTTP_POST, std::bind(&AmsWebServer::factoryResetPost, this));

	server.on(F("/robots.txt"), HTTP_GET, std::bind(&AmsWebServer::robotstxt, this));

	server.on(F("/mqtt-ca"), HTTP_POST, std::bind(&AmsWebServer::firmwarePost, this), std::bind(&AmsWebServer::mqttCaUpload, this));
	server.on(F("/mqtt-cert"), HTTP_POST, std::bind(&AmsWebServer::firmwarePost, this), std::bind(&AmsWebServer::mqttCertUpload, this));
	server.on(F("/mqtt-key"), HTTP_POST, std::bind(&AmsWebServer::firmwarePost, this), std::bind(&AmsWebServer::mqttKeyUpload, this));

	server.onNotFound(std::bind(&AmsWebServer::notFound, this));
	
	server.begin(); // Web server start

	config->getWebConfig(webConfig);
	MqttConfig mqttConfig;
	config->getMqttConfig(mqttConfig);
	mqttEnabled = strlen(mqttConfig.host) > 0;
}


void AmsWebServer::setMqtt(MQTTClient* mqtt) {
	this->mqtt = mqtt;
}

void AmsWebServer::setTimezone(Timezone* tz) {
	this->tz = tz;
}

void AmsWebServer::setMqttEnabled(bool enabled) {
	mqttEnabled = enabled;
}

void AmsWebServer::setEntsoeApi(EntsoeApi* eapi) {
	this->eapi = eapi;
}

void AmsWebServer::loop() {
	server.handleClient();

	if(maxPwr == 0 && meterState->getListType() > 1 && meterConfig->mainFuse > 0 && meterConfig->distributionSystem > 0) {
		int voltage = meterConfig->distributionSystem == 2 ? 400 : 230;
		if(meterState->isThreePhase()) {
			maxPwr = meterConfig->mainFuse * sqrt(3) * voltage;
		} else if(meterState->isTwoPhase()) {
			maxPwr = meterConfig->mainFuse * voltage;
		} else {
			maxPwr = meterConfig->mainFuse * 230;
		}
	}
}

bool AmsWebServer::checkSecurity(byte level) {
	bool access = WiFi.getMode() == WIFI_AP || webConfig.security < level;
	if(!access && webConfig.security >= level && server.hasHeader("Authorization")) {
		String expectedAuth = String(webConfig.username) + ":" + String(webConfig.password);

		String providedPwd = server.header("Authorization");
		providedPwd.replace("Basic ", "");

		#if defined(ESP8266)
		String expectedBase64 = base64::encode(expectedAuth, false);
		#elif defined(ESP32)
		String expectedBase64 = base64::encode(expectedAuth);
		#endif

		debugger->printf("Expected auth: %s\n", expectedBase64.c_str());
		debugger->printf("Provided auth: %s\n", providedPwd.c_str());

		access = providedPwd.equals(expectedBase64);
	}

	if(!access) {
		server.sendHeader(HEADER_AUTHENTICATE, AUTHENTICATE_BASIC);
		server.setContentLength(0);
		server.send(401, MIME_HTML, "");
	}
	return access;
}

void AmsWebServer::notFound() {
	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);
	server.send_P(404, MIME_HTML, PSTR("Not found"));
}

void AmsWebServer::githubSvg() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /github.svg over http...\n");

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_1HR);
	server.send_P(200, "image/svg+xml", GITHUB_SVG);
}

void AmsWebServer::faviconIco() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /favicon.ico over http...\n");
	notFound(); //TODO
}

void AmsWebServer::sysinfoJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /sysinfo.json over http...\n");

	DynamicJsonDocument doc(1024);
	doc[F("version")] = VERSION;
	#if defined(CONFIG_IDF_TARGET_ESP32S2)
	doc[F("chip")] = "esp32s2";
	#elif defined(CONFIG_IDF_TARGET_ESP32C3)
	doc[F("chip")] = "esp32c3";
	#elif defined(ESP32)
	doc[F("chip")] = "esp32";
	#elif defined(ESP8266)
	doc[F("chip")] = "esp8266";
	#endif

	uint32_t chipId;
	#if defined(ESP32)
		chipId = ( ESP.getEfuseMac() >> 32 ) % 0xFFFFFFFF;
	#else
		chipId = ESP.getChipId();
	#endif
	String chipIdStr = String(chipId, HEX);
	doc[F("chipId")] = chipIdStr;
	doc[F("mac")] = WiFi.macAddress();

	SystemConfig sys;
	config->getSystemConfig(sys);
	doc[F("board")] = sys.boardType;
	doc[F("vndcfg")] = sys.vendorConfigured;
	doc[F("usrcfg")] = sys.userConfigured;
	doc[F("fwconsent")] = sys.dataCollectionConsent;
	doc[F("country")] = sys.country;

	if(sys.userConfigured) {
		WiFiConfig wifiConfig;
		config->getWiFiConfig(wifiConfig);
		doc[F("hostname")] = wifiConfig.hostname;
	} else {
		doc[F("hostname")] = "ams-"+chipIdStr;
	}

	doc[F("booting")] = performRestart;
	doc[F("upgrading")] = rebootForUpgrade;

	doc[F("net")][F("ip")] = WiFi.localIP().toString();
	doc[F("net")][F("mask")] = WiFi.subnetMask().toString();
	doc[F("net")][F("gw")] = WiFi.gatewayIP().toString();
	doc[F("net")][F("dns1")] = WiFi.dnsIP(0).toString();
	doc[F("net")][F("dns2")] = WiFi.dnsIP(1).toString();

	doc[F("meter")][F("mfg")] = meterState->getMeterType();
	doc[F("meter")][F("model")] = meterState->getMeterModel();
	doc[F("meter")][F("id")] = meterState->getMeterId();

	serializeJson(doc, buf, BufferSize);
	server.send(200, MIME_JSON, buf);

	server.handleClient();
	delay(250);

	if(performRestart || rebootForUpgrade) {
		if(ds != NULL) {
			ds->save();
		}
		if(debugger->isActive(RemoteDebug::INFO)) debugger->printf(PSTR("Rebooting"));
		delay(1000);
		#if defined(ESP8266)
			ESP.reset();
		#elif defined(ESP32)
			ESP.restart();
		#endif
		performRestart = false;
	}
}

void AmsWebServer::dataJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /data.json over http...\n");
	uint64_t now = millis64();

	if(!checkSecurity(2))
		return;

	float vcc = hw->getVcc();
	int rssi = hw->getWifiRssi();

	uint8_t espStatus;
	#if defined(ESP8266)
	if(vcc < 2.0) { // Voltage not correct, ESP would not run on this voltage
		espStatus = 1;
	} else if(vcc > 3.1 && vcc < 3.5) {
		espStatus = 1;
	} else if(vcc > 3.0 && vcc < 3.6) {
		espStatus = 2;
	} else {
		espStatus = 3;
	}
	#elif defined(ESP32)
	if(vcc < 2.0) { // Voltage not correct, ESP would not run on this voltage
		espStatus = 1;
	} else if(vcc > 2.8 && vcc < 3.5) {
		espStatus = 1;
	} else if(vcc > 2.7 && vcc < 3.6) {
		espStatus = 2;
	} else {
		espStatus = 3;
	}
	#endif


	uint8_t hanStatus;
	if(meterConfig->baud == 0 || meterState->getLastUpdateMillis() == 0) {
		hanStatus = 0;
	} else if(now - meterState->getLastUpdateMillis() < 15000) {
		hanStatus = 1;
	} else if(now - meterState->getLastUpdateMillis() < 30000) {
		hanStatus = 2;
	} else {
		hanStatus = 3;
	}

	uint8_t wifiStatus;
	if(rssi > -75) {
		wifiStatus = 1;
	} else if(rssi > -95) {
		wifiStatus = 2;
	} else {
		wifiStatus = 3;
	}

	uint8_t mqttStatus;
	if(!mqttEnabled) {
		mqttStatus = 0;
	} else if(mqtt != NULL && mqtt->connected()) {
		mqttStatus = 1;
	} else if(mqtt != NULL && mqtt->lastError() == 0) {
		mqttStatus = 2;
	} else {
		mqttStatus = 3;
	}

	float price = ENTSOE_NO_VALUE;
	if(eapi != NULL && strlen(eapi->getToken()) > 0)
		price = eapi->getValueForHour(0);

	String peaks = "";
	for(uint8_t i = 1; i <= ea->getConfig()->hours; i++) {
		if(!peaks.isEmpty()) peaks += ",";
		peaks += String(ea->getPeak(i).value);
	}

	snprintf_P(buf, BufferSize, DATA_JSON,
		maxPwr == 0 ? meterState->isThreePhase() ? 20000 : 10000 : maxPwr,
		meterConfig->productionCapacity,
		meterConfig->mainFuse == 0 ? 32 : meterConfig->mainFuse,
		meterState->getActiveImportPower(),
		meterState->getActiveExportPower(),
		meterState->getReactiveImportPower(),
		meterState->getReactiveExportPower(),
		meterState->getActiveImportCounter(),
		meterState->getActiveExportCounter(),
		meterState->getReactiveImportCounter(),
		meterState->getReactiveExportCounter(),
		meterState->getL1Voltage(),
		meterState->getL2Voltage(),
		meterState->getL3Voltage(),
		meterState->getL1Current(),
		meterState->getL2Current(),
		meterState->getL3Current(),
		meterState->getPowerFactor(),
		meterState->getL1PowerFactor(),
		meterState->getL2PowerFactor(),
		meterState->getL3PowerFactor(),
		vcc,
		rssi,
		hw->getTemperature(),
		(uint32_t) (now / 1000),
		ESP.getFreeHeap(),
		espStatus,
		hanStatus,
		wifiStatus,
		mqttStatus,
		mqtt == NULL ? 0 : (int) mqtt->lastError(),
		price == ENTSOE_NO_VALUE ? "null" : String(price, 2).c_str(),
		meterState->getMeterType(),
		meterConfig->distributionSystem,
		ea->getMonthMax(),
		peaks.c_str(),
		ea->getCurrentThreshold(),
		ea->getUseThisHour(),
		ea->getCostThisHour(),
		ea->getProducedThisHour(),
		ea->getUseToday(),
		ea->getCostToday(),
		ea->getProducedToday(),
		ea->getUseThisMonth(),
		ea->getCostThisMonth(),
		ea->getProducedThisMonth(),
		eapi == NULL ? "" : eapi->getArea(),
		(uint32_t) time(nullptr)
	);

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	server.setContentLength(strlen(buf));
	server.send(200, MIME_JSON, buf);
}

void AmsWebServer::dayplotJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /dayplot.json over http...\n");

	if(!checkSecurity(2))
		return;

	if(ds == NULL) {
		notFound();
	} else {
		snprintf_P(buf, BufferSize, DAYPLOT_JSON,
			ds->getHourImport(0) / 1000.0,
			ds->getHourImport(1) / 1000.0,
			ds->getHourImport(2) / 1000.0,
			ds->getHourImport(3) / 1000.0,
			ds->getHourImport(4) / 1000.0,
			ds->getHourImport(5) / 1000.0,
			ds->getHourImport(6) / 1000.0,
			ds->getHourImport(7) / 1000.0,
			ds->getHourImport(8) / 1000.0,
			ds->getHourImport(9) / 1000.0,
			ds->getHourImport(10) / 1000.0,
			ds->getHourImport(11) / 1000.0,
			ds->getHourImport(12) / 1000.0,
			ds->getHourImport(13) / 1000.0,
			ds->getHourImport(14) / 1000.0,
			ds->getHourImport(15) / 1000.0,
			ds->getHourImport(16) / 1000.0,
			ds->getHourImport(17) / 1000.0,
			ds->getHourImport(18) / 1000.0,
			ds->getHourImport(19) / 1000.0,
			ds->getHourImport(20) / 1000.0,
			ds->getHourImport(21) / 1000.0,
			ds->getHourImport(22) / 1000.0,
			ds->getHourImport(23) / 1000.0,
			ds->getHourExport(0) / 1000.0,
			ds->getHourExport(1) / 1000.0,
			ds->getHourExport(2) / 1000.0,
			ds->getHourExport(3) / 1000.0,
			ds->getHourExport(4) / 1000.0,
			ds->getHourExport(5) / 1000.0,
			ds->getHourExport(6) / 1000.0,
			ds->getHourExport(7) / 1000.0,
			ds->getHourExport(8) / 1000.0,
			ds->getHourExport(9) / 1000.0,
			ds->getHourExport(10) / 1000.0,
			ds->getHourExport(11) / 1000.0,
			ds->getHourExport(12) / 1000.0,
			ds->getHourExport(13) / 1000.0,
			ds->getHourExport(14) / 1000.0,
			ds->getHourExport(15) / 1000.0,
			ds->getHourExport(16) / 1000.0,
			ds->getHourExport(17) / 1000.0,
			ds->getHourExport(18) / 1000.0,
			ds->getHourExport(19) / 1000.0,
			ds->getHourExport(20) / 1000.0,
			ds->getHourExport(21) / 1000.0,
			ds->getHourExport(22) / 1000.0,
			ds->getHourExport(23) / 1000.0
		);

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

		server.setContentLength(strlen(buf));
		server.send(200, MIME_JSON, buf);
	}
}

void AmsWebServer::monthplotJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /monthplot.json over http...\n");

	if(!checkSecurity(2))
		return;

	if(ds == NULL) {
		notFound();
	} else {
		snprintf_P(buf, BufferSize, MONTHPLOT_JSON,
			ds->getDayImport(1) / 1000.0,
			ds->getDayImport(2) / 1000.0,
			ds->getDayImport(3) / 1000.0,
			ds->getDayImport(4) / 1000.0,
			ds->getDayImport(5) / 1000.0,
			ds->getDayImport(6) / 1000.0,
			ds->getDayImport(7) / 1000.0,
			ds->getDayImport(8) / 1000.0,
			ds->getDayImport(9) / 1000.0,
			ds->getDayImport(10) / 1000.0,
			ds->getDayImport(11) / 1000.0,
			ds->getDayImport(12) / 1000.0,
			ds->getDayImport(13) / 1000.0,
			ds->getDayImport(14) / 1000.0,
			ds->getDayImport(15) / 1000.0,
			ds->getDayImport(16) / 1000.0,
			ds->getDayImport(17) / 1000.0,
			ds->getDayImport(18) / 1000.0,
			ds->getDayImport(19) / 1000.0,
			ds->getDayImport(20) / 1000.0,
			ds->getDayImport(21) / 1000.0,
			ds->getDayImport(22) / 1000.0,
			ds->getDayImport(23) / 1000.0,
			ds->getDayImport(24) / 1000.0,
			ds->getDayImport(25) / 1000.0,
			ds->getDayImport(26) / 1000.0,
			ds->getDayImport(27) / 1000.0,
			ds->getDayImport(28) / 1000.0,
			ds->getDayImport(29) / 1000.0,
			ds->getDayImport(30) / 1000.0,
			ds->getDayImport(31) / 1000.0,
			ds->getDayExport(1) / 1000.0,
			ds->getDayExport(2) / 1000.0,
			ds->getDayExport(3) / 1000.0,
			ds->getDayExport(4) / 1000.0,
			ds->getDayExport(5) / 1000.0,
			ds->getDayExport(6) / 1000.0,
			ds->getDayExport(7) / 1000.0,
			ds->getDayExport(8) / 1000.0,
			ds->getDayExport(9) / 1000.0,
			ds->getDayExport(10) / 1000.0,
			ds->getDayExport(11) / 1000.0,
			ds->getDayExport(12) / 1000.0,
			ds->getDayExport(13) / 1000.0,
			ds->getDayExport(14) / 1000.0,
			ds->getDayExport(15) / 1000.0,
			ds->getDayExport(16) / 1000.0,
			ds->getDayExport(17) / 1000.0,
			ds->getDayExport(18) / 1000.0,
			ds->getDayExport(19) / 1000.0,
			ds->getDayExport(20) / 1000.0,
			ds->getDayExport(21) / 1000.0,
			ds->getDayExport(22) / 1000.0,
			ds->getDayExport(23) / 1000.0,
			ds->getDayExport(24) / 1000.0,
			ds->getDayExport(25) / 1000.0,
			ds->getDayExport(26) / 1000.0,
			ds->getDayExport(27) / 1000.0,
			ds->getDayExport(28) / 1000.0,
			ds->getDayExport(29) / 1000.0,
			ds->getDayExport(30) / 1000.0,
			ds->getDayExport(31) / 1000.0
		);

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

		server.setContentLength(strlen(buf));
		server.send(200, MIME_JSON, buf);
	}
}

void AmsWebServer::energyPriceJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /energyprice.json over http...\n");

	if(!checkSecurity(2))
		return;

	float prices[36];
	for(int i = 0; i < 36; i++) {
		prices[i] = eapi == NULL ? ENTSOE_NO_VALUE : eapi->getValueForHour(i);
	}

	snprintf_P(buf, BufferSize, ENERGYPRICE_JSON, 
		eapi == NULL ? "" : eapi->getCurrency(),
		prices[0] == ENTSOE_NO_VALUE ? "null" : String(prices[0], 4).c_str(),
		prices[1] == ENTSOE_NO_VALUE ? "null" : String(prices[1], 4).c_str(),
		prices[2] == ENTSOE_NO_VALUE ? "null" : String(prices[2], 4).c_str(),
		prices[3] == ENTSOE_NO_VALUE ? "null" : String(prices[3], 4).c_str(),
		prices[4] == ENTSOE_NO_VALUE ? "null" : String(prices[4], 4).c_str(),
		prices[5] == ENTSOE_NO_VALUE ? "null" : String(prices[5], 4).c_str(),
		prices[6] == ENTSOE_NO_VALUE ? "null" : String(prices[6], 4).c_str(),
		prices[7] == ENTSOE_NO_VALUE ? "null" : String(prices[7], 4).c_str(),
		prices[8] == ENTSOE_NO_VALUE ? "null" : String(prices[8], 4).c_str(),
		prices[9] == ENTSOE_NO_VALUE ? "null" : String(prices[9], 4).c_str(),
		prices[10] == ENTSOE_NO_VALUE ? "null" : String(prices[10], 4).c_str(),
		prices[11] == ENTSOE_NO_VALUE ? "null" : String(prices[11], 4).c_str(),
		prices[12] == ENTSOE_NO_VALUE ? "null" : String(prices[12], 4).c_str(),
		prices[13] == ENTSOE_NO_VALUE ? "null" : String(prices[13], 4).c_str(),
		prices[14] == ENTSOE_NO_VALUE ? "null" : String(prices[14], 4).c_str(),
		prices[15] == ENTSOE_NO_VALUE ? "null" : String(prices[15], 4).c_str(),
		prices[16] == ENTSOE_NO_VALUE ? "null" : String(prices[16], 4).c_str(),
		prices[17] == ENTSOE_NO_VALUE ? "null" : String(prices[17], 4).c_str(),
		prices[18] == ENTSOE_NO_VALUE ? "null" : String(prices[18], 4).c_str(),
		prices[19] == ENTSOE_NO_VALUE ? "null" : String(prices[19], 4).c_str(),
		prices[20] == ENTSOE_NO_VALUE ? "null" : String(prices[20], 4).c_str(),
		prices[21] == ENTSOE_NO_VALUE ? "null" : String(prices[21], 4).c_str(),
		prices[22] == ENTSOE_NO_VALUE ? "null" : String(prices[22], 4).c_str(),
		prices[23] == ENTSOE_NO_VALUE ? "null" : String(prices[23], 4).c_str(),
		prices[24] == ENTSOE_NO_VALUE ? "null" : String(prices[24], 4).c_str(),
		prices[25] == ENTSOE_NO_VALUE ? "null" : String(prices[25], 4).c_str(),
		prices[26] == ENTSOE_NO_VALUE ? "null" : String(prices[26], 4).c_str(),
		prices[27] == ENTSOE_NO_VALUE ? "null" : String(prices[27], 4).c_str(),
		prices[28] == ENTSOE_NO_VALUE ? "null" : String(prices[28], 4).c_str(),
		prices[29] == ENTSOE_NO_VALUE ? "null" : String(prices[29], 4).c_str(),
		prices[30] == ENTSOE_NO_VALUE ? "null" : String(prices[30], 4).c_str(),
		prices[31] == ENTSOE_NO_VALUE ? "null" : String(prices[31], 4).c_str(),
		prices[32] == ENTSOE_NO_VALUE ? "null" : String(prices[32], 4).c_str(),
		prices[33] == ENTSOE_NO_VALUE ? "null" : String(prices[33], 4).c_str(),
		prices[34] == ENTSOE_NO_VALUE ? "null" : String(prices[34], 4).c_str(),
		prices[35] == ENTSOE_NO_VALUE ? "null" : String(prices[35], 4).c_str()
	);

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	server.setContentLength(strlen(buf));
	server.send(200, MIME_JSON, buf);
}

void AmsWebServer::temperatureJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /temperature.json over http...\n");

	if(!checkSecurity(2))
		return;

	int count = hw->getTempSensorCount();
	snprintf(buf, 16, "{\"c\":%d,\"s\":[", count);

	for(int i = 0; i < count; i++) {
		TempSensorData* data = hw->getTempSensorData(i);
		if(data == NULL) continue;

		TempSensorConfig* conf = config->getTempSensorConfig(data->address);
		char* pos = buf+strlen(buf);
		snprintf_P(pos, 72, TEMPSENSOR_JSON, 
			i,
			toHex(data->address, 8).c_str(),
			conf == NULL ? "" : String(conf->name).substring(0,16).c_str(),
			conf == NULL || conf->common ? 1 : 0,
			data->lastRead
		);
		delay(10);
	}
	char* pos = buf+strlen(buf);
	snprintf(count == 0 ? pos : pos-1, 8, "]}");

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	server.setContentLength(strlen(buf));
	server.send(200, MIME_JSON, buf);
}

void AmsWebServer::indexHtml() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /index.html over http...\n");

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	if(!checkSecurity(2))
		return;
	server.setContentLength(INDEX_HTML_LEN);
	server.send_P(200, MIME_HTML, INDEX_HTML);
}

void AmsWebServer::indexCss() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /index.css over http...\n");

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	if(!checkSecurity(2))
		return;

	server.setContentLength(INDEX_CSS_LEN);
	server.send_P(200, MIME_CSS, INDEX_CSS);
}

void AmsWebServer::indexJs() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /index.js over http...\n");

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	if(!checkSecurity(2))
		return;

	server.setContentLength(INDEX_JS_LEN);
	server.send_P(200, MIME_JS, INDEX_JS);
}

void AmsWebServer::configurationJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /configuration.json over http...\n");

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	if(!checkSecurity(1))
		return;

	DynamicJsonDocument doc(2048);
	doc[F("version")] = VERSION;

	NtpConfig ntpConfig;
	config->getNtpConfig(ntpConfig);
	WiFiConfig wifiConfig;
	config->getWiFiConfig(wifiConfig);
	WebConfig webConfig;
	config->getWebConfig(webConfig);

	doc[F("g")][F("t")] = ntpConfig.timezone;
	doc[F("g")][F("h")] = wifiConfig.hostname;
	doc[F("g")][F("s")] = webConfig.security;
	doc[F("g")][F("u")] = webConfig.username;
	doc[F("g")][F("p")] = strlen(webConfig.password) > 0 ? "***" : "";

	bool encen = false;
	for(uint8_t i = 0; i < 16; i++) {
		if(meterConfig->encryptionKey[i] > 0) {
			encen = true;
		}
	}

	config->getMeterConfig(*meterConfig);
	doc[F("m")][F("b")] = meterConfig->baud;
	doc[F("m")][F("p")] = meterConfig->parity;
	doc[F("m")][F("i")] = meterConfig->invert;
	doc[F("m")][F("d")] = meterConfig->distributionSystem;
	doc[F("m")][F("f")] = meterConfig->mainFuse;
	doc[F("m")][F("r")] = meterConfig->productionCapacity;
	doc[F("m")][F("e")][F("e")] = encen;
	doc[F("m")][F("e")][F("k")] = toHex(meterConfig->encryptionKey, 16);
	doc[F("m")][F("e")][F("a")] = toHex(meterConfig->authenticationKey, 16);
	doc[F("m")][F("m")][F("e")] = meterConfig->wattageMultiplier > 1 || meterConfig->voltageMultiplier > 1 || meterConfig->amperageMultiplier > 1 || meterConfig->accumulatedMultiplier > 1;
	doc[F("m")][F("m")][F("w")] = meterConfig->wattageMultiplier / 1000.0;
	doc[F("m")][F("m")][F("v")] = meterConfig->voltageMultiplier / 1000.0;
	doc[F("m")][F("m")][F("a")] = meterConfig->amperageMultiplier / 1000.0;
	doc[F("m")][F("m")][F("c")] = meterConfig->accumulatedMultiplier / 1000.0;

	EnergyAccountingConfig eac;
	config->getEnergyAccountingConfig(eac);
	doc[F("t")][F("t")][0] = eac.thresholds[0];
	doc[F("t")][F("t")][1] = eac.thresholds[1];
	doc[F("t")][F("t")][2] = eac.thresholds[2];
	doc[F("t")][F("t")][3] = eac.thresholds[3];
	doc[F("t")][F("t")][4] = eac.thresholds[4];
	doc[F("t")][F("t")][5] = eac.thresholds[5];
	doc[F("t")][F("t")][6] = eac.thresholds[6];
	doc[F("t")][F("t")][7] = eac.thresholds[7];
	doc[F("t")][F("t")][8] = eac.thresholds[8];
	doc[F("t")][F("t")][9] = eac.thresholds[9];
	doc[F("t")][F("h")] = eac.hours;

	doc[F("w")][F("s")] = wifiConfig.ssid;
	doc[F("w")][F("p")] = strlen(wifiConfig.psk) > 0 ? "***" : "";
	doc[F("w")][F("w")] = wifiConfig.power / 10.0;
	doc[F("w")][F("z")] = wifiConfig.sleep;

	doc[F("n")][F("m")] = strlen(wifiConfig.ip) > 0 ? "static" : "dhcp";
	doc[F("n")][F("i")] = wifiConfig.ip;
	doc[F("n")][F("s")] = wifiConfig.subnet;
	doc[F("n")][F("g")] = wifiConfig.gateway;
	doc[F("n")][F("d1")] = wifiConfig.dns1;
	doc[F("n")][F("d2")] = wifiConfig.dns2;
	doc[F("n")][F("d")] = wifiConfig.mdns;
	doc[F("n")][F("n1")] = ntpConfig.server;
	doc[F("n")][F("h")] = ntpConfig.dhcp;

	MqttConfig mqttConfig;
	config->getMqttConfig(mqttConfig);
	doc[F("q")][F("h")] = mqttConfig.host;
	doc[F("q")][F("p")] = mqttConfig.port;
	doc[F("q")][F("u")] = mqttConfig.username;
	doc[F("q")][F("a")] = strlen(mqttConfig.password) > 0 ? "***" : "";
	doc[F("q")][F("c")] = mqttConfig.clientId;
	doc[F("q")][F("b")] = mqttConfig.publishTopic;
	doc[F("q")][F("m")] = mqttConfig.payloadFormat;
	doc[F("q")][F("s")][F("e")] = mqttConfig.ssl;

	if(LittleFS.begin()) {
		doc[F("q")][F("s")][F("c")] = LittleFS.exists(FILE_MQTT_CA);
		doc[F("q")][F("s")][F("r")] = LittleFS.exists(FILE_MQTT_CERT);
		doc[F("q")][F("s")][F("k")] = LittleFS.exists(FILE_MQTT_KEY);
		LittleFS.end();
	} else {
		doc[F("q")][F("s")][F("c")] = false;
		doc[F("q")][F("s")][F("r")] = false;
		doc[F("q")][F("s")][F("k")] = false;
	}

	EntsoeConfig entsoe;
	config->getEntsoeConfig(entsoe);
	doc[F("p")][F("e")] = strlen(entsoe.token) > 0;
	doc[F("p")][F("t")] = entsoe.token;
	doc[F("p")][F("r")] = entsoe.area;
	doc[F("p")][F("c")] = entsoe.currency;
	doc[F("p")][F("m")] = entsoe.multiplier / 1000.0;

	DebugConfig debugConfig;
	config->getDebugConfig(debugConfig);
	doc[F("d")][F("s")] = debugConfig.serial;
	doc[F("d")][F("t")] = debugConfig.telnet;
	doc[F("d")][F("l")] = debugConfig.level;

	GpioConfig gpioConfig;
	config->getGpioConfig(gpioConfig);
	if(gpioConfig.hanPin == 0xff)
		doc[F("i")][F("h")] = nullptr;
	else
		doc[F("i")][F("h")] = gpioConfig.hanPin;
	
	if(gpioConfig.apPin == 0xff)
		doc[F("i")][F("a")] = nullptr;
	else
		doc[F("i")][F("a")] = gpioConfig.apPin;
	
	if(gpioConfig.ledPin == 0xff)
		doc[F("i")][F("l")][F("p")] = nullptr;
	else
		doc[F("i")][F("l")][F("p")] = gpioConfig.ledPin;
	
	doc[F("i")][F("l")][F("i")] = gpioConfig.ledInverted;
	
	if(gpioConfig.ledPinRed == 0xff)
		doc[F("i")][F("r")][F("r")] = nullptr;
	else
		doc[F("i")][F("r")][F("r")] = gpioConfig.ledPinRed;

	if(gpioConfig.ledPinGreen == 0xff)
		doc[F("i")][F("r")][F("g")] = nullptr;
	else
		doc[F("i")][F("r")][F("g")] = gpioConfig.ledPinGreen;

	if(gpioConfig.ledPinBlue == 0xff)
		doc[F("i")][F("r")][F("b")] = nullptr;
	else
		doc[F("i")][F("r")][F("b")] = gpioConfig.ledPinBlue;

	doc[F("i")][F("r")][F("i")] = gpioConfig.ledRgbInverted;

	if(gpioConfig.tempSensorPin == 0xff)
		doc[F("i")][F("t")][F("d")] = nullptr;
	else
		doc[F("i")][F("t")][F("d")] = gpioConfig.tempSensorPin;

	if(gpioConfig.tempAnalogSensorPin == 0xff)
		doc[F("i")][F("t")][F("a")] = nullptr;
	else
		doc[F("i")][F("t")][F("a")] = gpioConfig.tempAnalogSensorPin;

	if(gpioConfig.vccPin == 0xff)
		doc[F("i")][F("v")][F("p")] = nullptr;
	else
		doc[F("i")][F("v")][F("p")] = gpioConfig.vccPin;

	if(gpioConfig.vccOffset == 0)
		doc[F("i")][F("v")][F("o")] = nullptr;
	else
		doc[F("i")][F("v")][F("o")] = gpioConfig.vccOffset / 100.0;

	if(gpioConfig.vccMultiplier == 0)
		doc[F("i")][F("v")][F("m")] = nullptr;
	else
		doc[F("i")][F("v")][F("m")] = gpioConfig.vccMultiplier / 1000.0;

	if(gpioConfig.vccResistorVcc == 0)
		doc[F("i")][F("v")][F("d")][F("v")] = nullptr;
	else
		doc[F("i")][F("v")][F("d")][F("v")] = gpioConfig.vccResistorVcc;

	if(gpioConfig.vccResistorGnd == 0)
		doc[F("i")][F("v")][F("d")][F("g")] = nullptr;
	else
		doc[F("i")][F("v")][F("d")][F("g")] = gpioConfig.vccResistorGnd;

	if(gpioConfig.vccBootLimit == 0)
		doc[F("i")][F("v")][F("b")] = nullptr;
	else
		doc[F("i")][F("v")][F("b")] = gpioConfig.vccBootLimit / 10.0;

	serializeJson(doc, buf, BufferSize);
	server.send(200, MIME_JSON, buf);
}
void AmsWebServer::handleSave() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Handling save method from http"));
	if(!checkSecurity(1))
		return;

	bool success = true;
	if(server.hasArg(F("v")) && server.arg(F("v")) == F("true")) {
		int boardType = server.arg(F("vb")).toInt();
		int hanPin = server.arg(F("vh")).toInt();
		config->clear();

		#if defined(CONFIG_IDF_TARGET_ESP32S2)
			switch(boardType) {
				case 5: // Pow-K+
				case 7: // Pow-U+
				case 6: // Pow-P1
					config->clearGpio(*gpioConfig);
					gpioConfig->hanPin = 16;
					gpioConfig->apPin = 0;
					gpioConfig->ledPinRed = 13;
					gpioConfig->ledPinGreen = 14;
					gpioConfig->ledRgbInverted = true;
					gpioConfig->vccPin = 10;
					gpioConfig->vccResistorGnd = 22;
					gpioConfig->vccResistorVcc = 33;
					break;
				case 51: // Wemos S2 mini
					gpioConfig->ledPin = 15;
					gpioConfig->ledInverted = false;
					gpioConfig->apPin = 0;
				case 50: // Generic ESP32-S2
					gpioConfig->hanPin = hanPin > 0 ? hanPin : 18;
					break;
				default:
					success = false;
			}
		#elif defined(CONFIG_IDF_TARGET_ESP32C3)
		#elif defined(ESP32)
			switch(boardType) {
				case 201: // D32
					gpioConfig->hanPin = hanPin > 0 ? hanPin : 16;
					gpioConfig->apPin = 4;
					gpioConfig->ledPin = 5;
					gpioConfig->ledInverted = true;
					break;
				case 202: // Feather
				case 203: // DevKitC
				case 200: // ESP32
					gpioConfig->hanPin = hanPin > 0 ? hanPin : 16;
					gpioConfig->ledPin = 2;
					gpioConfig->ledInverted = false;
					break;
				default:
					success = false;
			}
		#elif defined(ESP8266)
			switch(boardType) {
				case 2: // spenceme
					config->clearGpio(*gpioConfig);
					gpioConfig->vccBootLimit = 33;
					gpioConfig->hanPin = 3;
					gpioConfig->apPin = 0;
					gpioConfig->ledPin = 2;
					gpioConfig->ledInverted = true;
					gpioConfig->tempSensorPin = 5;
					break;
				case 0: // roarfred
					config->clearGpio(*gpioConfig);
					gpioConfig->hanPin = 3;
					gpioConfig->apPin = 0;
					gpioConfig->ledPin = 2;
					gpioConfig->ledInverted = true;
					gpioConfig->tempSensorPin = 5;
					break;
				case 1: // Arnio Kamstrup
				case 3: // Pow-K UART0
				case 4: // Pow-U UART0
					config->clearGpio(*gpioConfig);
					gpioConfig->hanPin = 3;
					gpioConfig->apPin = 0;
					gpioConfig->ledPin = 2;
					gpioConfig->ledInverted = true;
					gpioConfig->ledPinRed = 13;
					gpioConfig->ledPinGreen = 14;
					gpioConfig->ledRgbInverted = true;
					break;
				case 5: // Pow-K GPIO12
				case 7: // Pow-U GPIO12
					config->clearGpio(*gpioConfig);
					gpioConfig->hanPin = 12;
					gpioConfig->apPin = 0;
					gpioConfig->ledPin = 2;
					gpioConfig->ledInverted = true;
					gpioConfig->ledPinRed = 13;
					gpioConfig->ledPinGreen = 14;
					gpioConfig->ledRgbInverted = true;
					break;
				case 101: // D1
					gpioConfig->hanPin = hanPin > 0 ? hanPin : 5;
					gpioConfig->apPin = 4;
					gpioConfig->ledPin = 2;
					gpioConfig->ledInverted = true;
					gpioConfig->vccMultiplier = 1100;
					break;
				case 100: // ESP8266
					gpioConfig->hanPin = hanPin > 0 ? hanPin : 3;
					gpioConfig->ledPin = 2;
					gpioConfig->ledInverted = true;
					break;
				default:
					success = false;
			}
		#endif
		config->setGpioConfig(*gpioConfig);

		SystemConfig sys;
		config->getSystemConfig(sys);
		sys.boardType = success ? boardType : 0xFF;
		sys.vendorConfigured = success;
		config->setSystemConfig(sys);
	}

	if(server.hasArg(F("s")) && server.arg(F("s")) == F("true")) {
		SystemConfig sys;
		config->getSystemConfig(sys);

		config->clear();

		WiFiConfig wifi;
		config->clearWifi(wifi);

		strcpy(wifi.ssid, server.arg(F("ss")).c_str());

		String psk = server.arg(F("sp"));
		if(!psk.equals("***")) {
			strcpy(wifi.psk, psk.c_str());
		}
		wifi.mode = 1; // WIFI_STA

		if(server.hasArg(F("sm")) && server.arg(F("sm")) == "static") {
			strcpy(wifi.ip, server.arg(F("si")).c_str());
			strcpy(wifi.gateway, server.arg(F("sg")).c_str());
			strcpy(wifi.subnet, server.arg(F("su")).c_str());
			strcpy(wifi.dns1, server.arg(F("sd")).c_str());
		}

		if(server.hasArg(F("sh")) && !server.arg(F("sh")).isEmpty()) {
			strcpy(wifi.hostname, server.arg(F("sh")).c_str());
			wifi.mdns = true;
		} else {
			wifi.mdns = false;
		}
		
		switch(sys.boardType) {
			case 6: // Pow-P1
				meterConfig->baud = 115200;
				meterConfig->parity = 3; // 8N1
				break;
			case 3: // Pow-K UART0
			case 5: // Pow-K+
				meterConfig->parity = 3; // 8N1
			case 2: // spenceme
			case 50: // Generic ESP32-S2
			case 51: // Wemos S2 mini
				meterConfig->baud = 2400;
				wifi.sleep = 1; // Modem sleep
				break;
			case 4: // Pow-U UART0
			case 7: // Pow-U+
				wifi.sleep = 2; // Light sleep
				break;
		}
		config->setWiFiConfig(wifi);
		config->setMeterConfig(*meterConfig);
		
		sys.userConfigured = success;
		sys.dataCollectionConsent = 0;
		config->setSystemConfig(sys);

		performRestart = true;
	} else if(server.hasArg(F("sf")) && !server.arg(F("sf")).isEmpty()) {
		SystemConfig sys;
		config->getSystemConfig(sys);
		sys.dataCollectionConsent = server.hasArg(F("sf")) && (server.arg(F("sf")) == F("true") || server.arg(F("sf")) == F("1")) ? 1 : 2;
		config->setSystemConfig(sys);
	}

	if(server.hasArg(F("m")) && server.arg(F("m")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received meter config"));
		config->getMeterConfig(*meterConfig);
		meterConfig->baud = server.arg(F("mb")).toInt();
		meterConfig->parity = server.arg(F("mp")).toInt();
		meterConfig->invert = server.hasArg(F("mi")) && server.arg(F("mi")) == F("true");
		meterConfig->distributionSystem = server.arg(F("md")).toInt();
		meterConfig->mainFuse = server.arg(F("mf")).toInt();
		meterConfig->productionCapacity = server.arg(F("mr")).toInt();
		maxPwr = 0;

		String encryptionKeyHex = server.arg(F("mek"));
		if(!encryptionKeyHex.isEmpty()) {
			encryptionKeyHex.replace(F("0x"), F(""));
			fromHex(meterConfig->encryptionKey, encryptionKeyHex, 16);
		}

		String authenticationKeyHex = server.arg(F("mea"));
		if(!authenticationKeyHex.isEmpty()) {
			authenticationKeyHex.replace(F("0x"), F(""));
			fromHex(meterConfig->authenticationKey, authenticationKeyHex, 16);
		}

		meterConfig->wattageMultiplier = server.arg(F("mmw")).toDouble() * 1000;
		meterConfig->voltageMultiplier = server.arg(F("mmv")).toDouble() * 1000;
		meterConfig->amperageMultiplier = server.arg(F("mma")).toDouble() * 1000;
		meterConfig->accumulatedMultiplier = server.arg(F("mmc")).toDouble() * 1000;
		config->setMeterConfig(*meterConfig);
	}

	if(server.hasArg(F("w")) && server.arg(F("w")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received WiFi config"));
		WiFiConfig wifi;
		config->getWiFiConfig(wifi);
		strcpy(wifi.ssid, server.arg(F("ws")).c_str());
		String psk = server.arg(F("wp"));
		if(!psk.equals("***")) {
			strcpy(wifi.psk, psk.c_str());
		}
		wifi.power = server.arg(F("ww")).toFloat() * 10;
		wifi.sleep = server.arg(F("wz")).toInt();
		config->setWiFiConfig(wifi);

		if(server.hasArg(F("nm")) && server.arg(F("nm")) == "static") {
			strcpy(wifi.ip, server.arg(F("ni")).c_str());
			strcpy(wifi.gateway, server.arg(F("ng")).c_str());
			strcpy(wifi.subnet, server.arg(F("ns")).c_str());
			strcpy(wifi.dns1, server.arg(F("nd1")).c_str());
			strcpy(wifi.dns2, server.arg(F("nd2")).c_str());
		}
		wifi.mdns = server.hasArg(F("nd")) && server.arg(F("nd")) == F("true");
		config->setWiFiConfig(wifi);
	}

	if(server.hasArg(F("ntp")) && server.arg(F("ntp")) == F("true")) {
		NtpConfig ntp;
		config->getNtpConfig(ntp);
		ntp.enable = true;
		ntp.dhcp = server.hasArg(F("ntpd")) && server.arg(F("ntpd")) == F("true");
		strcpy(ntp.server, server.arg(F("ntph")).c_str());
		config->setNtpConfig(ntp);
	}

	if(server.hasArg(F("q")) && server.arg(F("q")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received MQTT config"));
		MqttConfig mqtt;
		config->getMqttConfig(mqtt);
		if(server.hasArg(F("qh")) && !server.arg(F("qh")).isEmpty()) {
			strcpy(mqtt.host, server.arg(F("qh")).c_str());
			strcpy(mqtt.clientId, server.arg(F("qc")).c_str());
			strcpy(mqtt.publishTopic, server.arg(F("qb")).c_str());
			strcpy(mqtt.subscribeTopic, server.arg(F("qr")).c_str());
			strcpy(mqtt.username, server.arg(F("qu")).c_str());
			String pass = server.arg(F("qa"));
			if(!pass.equals("***")) {
				strcpy(mqtt.password, pass.c_str());
			}
			mqtt.payloadFormat = server.arg(F("qm")).toInt();
			#if defined(ESP8266)
			mqtt.ssl = false;
			#else
			mqtt.ssl = server.arg(F("qs")) == F("true");
			#endif

			mqtt.port = server.arg(F("qp")).toInt();
			if(mqtt.port == 0) {
				mqtt.port = mqtt.ssl ? 8883 : 1883;
			}
		} else {
			config->clearMqtt(mqtt);
		}
		config->setMqttConfig(mqtt);
	}

	if(server.hasArg(F("dc")) && server.arg(F("dc")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received Domoticz config"));
		DomoticzConfig domo {
			static_cast<uint16_t>(server.arg(F("elidx")).toInt()),
			static_cast<uint16_t>(server.arg(F("vl1idx")).toInt()),
			static_cast<uint16_t>(server.arg(F("vl2idx")).toInt()),
			static_cast<uint16_t>(server.arg(F("vl3idx")).toInt()),
			static_cast<uint16_t>(server.arg(F("cl1idx")).toInt())
		};
		config->setDomoticzConfig(domo);
	}


	if(server.hasArg(F("g")) && server.arg(F("g")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received web config"));
		webConfig.security = server.arg(F("gs")).toInt();
		if(webConfig.security > 0) {
			strcpy(webConfig.username, server.arg(F("gu")).c_str());
			String pass = server.arg(F("gp"));
			if(!pass.equals("***")) {
				strcpy(webConfig.password, pass.c_str());
			}
			debugger->setPassword(webConfig.password);
		} else {
			strcpy_P(webConfig.username, PSTR(""));
			strcpy_P(webConfig.password, PSTR(""));
			debugger->setPassword(F(""));
		}
		config->setWebConfig(webConfig);

		WiFiConfig wifi;
		config->getWiFiConfig(wifi);
		if(server.hasArg(F("gh")) && !server.arg(F("gh")).isEmpty()) {
			strcpy(wifi.hostname, server.arg(F("gh")).c_str());
		}
		config->setWiFiConfig(wifi);

		NtpConfig ntp;
		config->getNtpConfig(ntp);
		strcpy(ntp.timezone, server.arg(F("gt")).c_str());
		config->setNtpConfig(ntp);
	}

	if(server.hasArg(F("i")) && server.arg(F("i")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received GPIO config"));
		gpioConfig->hanPin = server.hasArg(F("ih")) && !server.arg(F("ih")).isEmpty() ? server.arg(F("ih")).toInt() : 3;
		gpioConfig->ledPin = server.hasArg(F("ilp")) && !server.arg(F("ilp")).isEmpty() ? server.arg(F("ilp")).toInt() : 0xFF;
		gpioConfig->ledInverted = server.hasArg(F("ili")) && server.arg(F("ili")) == F("true");
		gpioConfig->ledPinRed = server.hasArg(F("irr")) && !server.arg(F("irr")).isEmpty() ? server.arg(F("irr")).toInt() : 0xFF;
		gpioConfig->ledPinGreen = server.hasArg(F("irg")) && !server.arg(F("irg")).isEmpty() ? server.arg(F("irg")).toInt() : 0xFF;
		gpioConfig->ledPinBlue = server.hasArg(F("irb")) && !server.arg(F("irb")).isEmpty() ? server.arg(F("irb")).toInt() : 0xFF;
		gpioConfig->ledRgbInverted = server.hasArg(F("iri")) && server.arg(F("iri")) == F("true");
		gpioConfig->apPin = server.hasArg(F("ia")) && !server.arg(F("ia")).isEmpty() ? server.arg(F("ia")).toInt() : 0xFF;
		gpioConfig->tempSensorPin = server.hasArg(F("itd")) && !server.arg(F("itd")).isEmpty() ?server.arg(F("itd")).toInt() : 0xFF;
		gpioConfig->tempAnalogSensorPin = server.hasArg(F("ita")) && !server.arg(F("ita")).isEmpty() ?server.arg(F("ita")).toInt() : 0xFF;
		gpioConfig->vccPin = server.hasArg(F("ivp")) && !server.arg(F("ivp")).isEmpty() ? server.arg(F("ivp")).toInt() : 0xFF;
		gpioConfig->vccResistorGnd = server.hasArg(F("ivdg")) && !server.arg(F("ivdg")).isEmpty() ? server.arg(F("ivdg")).toInt() : 0;
		gpioConfig->vccResistorVcc = server.hasArg(F("ivdv")) && !server.arg(F("ivdv")).isEmpty() ? server.arg(F("ivdv")).toInt() : 0;
		config->setGpioConfig(*gpioConfig);
	}

	if(server.hasArg(F("iv")) && server.arg(F("iv")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received Vcc config"));
		gpioConfig->vccOffset = server.hasArg(F("ivo")) && !server.arg(F("ivo")).isEmpty() ? server.arg(F("ivo")).toFloat() * 100 : 0;
		gpioConfig->vccMultiplier = server.hasArg(F("ivm")) && !server.arg(F("ivm")).isEmpty() ? server.arg(F("ivm")).toFloat() * 1000 : 1000;
		gpioConfig->vccBootLimit = server.hasArg(F("ivb")) && !server.arg(F("ivb")).isEmpty() ? server.arg(F("ivb")).toFloat() * 10 : 0;
		config->setGpioConfig(*gpioConfig);
	}

	if(server.hasArg(F("d")) && server.arg(F("d")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received Debug config"));
		DebugConfig debug;
		config->getDebugConfig(debug);
		bool active = debug.serial || debug.telnet;

		debug.telnet = server.hasArg(F("dt")) && server.arg(F("dt")) == F("true");
		debug.serial = server.hasArg(F("ds")) && server.arg(F("ds")) == F("true");
		debug.level = server.arg(F("dl")).toInt();

		if(debug.telnet || debug.serial) {
			if(webConfig.security > 0) {
				debugger->setPassword(webConfig.password);
			} else {
				debugger->setPassword(F(""));
			}
			debugger->setSerialEnabled(debug.serial);
			WiFiConfig wifi;
			if(config->getWiFiConfig(wifi) && strlen(wifi.hostname) > 0) {
				debugger->begin(wifi.hostname, (uint8_t) debug.level);
				if(!debug.telnet) {
					debugger->stop();
				}
			}
		} else if(active) {
			performRestart = true;
		}
		config->setDebugConfig(debug);
	}

	if(server.hasArg(F("p")) && server.arg(F("p")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received price API config"));
		EntsoeConfig entsoe;
		strcpy(entsoe.token, server.arg(F("pt")).c_str());
		strcpy(entsoe.area, server.arg(F("pr")).c_str());
		strcpy(entsoe.currency, server.arg(F("pc")).c_str());
		entsoe.multiplier = server.arg(F("pm")).toFloat() * 1000;
		config->setEntsoeConfig(entsoe);
	}

	if(server.hasArg(F("t")) && server.arg(F("t")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Received energy accounting config"));
		EnergyAccountingConfig eac;
		eac.thresholds[0] = server.arg(F("t0")).toInt();
		eac.thresholds[1] = server.arg(F("t1")).toInt();
		eac.thresholds[2] = server.arg(F("t2")).toInt();
		eac.thresholds[3] = server.arg(F("t3")).toInt();
		eac.thresholds[4] = server.arg(F("t4")).toInt();
		eac.thresholds[5] = server.arg(F("t5")).toInt();
		eac.thresholds[6] = server.arg(F("t6")).toInt();
		eac.thresholds[7] = server.arg(F("t7")).toInt();
		eac.thresholds[8] = server.arg(F("t8")).toInt();
		eac.hours = server.arg(F("th")).toInt();
		config->setEnergyAccountingConfig(eac);
	}

	if(debugger->isActive(RemoteDebug::INFO)) debugger->printf(PSTR("Saving configuration now..."));

	if (config->save()) {
		if(debugger->isActive(RemoteDebug::INFO)) debugger->printf(PSTR("Successfully saved."));
		if(config->isWifiChanged() || performRestart) {
			performRestart = true;
		} else {
			hw->setup(gpioConfig, config);
		}
	} else {
		success = false;
	}

	snprintf_P(buf, BufferSize, RESPONSE_JSON,
		success ? "true" : "false",
		"",
		performRestart ? "true" : "false"
	);
	server.setContentLength(strlen(buf));
	server.send(200, MIME_JSON, buf);

	server.handleClient();
	delay(250);

	if(performRestart || rebootForUpgrade) {
		if(ds != NULL) {
			ds->save();
		}
		if(debugger->isActive(RemoteDebug::INFO)) debugger->printf(PSTR("Rebooting"));
		delay(1000);
		#if defined(ESP8266)
			ESP.reset();
		#elif defined(ESP32)
			ESP.restart();
		#endif
		performRestart = false;
	}
}

void AmsWebServer::wifiScanJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /wifiscan.json over http...\n");

	DynamicJsonDocument doc(512);

	serializeJson(doc, buf, BufferSize);
	server.send(200, MIME_JSON, buf);
}

void AmsWebServer::reboot() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /reboot over http...\n");

	DynamicJsonDocument doc(128);
	doc[F("reboot")] = true;

	serializeJson(doc, buf, BufferSize);
	server.send(200, MIME_JSON, buf);

	server.handleClient();
	delay(250);

	if(debugger->isActive(RemoteDebug::INFO)) debugger->printf(PSTR("Rebooting"));
	delay(1000);
	#if defined(ESP8266)
		ESP.reset();
	#elif defined(ESP32)
		ESP.restart();
	#endif
	performRestart = false;
}

void AmsWebServer::upgrade() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /upgrade over http...\n");

	if(!checkSecurity(1))
		return;

	SystemConfig sys;
	config->getSystemConfig(sys);

	snprintf_P(buf, BufferSize, RESPONSE_JSON,
		sys.dataCollectionConsent == 1 ? "true" : "false",
		"",
		sys.dataCollectionConsent == 1 ? "true" : "false"
	);
	server.setContentLength(strlen(buf));
	server.send(200, MIME_JSON, buf);

	if(sys.dataCollectionConsent == 1) {
		server.handleClient();
		delay(250);

		String customFirmwareUrl = "";
		if(server.hasArg(F("url"))) {
			customFirmwareUrl = server.arg(F("url"));
		}

		String url = customFirmwareUrl.isEmpty() || !customFirmwareUrl.startsWith(F("http")) ? F("http://ams2mqtt.rewiredinvent.no/hub/firmware/update") : customFirmwareUrl;

		if(server.hasArg(F("version"))) {
			url += "/" + server.arg(F("version"));
		}

		WiFiClient client;
		#if defined(ESP8266)
			String chipType = F("esp8266");
		#elif defined(CONFIG_IDF_TARGET_ESP32S2)
			String chipType = F("esp32s2");
		#elif defined(ESP32)
			#if defined(CONFIG_FREERTOS_UNICORE)
				String chipType = F("esp32solo");
			#else
				String chipType = F("esp32");
			#endif
		#endif

		#if defined(ESP8266)
			ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
			t_httpUpdate_return ret = ESPhttpUpdate.update(client, url, VERSION);
		#elif defined(ESP32)
			HTTPUpdate httpUpdate;
			httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
			HTTPUpdateResult ret = httpUpdate.update(client, url, String(VERSION) + "-" + chipType);
		#endif

		switch(ret) {
			case HTTP_UPDATE_FAILED:
				debugger->printf(PSTR("Update failed"));
				break;
			case HTTP_UPDATE_NO_UPDATES:
				debugger->printf(PSTR("No Update"));
				break;
			case HTTP_UPDATE_OK:
				debugger->printf(PSTR("Update OK"));
				break;
		}
	}
}

void AmsWebServer::firmwarePost() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Handling firmware post..."));
	if(!checkSecurity(1))
		return;

	server.send(200);
}


void AmsWebServer::firmwareUpload() {
	if(!checkSecurity(1))
		return;

	HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if(!filename.endsWith(".bin")) {
            server.send(500, MIME_PLAIN, "500: couldn't create file");
		} else {
			#if defined(ESP32)
				esp_task_wdt_delete(NULL);
				esp_task_wdt_deinit();
			#elif defined(ESP8266)
				ESP.wdtDisable();
			#endif
		}
	}
	uploadFile(FILE_FIRMWARE);
	if(upload.status == UPLOAD_FILE_END) {
		rebootForUpgrade = true;
		server.sendHeader("Location","/");
		server.send(302);
	}
}

HTTPUpload& AmsWebServer::uploadFile(const char* path) {
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_START){
		if(uploading) {
			if(debugger->isActive(RemoteDebug::ERROR)) debugger->printf(PSTR("Upload already in progress"));
			server.send_P(500, MIME_HTML, PSTR("<html><body><h1>Upload already in progress!</h1></body></html>"));
		} else if (!LittleFS.begin()) {
			if(debugger->isActive(RemoteDebug::ERROR)) debugger->printf(PSTR("An Error has occurred while mounting LittleFS"));
			server.send_P(500, MIME_HTML, PSTR("<html><body><h1>Unable to mount LittleFS!</h1></body></html>"));
		} else {
			uploading = true;
			if(debugger->isActive(RemoteDebug::DEBUG)) {
				debugger->printf_P(PSTR("handleFileUpload file: %s\n"), path);
			}
			if(LittleFS.exists(path)) {
				LittleFS.remove(path);
			}
		    file = LittleFS.open(path, "w");
			if(debugger->isActive(RemoteDebug::DEBUG)) {
				debugger->printf_P(PSTR("handleFileUpload Open file and write: %u\n"), upload.currentSize);
			}
            size_t written = file.write(upload.buf, upload.currentSize);
			if(debugger->isActive(RemoteDebug::DEBUG)) {
				debugger->printf_P(PSTR("handleFileUpload Written: %u\n"), written);
			}
	    } 
    } else if(upload.status == UPLOAD_FILE_WRITE) {
		if(debugger->isActive(RemoteDebug::DEBUG)) {
			debugger->printf_P(PSTR("handleFileUpload Writing: %u\n"), upload.currentSize);
		}
        if(file) {
            size_t written = file.write(upload.buf, upload.currentSize);
			if(debugger->isActive(RemoteDebug::DEBUG)) {
				debugger->printf_P(PSTR("handleFileUpload Written: %u\n"), written);
			}
			delay(1);
			if(written != upload.currentSize) {
				file.flush();
				file.close();
				LittleFS.remove(path);
				LittleFS.end();

				if(debugger->isActive(RemoteDebug::ERROR)) debugger->printf(PSTR("An Error has occurred while writing file"));
				snprintf_P(buf, BufferSize, RESPONSE_JSON,
					"false",
					"Unable to upload",
					"false"
				);
				server.setContentLength(strlen(buf));
				server.send(500, MIME_JSON, buf);
			}
		}
    } else if(upload.status == UPLOAD_FILE_END) {
        if(file) {
			file.flush();
            file.close();
//			LittleFS.end();
        } else {
			snprintf_P(buf, BufferSize, RESPONSE_JSON,
				"false",
				"Unable to upload",
				"false"
			);
			server.setContentLength(strlen(buf));
			server.send(500, MIME_JSON, buf);
        }
    }
	return upload;
}

void AmsWebServer::isAliveCheck() {
	server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
	server.send(200);
}

void AmsWebServer::factoryResetPost() {
	if(!checkSecurity(1))
		return;

	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Performing factory reset"));

	bool success = false;
	if(server.hasArg(F("perform")) && server.arg(F("perform")) == F("true")) {
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Formatting LittleFS"));
		LittleFS.format();
		if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf(PSTR("Clearing configuration"));
		config->clear();

		success = true;
	}

	snprintf_P(buf, BufferSize, RESPONSE_JSON,
		success ? "true" : "false",
		"",
		"true"
	);
	server.setContentLength(strlen(buf));
	server.send(200, MIME_JSON, buf);

	server.handleClient();
	delay(250);

	if(debugger->isActive(RemoteDebug::INFO)) debugger->printf(PSTR("Rebooting"));
	delay(1000);
	#if defined(ESP8266)
		ESP.reset();
	#elif defined(ESP32)
		ESP.restart();
	#endif
}

void AmsWebServer::robotstxt() {
	server.send_P(200, MIME_HTML, PSTR("User-agent: *\nDisallow: /\n"));
}

void AmsWebServer::mqttCaUpload() {
	if(!checkSecurity(1))
		return;

	uploadFile(FILE_MQTT_CA);
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_END) {
		server.sendHeader(HEADER_LOCATION,F("/configuration"));
		server.send(303);

		MqttConfig mqttConfig;
		if(config->getMqttConfig(mqttConfig) && mqttConfig.ssl) {
			config->setMqttChanged();
		}
	}
}

void AmsWebServer::mqttCertUpload() {
	if(!checkSecurity(1))
		return;

	uploadFile(FILE_MQTT_CERT);
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_END) {
		server.sendHeader(HEADER_LOCATION,F("/configuration"));
		server.send(303);
		MqttConfig mqttConfig;
		if(config->getMqttConfig(mqttConfig) && mqttConfig.ssl) {
			config->setMqttChanged();
		}
	}
}

void AmsWebServer::mqttKeyUpload() {
	if(!checkSecurity(1))
		return;

	uploadFile(FILE_MQTT_KEY);
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_END) {
		server.sendHeader(HEADER_LOCATION,F("/configuration"));
		server.send(303);
		MqttConfig mqttConfig;
		if(config->getMqttConfig(mqttConfig) && mqttConfig.ssl) {
			config->setMqttChanged();
		}
	}
}

void AmsWebServer::tariffJson() {
	if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf("Serving /tariff.json over http...\n");

	server.sendHeader(HEADER_CACHE_CONTROL, CACHE_CONTROL_NO_CACHE);
	server.sendHeader(HEADER_PRAGMA, PRAGMA_NO_CACHE);
	server.sendHeader(HEADER_EXPIRES, EXPIRES_OFF);

	if(!checkSecurity(1))
		return;

	EnergyAccountingConfig* eac = ea->getConfig();
	EnergyAccountingData data = ea->getData();

	DynamicJsonDocument doc(512);
	JsonArray thresholds = doc.createNestedArray(F("t"));
    for(uint8_t x = 0;x < 10; x++) {
		thresholds.add(eac->thresholds[x]);
	}
	JsonArray peaks = doc.createNestedArray(F("p"));
    for(uint8_t x = 0;x < min((uint8_t) 5, eac->hours); x++) {
		JsonObject p = peaks.createNestedObject();
		EnergyAccountingPeak peak = ea->getPeak(x+1);
		p["d"] = peak.day;
		p["v"] = peak.value / 100.0;
	}
	doc["c"] = ea->getCurrentThreshold();
	doc["m"] = ea->getMonthMax();

	serializeJson(doc, buf, BufferSize);
	server.send(200, MIME_JSON, buf);

}