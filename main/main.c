#include <stdio.h>
#include "../source/daemon/resource/daemon.h"

int main(int argc, char *argv[]) {
    printf("[*] Starting VPN Parser Daemon...\n");
    return start_daemon();
}