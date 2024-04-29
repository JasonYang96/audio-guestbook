/**
 * Audio Guestbook, Copyright (c) 2022 Playful Technology
 * 
 * Tested using a Teensy 4.0 with Teensy Audio Shield, although should work 
 * with minor modifications on other similar hardware
 * 
 * When handset is lifted, a pre-recorded greeting message is played, followed by a tone.
 * Then, recording starts, and continues until the handset is replaced.
 * Playback button allows all messages currently saved on SD card through earpiece 
 * 
 * Files are saved on SD card as 44.1kHz, 16-bit, mono signed integer RAW audio format 
 * --> changed this to WAV recording, DD4WH 2022_07_31
 * --> added MTP support, which enables copying WAV files from the SD card via the USB connection, DD4WH 2022_08_01
 * 
 * 
 * Frank DD4WH, August 1st 2022 
 * for a DBP 611 telephone (closed contact when handheld is lifted) & with recording to WAV file
 * contact for switch button 0 is closed when handheld is lifted
 * 
 * GNU GPL v3.0 license
 * 
 */

#include <Bounce.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TimeLib.h>
#include "MTP_Teensy.h"
#include "play_sd_wav.h" // local copy with fixes

// DEFINES
// Define pins used by Teensy Audio Shield
#define SDCARD_CS_PIN    10
#define SDCARD_MOSI_PIN  11
#define SDCARD_SCK_PIN   13
// And those used for inputs
#define HOOK_PIN 0

#define noINSTRUMENT_SD_WRITE

// GLOBALS
// Audio initialisation code can be generated using the GUI interface at https://www.pjrc.com/teensy/gui/
// Inputs
AudioSynthWaveform          waveform1; // To create the "beep" sfx
AudioInputI2S               i2s2; // I2S input from microphone on audio shield
AudioPlaySdWavX             playWav1; // Play 44.1kHz 16-bit PCM greeting WAV file
AudioRecordQueue            queue1; // Creating an audio buffer in memory before saving to SD
AudioMixer4                 mixer; // Allows merging several inputs to same output
AudioOutputI2S              i2s1; // I2S interface to Speaker/Line Out on Audio shield
AudioConnection patchCord1(waveform1, 0, mixer, 0); // wave to mixer 
AudioConnection patchCord3(playWav1, 0, mixer, 1); // wav file playback mixer
AudioConnection patchCord4(mixer, 0, i2s1, 0); // mixer output to speaker (L)
AudioConnection patchCord6(mixer, 0, i2s1, 1); // mixer output to speaker (R)
AudioConnection patchCord5(i2s2, 0, queue1, 0); // mic input to queue (L)
AudioControlSGTL5000      sgtl5000_1;

// Filename to save audio recording on SD card
char filename[15];
// The file object itself
File frec;

// Use long 40ms debounce time on both switches
Bounce buttonRecord = Bounce(HOOK_PIN, 40);

// Keep track of current state of the device
enum Mode {Initialising, Ready, WaitForUser, StartGreeting, PlayGreeting, PlayStartBeep, StartRecording, Recording, PlayEndBeep};
Mode mode = Mode::Initialising;

float beep_volume = 0.2f; // not too loud :-)

uint32_t MTPcheckInterval; // default value of device check interval [ms]

// variables for writing to WAV file
unsigned long ChunkSize = 0L;
unsigned long Subchunk1Size = 16;
unsigned int AudioFormat = 1;
unsigned int numChannels = 1;
unsigned long sampleRate = 44100;
unsigned int bitsPerSample = 16;
unsigned long byteRate = sampleRate*numChannels*(bitsPerSample/8);// samplerate x channels x (bitspersample / 8)
unsigned int blockAlign = numChannels*bitsPerSample/8;
unsigned long Subchunk2Size = 0L;
unsigned long recByteSaved = 0L;
unsigned long NumSamples = 0L;
byte byte1, byte2, byte3, byte4;

// variables to set up waiting
unsigned long previousMillis;
const long msToWaitForUser = 1000;

void setup() {
  Serial.begin(9600);
  while (!Serial && millis() < 5000) {
    // wait for serial port to connect.
  }
  Serial.println("Serial set up correctly");
  Serial.printf("Audio block set to %d samples\n",AUDIO_BLOCK_SAMPLES);
  print_mode();
  // Configure the input pins
  pinMode(HOOK_PIN, INPUT_PULLUP);

  // Audio connections require memory, and the record queue
  // uses this memory to buffer incoming audio.
  AudioMemory(60);

  // Enable the audio shield, select input, and enable output
  sgtl5000_1.enable();
  // Define which input on the audio shield to use (AUDIO_INPUT_LINEIN / AUDIO_INPUT_MIC)
  sgtl5000_1.inputSelect(AUDIO_INPUT_MIC);
  sgtl5000_1.adcHighPassFilterEnable();
  sgtl5000_1.micGain(39); // in decibels 
  sgtl5000_1.volume(0.95); // in percent

  mixer.gain(0, 1.0f);
  mixer.gain(1, 1.0f);

  // Initialize the SD card
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
  if (!(SD.begin(SDCARD_CS_PIN))) {
    // stop here if no SD card, but print a message
    while (1) {
      Serial.println("Unable to access the SD card");
      delay(500);
    }
  } else {
    Serial.println("SD card correctly initialized");
  }

  // mandatory to begin the MTP session.
  MTP.begin();

  // Add SD Card
  MTP.addFilesystem(SD, "Audio Guestbook"); // choose a nice name for the SD card volume to appear in your file explorer
  Serial.println("Added SD card via MTP");
  MTPcheckInterval = MTP.storage()->get_DeltaDeviceCheckTimeMS();

  // Synchronise the Time object used in the program code with the RTC time provider.
  // See https://github.com/PaulStoffregen/Time
  setSyncProvider(getTeensy3Time);
  
  // Define a callback that will assign the correct datetime for any file system operations
  // (i.e. saving a new audio recording onto the SD card)
  FsDateTime::setCallback(dateTime);

  mode = Mode::Ready; print_mode();

  // Play a beep to indicate system is online
  waveform1.begin(beep_volume, 440, WAVEFORM_SINE);
  delay(1000);
  waveform1.amplitude(0);
  delay(1000);
}

void loop() {
  // First, read the button
  buttonRecord.update();

  switch(mode){
    case Mode::Ready:
      // Falling edge occurs when the handset is lifted --> 611 telephone
      if (buttonRecord.fallingEdge()) {
        Serial.println("Handset lifted");
        // Wait a second for users to put the handset to their ear
        previousMillis = millis();
        mode = Mode::WaitForUser; print_mode();
      }
      break;

    case Mode::WaitForUser:
      if (buttonRecord.fallingEdge()) {
        mode = Mode::Ready; print_mode();
        break;
      }

      if (millis() - previousMillis >= msToWaitForUser) {
        mode = Mode::StartGreeting; print_mode();
      }

      break;

    case Mode::StartGreeting:
      if (buttonRecord.fallingEdge()) {
        mode = Mode::Ready; print_mode();
        break;
      }
      // Play the greeting inviting them to record their message
      playWav1.play("greeting.wav");
      mode = Mode::PlayGreeting; print_mode();
      break;

    case Mode::PlayGreeting:
      // If message still playing and handset is replaced
      if(buttonRecord.fallingEdge()) {
        playWav1.stop();
        mode = Mode::Ready; print_mode();
      } else if (playWav1.isStopped()) { // message has stopped playing
        mode = Mode::PlayStartBeep; print_mode();
      } // else message still playing, loop again
      break;

    case Mode::PlayStartBeep:
      if (buttonRecord.fallingEdge()) {
        mode = Mode::Ready; print_mode();
      } else {
        // Play the tone sound effect
        waveform1.begin(beep_volume, 440, WAVEFORM_SINE);
        delay(500);
        waveform1.amplitude(0);
        mode = Mode::StartRecording; print_mode();
      }
      break;

    case Mode::StartRecording:
      if (buttonRecord.fallingEdge()) {
        mode = Mode::Ready; print_mode();
      } else {
        startRecording(); // sets mode to Recording
      }

    case Mode::Recording:
      // Handset is replaced
      if(buttonRecord.fallingEdge()){
        Serial.println("Stopping Recording");
        stopRecording();
        mode = Mode::Ready; print_mode();
      }
      else {
        continueRecording();
      }
      break;

    case Mode::PlayEndBeep:
      if(buttonRecord.fallingEdge()) { // picked up the phone while the endbeep is playing
        mode = Mode::StartGreeting; print_mode();
      } else {
        // Play audio tone to confirm recording has ended
        end_Beep();
        mode = Mode::Ready; print_mode();
      }
      break;

    case Mode::Initialising: // to make compiler happy
      break;
  }   
  
  MTP.loop();  // This is mandatory to be placed in the loop code.
}

void setMTPdeviceChecks(bool enable)
{
  if (enable)
  {
    MTP.storage()->set_DeltaDeviceCheckTimeMS(MTPcheckInterval);
    Serial.print("En");
  }
  else
  {
    MTP.storage()->set_DeltaDeviceCheckTimeMS((uint32_t) -1);
    Serial.print("Dis");
  }
  Serial.println("enabled MTP storage device checks");
}
  

#if defined(INSTRUMENT_SD_WRITE)
static uint32_t worstSDwrite, printNext;
#endif // defined(INSTRUMENT_SD_WRITE)

void startRecording() {
  setMTPdeviceChecks(false); // disable MTP device checks while recording
  #if defined(INSTRUMENT_SD_WRITE)
    worstSDwrite = 0;
    printNext = 0;
  #endif // defined(INSTRUMENT_SD_WRITE)
  // Find the first available file number
  for (uint16_t i=0; i<9999; i++) {   
    // Format the counter as a five-digit number with leading zeroes, followed by file extension
    snprintf(filename, 11, " %05d.wav", i);
    // Create if does not exist, do not open existing, write, sync after write
    if (!SD.exists(filename)) {
      break;
    }
  }
  frec = SD.open(filename, FILE_WRITE);
  Serial.println("Opened file !");
  if(frec) {
    Serial.print("Recording to ");
    Serial.println(filename);
    queue1.begin();
    mode = Mode::Recording; print_mode();
    recByteSaved = 0L;
  }
  else {
    Serial.println("Couldn't open file to record!");
  }
  Serial.println("Starting Recording");
}

void continueRecording() {
  #if defined(INSTRUMENT_SD_WRITE)
    uint32_t started = micros();
  #endif // defined(INSTRUMENT_SD_WRITE)
  #define NBLOX 16  
  // Check if there is data in the queue
  if (queue1.available() >= NBLOX) {
    byte buffer[NBLOX*AUDIO_BLOCK_SAMPLES*sizeof(int16_t)];
    // Fetch 2 blocks from the audio library and copy
    // into a 512 byte buffer.  The Arduino SD library
    // is most efficient when full 512 byte sector size
    // writes are used.
    for (int i=0;i<NBLOX;i++)
    {
      memcpy(buffer+i*AUDIO_BLOCK_SAMPLES*sizeof(int16_t), queue1.readBuffer(), AUDIO_BLOCK_SAMPLES*sizeof(int16_t));
      queue1.freeBuffer();
    }
    // Write all 512 bytes to the SD card
    frec.write(buffer, sizeof buffer);
    recByteSaved += sizeof buffer;
  }
  
  #if defined(INSTRUMENT_SD_WRITE)
    started = micros() - started;
    if (started > worstSDwrite)
      worstSDwrite = started;

    if (millis() >= printNext)
    {
      Serial.printf("Worst write took %luus\n",worstSDwrite);
      worstSDwrite = 0;
      printNext = millis()+250;
    }
  #endif // defined(INSTRUMENT_SD_WRITE)
}

void stopRecording() {
  // Stop adding any new data to the queue
  queue1.end();
  // Flush all existing remaining data from the queue
  while (queue1.available() > 0) {
    // Save to open file
    frec.write((byte*)queue1.readBuffer(), AUDIO_BLOCK_SAMPLES*sizeof(int16_t));
    queue1.freeBuffer();
    recByteSaved += AUDIO_BLOCK_SAMPLES*sizeof(int16_t);
  }
  writeOutHeader();
  // Close the file
  frec.close();
  Serial.println("Closed file");
  setMTPdeviceChecks(true); // enable MTP device checks, recording is finished
}

// Retrieve the current time from Teensy built-in RTC
time_t getTeensy3Time(){
  return Teensy3Clock.get();
}

// Callback to assign timestamps for file system operations
void dateTime(uint16_t* date, uint16_t* time, uint8_t* ms10) {

  // Return date using FS_DATE macro to format fields.
  *date = FS_DATE(year(), month(), day());

  // Return time using FS_TIME macro to format fields.
  *time = FS_TIME(hour(), minute(), second());

  // Return low time bits in units of 10 ms.
  *ms10 = second() & 1 ? 100 : 0;
}

// Non-blocking delay, which pauses execution of main program logic,
// but while still listening for input 
void wait(unsigned int milliseconds) {
  elapsedMillis msec=0;

  while (msec <= milliseconds) {
    buttonRecord.update();
    if (buttonRecord.fallingEdge()) Serial.println("Button (pin 0) Handset lifted");
    if (buttonRecord.fallingEdge()) Serial.println("Button (pin 0) Handset returned");
  }
}

void writeOutHeader() { // update WAV header with final filesize/datasize
  ChunkSize = Subchunk2Size + 34; // was 36;
  frec.seek(0);
  frec.write("RIFF");
  byte1 = ChunkSize & 0xff;
  byte2 = (ChunkSize >> 8) & 0xff;
  byte3 = (ChunkSize >> 16) & 0xff;
  byte4 = (ChunkSize >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.write("WAVE");
  frec.write("fmt ");
  byte1 = Subchunk1Size & 0xff;
  byte2 = (Subchunk1Size >> 8) & 0xff;
  byte3 = (Subchunk1Size >> 16) & 0xff;
  byte4 = (Subchunk1Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = AudioFormat & 0xff;
  byte2 = (AudioFormat >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = numChannels & 0xff;
  byte2 = (numChannels >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = sampleRate & 0xff;
  byte2 = (sampleRate >> 8) & 0xff;
  byte3 = (sampleRate >> 16) & 0xff;
  byte4 = (sampleRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = byteRate & 0xff;
  byte2 = (byteRate >> 8) & 0xff;
  byte3 = (byteRate >> 16) & 0xff;
  byte4 = (byteRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = blockAlign & 0xff;
  byte2 = (blockAlign >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = bitsPerSample & 0xff;
  byte2 = (bitsPerSample >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  frec.write("data");
  byte1 = Subchunk2Size & 0xff;
  byte2 = (Subchunk2Size >> 8) & 0xff;
  byte3 = (Subchunk2Size >> 16) & 0xff;
  byte4 = (Subchunk2Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.close();
  Serial.println("header written"); 
  Serial.print("Subchunk2: "); 
  Serial.println(Subchunk2Size); 
}

void end_Beep(void) {
  waveform1.frequency(523.25);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
  wait(250);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
  wait(250);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
  wait(250);
  waveform1.amplitude(beep_volume);
  wait(250);
  waveform1.amplitude(0);
}

void print_mode(void) { // only for debugging
  Serial.print("Mode switched to: ");
  // Initialising, Ready, Prompting, Recording, Playing
  if(mode == Mode::Ready)               Serial.println(" Ready");
  else if(mode == Mode::WaitForUser)    Serial.println(" WaitForUser");
  else if(mode == Mode::StartGreeting)  Serial.println(" StartGreeting");
  else if(mode == Mode::PlayGreeting)   Serial.println(" PlayGreeting");
  else if(mode == Mode::PlayStartBeep)  Serial.println(" PlayStartBeep");
  else if(mode == Mode::StartRecording) Serial.println(" StartRecording");
  else if(mode == Mode::Recording)      Serial.println(" Recording");
  else if(mode == Mode::PlayEndBeep)    Serial.println(" PlayEndBeep");
  else if(mode == Mode::Initialising)   Serial.println(" Initialising");
  else Serial.println(" Undefined");
}
