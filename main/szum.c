#include "szum.h"
#include <stdlib.h>

int16_t generate_brown_noise(void) {
    static float last_output = 0.0f;
    float white = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    last_output = (last_output + (0.02f * white)) / 1.02f;
    return (int16_t)(last_output * 8000.0f);
}