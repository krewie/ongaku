#include "ongaku.h"
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
/* -------------------- Main -------------------- */
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.flac | folder> [volume 0.0-1.0]\n", argv[0]);
        return 1;
    }
    
    float volume = (argc >= 3) ? (float)atof(argv[2]) : 1.0f;
    return ongaku_play(argv[1], volume);
}