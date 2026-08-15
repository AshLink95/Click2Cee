#include "server.hpp"

//TODO: main app (server) logic; 2 threads: 1 for sending and the other for reading

int main() {
    cap_init().display();

    log_entry le = snd("", snd_typ::tcp);
    le.display();

    cap_end().display();
    return 0;
}
