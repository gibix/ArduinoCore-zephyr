/*
  WireGuard Web Client

  Connects to WiFi, establishes a WireGuard VPN tunnel,
  then makes an HTTP request through the tunnel.

  Fill in your credentials in arduino_secrets.h
*/

#include <WiFi.h>
#include <ZephyrClient.h>
#include <WireGuard.h>

#include "arduino_secrets.h"

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

// The server to connect to through the VPN tunnel
char server[] = "159.69.202.103";

ZephyrClient client;

void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ;
  }

  // Connect to WiFi
  int status = WL_IDLE_STATUS;
  while (status != WL_CONNECTED) {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    status = WiFi.begin(ssid, pass);
    delay(3000);
  }

  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  // Start WireGuard VPN
  Serial.println("Starting WireGuard...");
  int ret = WireGuard.begin(WG_LOCAL_IP, WG_PRIVATE_KEY,
                            WG_PEER_PUB_KEY, WG_ENDPOINT);
  if (ret < 0) {
    Serial.print("WireGuard failed: ");
    Serial.println(ret);
    while (true)
      ;
  }
  Serial.println("WireGuard tunnel established");

  // Allow time for handshake
  delay(2000);

  // Make HTTP request through the tunnel
  Serial.println("Connecting to server...");
  if (client.connect(server, 101010)) {
    Serial.println("Connected");
    client.println("GET / HTTP/1.1");
    client.print("Host: ");
    client.println(server);
    client.println("Connection: close");
    client.println();
  }
}

void loop() {
  while (client.available()) {
    char c = client.read();
    Serial.write(c);
  }

  if (!client.connected()) {
    Serial.println();
    Serial.println("Disconnected.");
    client.stop();
    while (true)
      ;
  }
}
