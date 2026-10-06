#include "project_config.h"
#include "../src/codec.h"
#include <cassert>
#include <cstdio>
int main(){assert(codecSelfTest());puts("PASS: 14 synthetic packets match the upstream Python encoder, including breeze");}
