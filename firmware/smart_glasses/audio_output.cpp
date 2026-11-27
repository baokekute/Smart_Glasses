#include "audio_output.h"

void initSpeaker()
{
    Serial.println("Speaker Ready");
}

void playAudio(String text)
{
    Serial.println(
        "Speaking: " + text
    );
}