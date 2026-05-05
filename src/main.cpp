/*
  Zoe Rochelle
  Final Project
  CSC530 - Dr. Blake
  04/27/2026
*/

#include <Arduino.h>
#include <U8x8lib.h>
#include "SdFat.h"
#include <IRremote.h>
//#include <IRremote.hpp>
#include <Wire.h> // needed for using Wire.begin()
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "esp_random.h"
#include "pcf8563.h" // used for logging timestamps to the SD card
#include "mic_code.h"

#define SD_CS_PIN 21
#define IR_RECEIVE_PIN D0
#define IR_RESET_CMD 0x19

PCF8563_Class rtc;
U8X8_SSD1306_128X64_NONAME_HW_I2C u8x8(U8X8_PIN_NONE);
SdFat SD;
File32 myFile;
const char FILE_NAME[] = "lock_config.txt";
const char EVENT_LOG_FILE[] = "unlock_log.txt";

int lock_state = 1;

/* IR input functions */
char input_code[5] = {0};   // size of 5 needed because of the null terminator
uint8_t input_counter = 0;

const char *color_array[] = {
  "Blue",    // 1
  "Cyan",    // 2
  "Green",   // 3
  "Magenta", // 4
  "Red",     // 5
  "White",   // 6
  "Yellow"   // 7
};

char color_pw[16] = "";
char color_index[4] = "";

char input_to_Digit(uint8_t cmd){
  if (cmd == 0x16) return '0';
  if (cmd == 0xC) return '1';
  if (cmd == 0x18) return '2';
  if (cmd == 0x5E) return '3';
  if (cmd == 0x8) return '4';
  if (cmd == 0x1C) return '5';
  if (cmd == 0x5A) return '6';
  if (cmd == 0x42) return '7';
  if (cmd == 0x52) return '8';
  if (cmd == 0x4A) return '9';
  return '\0';
}


/* SD card read/write functions */

// create new file
bool create_file(const char *filename){
  myFile = SD.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
  if (!myFile) return false;
  myFile.close();
  return true;
}

// open and write to the file
void write_to_file(const char *filename, const char *text){
  myFile = SD.open(filename, FILE_WRITE);
  if (myFile) {
    Serial.print("writing to ");
    Serial.print(filename);
    myFile.println(text);
    myFile.close();
    Serial.println("done.");
  }
  else{
    Serial.println("error opening txt file");
  }
}

// open and read from file
String read_from_file(const char *filename){
  // re-open the file to read from it
  myFile = SD.open(filename);
  if (myFile) {
    // read from the file until there's nothing else in it:
    String contents = "";
    while (myFile.available()) {
      contents += (char)myFile.read();
    }
    myFile.close();

    contents.trim();
    return contents;

  }
  else{
    Serial.print("error opening ");
    Serial.println(FILE_NAME);
    return "";
  }
}

/* OLED functionality */
void oled_init(){
  u8x8.begin();
  u8x8.setPowerSave(0);
  u8x8.setFont(u8x8_font_chroma48medium8_r);
  u8x8.clearDisplay();
}

void print_to_oled(const char* text_2 = ""){
  u8x8.clearDisplay();
  char text_1[9] = "LOCKED";
  if (lock_state == 0) {
    strcpy(text_1, "UNLOCKED");
  }
  u8x8.drawString(0, 1, text_1);
  u8x8.drawString(0, 2, text_2);
}


/* buzzer functions */

const int speakerPin = D3;

// Play one tone for duration_ms
void play_tone_helper(int freqHz, int duration_ms) {
  if (freqHz <= 0) { delay(duration_ms); return; }
  tone(speakerPin, freqHz, duration_ms);  // built-in Arduino tone()
  delay(duration_ms);
  noTone(speakerPin);
}

// Positive - play when successfully unlocked
void play_positive_melody() {
  play_tone_helper(880, 120);   // A5
  delay(30);
  play_tone_helper(1175, 160);  // D6
}

// Negative - play when failed unlock attempt
void play_negative_melody() {
  play_tone_helper(392, 160);   // G4
  delay(30);
  play_tone_helper(294, 220);   // D4
}


/* timestamp functions */
String rtcTimestamp(int config) {  // 0: full timestamp, 1: date only, 2: time only
  time_t epoch;
  time(&epoch);

  struct tm tm_utc;
  gmtime_r(&epoch, &tm_utc);

  if (config == 0) {
    char buf[24];
    strftime(buf, sizeof(buf), "%Y:%m:%d:%H:%M:%S", &tm_utc);
    return String(buf);
  }
  else if (config == 1) {
    char buf[16];
    strftime(buf, sizeof(buf), "%Y:%m:%d", &tm_utc);
    return String(buf);
  }
  else if (config == 2) {
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_utc);
    return String(buf);
  }
  else {
    return String("");
  }
}

// format -> YY:MM:DD:HH:MM:SS,STAGE,RESULT,DETAIL
void log_event(const char* filename, const char* message) {
  String timestamp = rtcTimestamp(0);          // e.g., "2026:03:04:22:00:00"
  char datetime_buff[128];
  snprintf(datetime_buff, sizeof(datetime_buff), "%s,%s", timestamp.c_str(), message);
  write_to_file(filename, datetime_buff);
}

/* reset functions */

void reset_code(){
  input_counter = 0;
  memset(input_code, 0, sizeof(input_code));
}

void reset_system_state() {
  print_to_oled("RESET");
  delay(500);
  print_to_oled("");

  lock_state = 1;
  reset_code();

  int random_num = rand() % 7;
  snprintf(color_index, sizeof(color_index), "%d", random_num + 1);
  strcpy(color_pw, color_array[random_num]);

  print_to_oled("");
  play_negative_melody();
  log_event(EVENT_LOG_FILE, "Stage: RESET, Result: PASS, Detail: remote reset");
}


/* authentication functions */

// Run the ML code on the microphone input to recognize the password
int ml_interface_begin() {
  return mic_listen_for_word(color_pw);
}

// re-write this to reduce the nested branching stmts?
void authentication_check(){

  // read then parse the code from the file on the SD card. Should be in the format "CODE:1234"
  String file_text = read_from_file(FILE_NAME);
  int colon_pos = file_text.indexOf(':');
  String unlock_code = "";
  if (colon_pos >= 0) {
    unlock_code = file_text.substring(colon_pos + 1);
    unlock_code.trim();
  }

  // if the code matches, then prompt for the color password and begin ML inference
  if (strcmp(input_code, unlock_code.c_str()) == 0){

    log_event(EVENT_LOG_FILE, "Stage: CODE, Result: PASS, Detail: correct code");

    // print the color to the OLED
    char oled_text[12];
    snprintf(oled_text, sizeof(oled_text), "Speak PW: %s", color_index);
    print_to_oled(oled_text);

    // begin processing the ML inference on the microphone input to recognize the password word
    int pass = ml_interface_begin();

    if(pass == 1){
      lock_state = 0; // unlocked
      play_positive_melody();
      print_to_oled("GRANTED");
      log_event(EVENT_LOG_FILE, "Stage: VOICE, Result: PASS, Detail: correct word");
    }
    else{
      lock_state = 1; // locked
      play_negative_melody();
      print_to_oled("DENIED");
      log_event(EVENT_LOG_FILE, "Stage: VOICE, Result: FAIL, Detail: incorrect word");
    }

  }
  else{
    lock_state = 1; // locked

    // // reset color password and generate a new random int from 1-7, and print it to OLED
    // int random_num = (rand() % 7);
    // strcpy(color_pw, color_array[random_num]);
    // snprintf(color_index, sizeof(color_index), "%d", random_num);

    // // update screen
    // char oled_text[12];
    // snprintf(oled_text, sizeof(oled_text), "Index: %s", color_index);
    // print_to_oled(oled_text);

    play_negative_melody();

    log_event(EVENT_LOG_FILE, "Stage: CODE, Result: FAIL, Detail: incorrect code");

    Serial.println("Incorrect code. Try again.");
  }
}

void setup(){
  Serial.begin(115200);
  unsigned long start = millis();                   // wait for the serial port to connect, this is needed for USB connection
  while (!Serial && millis() - start < 2000) { }    // wait up to 2 seconds - this is a non-blocking delay

  oled_init();

  Wire.begin();
  rtc.begin(Wire);
  rtc.syncToSystem();

  Serial.print("initialize SD card");
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("initialization failed!");
    return;
  }
  Serial.println("initialization done.");

  // initialize the file with the code "1234" for demonstration purposes
  // create_file(FILE_NAME);
  // write_to_file(FILE_NAME, "CODE:1234");
  // also create the event log file
  create_file(EVENT_LOG_FILE);
  // generate a random int from 1-7
  srand((unsigned)esp_random());    // apparently ESP32 has a hardware RNG

  // then map it to the PW vocab list
  int random_num = rand() % 7;
  snprintf(color_index, sizeof(color_index), "%d", random_num + 1);
  strcpy(color_pw, color_array[random_num]);

  // print the lock state to the OLED, wipe any previous text with ""
  print_to_oled("");

  // set up buzzer pin
  pinMode(speakerPin, OUTPUT);

  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  Serial.println("IR 4-digit code input ready.");
  Serial.println("Press digits; prints code when 4 digits entered.");
}


void loop(){
  // Serial.println("Listening for IR input...");

  // continuously check IR input
  if (!IrReceiver.decode()) return;

  // ignore repeated presses
  if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
    IrReceiver.resume();
    return;
  }

  // check for the reset button press
  if (IrReceiver.decodedIRData.command == IR_RESET_CMD) {
    Serial.println("Reset button pressed.");
    reset_system_state();
    IrReceiver.resume();
    return;
  }

  char digit = input_to_Digit(IrReceiver.decodedIRData.command);
  if (digit != '\0') {
    input_code[input_counter++] = digit;
    input_code[input_counter] = '\0';

    Serial.print("Typed: ");
    Serial.println(input_code);

    // update screen
    char oled_text[12];
    snprintf(oled_text, sizeof(oled_text), "Typed: %s", input_code);
    print_to_oled(oled_text);

    if (input_counter >= 4) {
      Serial.print("4-digit code: ");
      Serial.println(input_code);

      // check if the code matches the unlock code read from the SD card
      // if match, then unlock.
      authentication_check();
      reset_code();
    }
  }

  IrReceiver.resume();

  // delay(3000);
}

// implement functionality to reset upon button press.
