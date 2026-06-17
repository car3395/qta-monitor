#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

const char* WIFI_SSID = "SEU_WIFI";
const char* WIFI_PASS = "SUA_SENHA";

const char* FIREBASE_HOST = "https://qta-monitor-default-rtdb.firebaseio.com";

const int PINO_SENSOR_TENSAO = 34;
const int PINO_RELE_GERADOR = 4;

const int TENSAO_NOMINAL = 127;

const int ADC_MINIMO = 0;
const int ADC_MAXIMO = 4095;
const int TENSAO_MAXIMA_MEDIDA = (TENSAO_NOMINAL * 13) / 10;

const float FATOR_FALHA_BAIXA = 0.80;
const float FATOR_FALHA_ALTA = 1.15;
const float FATOR_RETORNO_BAIXA = 0.90;
const float FATOR_RETORNO_ALTA = 1.10;

const unsigned long INTERVALO_ENVIO = 5000;
const unsigned long TEMPO_CONFIRMAR_FALHA = 3000;
const unsigned long TEMPO_CONFIRMAR_RETORNO = 5000;
const unsigned long TEMPO_MINIMO_GERADOR_LIGADO = 10000;

// O rele abaixo deve comandar apenas a partida/contator da reserva.
// Use intertravamento fisico/eletrico entre rede e gerador no quadro QTA.
// O software reduz risco de comutacao rapida, mas nao substitui protecao fisica.

unsigned long ultimoEnvio = 0;
unsigned long inicioConfirmacaoFalha = 0;
unsigned long inicioConfirmacaoRetorno = 0;
unsigned long geradorLigadoDesde = 0;

bool modoAutomatico = true;
int tensaoAtual = TENSAO_NOMINAL;
bool tensaoDefinida = false;
bool redeOkAtual = true;
bool geradorLigado = false;
bool alarmeAtual = false;
String motivoAtual = "normal";

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PINO_SENSOR_TENSAO, INPUT);
  pinMode(PINO_RELE_GERADOR, OUTPUT);
  atualizarSaidasFisicas();

  conectarWiFi();

  Serial.println();
  Serial.println("======================================");
  Serial.println("QTA Monitor - ESP32 Arduino IDE");
  Serial.println("======================================");
  Serial.println("AUTO ON  -> ativa leitura do monitor de tensao no GPIO 34");
  Serial.println("AUTO OFF -> para leitura automatica");
  Serial.println("Digite um valor para teste manual. Ex: 127");
  Serial.println("======================================");
}

void loop() {
  verificarEntradaSerial();

  if (modoAutomatico && millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    tensaoAtual = lerTensaoMonitor();
    tensaoDefinida = true;
    atualizarIntertravamento(tensaoAtual);
    enviarSistemaFirebase(tensaoAtual);
    ultimoEnvio = millis();
  }

  if (!modoAutomatico && tensaoDefinida && millis() - ultimoEnvio >= INTERVALO_ENVIO) {
    atualizarIntertravamento(tensaoAtual);
    enviarSistemaFirebase(tensaoAtual);
    ultimoEnvio = millis();
  }
}

void conectarWiFi() {
  WiFi.mode(WIFI_STA);
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
  Serial.print("[WiFi] Sinal RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

void verificarEntradaSerial() {
  if (!Serial.available()) return;

  String entrada = Serial.readStringUntil('\n');
  entrada.trim();
  entrada.toUpperCase();

  if (entrada.length() == 0) return;

  if (entrada == "AUTO ON") {
    modoAutomatico = true;
    Serial.println("[OK] Leitura automatica pelo monitor de tensao ativada.");
    return;
  }

  if (entrada == "AUTO OFF") {
    modoAutomatico = false;
    Serial.println("[OK] Leitura automatica desligada.");
    return;
  }

  int tensaoDigitada = entrada.toInt();

  if (tensaoDigitada <= 0) {
    Serial.println("[ERRO] Digite uma tensao valida. Ex: 127");
    return;
  }

  modoAutomatico = false;
  tensaoAtual = tensaoDigitada;
  tensaoDefinida = true;

  Serial.print("[TESTE MANUAL] Tensao digitada: ");
  Serial.print(tensaoAtual);
  Serial.println(" V");

  atualizarIntertravamento(tensaoAtual);
  enviarSistemaFirebase(tensaoAtual);
  ultimoEnvio = millis();
}

void atualizarIntertravamento(int tensaoRede) {
  bool redeDentroRetorno = tensaoDentroDaFaixaRetorno(tensaoRede);
  bool redeEmFalha = tensaoForaDaFaixaFalha(tensaoRede);
  motivoAtual = motivoFalha(tensaoRede);

  if (!geradorLigado) {
    if (redeEmFalha) {
      if (inicioConfirmacaoFalha == 0) {
        inicioConfirmacaoFalha = millis();
        Serial.println("[INTERTRAVAMENTO] Falha detectada. Confirmando antes de ligar gerador...");
      }

      if (millis() - inicioConfirmacaoFalha >= TEMPO_CONFIRMAR_FALHA) {
        ligarGerador();
      }
    } else {
      inicioConfirmacaoFalha = 0;
      redeOkAtual = true;
      alarmeAtual = false;
      motivoAtual = "normal";
    }

    atualizarSaidasFisicas();
    return;
  }

  redeOkAtual = false;
  alarmeAtual = true;

  if (redeDentroRetorno) {
    if (inicioConfirmacaoRetorno == 0) {
      inicioConfirmacaoRetorno = millis();
      Serial.println("[INTERTRAVAMENTO] Rede voltou. Confirmando estabilidade antes de desligar gerador...");
    }

    bool retornoConfirmado = millis() - inicioConfirmacaoRetorno >= TEMPO_CONFIRMAR_RETORNO;
    bool tempoMinimoOk = millis() - geradorLigadoDesde >= TEMPO_MINIMO_GERADOR_LIGADO;

    if (retornoConfirmado && tempoMinimoOk) {
      desligarGerador();
      redeOkAtual = true;
      alarmeAtual = false;
      motivoAtual = "normal";
    } else if (retornoConfirmado && !tempoMinimoOk) {
      Serial.println("[INTERTRAVAMENTO] Rede estavel, aguardando tempo minimo do gerador.");
    }
  } else {
    inicioConfirmacaoRetorno = 0;
  }

  atualizarSaidasFisicas();
}

void ligarGerador() {
  geradorLigado = true;
  redeOkAtual = false;
  alarmeAtual = true;
  geradorLigadoDesde = millis();
  inicioConfirmacaoFalha = 0;
  inicioConfirmacaoRetorno = 0;
  Serial.println("[INTERTRAVAMENTO] Gerador acionado pelo rele.");
}

void desligarGerador() {
  geradorLigado = false;
  inicioConfirmacaoFalha = 0;
  inicioConfirmacaoRetorno = 0;
  Serial.println("[INTERTRAVAMENTO] Gerador desligado. Retorno para rede normal.");
}

void atualizarSaidasFisicas() {
  digitalWrite(PINO_RELE_GERADOR, geradorLigado ? HIGH : LOW);
}

int lerTensaoMonitor() {
  int leituraAdc = analogRead(PINO_SENSOR_TENSAO);
  int tensao = converterAdcParaTensao(leituraAdc);

  Serial.print("[MONITOR TENSAO] GPIO34 ADC=");
  Serial.print(leituraAdc);
  Serial.print(" | Tensao=");
  Serial.print(tensao);
  Serial.println(" V");

  return tensao;
}

int converterAdcParaTensao(int leituraAdc) {
  leituraAdc = constrain(leituraAdc, ADC_MINIMO, ADC_MAXIMO);
  return map(leituraAdc, ADC_MINIMO, ADC_MAXIMO, 0, TENSAO_MAXIMA_MEDIDA);
}

void enviarSistemaFirebase(int tensaoRede) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERRO] WiFi desconectado. Reconectando...");
    conectarWiFi();
  }

  String modo = geradorLigado ? "gerador" : "rede";
  String estado = redeOkAtual ? "REDE_NORMAL" : "REDE_FALHA_RESERVA_ATIVA";
  String fonteAtiva = geradorLigado ? "GERADOR" : "REDE";

  String json = "{";

  json += "\"modo\":\"" + modo + "\",";
  json += "\"estado\":\"" + estado + "\",";
  json += "\"alarme\":" + boolJson(alarmeAtual) + ",";
  json += "\"fonte_ativa\":\"" + fonteAtiva + "\",";
  json += "\"tensao_nominal\":" + String(TENSAO_NOMINAL) + ",";
  json += "\"motivo_falha_rede\":\"" + motivoAtual + "\",";

  json += "\"limite_falha_baixa\":" + String(limiteFalhaBaixa()) + ",";
  json += "\"limite_falha_alta\":" + String(limiteFalhaAlta()) + ",";
  json += "\"limite_retorno_baixa\":" + String(limiteRetornoBaixa()) + ",";
  json += "\"limite_retorno_alta\":" + String(limiteRetornoAlta()) + ",";

  json += "\"rede_status\":" + boolJson(redeOkAtual) + ",";
  json += "\"gerador_status\":" + boolJson(geradorLigado) + ",";
  json += "\"tensao\":" + String(tensaoRede) + ",";

  json += "\"rede\":{";
  json += "\"status\":" + boolJson(redeOkAtual) + ",";
  json += "\"tensao\":" + String(tensaoRede);
  json += "},";

  json += "\"gerador\":{";
  json += "\"status\":" + boolJson(geradorLigado) + ",";
  json += "\"tensao\":0";
  json += "}";

  json += "}";

  enviarFirebase("/sistema", json);
}

void enviarFirebase(String caminho, String json) {
  const int tentativasMaximas = 3;

  for (int tentativa = 1; tentativa <= tentativasMaximas; tentativa++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[ERRO] WiFi desconectado. Reconectando...");
      conectarWiFi();
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(15000);
    http.setReuse(false);

    String url = String(FIREBASE_HOST) + caminho + ".json";

    Serial.println();
    Serial.print("[Firebase] Tentativa ");
    Serial.print(tentativa);
    Serial.print(" de ");
    Serial.println(tentativasMaximas);
    Serial.print("[Firebase] URL: ");
    Serial.println(url);

    if (!http.begin(client, url)) {
      Serial.println("[ERRO] Falha ao iniciar HTTP.");
      http.end();
      delay(2000);
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");

    int httpCode = http.PUT(json);

    Serial.print("[Firebase] HTTP CODE: ");
    Serial.println(httpCode);

    if (httpCode < 0) {
      Serial.print("[Firebase] Erro: ");
      Serial.println(http.errorToString(httpCode));
    }

    String resposta = http.getString();

    if (httpCode >= 200 && httpCode < 300) {
      Serial.println("[OK] Firebase atualizado!");
      http.end();
      client.stop();
      return;
    }

    Serial.println("[ERRO] Firebase nao recebeu.");

    if (resposta.length() > 0) {
      Serial.print("Resposta: ");
      Serial.println(resposta);
    } else {
      Serial.println("Resposta vazia.");
    }

    http.end();
    client.stop();

    Serial.println("[Firebase] Aguardando para tentar novamente...");
    delay(3000);
  }

  Serial.println("[ERRO] Todas as tentativas falharam.");
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

bool tensaoForaDaFaixaFalha(int tensao) {
  return tensao < limiteFalhaBaixa() || tensao > limiteFalhaAlta();
}

String motivoFalha(int tensao) {
  if (tensao < limiteFalhaBaixa()) return "subtensao";
  if (tensao > limiteFalhaAlta()) return "sobretensao";
  return "normal";
}
