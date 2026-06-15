#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

const char* WIFI_SSID = "SEU_WIFI";
const char* WIFI_PASS = "SUA_SENHA";

const char* FIREBASE_HOST = "https://qta-monitor-default-rtdb.firebaseio.com";

const int PINO_SENSOR_TENSAO = A0;
const int TENSAO_NOMINAL = 127;

const float FATOR_FALHA_BAIXA = 0.80;
const float FATOR_FALHA_ALTA = 1.15;
const float FATOR_RETORNO_BAIXA = 0.90;
const float FATOR_RETORNO_ALTA = 1.10;

const unsigned long INTERVALO_ENVIO = 5000;

unsigned long ultimoEnvio = 0;
bool modoAutomatico = true;

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PINO_SENSOR_TENSAO, INPUT);

  conectarWiFi();

  Serial.println();
  Serial.println("======================================");
  Serial.println("QTA Monitor - Teste Firebase ESP8266");
  Serial.println("======================================");
  Serial.println("Digite uma tensao para testar. Ex: 127");
  Serial.println("Digite AUTO ON para ativar leitura A0");
  Serial.println("Digite AUTO OFF para parar leitura A0");
  Serial.println("======================================");
}

void loop() {
  verificarEntradaSerial();

  if (modoAutomatico && millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    int tensao = lerTensaoAnalogica();
    enviarSistemaFirebase(tensao);
    ultimoEnvio = millis();
  }
}

void conectarWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Conectando WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("[OK] WiFi conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void verificarEntradaSerial() {
  if (!Serial.available()) return;

  String entrada = Serial.readStringUntil('\n');
  entrada.trim();
  entrada.toUpperCase();

  if (entrada.length() == 0) return;

  if (entrada == "AUTO ON") {
    modoAutomatico = true;
    Serial.println("[OK] Leitura automatica pelo A0 ativada.");
    return;
  }

  if (entrada == "AUTO OFF") {
    modoAutomatico = false;
    Serial.println("[OK] Leitura automatica pelo A0 desligada.");
    return;
  }

  int tensaoDigitada = entrada.toInt();

  if (tensaoDigitada <= 0) {
    Serial.println("[ERRO] Digite uma tensao valida. Ex: 127");
    return;
  }

  Serial.print("[TESTE MANUAL] Tensao digitada: ");
  Serial.print(tensaoDigitada);
  Serial.println(" V");

  enviarSistemaFirebase(tensaoDigitada);
}

int lerTensaoAnalogica() {
  int leituraAdc = analogRead(PINO_SENSOR_TENSAO);
  int tensao = map(leituraAdc, 0, 1023, 0, TENSAO_NOMINAL * 1.30);

  Serial.print("[LEITURA A0] ADC=");
  Serial.print(leituraAdc);
  Serial.print(" | Tensao=");
  Serial.print(tensao);
  Serial.println(" V");

  return tensao;
}

void enviarSistemaFirebase(int tensaoRede) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERRO] WiFi desconectado. Reconectando...");
    conectarWiFi();
  }

  bool redeOk = tensaoDentroDaFaixaRetorno(tensaoRede);
  String motivo = motivoFalha(tensaoRede);

  String modo = redeOk ? "rede" : "gerador";
  String estado = redeOk ? "REDE_NORMAL" : "REDE_FALHA_RESERVA_ATIVA";
  String fonteAtiva = redeOk ? "REDE" : "GERADOR";
  bool alarme = !redeOk;
  bool geradorStatus = !redeOk;

  String json = "{";

  json += "\"modo\":\"" + modo + "\",";
  json += "\"estado\":\"" + estado + "\",";
  json += "\"alarme\":" + boolJson(alarme) + ",";
  json += "\"fonte_ativa\":\"" + fonteAtiva + "\",";
  json += "\"tensao_nominal\":" + String(TENSAO_NOMINAL) + ",";
  json += "\"motivo_falha_rede\":\"" + motivo + "\",";

  json += "\"limite_falha_baixa\":" + String(limiteFalhaBaixa()) + ",";
  json += "\"limite_falha_alta\":" + String(limiteFalhaAlta()) + ",";
  json += "\"limite_retorno_baixa\":" + String(limiteRetornoBaixa()) + ",";
  json += "\"limite_retorno_alta\":" + String(limiteRetornoAlta()) + ",";

  json += "\"rede_status\":" + boolJson(redeOk) + ",";
  json += "\"gerador_status\":" + boolJson(geradorStatus) + ",";
  json += "\"tensao\":" + String(tensaoRede) + ",";

  json += "\"rede\":{";
  json += "\"status\":" + boolJson(redeOk) + ",";
  json += "\"tensao\":" + String(tensaoRede);
  json += "},";

  json += "\"gerador\":{";
  json += "\"status\":" + boolJson(geradorStatus) + ",";
  json += "\"tensao\":0";
  json += "}";

  json += "}";

  enviarFirebase("/sistema", json);
}

void enviarFirebase(String caminho, String json) {
  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(FIREBASE_HOST) + caminho + ".json";

  Serial.println();
  Serial.print("[Firebase] URL: ");
  Serial.println(url);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.PATCH(json);

  Serial.print("[Firebase] HTTP CODE: ");
  Serial.println(httpCode);

  String resposta = http.getString();

  if (httpCode >= 200 && httpCode < 300) {
    Serial.println("[OK] Firebase atualizado!");
  } else {
    Serial.println("[ERRO] Firebase nao recebeu.");
    Serial.print("Resposta: ");
    Serial.println(resposta);
  }

  http.end();
}

String boolJson(bool valor) {
  return valor ? "true" : "false";
}

int limiteFalhaBaixa() {
  return round(TENSAO_NOMINAL * FATOR_FALHA_BAIXA);
}

int limiteFalhaAlta() {
  return round(TENSAO_NOMINAL * FATOR_FALHA_ALTA);
}

int limiteRetornoBaixa() {
  return round(TENSAO_NOMINAL * FATOR_RETORNO_BAIXA);
}

int limiteRetornoAlta() {
  return round(TENSAO_NOMINAL * FATOR_RETORNO_ALTA);
}

bool tensaoDentroDaFaixaRetorno(int tensao) {
  return tensao >= limiteRetornoBaixa() && tensao <= limiteRetornoAlta();
}

String motivoFalha(int tensao) {
  if (tensao < limiteFalhaBaixa()) return "subtensao";
  if (tensao > limiteFalhaAlta()) return "sobretensao";
  return "normal";
}
