#pragma once

#include "vec4.h"
#include "colour.h"

// keep light straightforward - struct for storing information
struct Light {
    vec4 omega_i; // light direction
    colour L; // light colour
    colour ambient; // ambient light component 
};

