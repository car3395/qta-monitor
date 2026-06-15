#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#else
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif
#include <time.h>

const char* WIFI_SSID = "SEU_WIFI";
const char* WIFI_PASS = "SUA_SENHA";

const char* FIREBASE_HOST = "https://qta-monitor-default-rtdb.firebaseio.com";

const bool MODO_SIMULACAO = false;
const bool LOG_LEITURA_TENSAO = true;
const int TENSAO_NOMINAL = 127;

const float FATOR_FALHA_BAIXA = 0.80;
const float FATOR_FALHA_ALTA = 1.15;
const float FATOR_RETORNO_BAIXA = 0.90;
const float FATOR_RETORNO_ALTA = 1.10;

const unsigned long TEMPO_CONFIRMAR_FALHA = 3000;
const unsigned long TEMPO_CONFIRMAR_RETORNO = 5000;

#if defined(ESP8266)
const int PINO_RELE = D1;
const int PINO_LED_REDE = D2;
const int PINO_LED_GERADOR = D7;
const int PINO_BUZZER = D0;
const int PINO_SENSOR_TENSAO_REDE = A0;
const int PINO_PZEM_RX = D5;
const int PINO_PZEM_TX = D6;
const int ADC_MAXIMO = 1023;
#else
const int PINO_RELE = 23;
const int PINO_LED_REDE = 18;
const int PINO_LED_GERADOR = 19;
const int PINO_BUZZER = 21;
const int PINO_SENSOR_TENSAO_REDE = 34;
const int ADC_MAXIMO = 4095;
#endif

bool redeStatus = true;
bool redeAnterior = true;

bool geradorStatus = false;
bool geradorAnterior = false;

bool alarme = false;

String modo = "rede";
String estado = "REDE_NORMAL";
String fonteAtiva = "REDE";

int redeTensao = TENSAO_NOMINAL;
int geradorTensao = 0;
String motivoFalhaRede = "normal";

int quedasEnergia = 0;
int acionamentosGerador = 0;

unsigned long falhaInicioMillis = 0;
unsigned long tempoTotalSemRede = 0;
unsigned long falhaInicioFirebaseSeg = 0;
unsigned long inicioConfirmacaoFalha = 0;
unsigned long inicioConfirmacaoRetorno = 0;

unsigned long ultimoTeste = 0;
unsigned long ultimoEnvioStatus = 0;
bool simulouQueda = false;

void conectarWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.println();
  Serial.println("========================================");
  Serial.println("QTA Monitor - iniciando conexao");
  Serial.println("========================================");
  Serial.print("Conectando WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("[OK] WiFi conectado com sucesso!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("Abra este IP no roteador/celular apenas se precisar conferir a rede.");
}

void configurarRelogio() {
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  Serial.print("Sincronizando relogio");
  time_t agora = time(nullptr);
  int tentativas = 0;

  while (agora < 1700000000 && tentativas < 20) {
    delay(500);
    Serial.print(".");
    agora = time(nullptr);
    tentativas++;
  }

  Serial.println();

  if (agora >= 1700000000) {
    Serial.println("Relogio sincronizado.");
  } else {
    Serial.println("Relogio ainda nao sincronizado. Tentara novamente nos proximos envios.");
  }
}

unsigned long agoraSeg() {
  time_t agora = time(nullptr);

  if (agora < 1700000000) {
    return 0;
  }

  return (unsigned long)agora;
}

String timestampAtual() {
  time_t agora = time(nullptr);

  if (agora < 1700000000) {
    return "sem_hora_sincronizada";
  }

  struct tm tempoInfo;
  localtime_r(&agora, &tempoInfo);

  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tempoInfo);
  return String(buffer);
}

String jsonBool(bool valor) {
  return valor ? "true" : "false";
}

bool firebaseRequest(const String& metodo, const String& path, const String& json) {
#if defined(ESP8266)
  BearSSL::WiFiClientSecure client;
#else
  WiFiClientSecure client;
#endif
  client.setInsecure();

  HTTPClient https;
  String url = String(FIREBASE_HOST) + path + ".json";

  Serial.println();
  Serial.print(metodo);
  Serial.print(" URL: ");
  Serial.println(url);

  if (!https.begin(client, url)) {
    Serial.println("Erro ao iniciar HTTPS");
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  int httpCode = 0;

  if (metodo == "PUT") {
    httpCode = https.PUT(json);
  } else if (metodo == "POST") {
    httpCode = https.POST(json);
  } else {
    httpCode = https.sendRequest(metodo.c_str(), json);
  }

  Serial.print("HTTP CODE: ");
  Serial.println(httpCode);

  String resposta = "";

  if (httpCode > 0) {
    resposta = https.getString();
  } else {
    Serial.println("[ERRO] Falha na requisicao Firebase.");
    Serial.println("Confira WiFi, internet e URL do Realtime Database.");
  }

  https.end();

  bool sucesso = (httpCode >= 200 && httpCode < 300);

  if (sucesso) {
    Serial.print("[OK] Firebase recebeu os dados em ");
    Serial.println(path);
  } else if (httpCode > 0) {
    Serial.print("[ERRO] Firebase recusou a gravacao em ");
    Serial.println(path);
    Serial.print("Resposta: ");
    Serial.println(resposta);
  }

  return sucesso;
}

bool firebasePATCH(const String& path, const String& json) {
  return firebaseRequest("PATCH", path, json);
}

bool firebasePUT(const String& path, const String& json) {
  return firebaseRequest("PUT", path, json);
}

bool firebasePOST(const String& path, const String& json) {
  return firebaseRequest("POST", path, json);
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

bool redeForaDaFaixaDeFalha(int tensao) {
  return tensao < limiteFalhaBaixa() || tensao > limiteFalhaAlta();
}

bool redeDentroDaFaixaDeRetorno(int tensao) {
  return tensao >= limiteRetornoBaixa() && tensao <= limiteRetornoAlta();
}

String motivoFalhaPorTensao(int tensao) {
  if (tensao < limiteFalhaBaixa()) return "subtensao";
  if (tensao > limiteFalhaAlta()) return "sobretensao";
  return "normal";
}

int converterLeituraAnalogicaParaTensao(int leituraAdc) {
  // Ajuste este mapeamento depois conforme o monitor/sensor de tensao usado na bancada.
  return map(leituraAdc, 0, ADC_MAXIMO, 0, (long)(TENSAO_NOMINAL * 1.30));
}

int lerTensaoRede() {
  int leitura = analogRead(PINO_SENSOR_TENSAO_REDE);
  int tensaoCalculada = converterLeituraAnalogicaParaTensao(leitura);

  if (LOG_LEITURA_TENSAO) {
    Serial.print("[LEITURA] ADC=");
    Serial.print(leitura);
    Serial.print(" | Rede=");
    Serial.print(tensaoCalculada);
    Serial.println(" V");
  }

  return tensaoCalculada;
}

void atualizarSaidasFisicas() {
  digitalWrite(PINO_RELE, geradorStatus ? HIGH : LOW);
  digitalWrite(PINO_LED_REDE, redeStatus ? HIGH : LOW);
  digitalWrite(PINO_LED_GERADOR, geradorStatus ? HIGH : LOW);
  digitalWrite(PINO_BUZZER, alarme ? HIGH : LOW);
}

void atualizarEstadoDerivado() {
  if (redeStatus) {
    modo = "rede";
    estado = "REDE_NORMAL";
    fonteAtiva = "REDE";
    geradorStatus = false;
    geradorTensao = 0;
    alarme = false;
    motivoFalhaRede = "normal";
  } else {
    modo = "gerador";
    estado = motivoFalhaRede == "sobretensao" ? "REDE_SOBRETENSAO_RESERVA_ATIVA" : "REDE_FALHA_RESERVA_ATIVA";
    fonteAtiva = "GERADOR";
    geradorStatus = true;
    geradorTensao = 0;
    alarme = true;
  }
}

bool enviarStatusSistema() {
  String json = "{";

  json += "\"modo\":\"" + modo + "\",";
  json += "\"estado\":\"" + estado + "\",";
  json += "\"alarme\":" + jsonBool(alarme) + ",";
  json += "\"fonte_ativa\":\"" + fonteAtiva + "\",";
  json += "\"tensao_nominal\":" + String(TENSAO_NOMINAL) + ",";
  json += "\"motivo_falha_rede\":\"" + motivoFalhaRede + "\",";
  json += "\"limite_falha_baixa\":" + String(limiteFalhaBaixa()) + ",";
  json += "\"limite_falha_alta\":" + String(limiteFalhaAlta()) + ",";
  json += "\"limite_retorno_baixa\":" + String(limiteRetornoBaixa()) + ",";
  json += "\"limite_retorno_alta\":" + String(limiteRetornoAlta()) + ",";

  json += "\"rede_status\":" + jsonBool(redeStatus) + ",";
  json += "\"gerador_status\":" + jsonBool(geradorStatus) + ",";
  json += "\"tensao\":" + String(redeTensao) + ",";
  json += "\"falha_inicio\":";
  if (!redeStatus && falhaInicioFirebaseSeg > 0) {
    json += String(falhaInicioFirebaseSeg) + "000";
  } else {
    json += "null";
  }
  json += ",";

  json += "\"rede\":{";
  json += "\"status\":" + jsonBool(redeStatus) + ",";
  json += "\"tensao\":" + String(redeTensao);
  json += "},";

  json += "\"gerador\":{";
  json += "\"status\":" + jsonBool(geradorStatus) + ",";
  json += "\"tensao\":" + String(geradorTensao);
  json += "}";

  json += "}";

  return firebasePATCH("/sistema", json);
}

bool enviarEstatisticas() {
  String json = "{";

  json += "\"quedas_energia\":" + String(quedasEnergia) + ",";
  json += "\"tempo_total_sem_rede\":" + String(tempoTotalSemRede) + ",";
  json += "\"acionamentos_gerador\":" + String(acionamentosGerador);

  json += "}";

  return firebasePATCH("/estatisticas", json);
}

void registrarEvento(const String& tipo) {
  String json = "{";

  json += "\"tipo\":\"" + tipo + "\",";
  json += "\"modo\":\"" + modo + "\",";
  json += "\"fonte_ativa\":\"" + fonteAtiva + "\",";
  json += "\"timestamp\":\"" + timestampAtual() + "\"";

  json += "}";

  firebasePOST("/eventos", json);
}

void processarMudancasDeEstado() {
  if (redeStatus == false && redeAnterior == true) {
    Serial.println("QUEDA DE REDE");
    Serial.print("Motivo: ");
    Serial.println(motivoFalhaRede);

    quedasEnergia++;

    falhaInicioMillis = millis();
    falhaInicioFirebaseSeg = agoraSeg();

    atualizarEstadoDerivado();
    atualizarSaidasFisicas();

    registrarEvento("queda_rede");
    registrarEvento("gerador_ligado");
  }

  if (redeStatus == true && redeAnterior == false) {
    Serial.println("RETORNO DA REDE");

    unsigned long duracaoFalha = 0;

    if (falhaInicioMillis > 0) {
      duracaoFalha = (millis() - falhaInicioMillis) / 1000;
    }

    tempoTotalSemRede += duracaoFalha;
    falhaInicioMillis = 0;
    falhaInicioFirebaseSeg = 0;

    atualizarEstadoDerivado();
    atualizarSaidasFisicas();

    registrarEvento("retorno_rede");
    registrarEvento("gerador_desligado");
  }

  if (geradorStatus == true && geradorAnterior == false) {
    acionamentosGerador++;
  }

  atualizarEstadoDerivado();
  atualizarSaidasFisicas();
  enviarStatusSistema();
  enviarEstatisticas();

  redeAnterior = redeStatus;
  geradorAnterior = geradorStatus;
}

void avaliarRedePorTensaoReal() {
  if (MODO_SIMULACAO) {
    return;
  }

  redeTensao = lerTensaoRede();

  geradorTensao = 0;

  if (redeStatus) {
    if (redeForaDaFaixaDeFalha(redeTensao)) {
      if (inicioConfirmacaoFalha == 0) {
        inicioConfirmacaoFalha = millis();
        motivoFalhaRede = motivoFalhaPorTensao(redeTensao);

        Serial.print("Rede fora da faixa. Aguardando confirmacao: ");
        Serial.print(redeTensao);
        Serial.print(" V / motivo: ");
        Serial.println(motivoFalhaRede);
      }

      if (millis() - inicioConfirmacaoFalha >= TEMPO_CONFIRMAR_FALHA) {
        redeStatus = false;
        inicioConfirmacaoFalha = 0;
        inicioConfirmacaoRetorno = 0;
        processarMudancasDeEstado();
      }
    } else {
      inicioConfirmacaoFalha = 0;
      motivoFalhaRede = "normal";
    }

    return;
  }

  if (redeDentroDaFaixaDeRetorno(redeTensao)) {
    if (inicioConfirmacaoRetorno == 0) {
      inicioConfirmacaoRetorno = millis();

      Serial.print("Rede dentro da faixa de retorno. Aguardando estabilizar: ");
      Serial.print(redeTensao);
      Serial.println(" V");
    }

    if (millis() - inicioConfirmacaoRetorno >= TEMPO_CONFIRMAR_RETORNO) {
      redeStatus = true;
      inicioConfirmacaoFalha = 0;
      inicioConfirmacaoRetorno = 0;
      processarMudancasDeEstado();
    }
  } else {
    inicioConfirmacaoRetorno = 0;
    motivoFalhaRede = motivoFalhaPorTensao(redeTensao);
  }
}

void simularSistema() {
  if (!MODO_SIMULACAO) {
    return;
  }

  if (!simulouQueda && millis() > 10000) {
    Serial.println();
    Serial.println("[SIMULACAO] QUEDA DE ENERGIA");

    redeStatus = false;
    redeTensao = 0;
    geradorTensao = 0;
    motivoFalhaRede = "subtensao";
    processarMudancasDeEstado();

    simulouQueda = true;
    ultimoTeste = millis();
  }

  if (simulouQueda && millis() - ultimoTeste > 10000) {
    Serial.println();
    Serial.println("[SIMULACAO] REDE VOLTOU");

    redeStatus = true;
    redeTensao = TENSAO_NOMINAL;
    geradorTensao = 0;
    motivoFalhaRede = "normal";
    processarMudancasDeEstado();

    simulouQueda = false;
    ultimoTeste = millis() + 9999999;
  }
}

void enviarStatusPeriodico() {
  if (millis() - ultimoEnvioStatus < 5000) {
    return;
  }

  atualizarEstadoDerivado();
  atualizarSaidasFisicas();
  enviarStatusSistema();
  enviarEstatisticas();

  ultimoEnvioStatus = millis();
}

void setup() {
  Serial.begin(115200);

  pinMode(PINO_RELE, OUTPUT);
  pinMode(PINO_LED_REDE, OUTPUT);
  pinMode(PINO_LED_GERADOR, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  pinMode(PINO_SENSOR_TENSAO_REDE, INPUT);

  redeTensao = TENSAO_NOMINAL;
  atualizarEstadoDerivado();
  atualizarSaidasFisicas();

  conectarWiFi();
  configurarRelogio();

  bool statusOk = enviarStatusSistema();
  bool estatisticasOk = enviarEstatisticas();
  registrarEvento("inicializacao");

  Serial.println();
  Serial.println("========================================");
  if (statusOk && estatisticasOk) {
    Serial.println("[OK] Sistema conectado ao Firebase.");
    Serial.println("[OK] Supervisorios mobile e desktop ja podem ler os dados.");
  } else {
    Serial.println("[ATENCAO] WiFi conectou, mas o Firebase nao confirmou todos os dados.");
    Serial.println("Confira regras do Realtime Database e o link do projeto.");
  }
  Serial.println("========================================");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado. Reconectando...");
    conectarWiFi();
  }

  simularSistema();
  avaliarRedePorTensaoReal();
  enviarStatusPeriodico();

  delay(500);
}
