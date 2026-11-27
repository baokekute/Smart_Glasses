#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h> 

void initDisplay();
void displayMessage(String msg);
void resetLiveTranscriptDisplay();
void setLiveTranscriptStatus(String status);
void displayLiveTranscript(String text);
void displayLiveTranscriptParts(String stableText, String interimText);
void displayCorrectedLiveTranscript(String text);
void updateLiveTranscriptDisplay();
void displayLiveTranslation(String text);
void updateLiveTranslationDisplay();
bool isLiveTranslationTyping();
void holdLiveTranslationDisplay();
void displayFinalTranscript(String text);
void displayTranslation(
    String original,
    String translated
);
void displayTranslationOnly(String translated);

#endif