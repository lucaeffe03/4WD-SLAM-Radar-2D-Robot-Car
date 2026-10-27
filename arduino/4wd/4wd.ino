#include <Servo.h>
#include <WiFiS3.h>

const char* ssid = "";
const char* password = "";

WiFiServer server(80);

// CABLAGGIO DEFINITIVO MOTORI (Con cappucci jumper inseriti su ENA e ENB) 
const int IN1 = 4;   // Motore SX (Direzione 1)
const int IN2 = 12;  // Motore SX (Direzione 2)
const int IN3 = 13;  // Motore DX (Direzione 1)
const int IN4 = 6;   // Motore DX (Direzione 2)

// TUTTI E 3 I SENSORI IR ATTIVI (Traguardo Finale)
const int IR_SX     = 2;  
const int IR_CENTRO = 10; 
const int IR_DX     = 11; 

// PARAMETRI DI SICUREZZA ANTI-SFONDAMENTO
const float PASSO_STIMATO_CM         = 13.0; 
const float RETROMARCIA_CM           = 4.5;  

const int DISTANZA_SICUREZZA_AVVIO   = 19; // Parte SOLO se ha almeno 19 cm veri davanti
const int DISTANZA_FRENO_EMERGENZA   = 14; // Inchioda subito a 14 cm dal muro
const unsigned long TEMPO_MAX_PASSO  = 580;

// Calibrazione Rotazione 4WD
const unsigned long TEMPO_ROTAZIONE_MS = 250; 
const float GRADI_ROTAZIONE_REALI      = 18.0; 

// ULTRASUONI HC-SR04 E SERVOMOTORE SG90 
const int servoPin = 7;
const int trigPin  = 8;
const int echoPin  = 9;
Servo testa;

float posX = 0.0, posY = 0.0, theta = 90.0;
bool robotInMovimento = false;
bool percorsoCompletato = false; 
unsigned long tempoAvvioMovimento = 0;

// Valori salvati durante la scansione a 7 angoli
int distEstremaDx = 100; // 5°
int distDiagDx    = 100; // 35°
int distCentroDx  = 100; // 65°
int distCentro    = 100; // 90°
int distCentroSx  = 100; // 115°
int distDiagSx    = 100; // 145°
int distEstremaSx = 100; // 175°

// Memoria di manovra: evita che a metà curva cambi idea o torni indietro
// 0 = nessuna, 1 = sta girando a Destra, 2 = sta girando a Sinistra
int direzioneInCorso = 0; 

int contatoreScansioni = 0;
String ultimaScansione = "MAP:0.0,0.0,90|5:100|35:100|65:100|90:100|115:100|145:100|175:100";

void normalizzaTheta() {
  while (theta < 0.0)   theta += 360.0;
  while (theta >= 360.0) theta -= 360.0;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  fermaMotori();

  pinMode(IR_SX, INPUT);
  pinMode(IR_CENTRO, INPUT);
  pinMode(IR_DX, INPUT);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  
  testa.attach(servoPin, 500, 2400);
  testa.write(90);

  Serial.print("Connessione a "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  delay(3000);
  server.begin();
  Serial.println("\nWi-Fi Connesso!");
  Serial.print("IP Robot: ");
  Serial.println(WiFi.localIP());
}

bool attendiConWiFi(unsigned long ms) {
  unsigned long inizio = millis();
  while (millis() - inizio < ms) {
    gestisciServerWiFi();
    if (!robotInMovimento || percorsoCompletato) {
      fermaMotori();
      return false;
    }
    delay(5);
  }
  return true;
}

int letturaGrezzaUS(unsigned long timeoutUs) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long dur = pulseIn(echoPin, HIGH, timeoutUs);
  if (dur == 0) return 150;
  return (int)(dur * 0.034 / 2);
}

// Lettura sicura: scarta solo i falsi zeri (<= 3 cm), ma NON ignora mai i muri vicini!
int misuraDistanza() {
  int d1 = letturaGrezzaUS(25000);
  if (d1 <= 3) {
    delay(12);
    int d2 = letturaGrezzaUS(25000);
    return (d2 <= 3) ? 150 : d2;
  }
  return d1;
}

bool ostacoloVeroDavanti() {
  int d1 = letturaGrezzaUS(10000);
  if (d1 > 3 && d1 <= DISTANZA_FRENO_EMERGENZA) {
    delay(6);
    int d2 = letturaGrezzaUS(10000);
    if (d2 > 3 && d2 <= DISTANZA_FRENO_EMERGENZA + 2) {
      return true;
    }
  }
  return false;
}

bool controllaTraguardo() {
  if (millis() - tempoAvvioMovimento < 1500) {
    return false;
  }
  if (digitalRead(IR_SX) == HIGH || digitalRead(IR_CENTRO) == HIGH || digitalRead(IR_DX) == HIGH) {
    fermaMotori();
    percorsoCompletato = true;
    robotInMovimento = false;
    Serial.println("🏆 TRAGUARDO FINALE RAGGIUNTO!");
    return true;
  }
  return false;
}

bool avanzaProtetto(unsigned long timeoutMaxMs, float &cmPercorsi) {
  testa.write(90);
  vaiAvanti();
  unsigned long inizio = millis();
  unsigned long ultimoControlloSonar = millis();

  while (millis() - inizio < 120) {
    gestisciServerWiFi();
    if (!robotInMovimento || percorsoCompletato) { fermaMotori(); return false; }
    if (controllaTraguardo()) { cmPercorsi = 0.0; return false; }
    if (ostacoloVeroDavanti()) {
      fermaMotori();
      cmPercorsi = 0.0;
      return true;
    }
    delay(4);
  }

  while (millis() - inizio < timeoutMaxMs) {
    gestisciServerWiFi();
    if (!robotInMovimento || percorsoCompletato) { fermaMotori(); return false; }

    if (controllaTraguardo()) {
      cmPercorsi = 0.0;
      return false;
    }

    if (millis() - ultimoControlloSonar >= 55) {
      ultimoControlloSonar = millis();
      if (ostacoloVeroDavanti()) {
        fermaMotori();
        unsigned long tempoTrascorso = millis() - inizio;
        cmPercorsi = PASSO_STIMATO_CM * ((float)tempoTrascorso / (float)timeoutMaxMs);
        return true;
      }
    }
    delay(4);
  }

  fermaMotori();
  cmPercorsi = PASSO_STIMATO_CM;
  return true;
}

void loop() {
  gestisciServerWiFi();

  if (!robotInMovimento || percorsoCompletato) {
    fermaMotori();
    return;
  }

  if (controllaTraguardo()) return;

  // 1. SCANSIONE RADAR COMPLETA A 7 ANGOLI
  scansionaEInviaMappa();
  if (!robotInMovimento || percorsoCompletato) { fermaMotori(); return; }
  if (!attendiConWiFi(80)) return;
  if (controllaTraguardo()) return;

  // Per avanzare in sicurezza, sia il centro (90°) sia le due semidiagonali (65° e 115°) 
  // non devono avere un muro incollato al muso
  bool stradaDavantiLibera = (distCentro > DISTANZA_SICUREZZA_AVVIO) && 
                             (distCentroDx > 12) && 
                             (distCentroSx > 12);

  // 2. MOVIMENTO IN AVANTI
  if (stradaDavantiLibera) {
    float cmFatti = 0.0;

    if (!avanzaProtetto(TEMPO_MAX_PASSO, cmFatti)) {
      return;
    }

    if (cmFatti > 5.5) {
      float rad = theta * 3.14159 / 180.0;
      posX += cmFatti * cos(rad);
      posY += cmFatti * sin(rad);
      // Ha avanzato con successo! Azzera la memoria di curva
      direzioneInCorso = 0; 
    }
  } 
  else {
    // 3. OSTACOLO DAVANTI -> MANOVRA DI SBLOCCO E STERZATA SUGLI ESTREMI (5° e 175°)
    
    // Fa retromarcia SOLO all'inizio della manovra (quando direzioneInCorso == 0)
    // Così non continua a fare avanti e indietro se gli servono 2 scatti di rotazione per curvare!
    if (direzioneInCorso == 0) {
      vaiIndietro();
      if (!attendiConWiFi(220)) return;
      fermaMotori();

      if (controllaTraguardo()) return;

      float rad = theta * 3.14159 / 180.0;
      posX -= RETROMARCIA_CM * cos(rad);
      posY -= RETROMARCIA_CM * sin(rad);
      if (!attendiConWiFi(120)) return;

      // Misura di nuovo gli ESTREMI ASSOLUTI 5° (Destra) e 175° (Sinistra) dopo essere arretrato
      testa.write(5);   if (!attendiConWiFi(320)) return; int dDx5   = misuraDistanza();
      testa.write(175); if (!attendiConWiFi(380)) return; int dSx175 = misuraDistanza();
      testa.write(90);  if (!attendiConWiFi(220)) return;

      // Combina l'estrema destra (5°) e l'estrema sinistra (175°) con i valori della scansione
      int aperturaDestra   = max(dDx5, distEstremaDx) + (distDiagDx / 2);
      int aperturaSinistra = max(dSx175, distEstremaSx) + (distDiagSx / 2);

      // Sceglie la direzione e la BLOCCA in memoria finché non avrà strada libera davanti
      if (aperturaDestra > aperturaSinistra) {
        direzioneInCorso = 1; // Destra
      } else {
        direzioneInCorso = 2; // Sinistra
      }
    }

    // Esegue la rotazione mantenendo la direzione scelta (senza tornare indietro)
    if (direzioneInCorso == 1) {
      giraDestra();
      if (!attendiConWiFi(TEMPO_ROTAZIONE_MS)) return;
      fermaMotori();
      theta -= GRADI_ROTAZIONE_REALI;
    } else {
      giraSinistra();
      if (!attendiConWiFi(TEMPO_ROTAZIONE_MS)) return;
      fermaMotori();
      theta += GRADI_ROTAZIONE_REALI;
    }
    normalizzaTheta();

    // Rimosso l'avanzamento cieco dopo la rotazione
    // Al prossimo ciclo del loop farà subito una scansione da fermo: 
    // - Se la strada davanti è finalmente libera, avanzerà in sicurezza.
    // - Se non basta, farà un altro scatto di rotazione nella STESSA direzione senza fare retromarcia.
  }

  attendiConWiFi(140);
}

int scansionaEInviaMappa() {
  int angoli[7] = {5, 35, 65, 90, 115, 145, 175};
  String pkt = "MAP:" + String(posX, 1) + "," + String(posY, 1) + "," + String(theta, 1);

  for (int i = 0; i < 7; i++) {
    if (!robotInMovimento || percorsoCompletato) { fermaMotori(); testa.write(90); return 0; }
    if (controllaTraguardo()) { testa.write(90); return 0; }

    testa.write(angoli[i]);
    
    unsigned long tempoAttesa = (i == 0) ? 320 : 145;
    if (!attendiConWiFi(tempoAttesa)) { testa.write(90); return 0; }
    
    int d = misuraDistanza();
    if (angoli[i] == 5)   distEstremaDx = d;
    if (angoli[i] == 35)  distDiagDx    = d;
    if (angoli[i] == 65)  distCentroDx  = d;
    if (angoli[i] == 90)  distCentro    = d;
    if (angoli[i] == 115) distCentroSx  = d;
    if (angoli[i] == 145) distDiagSx    = d;
    if (angoli[i] == 175) distEstremaSx = d;

    pkt += "|" + String(angoli[i]) + ":" + String(d);
  }
  testa.write(90);
  attendiConWiFi(140);
  ultimaScansione = pkt;
  contatoreScansioni++;
  Serial.println(pkt);
  return distCentro;
}

void gestisciServerWiFi() {
  WiFiClient client = server.available();
  if (!client) return;

  String currentLine = "", req = "";
  unsigned long timeout = millis();
  while (client.connected() && (millis() - timeout < 250)) {
    if (client.available()) {
      char c = client.read();
      req += c;
      if (c == '\n') {
        if (currentLine.length() == 0) {
          if (req.indexOf("GET /start") >= 0) {
            posX = 0.0;
            posY = 0.0;
            theta = 90.0;
            direzioneInCorso = 0;
            percorsoCompletato = false;
            robotInMovimento = true;
            tempoAvvioMovimento = millis();
          } 
          else if (req.indexOf("GET /stop") >= 0) {
            robotInMovimento = false;
            fermaMotori();
          }

          client.println("HTTP/1.1 200 OK");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
          client.println("Access-Control-Allow-Headers: *");
          client.println("Content-Type: text/plain;charset=utf-8");
          client.println("Connection: close");
          client.println();
          
          String risposta = String(contatoreScansioni) + "#" + ultimaScansione;
          if (percorsoCompletato) {
            risposta += "#TRAGUARDO";
          }
          client.print(risposta);
          break;
        } else { currentLine = ""; }
      } else if (c != '\r') { currentLine += c; }
    }
  }
  client.stop();
}

void vaiAvanti() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

void vaiIndietro() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void giraDestra() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void giraSinistra() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

void fermaMotori() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);
}