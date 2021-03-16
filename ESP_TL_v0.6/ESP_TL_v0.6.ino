/* Version history:
 *  V0.6:     Now all 15 traffic lights are known by the software
 *  V0.5:     Broker mwau.webhop.me is now standard broker and user login is required for it
 *            Bug with string lengths in callback fixed, additional debug output and a reconnect() in mode_iot()
 *            Additional call to pubsubBroker() in reconnect() to register as a new client on demand
 *  V0.4:     Another bug in blinkResponse fixed: The pressing of a button during blinkResponse did not result in another blink response
 *  V0.3:     Code cleanup and optimization in blinkResponse function
 *  V0.2:     Skipping WiFi connection by holding the select button on startup. This leads to a standalone traffic light that will not support IoT.
 *            During the first trial of connection with the MQTT broker, the green LED will blink during the waiting time for next trial.
 *            As soon as the broker connection was successful, the LED will not blink any more on new reconnects.
 *  V0.1:     Random client IDs. Publish of software version with topic TL0xversion
 */
char swVersion[] = "V0.6";

/************/
/* Includes */
/************/

#include <WiFi.h>
#include <PubSubClient.h>

/***********/
/* DEFINES */
/***********/

// ESP-32 output pins
#define RED 25
#define YELLOW 26
#define GREEN 27
// Button pins
#define MODE_BTN 32
#define SELECT_BTN 33
// TL phases
#define Pred 0
#define Predyellow 1
#define Pgreen 2
#define Pyellow 3
// MQTT and server stuff
#define mqtt_port 1883   
const char* mqtt_server = "";
#define MQTT_USER ""
#define MQTT_PASSWORD ""
#define MQTT_willTopic ""
#define MQTT_willMessage ""
char ssid[] = "";
char password[] = "";
// MAC Address to traffic light id
#define NUMBER_OF_TL 15
String mac2id[NUMBER_OF_TL][2] = { { String("24:62:AB:F1:E1:A0"), String("01") },
                                   { String("24:62:AB:F1:E2:54"), String("02") },
                                   { String("24:6F:28:7B:E5:04"), String("03") },
                                   { String("24:6F:28:7B:DC:AC"), String("04") },
                                   { String("24:62:AB:F2:DD:F0"), String("05") },
                                   { String("24:6F:28:7C:04:94"), String("06") },
                                   { String("24:62:AB:F1:CC:60"), String("07") },
                                   { String("24:62:AB:F3:53:D0"), String("08") },
                                   { String("24:62:AB:F3:B5:8C"), String("09") },
                                   { String("24:62:AB:F2:DF:BC"), String("10") },
                                   { String("24:6F:28:7B:DC:44"), String("11") },
                                   { String("24:6F:28:7C:00:5C"), String("12") },
                                   { String("24:62:AB:F3:C8:70"), String("13") },
                                   { String("24:62:AB:F1:CF:7C"), String("14") },
                                   { String("24:62:AB:F1:93:3C"), String("15") } };
// Dummy values for topics - will be adapted according to the mac2id definitions in function updateTopicStrings
String topic_send = "TL0x";
String topic_version = "TL0xversion";
String topic_send_mode = "TL0xmode";
String topic_receive = "TL0xctrl";
// Modes
#define Mauto 0 /*TL switches automatically through all phases*/
#define Mmanual 1 /*State can be switched manually with SELECT button*/
#define MIoT 2 /*State will be switched by IoT broker or other clients*/

/********************/
/* Global variables */
/********************/
// TL durations, see "VwV-StVO zu § 37, Randnummer 17, Punkt IX": http://www.verwaltungsvorschriften-im-internet.de/bsvwvbund_26012001_S3236420014.htm
// Extract: Die Übergangszeit Rot und Gelb (gleichzeitig) soll für Kraftfahrzeugströme eine Sekunde dauern, darf aber nicht länger als zwei Sekunden sein
// Extract: In der Regel beträgt die Gelbzeit 3 s bei zul. V = 50 km/h, 4 s bei zul. V = 60 km/h und 5 s bei zul. V = 70 km/h
unsigned long durations[] = { 7000, 2000, 7000, 3000}; //ms
// current traffic light phase either in auto or manual mode
unsigned int current_phase = Pred; 
// Time when current TL phase was started
unsigned long phase_starttime = millis(); 
// current TL mode
unsigned int current_mode = Mauto; 
// MODE button was released after it was pressed
bool mode_btn_released = true; 
// SELECT button was released after it was pressed
bool select_btn_released = true; 
// Connection to internet router
WiFiClient wifiClient;
// MQTT client
PubSubClient client(wifiClient);
// standalone mode without WiFi and MQTT
bool standalone = false;

// ESP-32 setup function
// called once by MCU
void setup()
{
    // Set up pins of ESP-32
    pinMode(MODE_BTN, INPUT_PULLUP);
    pinMode(SELECT_BTN, INPUT_PULLUP);
    pinMode(RED, OUTPUT);
    pinMode(YELLOW, OUTPUT);
    pinMode(GREEN, OUTPUT);
    // Make the random number sequence to vary after each restart
    randomSeed(analogRead(0)); 
    // Set up serial interface (debug messages)
    Serial.begin(115200);
    Serial.setTimeout(500);
    // Initial phase and mode
    current_phase = Pred;
    current_mode = Mauto;
    // Detect standalone request from select button
    if ( digitalRead(SELECT_BTN) == LOW ) // pressed
    {
      standalone = true;
    }
    // Wifi and server connection
    if (!standalone)
    {
      setup_wifi();
      client.setServer(mqtt_server, mqtt_port);
      client.setCallback(callback);
    }
    // Apply the initial phase
    TL_phase(current_phase);
    // Time when current TL phase was started
    phase_starttime = millis();
}

// ESP-32 loop function
// called repeatedly by MCU
void loop()
{
  // Handle the MQTT client
  client.loop();
  // MODE button
  if ( handleButton(MODE_BTN, mode_btn_released) )
  {
    // Change the current mode (Mauto -> Mmanual -> MIoT -> Mauto ...)
    switchMode();
    // Show current mode by number of blinking LEDs. The while loop is for handling pressing of buttons inside blinkResponse
    while (!blinkResponse())
    {
    }
    // Switch to current phase
    TL_phase(current_phase);
  }
  // Continue depending on mode
  switch (current_mode)
  {
    case Mauto:
      mode_auto();
      break;
    case Mmanual:
      mode_manual();
      break;
    case MIoT:
      mode_iot();
      break;
    default:
      TL_off();
      break;
  }
}

// Switch mode and initialize new mode
void switchMode()
{
  // Normally we have 3 modes
  int numberOfModes = 3;
  // In standalone (without IoT connection) we have only 2 modes
  if (standalone)
  {
    numberOfModes = 2;
  }
  // Next mode (Mauto -> Mmanual -> MIoT -> Mauto ...)
  current_mode = (current_mode + 1) % numberOfModes;
  // Initial phase is always red
  current_phase = Pred;
  // Time when current TL phase was started
  phase_starttime = millis();
  // Apply current phase
  TL_phase(current_phase);
}

// Gives respone by blinking LEDs
// Mode 0: only red blinks 3 times
// Mode 1: red and yellow blink 3 times
// Mode 2: all blink 3 times
// Function returns false, if the response was aborted by an additional button press
bool blinkResponse()
{
  // half of the blinking period in ms
  const unsigned long DURATION = 300;
  // completion of response
  bool response_complete = false;
  // abortion of response
  bool response_aborted = false;
  while ( !response_complete && !response_aborted)
  {
    int i = 0;
    // Let the LEDs blink 3 times
    for (i = 0; i < 3; i++)
    {
      // ON - number of LEDs depend on current mode
      TL_mode(current_mode);
      unsigned long starttime = millis();
      unsigned long runtime = millis() - starttime;
      while (runtime < DURATION)
      {
        // MODE button should be handled to be able to switch the mode further
        if ( handleButton(MODE_BTN, mode_btn_released) )
        {
          switchMode();
          // the following variable is used to break the upper while loop
          response_aborted = true;
          break;
        }
        runtime = millis() - starttime;
      }
      // Leave the upper while loop - because again mode was changed
      if (response_aborted)
      {
        break;
      }
      // OFF
      TL_off();
      starttime = millis();
      runtime = millis() - starttime;
      while (runtime < DURATION)
      {
        // MODE button
        if ( handleButton(MODE_BTN, mode_btn_released) )
        {
          switchMode();
          // the following variable is used to break the upper while loop
          response_aborted = true;
          break;
        }
        runtime = millis() - starttime;
      }
      // Leave the upper while loop - because again mode was changed
      if (response_aborted)
      {
        break;
      }
    }
    // LEDs blinked 3 times -> response is finished
    if (i>=3)
    {
      response_complete = true;
    }
  }
  // Return if the response was really completed or aborted by another button press
  return response_complete;
}

// Loop function for auto mode
void mode_auto()
{
  // current time
  unsigned long t = millis();
  // phase complete?
  if (t - phase_starttime > durations[current_phase])
  {
    // Switch to next phase
    current_phase = (current_phase + 1) % 4;
    // Apply phase
    TL_phase(current_phase);
    // Time when current TL phase was started
    phase_starttime = millis();
  }
}

// Loop function for manual mode
void mode_manual()
{
  // Select button check
  if (handleButton(SELECT_BTN, select_btn_released) == true)
  {
    // Switch to next phase
    current_phase = (current_phase + 1) % 4;
    // Apply phase
    TL_phase(current_phase);
    // Time when current TL phase was started
    phase_starttime = millis();
  }
}

// Loop function for IoT mode
void mode_iot()
{
  // Nothing to be done - traffic lights are controlled on receival of message
  // For debugging: Print if WiFi not connected
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi disconnected");
  }
  // For debugging: Print if client not connected
  if (!client.connected())
  {
    Serial.println("client disconnected");
  }
  // For debugging: At least make sure if the client is still connected
  reconnect();
}

// Functions defined to set TL-LEDs
void TL_phase(unsigned int phase)
{
    switch (phase)
    {
      case Pred:
        TL_red();
        break;
      case Predyellow:
        TL_redyellow();
        break;
      case Pgreen:
        TL_green();
        break;
      case Pyellow:
        TL_yellow();
        break;
      default: // invalid state - switch all lights off
        TL_off();
        break;
    }
    // If standalone mode is not selected, publish the current state
    if (!standalone)
    {
      pubsubBroker();
    }
}
// Functions defined to set TL-LEDs for displaying the mode
void TL_mode(unsigned int m)
{
    switch (m)
    {
      case Mauto:
        TL_red();
        break;
      case Mmanual:
        TL_redyellow();
        break;
      case MIoT:
        TL_redyellowgreen();
        break;
      default: // invalid state - switch all lights off
        TL_off();
        break;
    }
}

// Applies the red phase
void TL_red()
{
    digitalWrite(RED, HIGH);
    digitalWrite(YELLOW, LOW);
    digitalWrite(GREEN, LOW);
}
// Applies the red/yellow phase
void TL_redyellow()
{
    digitalWrite(RED, HIGH);
    digitalWrite(YELLOW, HIGH);
    digitalWrite(GREEN, LOW);
}
// Applies the green phase
void TL_green()
{
    digitalWrite(RED, LOW);
    digitalWrite(YELLOW, LOW);
    digitalWrite(GREEN, HIGH);
}
// Applies the yellow phase
void TL_yellow()
{
    digitalWrite(RED, LOW);
    digitalWrite(YELLOW, HIGH);
    digitalWrite(GREEN, LOW);
}
// Switches off all LEDs
void TL_off()
{
    digitalWrite(RED, LOW);
    digitalWrite(YELLOW, LOW);
    digitalWrite(GREEN, LOW);
}
// Applies the red/yellow/green phase
void TL_redyellowgreen()
{
    digitalWrite(RED, HIGH);
    digitalWrite(YELLOW, HIGH);
    digitalWrite(GREEN, HIGH);
}

/* Button handling
   The function detects state changes of the defined button.
   It implements also a 100ms delay time to overcome a (mechanical) bouncing effect of the button.
   Parameters:
   - button_id: The button that shall be handled (MODE_BTN or SELECT_BTN)
   - button_logic: a boolean variable that can be used by the function to detect the state changes beyond many calls
                   this should be a global unique variable for one specific button
   Returns true, if press event was accepted
*/
bool handleButton( unsigned int button_id, bool & button_logic )
{
  bool retVal = false;
  // last known button state is released
  if ( button_logic ) 
  {
    // button pressed
    if ( digitalRead(button_id) == LOW ) 
    {
      // debouncing
      delay(100); 
      retVal = true;
      button_logic = false;
    }
  }
  // last known button state is pressed
  else
  {
    // button released
    if ( digitalRead(button_id) == HIGH ) 
    {
      // debouncing
      delay(100); 
      button_logic = true;
    }
  }
  return retVal;
}

// Start connection with wifi router
// call it once in setup
// Attention: function never ends if connection not possible
void setup_wifi()
{
    //delay(10);
    // We start by connecting to a WiFi network
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    // Print MAC address - useful for updating table mac2id
    Serial.print("ESP Board MAC Address: ");
    Serial.println(WiFi.macAddress());
    // Update the topic strings according to table mac2id
    updateTopicStrings();
    Serial.print("Traffic light name: ");
    Serial.println(topic_send);
    // During connection process, let the yellow LED blink
    unsigned int counter = 0;
    while (WiFi.status() != WL_CONNECTED)
    {
        // One blinking sequence
        TL_yellow();
        delay(250);
        TL_off();
        delay(250);
        // Also show it in debug output
        Serial.print(".");
        counter++;
        // After 10 attempts, restart the ESP because it could be an initialization problem
        if (counter > 10)
        {
          ESP.restart();
        }
    }
    // Output of successful connection
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

// Connect to MQTT broker
// call it when you want to publish something and client is not connected
// Attention: function never ends if connection not possible
// When trying to connect the first time, the green LED will blink until connected
void reconnect()
{
  // Currently unused variable
  static bool onceConnected = false;
  // Loop until we're reconnected
  while (!client.connected())
  {
    //delay(3000); // Debugging - for seeing the output in the Serial after start
    // A random client number. If we use a static number, this could confuse the broker if more than one clients connect with the same number
    char chr_rndNb[15];
    long rndNb = random(2147483645); // maximum long
    // Also add the send topic to the client number
    String str_rndNb = topic_send + String(rndNb);
    str_rndNb.toCharArray(chr_rndNb, 15);
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    unsigned int counter = 0;
    while ( ! client.connect(chr_rndNb, MQTT_USER, MQTT_PASSWORD) )
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      for ( counter = 0; counter < 10; counter++ )
      {
        TL_green();
        delay(250);
        TL_off();
        delay(250);
        Serial.print(".");
      }
    }
    // Output of successful connection
    Serial.println("connected");
    pubsubBroker();
  }
}

// Here the TL states are published
// do not call to often when using an online broker that doesn't allow too much publishing during time
void pubsubBroker()
{
  if (!client.connected())
  {
    reconnect();
    // Subscribe to the remote control topic
    char dummy4[10] = "";
    topic_receive.toCharArray(dummy4, 10);
    client.subscribe(dummy4);
    Serial.print("subscribed for the topic ");
    Serial.println(topic_receive);
  }
  // Convert the send topics strings
  char dummy[10] = "";
  topic_send.toCharArray(dummy, 10);
  char dummy2[10] = "";
  topic_send_mode.toCharArray(dummy2, 10);
  char dummy3[12] = "";
  topic_version.toCharArray(dummy3, 12);
  // Once connected, publish the phase of the traffic light
  switch (current_phase)
  {
    case Pred:
      client.publish(dummy, "red", true);
      break;
    case Predyellow:
      client.publish(dummy, "redyellow", true);
      break;
    case Pgreen:
      client.publish(dummy, "green", true);
      break;
    case Pyellow:
      client.publish(dummy, "yellow", true);
      break;
    default:
      client.publish(dummy, "", true);
      break;
  }
  // Publish the mode of the traffic light
  switch (current_mode)
  {
    case Mauto:
      client.publish(dummy2, "auto", true);
      break;
    case Mmanual:
      client.publish(dummy2, "manual", true);
      break;
    case MIoT:
      client.publish(dummy2, "remote", true);
      break;
    default:
      client.publish(dummy2, "", true);
      break;
  }
  // Publish the software version
  client.publish(dummy3, swVersion, true);
}

// MQTT callback function
// called when MQTT message received
void callback(char* topic, byte *payload, unsigned int length)
{
    // Conversion of the payload and debug output
    Serial.println("callback called");
    String strPayload = String((char*)payload);
    strPayload = strPayload.substring(0, length);
    Serial.println("Message received");
    Serial.print(topic);
    Serial.print(" = ");
    Serial.println(strPayload);
    //Serial.println(length, DEC);
    //payload[length] = '\0';
    // Handle the received TL phase only if it is in mode IoT
    if (current_mode == MIoT)
    {
      Serial.println("IoT mode ok");
      //Serial.print("Substring 3:");
      //Serial.println(strPayload.substring(0,3));
      // Analyze the payload and detect the requested TL phase
      if (strPayload == "red")
      {
        current_phase = Pred;
      }
      else if (strPayload == "redyellow")
      {
        current_phase = Predyellow;
      }
      else if (strPayload == "yellow")
      {
        current_phase = Pyellow;
      }
      else if (strPayload == "green")
      {
        current_phase = Pgreen;
      }
      else
      {
        current_phase = -1; // invalid value
      }
      // Apply the requested phase
      TL_phase(current_phase);
   }
}

// Create the real topic strings based on the table mac2id
// If MAC id could not be found, the default topic strings will be kept
void updateTopicStrings()
{
  String mac = WiFi.macAddress();
  unsigned int i = 0;
  for (int i=0; i<NUMBER_OF_TL; i++)
  {
    if (mac == mac2id[i][0])
    {
      topic_send = String("TL") + mac2id[i][1];
      topic_send_mode = topic_send + String("mode");
      topic_receive = topic_send + String("ctrl");
      topic_version = topic_send + String("version");
    }
  }
}
