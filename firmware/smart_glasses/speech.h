#ifndef SPEECH_H
#define SPEECH_H

#include <Arduino.h>

void initSpeechRecognition();

String listenSpeech();
String getRecognizedSpeech();

#endif